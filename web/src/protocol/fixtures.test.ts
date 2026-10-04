// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { readFileSync } from "node:fs";
import { join } from "node:path";

import { describe, expect, it } from "vitest";

import { RemoteClient } from "./client";
import { decodeMessage, decodeState, msg, section } from "./messages";
import type { Frame } from "./mirror";
import { kStreamHeaderBytes, RecordFramer, RecordType } from "./records";

/** Written by `sweeppp-web-fixtures` from a real loopback server; ctest sets
 * the variable. Without it these are skipped. */
const directory = process.env.SWEEPPP_WEB_FIXTURES;

interface Expected {
  serverName: string;
  frames: {
    segmentId: number;
    line: number;
    sequence: number;
    passComplete: boolean;
    startHz: number;
    binWidthHz: number;
    levels: number[];
  }[];
  states: { ackSeq: number; sections: string[]; encoded: string }[];
  telemetryRecords: number;
  eventRecords: number;
  finalPlanLowestHz: number;
}

function load(): { stream: Uint8Array; expected: Expected } {
  return {
    stream: new Uint8Array(readFileSync(join(directory!, "stream.bin"))),
    expected: JSON.parse(readFileSync(join(directory!, "expected.json"), "utf8")) as Expected,
  };
}

function hex(bytes: Uint8Array): string {
  return [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
}

/** A deterministic generator, so a failure here fails the same way again. */
function random(seed: number): () => number {
  let state = seed >>> 0;
  return () => {
    state = (state * 1664525 + 1013904223) >>> 0;
    return state / 2 ** 32;
  };
}

/** Feeds `stream` to a fresh client in pieces of random size. */
function run(stream: Uint8Array, seed: number) {
  const frames: Frame[] = [];
  let telemetry = 0;
  let events = 0;
  let serverName = "";
  let closed: string | null = null;
  const client = new RemoteClient(
    { send: () => undefined, close: () => undefined },
    {
      welcome: (w) => (serverName = w.serverName),
      frame: (frame) => frames.push(frame),
      telemetry: () => ++telemetry,
      event: () => ++events,
      closed: (reason) => (closed = reason),
    },
  );
  const next = random(seed);
  for (let at = 0; at < stream.length; ) {
    const take = 1 + Math.floor(next() * 4096);
    client.receive(stream.subarray(at, at + take));
    at += take;
  }
  return { client, frames, telemetry, events, serverName, closed: closed as string | null };
}

describe.skipIf(!directory)("the browser decodes what the C++ server sent", () => {
  it("frame for frame, bin for bin", () => {
    const { stream, expected } = load();
    const result = run(stream, 1);
    expect(result.closed).toBeNull();
    expect(result.serverName).toBe(expected.serverName);
    expect(result.telemetry).toBe(expected.telemetryRecords);
    expect(result.events).toBe(expected.eventRecords);
    expect(result.frames).toHaveLength(expected.frames.length);
    expect(new Set(expected.frames.map((f) => f.segmentId)).size).toBeGreaterThan(1);

    expected.frames.forEach((want, i) => {
      const got = result.frames[i]!;
      expect(got.commit.segmentId).toBe(want.segmentId);
      expect(got.commit.line).toBe(want.line);
      expect(Number(got.commit.sequence)).toBe(want.sequence);
      expect(got.commit.passComplete).toBe(want.passComplete);
      expect(got.startHz).toBe(want.startHz);
      expect(got.binWidthHz).toBe(want.binWidthHz);
      expect(got.levels.length).toBe(want.levels.length);
      for (let bin = 0; bin < want.levels.length; ++bin) {
        if (got.levels[bin] !== Math.fround(want.levels[bin]!)) {
          expect.fail(`frame ${i} bin ${bin}: ${got.levels[bin]} != ${want.levels[bin]}`);
        }
      }
    });

    // The client's own copy ends where the server's state did.
    expect(result.client.section(section.plan).getHashes("segments")[0]?.getFloat("startHz")).toBe(
      expected.finalPlanLowestHz,
    );
  });

  it("every state's sections, re-encoded byte for byte", () => {
    const { stream, expected } = load();
    const framer = new RecordFramer();
    framer.feed(stream.subarray(kStreamHeaderBytes));
    const states: { ackSeq: bigint; encoded: string }[] = [];
    for (let record = framer.next(); record; record = framer.next()) {
      if (record.type !== RecordType.PluginData) {
        continue;
      }
      const message = decodeMessage(record);
      if (message.name === msg.state) {
        const state = decodeState(message.body);
        states.push({ ackSeq: state.ackSeq, encoded: hex(state.sections.toBytes()) });
      }
    }
    expect(states).toHaveLength(expected.states.length);
    states.forEach((state, i) => {
      expect(Number(state.ackSeq)).toBe(expected.states[i]!.ackSeq);
      expect(state.encoded).toBe(expected.states[i]!.encoded);
    });
  });

  it("fails cleanly, never by throwing, on a stream cut short or corrupted", () => {
    const { stream } = load();
    const next = random(7);
    for (let trial = 0; trial < 200; ++trial) {
      const cut = stream.slice(0, Math.floor(next() * stream.length));
      if (trial % 2 === 1 && cut.length > kStreamHeaderBytes) {
        for (let flips = 0; flips < 4; ++flips) {
          const at = kStreamHeaderBytes + Math.floor(next() * (cut.length - kStreamHeaderBytes));
          cut[at]! ^= 1 << Math.floor(next() * 8);
        }
      }
      expect(() => run(cut, trial)).not.toThrow();
    }
  });
});
