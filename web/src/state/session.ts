// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { Store } from "@tanstack/react-store";

import { TraceStore } from "../model/traces";
import { RemoteClient } from "../protocol/client";
import type { Message, Notice } from "../protocol/messages";
import { Metadata } from "../protocol/metadata";
import type { Frame } from "../protocol/mirror";
import { viewStore } from "./view";

export type LinkStatus = "connecting" | "live" | "reconnecting" | "refused" | "unauthorised";

export interface SessionState {
  status: LinkStatus;
  serverName: string;
  shared: boolean;
  /** Why the link went, or why it was turned away. */
  error: string;
  /** Bumped whenever a state section changes, for components to re-read. */
  sectionsVersion: number;
  roundTripMs: number;
  bytesPerSec: number;
  linesPerSec: number;
  framesPerSec: number;
  /** Seconds until the next attempt while reconnecting. */
  retryInSec: number;
  attempt: number;
}

export const sessionStore = new Store<SessionState>({
  status: "connecting",
  serverName: "",
  shared: false,
  error: "",
  sectionsVersion: 0,
  roundTripMs: 0,
  bytesPerSec: 0,
  linesPerSec: 0,
  framesPerSec: 0,
  retryInSec: 0,
  attempt: 0,
});

/** A pass, queued for every panel's waterfall to take once. */
export interface WaterfallLine {
  levels: Float32Array;
  startHz: number;
  binWidthHz: number;
  hostNs: bigint;
}

type LineListener = (line: WaterfallLine) => void;
type NoticeListener = (notice: Notice) => void;
type MessageListener = (message: Message) => void;

const kBackoffSec = [1, 2, 4, 8, 15, 30];

/** This page's one connection to the server, and everything it feeds. */
class Session {
  readonly traces = new TraceStore();
  /** The newest frame, for the traces' consumer to draw. */
  latest: Frame | null = null;
  telemetry = new Metadata();

  private client: RemoteClient | null = null;
  private socket: WebSocket | null = null;
  private lineListeners = new Set<LineListener>();
  private noticeListeners = new Set<NoticeListener>();
  private messageListeners = new Set<MessageListener>();
  private tickTimer: number | undefined;
  private retryTimer: number | undefined;
  private attempt = 0;
  private wanted = false;
  private frameCount = 0;
  private lineCount = 0;
  private rateWindowStart = performance.now();

  get remote(): RemoteClient | null {
    return this.client;
  }

  section(name: string): Metadata {
    return this.client?.section(name) ?? new Metadata();
  }

  onLine(listener: LineListener): () => void {
    this.lineListeners.add(listener);
    return () => this.lineListeners.delete(listener);
  }

  /** Messages the client does not handle itself: history answers. */
  onMessage(listener: MessageListener): () => void {
    this.messageListeners.add(listener);
    return () => this.messageListeners.delete(listener);
  }

  onNotice(listener: NoticeListener): () => void {
    this.noticeListeners.add(listener);
    return () => this.noticeListeners.delete(listener);
  }

  /** Connects, and keeps reconnecting with backoff until `stop()`. */
  start(): void {
    this.wanted = true;
    this.connect();
  }

  stop(): void {
    this.wanted = false;
    window.clearTimeout(this.retryTimer);
    this.client?.close();
    this.teardown();
  }

  private connect(): void {
    window.clearTimeout(this.retryTimer);
    const scheme = location.protocol === "https:" ? "wss:" : "ws:";
    const socket = new WebSocket(`${scheme}//${location.host}/ws`);
    socket.binaryType = "arraybuffer";
    this.socket = socket;
    sessionStore.setState((s) => ({ ...s, status: this.attempt === 0 ? "connecting" : "reconnecting" }));

    const client = new RemoteClient(
      {
        send: (bytes) => {
          if (socket.readyState === WebSocket.OPEN) {
            socket.send(bytes as Uint8Array<ArrayBuffer>);
          }
        },
        close: () => socket.close(),
      },
      {
        welcome: (welcome) => {
          this.attempt = 0;
          sessionStore.setState((s) => ({
            ...s,
            status: "live",
            serverName: welcome.serverName,
            shared: welcome.shared,
            error: "",
            attempt: 0,
          }));
        },
        refused: (refused) => {
          this.wanted = false;
          sessionStore.setState((s) => ({ ...s, status: "refused", error: refused.message || refused.reason }));
        },
        frame: (frame, hostNs) => this.takeFrame(frame, hostNs),
        state: () => sessionStore.setState((s) => ({ ...s, sectionsVersion: s.sectionsVersion + 1 })),
        notice: (notice) => this.noticeListeners.forEach((listener) => listener(notice)),
        message: (message) => this.messageListeners.forEach((listener) => listener(message)),
        telemetry: (report) => {
          this.telemetry = report;
        },
        closed: (reason) => this.lost(reason),
      },
    );
    this.client = client;

    socket.onopen = () => {
      client.open(clientName(), clientId());
      window.clearInterval(this.tickTimer);
      this.tickTimer = window.setInterval(() => this.tick(), 250);
    };
    socket.onmessage = (event) => client.receive(new Uint8Array(event.data as ArrayBuffer));
    socket.onclose = (event) => {
      // Closed before the server said anything: the upgrade itself was
      // refused, which is how an expired login shows. Back to the login page
      // then, rather than round again.
      if (client.welcome === null && !client.closed) {
        void checkAuth().then((authorised) =>
          authorised ? client.lost("could not reach the server") : this.unauthorised(),
        );
        return;
      }
      client.lost(event.reason || "the connection closed");
    };
  }

  private unauthorised(): void {
    this.wanted = false;
    this.teardown();
    sessionStore.setState((s) => ({ ...s, status: "unauthorised" }));
  }

  private takeFrame(frame: Frame, hostNs: bigint): void {
    // A pass from while this page was away belongs in the waterfall's past,
    // not on the live trace.
    if (frame.commit.replayed) {
      const line = { levels: frame.levels, startHz: frame.startHz, binWidthHz: frame.binWidthHz, hostNs };
      this.lineListeners.forEach((listener) => listener(line));
      return;
    }
    const view = viewStore.state;
    this.latest = frame;
    this.traces.update(frame.startHz, frame.binWidthHz, frame.levels, hostNs, {
      smoothing: view.smoothing,
      maxHoldDecayDbPerSec: view.maxHoldDecayDbPerSec,
      averageWindow: view.averageWindow,
      maxHold: view.showMaxHold,
      minHold: view.showMinHold,
      average: view.showAverage,
    });
    ++this.frameCount;
    // A row a pass while sweeping, a row a frame at a fixed tune: what the
    // desktop's waterfall shows.
    const sweeping = this.section("run").getBool("sweeping");
    if (!sweeping || frame.commit.passComplete) {
      ++this.lineCount;
      const line = { levels: frame.levels, startHz: frame.startHz, binWidthHz: frame.binWidthHz, hostNs };
      this.lineListeners.forEach((listener) => listener(line));
    }
  }

  private tick(): void {
    const client = this.client;
    if (!client) {
      return;
    }
    client.tick();
    const now = performance.now();
    const seconds = (now - this.rateWindowStart) / 1000;
    if (seconds >= 1) {
      const link = client.link;
      sessionStore.setState((s) => ({
        ...s,
        roundTripMs: link.roundTripMs,
        bytesPerSec: link.bytesPerSec,
        framesPerSec: this.frameCount / seconds,
        linesPerSec: this.lineCount / seconds,
      }));
      this.frameCount = 0;
      this.lineCount = 0;
      this.rateWindowStart = now;
    }
  }

  private lost(reason: string): void {
    this.teardown();
    if (!this.wanted) {
      return;
    }
    const delay = kBackoffSec[Math.min(this.attempt, kBackoffSec.length - 1)]!;
    ++this.attempt;
    sessionStore.setState((s) => ({
      ...s,
      status: "reconnecting",
      error: reason,
      retryInSec: delay,
      attempt: this.attempt,
    }));
    this.retryTimer = window.setTimeout(() => this.connect(), delay * 1000);
  }

  /** Tries again now rather than at the next backoff. */
  retryNow(): void {
    if (sessionStore.state.status === "reconnecting") {
      this.connect();
    }
  }

  private teardown(): void {
    window.clearInterval(this.tickTimer);
    if (this.socket) {
      this.socket.onclose = null;
      this.socket.onmessage = null;
      if (this.socket.readyState === WebSocket.OPEN || this.socket.readyState === WebSocket.CONNECTING) {
        this.socket.close();
      }
    }
    this.socket = null;
  }
}

export const session = new Session();

export async function checkAuth(): Promise<boolean> {
  try {
    const response = await fetch("/api/auth");
    const auth = (await response.json()) as { required: boolean; authenticated: boolean };
    return !auth.required || auth.authenticated;
  } catch {
    return false;
  }
}

/** "Firefox on iPhone": what the server lists this page as. */
export function clientName(): string {
  const agent = navigator.userAgent;
  const browser = /Edg\//.test(agent)
    ? "Edge"
    : /Firefox\//.test(agent)
      ? "Firefox"
      : /Chrome\//.test(agent)
        ? "Chrome"
        : /Safari\//.test(agent)
          ? "Safari"
          : "Browser";
  const platform = /iPhone/.test(agent)
    ? "iPhone"
    : /iPad/.test(agent)
      ? "iPad"
      : /Android/.test(agent)
        ? "Android"
        : /Mac OS X/.test(agent)
          ? "Mac"
          : /Windows/.test(agent)
            ? "Windows"
            : /Linux/.test(agent)
              ? "Linux"
              : "";
  return platform ? `${browser} on ${platform}` : browser;
}

/** Random, kept in this browser, so a reconnect is known as one. */
export function clientId(): string {
  const key = "sweeppp.clientId";
  try {
    let id = localStorage.getItem(key);
    if (!id) {
      id = [...crypto.getRandomValues(new Uint8Array(16))].map((b) => b.toString(16).padStart(2, "0")).join("");
      localStorage.setItem(key, id);
    }
    return id;
  } catch {
    return "";
  }
}
