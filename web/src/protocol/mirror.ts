// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { ByteReader, ProtocolError } from "./bytes";
import type { FrameCommit } from "./messages";
import { RecordType, type StreamRecord } from "./records";

export const kTileBins = 1024;
export const kMaxGridBins = 16 * 1024 * 1024;
export const kDbPerStep = 0.5;
/** What an unmeasured bin reads as, outside the quantised form. */
export const kUnmeasuredDb = -200;

/** Whether a level is a reading at all: a floor rather than an equality. */
export function isMeasuredDb(db: number): boolean {
  return db > kUnmeasuredDb + 10;
}

/** Stored byte -> dB, exactly as sweeps::dequantiseDb and then to f32. */
export function dequantiseDb(byte: number, originDb: number): number {
  return byte === 0 ? Math.fround(kUnmeasuredDb) : Math.fround(originDb + byte * kDbPerStep);
}

export interface AcquisitionConfig {
  centerHz: number;
  spanHz: number;
  sampleRate: number;
  fftSize: number;
  window: number;
  windowBeta: number;
  windowEnbw: number;
  overlap: number;
  rbwHz: number;
  referenceLevelDbm: number;
  dbfsToDbmOffset: number;
  deviceId: string;
  deviceLabel: string;
  gains: [string, number][];
}

export interface SegmentGrid {
  startHz: number;
  binWidthHz: number;
  binCount: number;
}

export interface SegmentInfo {
  id: number;
  grid: SegmentGrid;
  startWallNs: bigint;
  startMonotonicNs: bigint;
  reason: string;
  config: AcquisitionConfig;
}

/** One line of the spectrum, as the server measured it. */
export interface Frame {
  commit: FrameCommit;
  startHz: number;
  binWidthHz: number;
  levels: Float32Array;
  config: AcquisitionConfig;
}

export function decodeAcquisitionConfig(in_: ByteReader): AcquisitionConfig {
  const config: AcquisitionConfig = {
    centerHz: in_.f64(),
    spanHz: in_.f64(),
    sampleRate: in_.f64(),
    fftSize: in_.u32(),
    window: in_.u32(),
    windowBeta: in_.f64(),
    windowEnbw: in_.f64(),
    overlap: in_.f64(),
    rbwHz: in_.f64(),
    referenceLevelDbm: in_.f64(),
    dbfsToDbmOffset: in_.f64(),
    deviceId: in_.string(),
    deviceLabel: in_.string(),
    gains: [],
  };
  const count = in_.u32();
  if (count > Math.floor(in_.remaining / 12)) {
    throw new ProtocolError(`${count} gains cannot fit in ${in_.remaining} bytes`);
  }
  for (let i = 0; i < count; ++i) {
    config.gains.push([in_.string(), in_.f64()]);
  }
  return config;
}

export function decodeSegmentOpen(payload: Uint8Array): SegmentInfo {
  const in_ = new ByteReader(payload);
  const id = in_.u32();
  const grid = { startHz: in_.f64(), binWidthHz: in_.f64(), binCount: in_.u32() };
  const startWallNs = in_.u64();
  const startMonotonicNs = in_.u64();
  const reason = in_.string();
  return { id, grid, startWallNs, startMonotonicNs, reason, config: decodeAcquisitionConfig(in_) };
}

export interface TileHeader {
  segmentId: number;
  lod: number;
  timeBlock: number;
  freqBlock: number;
  lines: number;
  bins: number;
  originDb: number;
  firstLineNs: bigint;
  lastLineNs: bigint;
}

export const kTileHeaderBytes = 44;

export function decodeTile(payload: Uint8Array): { header: TileHeader; data: Uint8Array } {
  const in_ = new ByteReader(payload);
  const header: TileHeader = {
    segmentId: in_.u32(),
    lod: in_.u32(),
    timeBlock: in_.u32(),
    freqBlock: in_.u32(),
    lines: in_.u32(),
    bins: in_.u32(),
    originDb: in_.f32(),
    firstLineNs: in_.u64(),
    lastLineNs: in_.u64(),
  };
  const bytes = header.lines * header.bins;
  if (bytes > in_.remaining) {
    throw new ProtocolError(`tile claims ${bytes} bytes of data but only ${in_.remaining} remain`);
  }
  return { header, data: in_.bytes(bytes) };
}

/** The receiving half of the frame codec: SegmentOpen and Tile records into
 * the current line, which each `frame` commit hands out whole. */
export class FrameMirror {
  private current: SegmentInfo | null = null;
  private levels = new Float32Array(0);

  get segment(): SegmentInfo | null {
    return this.current;
  }

  apply(record: StreamRecord): void {
    switch (record.type) {
      case RecordType.SegmentOpen:
        this.applySegmentOpen(record.payload);
        break;
      case RecordType.Tile:
        this.applyTile(record.payload);
        break;
      case RecordType.SegmentClose:
        // The levels stay what they were; the next SegmentOpen replaces them.
        new ByteReader(record.payload).u32();
        break;
      default:
        break;
    }
  }

  private applySegmentOpen(payload: Uint8Array): void {
    const segment = decodeSegmentOpen(payload);
    const { binCount, startHz, binWidthHz } = segment.grid;
    if (binCount === 0 || binCount > kMaxGridBins) {
      throw new ProtocolError(`segment ${segment.id} has ${binCount} bins; at most ${kMaxGridBins} are accepted`);
    }
    if (!Number.isFinite(startHz) || !Number.isFinite(binWidthHz) || binWidthHz <= 0) {
      throw new ProtocolError(`segment ${segment.id} has no usable grid`);
    }
    this.levels = new Float32Array(binCount).fill(kUnmeasuredDb);
    this.current = segment;
  }

  private applyTile(payload: Uint8Array): void {
    const { header, data } = decodeTile(payload);
    if (!this.current || header.segmentId !== this.current.id) {
      return;
    }
    if (header.lod !== 0 || header.lines !== 1) {
      throw new ProtocolError(`a tile of ${header.lines} lines at level ${header.lod} on a live stream`);
    }
    const binCount = this.current.grid.binCount;
    const first = header.freqBlock * kTileBins;
    if (first >= binCount || header.bins !== Math.min(kTileBins, binCount - first)) {
      throw new ProtocolError(`tile ${header.freqBlock} of ${header.bins} bins does not fit a ${binCount}-bin grid`);
    }
    if (!Number.isFinite(header.originDb)) {
      throw new ProtocolError(`tile ${header.freqBlock} has no usable origin`);
    }
    for (let i = 0; i < header.bins; ++i) {
      this.levels[first + i] = dequantiseDb(data[i]!, header.originDb);
    }
  }

  /** The line as of this commit; a copy, so later tiles do not change it. */
  commit(commit: FrameCommit): Frame {
    if (!this.current || commit.segmentId !== this.current.id) {
      throw new ProtocolError(`a frame for segment ${commit.segmentId}, which is not open`);
    }
    return {
      commit,
      startHz: this.current.grid.startHz,
      binWidthHz: this.current.grid.binWidthHz,
      levels: this.levels.slice(),
      config: this.current.config,
    };
  }

  reset(): void {
    this.current = null;
    this.levels = new Float32Array(0);
  }
}
