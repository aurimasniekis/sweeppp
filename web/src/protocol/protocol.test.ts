// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { describe, expect, it } from "vitest";

import { ByteReader, ByteWriter, ProtocolError, utf8 } from "./bytes";
import { RemoteClient, type Transport } from "./client";
import { ClockMap } from "./clock";
import {
  appendMessage,
  decodeMessage,
  encodeCommand,
  msg,
  NoticeKind,
  op,
  section,
  sectionsTouchedBy,
  viewerMay,
} from "./messages";
import { kMaxMetadataDepth, Metadata, value, ValueType } from "./metadata";
import { dequantiseDb, FrameMirror } from "./mirror";
import { appendRecord, crc32, encodeStreamHeader, RecordFramer, RecordType } from "./records";

describe("records", () => {
  it("checksums as CRC-32/IEEE", () => {
    expect(crc32(utf8("123456789"))).toBe(0xcbf43926);
    expect(crc32(new Uint8Array(0))).toBe(0);
  });

  it("frames records however the bytes arrive", () => {
    const out = new ByteWriter();
    appendRecord(out, RecordType.Event, utf8("one"));
    appendRecord(out, RecordType.Telemetry, new Uint8Array(0));
    appendRecord(out, RecordType.PluginData, utf8("three"));
    const bytes = out.finish();
    const framer = new RecordFramer();
    const seen: string[] = [];
    for (const byte of bytes) {
      framer.feed(new Uint8Array([byte]));
      for (let record = framer.next(); record; record = framer.next()) {
        seen.push(`${record.type}:${new TextDecoder().decode(record.payload)}`);
      }
    }
    expect(seen).toEqual(["4:one", "8:", "9:three"]);
    expect(framer.buffered).toBe(0);
  });

  it("breaks for good on a bad checksum or an oversized record", () => {
    const out = new ByteWriter();
    appendRecord(out, RecordType.Event, utf8("payload"));
    const corrupt = out.finish();
    corrupt[corrupt.length - 1]! ^= 0xff;
    const framer = new RecordFramer();
    framer.feed(corrupt);
    expect(() => framer.next()).toThrow(ProtocolError);
    expect(() => framer.next()).toThrow(ProtocolError);

    const big = new ByteWriter().u16(5).u16(0).u32(1000).u32(0).finish();
    const small = new RecordFramer(100);
    small.feed(big);
    expect(() => small.next()).toThrow(/exceeds/);
  });
});

describe("metadata", () => {
  it("round-trips every type, keys in byte order", () => {
    const inner = new Metadata().setFloat("x", 1.5);
    const m = new Metadata()
      .setString("zeta", "last")
      .setString("alpha", "ünïcode")
      .setInt("big", -9007199254740993n)
      .setBool("flag", true)
      .setHash("inner", inner)
      .set("bytes", value.bytes(new Uint8Array([1, 2, 3])))
      .set("list", value.array(ValueType.Int, [value.int(1), value.int(2)]));
    const bytes = m.toBytes();
    const back = Metadata.fromBytes(bytes);
    expect(back.toBytes()).toEqual(bytes);
    expect(back.getString("alpha")).toBe("ünïcode");
    expect(back.getBigInt("big")).toBe(-9007199254740993n);
    expect(back.getHash("inner").getFloat("x")).toBe(1.5);
    expect(back.getArray("list")).toHaveLength(2);
    // The first key encoded is the smallest.
    const reader = new ByteReader(bytes);
    reader.u32();
    expect(reader.string()).toBe("alpha");
  });

  it("reads a field of another type as the fallback, never converted", () => {
    const m = new Metadata().setInt("n", 3).setFloat("f", 2.5);
    expect(m.getFloat("n", -1)).toBe(-1);
    expect(m.getInt("f", -1)).toBe(-1);
    expect(m.getString("missing", "x")).toBe("x");
  });

  it("refuses counts that cannot fit and nesting past the cap", () => {
    expect(() => Metadata.fromBytes(new ByteWriter().u32(1_000_000).finish())).toThrow(ProtocolError);
    let deep = new Metadata();
    for (let i = 0; i < kMaxMetadataDepth + 2; ++i) {
      deep = new Metadata().setHash("d", deep);
    }
    expect(() => Metadata.fromBytes(deep.toBytes())).toThrow(/deep/);
    const badTag = new ByteWriter().u32(1).string("k").u8(42).finish();
    expect(() => Metadata.fromBytes(badTag)).toThrow(/not defined/);
  });
});

describe("messages", () => {
  it("carries a command and decodes it back", () => {
    const out = new ByteWriter();
    const args = new Metadata().setString("key", "gain");
    appendMessage(out, msg.command, encodeCommand({ seq: 7n, op: op.setParameter, args }), 99n);
    const framer = new RecordFramer();
    framer.feed(out.finish());
    const message = decodeMessage(framer.next()!);
    expect(message.name).toBe(msg.command);
    expect(message.monotonicNs).toBe(99n);
    expect(message.body.getBigInt("seq")).toBe(7n);
    expect(message.body.getHash("args").getString("key")).toBe("gain");
  });

  it("knows what each command touches, and what a viewer may send", () => {
    expect(sectionsTouchedBy(op.setParameter)).toContain(section.values);
    expect(sectionsTouchedBy(op.takeControl)).toEqual([section.control, section.clients]);
    expect(sectionsTouchedBy("teleport")).toEqual([]);
    expect(viewerMay(op.setLinkResolution)).toBe(true);
    expect(viewerMay(op.start)).toBe(false);
  });
});

describe("mirror", () => {
  it("dequantises as the C++ does, to f32", () => {
    expect(dequantiseDb(0, -100)).toBe(Math.fround(-200));
    expect(dequantiseDb(1, -100.25)).toBe(Math.fround(-99.75));
    expect(dequantiseDb(255, -90)).toBe(Math.fround(37.5));
  });

  it("refuses a frame for a segment that is not open", () => {
    const mirror = new FrameMirror();
    expect(() =>
      mirror.commit({
        segmentId: 0,
        line: 0,
        sequence: 0n,
        hostTimeNs: 0n,
        wallTimeNs: 0n,
        deviceTimeNs: 0n,
        sweepPass: 0n,
        sweepStep: 0,
        passComplete: false,
        averageCount: 1,
        clippedFraction: 0,
        replayed: false,
      }),
    ).toThrow(/not open/);
  });
});

describe("clock", () => {
  it("takes the offset from the fastest round trip, and only moves forward", () => {
    const clocks = new ClockMap();
    expect(clocks.toClient(5n, 1000n)).toBe(1000n);
    clocks.observe(1000n, 50_000n, 1400n); // 400 ns trip, offset 1200 - 50000
    clocks.observe(2000n, 51_000n, 2100n); // 100 ns trip, offset 2050 - 51000
    expect(clocks.bestRoundTripNs).toBe(100n);
    expect(clocks.toClient(52_000n, 10_000n)).toBe(52_000n + 2050n - 51_000n);
    // Never after now.
    expect(clocks.toClient(1_000_000n, 10_000n)).toBe(10_000n);
    // Never before what it last said.
    expect(clocks.toClient(51_000n, 20_000n)).toBe(10_000n);
  });
});

/** A server's half, by hand: records into the client, commands out of it. */
function harness() {
  const sent: Uint8Array[] = [];
  const transport: Transport = { send: (bytes) => sent.push(bytes), close: () => undefined };
  const notices: string[] = [];
  const changed: string[][] = [];
  let now = 1_000_000_000n;
  const client = new RemoteClient(
    transport,
    {
      notice: (n) => notices.push(`${n.kind}:${n.text}`),
      state: (names) => changed.push([...names]),
    },
    () => now,
  );
  const server = (build: (out: ByteWriter) => void) => {
    const out = new ByteWriter();
    build(out);
    client.receive(out.finish());
  };
  const header = new ByteWriter();
  encodeStreamHeader(header);
  client.receive(header.finish());
  return {
    client,
    sent,
    notices,
    changed,
    server,
    advance: (ns: bigint) => {
      now += ns;
    },
  };
}

function stateMessage(out: ByteWriter, ackSeq: bigint, sections: Record<string, Metadata>): void {
  const body = new Metadata().setInt("ackSeq", ackSeq).setHash("sections", Metadata.of(
    Object.fromEntries(Object.entries(sections).map(([k, v]) => [k, value.hash(v)])),
  ));
  appendMessage(out, msg.state, body);
}

describe("client", () => {
  it("holds an edit against older state until the server acknowledges it", () => {
    const h = harness();
    h.server((out) => stateMessage(out, 0n, { values: new Metadata().setInt("gain", 10) }));
    expect(h.client.section(section.values).getInt("gain")).toBe(10);

    const seq = h.client.command(op.setParameter, new Metadata(), (sections) => {
      sections.set(section.values, new Metadata().setInt("gain", 30));
    });
    expect(h.client.section(section.values).getInt("gain")).toBe(30);

    // A state taken before the command ran cannot undo it...
    h.server((out) => stateMessage(out, seq - 1n, { values: new Metadata().setInt("gain", 10) }));
    expect(h.client.section(section.values).getInt("gain")).toBe(30);
    // ...but the acknowledgement's word is final, whatever it says.
    h.server((out) => stateMessage(out, seq, { values: new Metadata().setInt("gain", 24) }));
    expect(h.client.section(section.values).getInt("gain")).toBe(24);
  });

  it("turns a refused command into an error notice", () => {
    const h = harness();
    h.server((out) =>
      appendMessage(
        out,
        msg.reply,
        new Metadata().setInt("seq", 1).setBool("ok", false).setString("message", "desk has control"),
      ),
    );
    expect(h.notices).toEqual([`${NoticeKind.Error}:desk has control`]);
  });

  it("pings once a second and gives up after ten silent seconds", () => {
    const h = harness();
    h.client.tick();
    h.client.tick();
    expect(h.sent).toHaveLength(1);
    h.advance(1_000_000_000n);
    h.client.tick();
    expect(h.sent).toHaveLength(2);
    h.advance(11_000_000_000n);
    h.client.tick();
    expect(h.client.closed).toBe(true);
  });

  it("can control until the server says otherwise", () => {
    const h = harness();
    expect(h.client.canControl).toBe(true);
    h.server((out) =>
      stateMessage(out, 0n, {
        control: new Metadata().setBool("shared", true).setBool("held", true).setString("controller", "desk"),
      }),
    );
    expect(h.client.canControl).toBe(false);
    expect(h.client.control.controller).toBe("desk");
  });

  it("closes on a stream that does not start with the header", () => {
    const sent: Uint8Array[] = [];
    let reason = "";
    const client = new RemoteClient(
      { send: (b) => sent.push(b), close: () => undefined },
      { closed: (r) => (reason = r) },
    );
    client.receive(new Uint8Array(16).fill(0x41));
    expect(client.closed).toBe(true);
    expect(reason).toMatch(/magic/);
  });
});
