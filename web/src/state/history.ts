// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { msg, op } from "../protocol/messages";
import { Metadata } from "../protocol/metadata";
import { dequantiseDb } from "../protocol/mirror";
import { session } from "./session";

/** A recording on the server, as opening it describes it. */
export interface HistorySummary {
  handle: number;
  name: string;
  firstLineNs: bigint;
  lastLineNs: bigint;
  createdWallNs: bigint;
  totalLines: number;
  lowestHz: number;
  highestHz: number;
  segments: { id: number; startHz: number; binWidthHz: number; binCount: number; startMonotonicNs: bigint }[];
}

/** A block of a recording: `lines` lines of `bins` quantised levels. */
export interface HistoryTile {
  segmentId: number;
  lod: number;
  lines: number;
  bins: number;
  firstLineNs: bigint;
  lastLineNs: bigint;
  startHz: number;
  binWidthHz: number;
  originDb: number;
  data: Uint8Array;
}

export function tileDb(tile: HistoryTile, line: number, bin: number): number {
  return dequantiseDb(tile.data[line * tile.bins + bin]!, tile.originDb);
}

export interface HistoryRange {
  fromNs: bigint;
  toNs: bigint;
  fromHz: number;
  toHz: number;
  lines: number;
  bins: number;
}

/** Whatever answers the server owes this page, by what is being waited for. */
const waiting = new Map<string, (body: Metadata) => void>();
let listening = false;

function listen(): void {
  if (listening) {
    return;
  }
  listening = true;
  session.onMessage((message) => {
    if (message.name !== msg.history) {
      return;
    }
    const kind = message.body.getString("kind");
    const key =
      kind === "opened"
        ? `opened:${message.body.getString("name")}`
        : `tiles:${message.body.getBigInt("query")}`;
    waiting.get(key)?.(message.body);
  });
}

function decodeTile(row: Metadata): HistoryTile {
  return {
    segmentId: row.getInt("segmentId"),
    lod: row.getInt("lod"),
    lines: row.getInt("lines"),
    bins: row.getInt("bins"),
    firstLineNs: row.getBigInt("firstLineNs"),
    lastLineNs: row.getBigInt("lastLineNs"),
    startHz: row.getFloat("startHz"),
    binWidthHz: row.getFloat("binWidthHz"),
    originDb: row.getFloat("originDb"),
    data: row.getBytes("data") ?? new Uint8Array(0),
  };
}

const kTimeoutMs = 15_000;

/** Opens `name` on the server for reading. */
export function openHistory(name: string): Promise<HistorySummary> {
  listen();
  const remote = session.remote;
  if (!remote) {
    return Promise.reject(new Error("not connected"));
  }
  return new Promise((resolve, reject) => {
    const key = `opened:${name}`;
    const timer = window.setTimeout(() => {
      waiting.delete(key);
      reject(new Error(`${name} did not open`));
    }, kTimeoutMs);
    waiting.set(key, (body) => {
      window.clearTimeout(timer);
      waiting.delete(key);
      resolve({
        handle: body.getInt("handle"),
        name,
        firstLineNs: body.getBigInt("firstLineNs"),
        lastLineNs: body.getBigInt("lastLineNs"),
        createdWallNs: body.getBigInt("createdWallNs"),
        totalLines: body.getInt("totalLines"),
        lowestHz: body.getFloat("lowestHz"),
        highestHz: body.getFloat("highestHz"),
        segments: body.getHashes("segments").map((s) => ({
          id: s.getInt("id"),
          startHz: s.getFloat("startHz"),
          binWidthHz: s.getFloat("binWidthHz"),
          binCount: s.getInt("binCount"),
          startMonotonicNs: s.getBigInt("startMonotonicNs"),
        })),
      });
    });
    remote.command(op.historyOpen, new Metadata().setString("name", name));
  });
}

/** The tiles that cover `range`, at the detail its lines and bins ask for. */
export function queryHistory(handle: number, range: HistoryRange): Promise<HistoryTile[]> {
  listen();
  const remote = session.remote;
  if (!remote) {
    return Promise.reject(new Error("not connected"));
  }
  const args = new Metadata()
    .setInt("handle", handle)
    .setInt("fromNs", range.fromNs)
    .setInt("toNs", range.toNs)
    .setFloat("fromHz", range.fromHz)
    .setFloat("toHz", range.toHz)
    .setInt("lines", range.lines)
    .setInt("bins", range.bins);
  return new Promise((resolve, reject) => {
    const tiles: HistoryTile[] = [];
    const seq = remote.command(op.historyQuery, args);
    const key = `tiles:${seq}`;
    const timer = window.setTimeout(() => {
      waiting.delete(key);
      reject(new Error("the query timed out"));
    }, kTimeoutMs);
    waiting.set(key, (body) => {
      tiles.push(...body.getHashes("tiles").map(decodeTile));
      if (body.getBool("last")) {
        window.clearTimeout(timer);
        waiting.delete(key);
        resolve(tiles);
      }
    });
  });
}

export function closeHistory(handle: number): void {
  session.remote?.command(op.historyClose, new Metadata().setInt("handle", handle));
}
