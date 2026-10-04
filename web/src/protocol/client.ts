// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { ByteWriter, ProtocolError } from "./bytes";
import { ClockMap } from "./clock";
import {
  appendMessage,
  clientKind,
  type ControlState,
  decodeControl,
  decodeFrameCommit,
  decodeMessage,
  decodeNotice,
  decodePong,
  decodeRefused,
  decodeReply,
  decodeState,
  decodeTelemetry,
  decodeWelcome,
  encodeBye,
  encodeCommand,
  encodeHello,
  encodePing,
  kMaxServerRecordBytes,
  kPingIntervalMs,
  kProtocolVersion,
  kSilenceTimeoutMs,
  type Message,
  msg,
  type Notice,
  NoticeKind,
  refusal,
  type Refused,
  section,
  sectionsTouchedBy,
  type Welcome,
} from "./messages";
import { Metadata, ValueType } from "./metadata";
import { type Frame, FrameMirror } from "./mirror";
import {
  decodeStreamHeader,
  encodeStreamHeader,
  kStreamHeaderBytes,
  RecordFramer,
  RecordType,
  type StreamRecord,
} from "./records";

/** Where the bytes go: a WebSocket in the page, an array in a test. */
export interface Transport {
  send(bytes: Uint8Array): void;
  close(): void;
}

export interface ClientEvents {
  welcome?(welcome: Welcome): void;
  refused?(refused: Refused): void;
  /** A frame, with its times on this page's clock. */
  frame?(frame: Frame, hostNs: bigint, wallMs: number): void;
  /** Sections that changed, by name. */
  state?(changed: ReadonlySet<string>): void;
  notice?(notice: Notice): void;
  telemetry?(report: Metadata): void;
  /** An Event record, undecoded. */
  event?(record: StreamRecord): void;
  /** A message nothing here handles: a recording's chunk, a newer kind. */
  message?(message: Message): void;
  closed?(reason: string): void;
}

export interface LinkStats {
  roundTripMs: number;
  bytesReceived: number;
  bytesPerSec: number;
}

/** This page's monotonic clock, in nanoseconds. */
export function monotonicNs(): bigint {
  return BigInt(Math.round(performance.now() * 1e6));
}

const kRateWindowNs = 1_000_000_000n;

/** The server's state as this page knows it, and the link that keeps it so.
 * A port of RemoteInstrument's cache, transport and UI left out.
 *
 * An edit lands in the local copy at once and goes to the server as a command;
 * the server's word on what that edit touched is taken again only once it has
 * acknowledged the command, so a late state cannot undo what was just done.
 * A command the server refuses comes back as a notice. */
export class RemoteClient {
  readonly sections = new Map<string, Metadata>();

  private readonly framer = new RecordFramer(kMaxServerRecordBytes);
  private readonly mirror = new FrameMirror();
  private readonly clocks = new ClockMap();
  private readonly pending = new Map<string, bigint>();
  private header = new Uint8Array(0);
  private sawHeader = false;
  private seq = 0n;
  private pingId = 0n;
  private nextPingNs = 0n;
  private lastHeardNs: bigint;
  private rateWindowNs = 0n;
  private rateWindowBytes = 0;
  private closedReason: string | null = null;

  welcome: Welcome | null = null;
  bytesReceived = 0;
  bytesPerSec = 0;

  constructor(
    private readonly transport: Transport,
    private readonly events: ClientEvents,
    private readonly now: () => bigint = monotonicNs,
  ) {
    this.lastHeardNs = now();
  }

  /** Opens the stream: the header, then who this page is. */
  open(name: string, clientId: string, software = "Sweep++ web"): void {
    const out = new ByteWriter();
    encodeStreamHeader(out);
    appendMessage(
      out,
      msg.hello,
      encodeHello({ protocolVersion: kProtocolVersion, software, kind: clientKind.web, name, clientId }),
      this.now(),
    );
    this.transport.send(out.finish());
  }

  get closed(): boolean {
    return this.closedReason !== null;
  }

  /** Says goodbye and closes; the server stops the radio if nobody is left. */
  close(): void {
    if (this.closedReason !== null) {
      return;
    }
    const out = new ByteWriter();
    appendMessage(out, msg.bye, encodeBye("closed"), this.now());
    this.transport.send(out.finish());
    this.end("closed");
    this.transport.close();
  }

  /** The transport went: whatever it says now is why. */
  lost(reason: string): void {
    this.end(reason);
  }

  private end(reason: string): void {
    if (this.closedReason !== null) {
      return;
    }
    this.closedReason = reason;
    this.events.closed?.(reason);
  }

  // ---- receiving -----------------------------------------------------------------

  receive(data: Uint8Array): void {
    if (this.closedReason !== null) {
      return;
    }
    this.bytesReceived += data.length;
    this.lastHeardNs = this.now();
    try {
      if (!this.sawHeader) {
        const joined = new Uint8Array(this.header.length + data.length);
        joined.set(this.header);
        joined.set(data, this.header.length);
        if (joined.length < kStreamHeaderBytes) {
          this.header = joined;
          return;
        }
        decodeStreamHeader(joined.subarray(0, kStreamHeaderBytes));
        this.sawHeader = true;
        this.header = new Uint8Array(0);
        data = joined.subarray(kStreamHeaderBytes);
      }
      this.framer.feed(data);
      for (let record = this.framer.next(); record; record = this.framer.next()) {
        this.handle(record);
        if (this.closedReason !== null) {
          return;
        }
      }
    } catch (error) {
      this.end(error instanceof Error ? error.message : String(error));
      this.transport.close();
    }
  }

  private handle(record: StreamRecord): void {
    switch (record.type) {
      case RecordType.SegmentOpen:
      case RecordType.Tile:
      case RecordType.SegmentClose:
        this.mirror.apply(record);
        return;
      case RecordType.Event:
        this.events.event?.(record);
        return;
      case RecordType.Telemetry:
        this.events.telemetry?.(decodeTelemetry(record));
        return;
      case RecordType.EndOfStream:
        this.end("the server ended the stream");
        return;
      case RecordType.PluginData:
        break;
      default:
        return;
    }

    const message = decodeMessage(record);
    const now = this.now();
    switch (message.name) {
      case msg.welcome:
        this.welcome = decodeWelcome(message.body);
        this.events.welcome?.(this.welcome);
        break;
      case msg.refused: {
        const refused = decodeRefused(message.body);
        this.events.refused?.(refused);
        this.end(refusalText(refused));
        break;
      }
      case msg.frame: {
        const commit = decodeFrameCommit(message.body);
        const frame = this.mirror.commit(commit);
        const hostNs = this.clocks.toClient(commit.hostTimeNs, now);
        const wallMs = Date.now() - Number(now - hostNs) / 1e6;
        this.events.frame?.(frame, hostNs, wallMs);
        break;
      }
      case msg.pong: {
        const pong = decodePong(message.body);
        this.clocks.observe(pong.clientNs, pong.serverNs, now);
        break;
      }
      case msg.state: {
        const state = decodeState(message.body);
        this.applyState(state.ackSeq, state.sections);
        break;
      }
      case msg.reply: {
        const reply = decodeReply(message.body);
        if (!reply.ok) {
          this.events.notice?.({ kind: NoticeKind.Error, text: reply.message });
        }
        break;
      }
      case msg.notice:
        this.events.notice?.(decodeNotice(message.body));
        break;
      case msg.bye: {
        const reason = message.body.getString("reason");
        this.end(reason === refusal.shutdown ? "the server shut down" : `the server said goodbye: ${reason}`);
        break;
      }
      default:
        this.events.message?.(message);
        break;
    }
  }

  private applyState(ackSeq: bigint, sections: Metadata): void {
    const changed = new Set<string>();
    for (const [name, v] of sections.entries) {
      if (v.type !== ValueType.Hash) {
        continue;
      }
      const held = this.pending.get(name);
      if (held !== undefined) {
        if (ackSeq < held) {
          continue;
        }
        this.pending.delete(name);
      }
      this.sections.set(name, v.value);
      changed.add(name);
    }
    if (changed.size > 0) {
      this.events.state?.(changed);
    }
  }

  // ---- sending -------------------------------------------------------------------

  /** One section as last known; empty before the server has sent it. */
  section(name: string): Metadata {
    return this.sections.get(name) ?? new Metadata();
  }

  get control(): ControlState {
    return decodeControl(this.section(section.control));
  }

  /** Whether this page may change anything; true until the server says. */
  get canControl(): boolean {
    return !this.sections.has(section.control) || this.control.you;
  }

  /** Queues `op`, holding what it touches until it is answered. `local` puts
   * the edit into this page's copy at once. Its sequence number, or zero with
   * no link to send it on. */
  command(op: string, args = new Metadata(), local?: (sections: Map<string, Metadata>) => void): bigint {
    if (this.closedReason !== null) {
      return 0n;
    }
    const seq = ++this.seq;
    for (const name of sectionsTouchedBy(op)) {
      this.pending.set(name, seq);
    }
    if (local) {
      local(this.sections);
      this.events.state?.(new Set(sectionsTouchedBy(op)));
    }
    const out = new ByteWriter();
    appendMessage(out, msg.command, encodeCommand({ seq, op, args }), this.now());
    this.transport.send(out.finish());
    return seq;
  }

  /** Pings once a second, measures the rate, and notices silence. */
  tick(): void {
    if (this.closedReason !== null) {
      return;
    }
    const now = this.now();
    if (now - this.lastHeardNs > BigInt(kSilenceTimeoutMs) * 1_000_000n) {
      this.end(`nothing heard for ${kSilenceTimeoutMs / 1000} s`);
      this.transport.close();
      return;
    }
    if (now >= this.nextPingNs) {
      const out = new ByteWriter();
      appendMessage(out, msg.ping, encodePing(++this.pingId, now), now);
      this.transport.send(out.finish());
      this.nextPingNs = now + BigInt(kPingIntervalMs) * 1_000_000n;
    }
    if (this.rateWindowNs === 0n) {
      this.rateWindowNs = now;
      this.rateWindowBytes = this.bytesReceived;
    } else if (now - this.rateWindowNs >= kRateWindowNs) {
      this.bytesPerSec = ((this.bytesReceived - this.rateWindowBytes) * 1e9) / Number(now - this.rateWindowNs);
      this.rateWindowNs = now;
      this.rateWindowBytes = this.bytesReceived;
    }
  }

  get link(): LinkStats {
    return {
      roundTripMs: Number(this.clocks.lastRoundTripNs) / 1e6,
      bytesReceived: this.bytesReceived,
      bytesPerSec: this.bytesPerSec,
    };
  }
}

export function refusalText(refused: Refused): string {
  switch (refused.reason) {
    case refusal.busy:
      return "The radio is in use by another client.";
    case refusal.limit:
      return "The server has as many clients as it takes.";
    case refusal.version:
      return refused.message;
    default:
      return refused.message || refused.reason;
  }
}

export { ProtocolError };
