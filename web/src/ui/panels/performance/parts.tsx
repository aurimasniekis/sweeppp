// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import type { ReactNode } from "react";

export const kHistoryLength = 120;

/** Recent values per figure, oldest first. */
export class Histories {
  private series = new Map<string, number[]>();

  push(key: string, value: number): void {
    let values = this.series.get(key);
    if (!values) {
      values = [];
      this.series.set(key, values);
    }
    values.push(Number.isFinite(value) ? value : 0);
    if (values.length > kHistoryLength) {
      values.shift();
    }
  }

  get(key: string): readonly number[] {
    return this.series.get(key) ?? [];
  }

  /** The largest value held, but never below `floor`. */
  max(key: string, floor: number): number {
    return Math.max(floor, ...this.get(key));
  }

  clear(): void {
    this.series.clear();
  }
}

export function Sparkline({
  values,
  min = 0,
  max,
  color,
}: {
  values: readonly number[];
  min?: number;
  max: number;
  color: string;
}) {
  if (values.length < 2) {
    return null;
  }
  const span = max - min || 1;
  const last = values.length - 1;
  const points = values
    .map((v, i) => {
      const y = 17 - Math.min(Math.max((v - min) / span, 0), 1) * 16;
      return `${((i / last) * 100).toFixed(2)},${y.toFixed(2)}`;
    })
    .join(" ");
  return (
    <svg viewBox="0 0 100 18" preserveAspectRatio="none" className="block h-[18px] w-full" aria-hidden>
      <polygon points={`0,18 ${points} 100,18`} fill={color} fillOpacity={0.15} />
      <polyline points={points} fill="none" stroke={color} strokeWidth={1.25} vectorEffect="non-scaling-stroke" />
    </svg>
  );
}

/** A filled bar with its figure written across it. */
export function Bar({ fraction, label, color = "var(--color-accent)" }: { fraction: number; label: string; color?: string }) {
  const width = Number.isFinite(fraction) ? Math.min(Math.max(fraction, 0), 1) * 100 : 0;
  return (
    <div className="relative my-1 h-5 overflow-hidden rounded border border-border bg-input text-xs">
      <div className="absolute inset-y-0 left-0 opacity-60" style={{ width: `${width}%`, background: color }} />
      <div className="relative flex h-full items-center justify-center tabular-nums">{label}</div>
    </div>
  );
}

export function SectionTitle({ title, children }: { title: string; children?: ReactNode }) {
  return (
    <div className="section-title flex items-center justify-between">
      <span>{title}</span>
      {children}
    </div>
  );
}

/** Label, figure and sparkline columns. */
export function Stats({ children }: { children: ReactNode }) {
  return (
    <div className="grid grid-cols-[minmax(0,9rem)_minmax(0,auto)_minmax(3rem,1fr)] items-center gap-x-3 gap-y-0.5 text-xs">
      {children}
    </div>
  );
}

export interface Trend {
  values: readonly number[];
  min?: number;
  max: number;
  color: string;
}

export function Stat({
  label,
  value,
  hint,
  trend,
  indent,
  className = "",
}: {
  label: string;
  value: ReactNode;
  hint?: string;
  trend?: Trend;
  indent?: boolean;
  className?: string;
}) {
  return (
    <div className="contents" title={hint}>
      <div className={`truncate text-dim ${indent ? "pl-3" : ""}`}>{label}</div>
      <div className={`whitespace-nowrap tabular-nums ${className}`}>{value}</div>
      <div className="min-h-[18px]">{trend && <Sparkline {...trend} />}</div>
    </div>
  );
}
