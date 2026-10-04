// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { isMeasuredDb, kUnmeasuredDb } from "../protocol/mirror";

export type TraceKind = "live" | "maxHold" | "minHold" | "average";

export interface TraceOptions {
  smoothing: number;
  maxHoldDecayDbPerSec: number;
  averageWindow: number;
  maxHold: boolean;
  minHold: boolean;
  average: boolean;
}

/** One trace's strongest and weakest level under each pixel. */
export interface Envelope {
  minimum: Float32Array;
  maximum: Float32Array;
}

/** The spectrum traces, as the desktop keeps them (ui/TraceStore): the live
 * line, optionally smoothed, and the holds and average built from it. */
export class TraceStore {
  startHz = 0;
  binWidthHz = 0;
  binCount = 0;
  /** Bumped on every update, for a renderer to know it has new levels. */
  generation = 0;

  private readonly traces: Record<TraceKind, Float32Array> = {
    live: new Float32Array(0),
    maxHold: new Float32Array(0),
    minHold: new Float32Array(0),
    average: new Float32Array(0),
  };
  private liveCount = 0;
  private averageCount = 0;
  private lastFrameNs = 0n;

  get stopHz(): number {
    return this.startHz + this.binWidthHz * this.binCount;
  }

  trace(kind: TraceKind): Float32Array {
    return this.traces[kind];
  }

  clear(): void {
    this.binCount = 0;
    for (const kind of Object.keys(this.traces) as TraceKind[]) {
      this.traces[kind] = new Float32Array(0);
    }
    this.liveCount = 0;
    this.averageCount = 0;
    ++this.generation;
  }

  resetHolds(): void {
    this.traces.maxHold = new Float32Array(0);
    this.traces.minHold = new Float32Array(0);
    this.traces.average = new Float32Array(0);
    this.averageCount = 0;
  }

  update(startHz: number, binWidthHz: number, levels: Float32Array, hostNs: bigint, options: TraceOptions): void {
    if (levels.length === 0) {
      return;
    }
    // A max-hold from another grid is not merely stale: its bins mean other
    // frequencies.
    const gridChanged =
      levels.length !== this.binCount ||
      Math.abs(startHz - this.startHz) > 1e-3 ||
      Math.abs(binWidthHz - this.binWidthHz) > 1e-9;
    if (gridChanged) {
      this.startHz = startHz;
      this.binWidthHz = binWidthHz;
      this.binCount = levels.length;
      this.traces.live = new Float32Array(levels.length).fill(kUnmeasuredDb);
      this.liveCount = 0;
      this.resetHolds();
    }

    const live = this.traces.live;
    if (options.smoothing > 0 && this.liveCount > 0) {
      for (let i = 0; i < live.length; ++i) {
        live[i]! += (levels[i]! - live[i]!) * (1 - options.smoothing);
      }
    } else {
      live.set(levels);
    }
    ++this.liveCount;

    if (options.maxHold) {
      let decay = 0;
      if (options.maxHoldDecayDbPerSec > 0 && this.lastFrameNs !== 0n && hostNs > this.lastFrameNs) {
        decay = (options.maxHoldDecayDbPerSec * Number(hostNs - this.lastFrameNs)) / 1e9;
      }
      if (this.traces.maxHold.length !== live.length) {
        this.traces.maxHold = live.slice();
      } else {
        const hold = this.traces.maxHold;
        for (let i = 0; i < hold.length; ++i) {
          hold[i] = Math.max(hold[i]! - decay, live[i]!);
        }
      }
    } else if (this.traces.maxHold.length > 0) {
      this.traces.maxHold = new Float32Array(0);
    }
    this.lastFrameNs = hostNs;

    if (options.minHold) {
      if (this.traces.minHold.length !== live.length) {
        this.traces.minHold = live.slice();
      } else {
        const hold = this.traces.minHold;
        for (let i = 0; i < hold.length; ++i) {
          // Never-measured bins must not drag the min-hold to the floor.
          if (live[i]! > kUnmeasuredDb) {
            hold[i] = Math.min(hold[i]!, live[i]!);
          }
        }
      }
    } else if (this.traces.minHold.length > 0) {
      this.traces.minHold = new Float32Array(0);
    }

    if (options.average) {
      if (this.traces.average.length !== live.length) {
        this.traces.average = live.slice();
        this.averageCount = 1;
      } else {
        const weight = 1 / Math.min(this.averageCount + 1, Math.max(options.averageWindow, 1));
        const average = this.traces.average;
        for (let i = 0; i < average.length; ++i) {
          average[i]! += (live[i]! - average[i]!) * weight;
        }
        ++this.averageCount;
      }
    } else if (this.traces.average.length > 0) {
      this.traces.average = new Float32Array(0);
      this.averageCount = 0;
    }
    ++this.generation;
  }

  /** The live level at `hz`, or the unmeasured level outside the grid. */
  levelAt(hz: number): number {
    const live = this.traces.live;
    if (live.length === 0 || this.binWidthHz <= 0) {
      return kUnmeasuredDb;
    }
    const bin = Math.floor((hz - this.startHz) / this.binWidthHz);
    return bin >= 0 && bin < live.length ? live[bin]! : kUnmeasuredDb;
  }

  /** The strongest measured live bin in [fromHz, toHz], at its centre. */
  peakIn(fromHz: number, toHz: number): { hz: number; db: number } | null {
    const live = this.traces.live;
    if (live.length === 0 || this.binWidthHz <= 0) {
      return null;
    }
    const first = Math.max(Math.floor((fromHz - this.startHz) / this.binWidthHz), 0);
    const last = Math.min(Math.floor((toHz - this.startHz) / this.binWidthHz) + 1, live.length);
    let best = -Infinity;
    let bestBin = -1;
    for (let bin = first; bin < last; ++bin) {
      const level = live[bin]!;
      if (level > kUnmeasuredDb && level > best) {
        best = level;
        bestBin = bin;
      }
    }
    return bestBin < 0 ? null : { hz: this.startHz + this.binWidthHz * (bestBin + 0.5), db: best };
  }

  /** Each pixel's extremes over [fromHz, toHz): every bin under a pixel
   * counts, so a one-bin carrier survives any zoom. A pixel narrower than a
   * bin takes the bin it sits in; one with nothing measured is a gap. */
  envelope(kind: TraceKind, fromHz: number, toHz: number, pixels: number): Envelope {
    const values = this.traces[kind];
    const out = {
      minimum: new Float32Array(pixels).fill(kUnmeasuredDb),
      maximum: new Float32Array(pixels).fill(kUnmeasuredDb),
    };
    if (values.length === 0 || this.binWidthHz <= 0 || pixels <= 0 || !(toHz > fromHz)) {
      return out;
    }
    const hzPerPixel = (toHz - fromHz) / pixels;
    for (let p = 0; p < pixels; ++p) {
      const lo = (fromHz + p * hzPerPixel - this.startHz) / this.binWidthHz;
      const hi = (fromHz + (p + 1) * hzPerPixel - this.startHz) / this.binWidthHz;
      let first = Math.floor(lo);
      let last = Math.ceil(hi);
      if (last <= first + 1) {
        first = Math.floor((lo + hi) / 2);
        last = first + 1;
      }
      first = Math.max(first, 0);
      last = Math.min(last, values.length);
      let minimum = Infinity;
      let maximum = -Infinity;
      for (let bin = first; bin < last; ++bin) {
        const level = values[bin]!;
        if (isMeasuredDb(level)) {
          if (level < minimum) minimum = level;
          if (level > maximum) maximum = level;
        }
      }
      if (maximum > -Infinity) {
        out.minimum[p] = minimum;
        out.maximum[p] = maximum;
      }
    }
    return out;
  }
}

/** The strongest measured level of each group of `group` bins: how a line is
 * fitted to fewer columns without losing a narrow carrier. */
export function reduceMax(levels: Float32Array, columns: number): Float32Array {
  if (levels.length <= columns) {
    return levels;
  }
  const out = new Float32Array(columns).fill(kUnmeasuredDb);
  const group = levels.length / columns;
  for (let c = 0; c < columns; ++c) {
    const first = Math.floor(c * group);
    const last = Math.min(Math.floor((c + 1) * group), levels.length);
    let strongest = kUnmeasuredDb;
    for (let i = first; i < last; ++i) {
      const level = levels[i]!;
      if (isMeasuredDb(level) && (!isMeasuredDb(strongest) || level > strongest)) {
        strongest = level;
      }
    }
    out[c] = strongest;
  }
  return out;
}
