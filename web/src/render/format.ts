// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

function scaled(hz: number): [number, string] {
  const magnitude = Math.abs(hz);
  if (magnitude >= 1e9) return [hz / 1e9, "GHz"];
  if (magnitude >= 1e6) return [hz / 1e6, "MHz"];
  if (magnitude >= 1e3) return [hz / 1e3, "kHz"];
  return [hz, "Hz"];
}

export { formatFrequencyShort as frequencyShort } from "../protocol/wire";

export function frequency(hz: number, decimals = 3): string {
  const [value, unit] = scaled(hz);
  return `${value.toFixed(decimals)} ${unit}`;
}

/** "2.4G", "433.92M", "100k": what the range fields accept back. */
export function frequencyField(hz: number): string {
  const [value, unit] = scaled(hz);
  return `${parseFloat(value.toPrecision(10))}${unit === "Hz" ? "" : unit[0]}`;
}

/** "2.4G", "100 MHz", "868.3 mhz", "1_000_000": Hz, or null. */
export function parseFrequency(text: string): number | null {
  const match = /^\s*([-+]?[\d_]*\.?[\d_]+(?:e[-+]?\d+)?)\s*([kmgt]?)(?:hz)?\s*$/i.exec(text);
  if (!match) {
    return null;
  }
  const value = parseFloat(match[1]!.replaceAll("_", ""));
  const scale = { "": 1, k: 1e3, m: 1e6, g: 1e9, t: 1e12 }[match[2]!.toLowerCase()] ?? 1;
  return Number.isFinite(value) ? value * scale : null;
}

export function level(db: number): string {
  return db > -190 ? `${db.toFixed(1)} dBFS` : "-";
}

export function bytes(count: number): string {
  if (count >= 1024 ** 3) return `${(count / 1024 ** 3).toFixed(2)} GiB`;
  if (count >= 1024 ** 2) return `${(count / 1024 ** 2).toFixed(1)} MiB`;
  if (count >= 1024) return `${(count / 1024).toFixed(1)} KiB`;
  return `${count} B`;
}

export function byteRate(perSecond: number): string {
  if (perSecond >= 1e9) return `${(perSecond / 1e9).toFixed(2)} GB/s`;
  if (perSecond >= 1e6) return `${(perSecond / 1e6).toFixed(2)} MB/s`;
  if (perSecond >= 1e3) return `${(perSecond / 1e3).toFixed(1)} kB/s`;
  return `${perSecond.toFixed(0)} B/s`;
}

export function duration(seconds: number): string {
  if (!Number.isFinite(seconds)) return "-";
  if (seconds < 1e-3) return `${(seconds * 1e6).toFixed(0)} µs`;
  if (seconds < 1) return `${(seconds * 1e3).toFixed(1)} ms`;
  if (seconds < 60) return `${seconds.toFixed(2)} s`;
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return `${minutes} min ${Math.round(seconds % 60)} s`;
  const hours = Math.floor(minutes / 60);
  if (hours < 48) return `${hours} h ${minutes % 60} min`;
  return `${Math.floor(hours / 24)} d ${hours % 24} h`;
}

export function count(n: number): string {
  return n.toLocaleString("en-US").replaceAll(",", " ");
}

/** Round grid steps: 1, 2 or 5 times a power of ten, at least `minimum`. */
export function niceStep(minimum: number): number {
  const power = 10 ** Math.floor(Math.log10(minimum));
  for (const factor of [1, 2, 5, 10]) {
    if (factor * power >= minimum) {
      return factor * power;
    }
  }
  return 10 * power;
}
