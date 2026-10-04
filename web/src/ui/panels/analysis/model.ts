// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import {
  OverlapResolution,
  type OverlapResolutionId,
  PortStrategy,
  type PortStrategyId,
  SweepMode,
  type SweepModeId,
  type SweepPlan,
  ThrottleMode,
  type ThrottleModeId,
  WindowType,
  type WindowTypeId,
} from "../../../protocol/wire";
import type { Choice } from "../../controls";

/** `SweepPlan::applyMode`: the fields a mode sets, and nothing else. */
export function withMode(plan: SweepPlan, mode: SweepModeId): SweepPlan {
  return mode === SweepMode.Fast
    ? { ...plan, mode, averageCount: 1, fftOverlap: 0, dwellSeconds: 0, stepOverlap: 0.05, usableBandwidthFraction: 0.8 }
    : { ...plan, mode, averageCount: 8, fftOverlap: 0.5, dwellSeconds: 0.002, stepOverlap: 0.15, usableBandwidthFraction: 0.7 };
}

/** Powers of two up to `snapFftSize`'s ceiling, so every size the RBW field
 * can reach is one this list can name. */
export const kFftSizes = [
  64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576,
] as const;

/** Even and within what every backend accepts; not a power of two, so a
 * typed RBW is met rather than rounded away. */
export function snapFftSize(requested: number): number {
  const size = Math.round(Math.min(Math.max(requested, 16), 1048576));
  return size % 2 === 0 ? size : size + 1;
}

/** The largest listed size not above `points`. */
export function listedFftSize(points: number): number {
  let chosen: number = kFftSizes[0];
  for (const size of kFftSizes) {
    if (size <= points) {
      chosen = size;
    }
  }
  return chosen;
}

/** `LocalInstrument::learnPasses()` and `kLearnFrames`, which the server does
 * not send. */
export const kLearnPasses = 10;
export const kLearnFrames = 200;

export const windowChoices: Choice<`${WindowTypeId}`>[] = [
  { value: `${WindowType.Rectangular}`, label: "Rectangular" },
  { value: `${WindowType.Hann}`, label: "Hann" },
  { value: `${WindowType.Hamming}`, label: "Hamming" },
  { value: `${WindowType.BlackmanHarris}`, label: "Blackman-Harris" },
  { value: `${WindowType.FlatTop}`, label: "Flat-top" },
  { value: `${WindowType.Kaiser}`, label: "Kaiser" },
];

export const windowDescriptions: Record<WindowTypeId, string> = {
  [WindowType.Rectangular]:
    "No windowing. Sharpest resolution, worst leakage — only for signals that are exactly periodic in the window.",
  [WindowType.Hann]: "General purpose. Good leakage suppression at modest resolution cost. The sensible default.",
  [WindowType.Hamming]: "Lower first sidelobe than Hann, but the far sidelobes fall off more slowly.",
  [WindowType.BlackmanHarris]:
    "Very low sidelobes (-92 dB). Use when a weak signal sits close to a strong one. Costs resolution.",
  [WindowType.FlatTop]:
    "Amplitude-accurate to ~0.01 dB regardless of where a tone falls between bins. Use for level measurement, not for resolution.",
  [WindowType.Kaiser]:
    "Adjustable resolution/leakage trade-off via beta. Higher beta means lower sidelobes and a wider main lobe.",
};

export const throttleChoices: Choice<`${ThrottleModeId}`>[] = [
  { value: `${ThrottleMode.EveryNth}`, label: "Every Nth" },
  { value: `${ThrottleMode.Auto}`, label: "Auto" },
  { value: `${ThrottleMode.AllSamples}`, label: "All samples" },
];

export const portStrategyChoices: Choice<`${PortStrategyId}`>[] = [
  { value: `${PortStrategy.TightestFit}`, label: "Tightest fit" },
  { value: `${PortStrategy.PortOrder}`, label: "Port order" },
  { value: `${PortStrategy.HighestGain}`, label: "Most gain" },
  { value: `${PortStrategy.FewestSwitches}`, label: "Fewest switches" },
];

export const overlapResolutionChoices: Choice<`${OverlapResolutionId}`>[] = [
  { value: `${OverlapResolution.Best}`, label: "Best" },
  { value: `${OverlapResolution.Max}`, label: "Max" },
  { value: `${OverlapResolution.Mean}`, label: "Mean" },
];

const kCosineSums: Partial<Record<WindowTypeId, readonly number[]>> = {
  [WindowType.Hann]: [0.5, 0.5],
  [WindowType.Hamming]: [0.54, 0.46],
  [WindowType.BlackmanHarris]: [0.35875, 0.48829, 0.14128, 0.01168],
  [WindowType.FlatTop]: [0.21557895, 0.41663158, 0.277263158, 0.083578947, 0.006947368],
};

function besselI0(x: number): number {
  let sum = 1;
  let term = 1;
  const halfXSquared = (x * x) / 4;
  for (let k = 1; k < 64; ++k) {
    term *= halfXSquared / (k * k);
    sum += term;
    if (term < sum * 1e-17) {
      break;
    }
  }
  return sum;
}

/** Equivalent noise bandwidth in bins, measured from a 1024-point window
 * generated as the server's `Window::create` does, so RBW = rate x ENBW / N
 * reads the same here as on the server. */
export function windowEnbw(type: WindowTypeId, beta: number): number {
  const size = 1024;
  const denominator = size - 1;
  const coefficients = kCosineSums[type];
  const i0Beta = besselI0(beta);
  let sum = 0;
  let sumSquares = 0;
  for (let n = 0; n < size; ++n) {
    let w = 1;
    if (coefficients) {
      const phase = (2 * Math.PI * n) / denominator;
      w = coefficients.reduce((acc, a, i) => acc + (i % 2 === 0 ? a : -a) * Math.cos(i * phase), 0);
    } else if (type === WindowType.Kaiser) {
      const ratio = (2 * n) / denominator - 1;
      w = besselI0(beta * Math.sqrt(Math.max(0, 1 - ratio * ratio))) / i0Beta;
    }
    w = Math.fround(w);
    sum += w;
    sumSquares += w * w;
  }
  return sum === 0 ? 1 : (size * sumSquares) / (sum * sum);
}
