// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { Store } from "@tanstack/react-store";

import { formatFrequencyShort } from "../protocol/wire";

/** A named range to come back to, as `SweepPreset` has it. */
export interface SweepPreset {
  name: string;
  segments: { startHz: number; stopHz: number }[];
  favourite: boolean;
  builtin: boolean;
}

/** What this browser keeps: its own presets, and what it did to the built-ins. */
interface Saved {
  presets: { name: string; segments: { startHz: number; stopHz: number }[] }[];
  favourites: string[];
  hiddenBuiltins: string[];
}

const kStorageKey = "sweeppp.presets";

function load(): Saved {
  try {
    const parsed = JSON.parse(localStorage.getItem(kStorageKey) ?? "{}") as Partial<Saved>;
    return { presets: parsed.presets ?? [], favourites: parsed.favourites ?? [], hiddenBuiltins: parsed.hiddenBuiltins ?? [] };
  } catch {
    return { presets: [], favourites: [], hiddenBuiltins: [] };
  }
}

interface PresetsState {
  builtins: { name: string; segments: { startHz: number; stopHz: number }[] }[];
  saved: Saved;
}

export const presetsStore = new Store<PresetsState>({ builtins: [], saved: load() });

let requested = false;

/** The server's built-in list, once. */
export function loadBuiltinPresets(): void {
  if (requested) {
    return;
  }
  requested = true;
  fetch("/api/presets")
    .then((r) => (r.ok ? (r.json() as Promise<PresetsState["builtins"]>) : []))
    .then((builtins) => presetsStore.setState((s) => ({ ...s, builtins })))
    .catch(() => {
      requested = false;
    });
}

function save(change: (saved: Saved) => Saved): void {
  presetsStore.setState((s) => {
    const saved = change(s.saved);
    try {
      localStorage.setItem(kStorageKey, JSON.stringify(saved));
    } catch {
      // A private window: kept for this page only.
    }
    return { ...s, saved };
  });
}

/** Built-ins less the hidden ones, then this browser's own; favourites first,
 * then by name. */
export function presetList(state: PresetsState): SweepPreset[] {
  const { builtins, saved } = state;
  const favourite = (name: string) => saved.favourites.includes(name);
  const list: SweepPreset[] = [
    ...builtins
      .filter((b) => !saved.hiddenBuiltins.includes(b.name))
      .map((b) => ({ ...b, favourite: favourite(b.name), builtin: true })),
    ...saved.presets.map((p) => ({ ...p, favourite: favourite(p.name), builtin: false })),
  ];
  return list.sort((a, b) => Number(b.favourite) - Number(a.favourite) || (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
}

/** Saves the ranges under a name, replacing this browser's preset of that name. */
export function addPreset(name: string, segments: { startHz: number; stopHz: number }[]): void {
  save((s) => ({
    ...s,
    presets: [...s.presets.filter((p) => p.name !== name), { name, segments: segments.map(({ startHz, stopHz }) => ({ startHz, stopHz })) }],
  }));
}

/** Deletes one of this browser's presets, or hides a built-in. */
export function removePreset(preset: SweepPreset): void {
  save((s) =>
    preset.builtin
      ? { ...s, hiddenBuiltins: [...s.hiddenBuiltins, preset.name] }
      : { ...s, presets: s.presets.filter((p) => p.name !== preset.name) },
  );
}

export function setFavourite(name: string, favourite: boolean): void {
  save((s) => ({
    ...s,
    favourites: favourite ? [...s.favourites.filter((f) => f !== name), name] : s.favourites.filter((f) => f !== name),
  }));
}

export function lowestHz(segments: { startHz: number }[]): number {
  return segments.length ? Math.min(...segments.map((s) => s.startHz)) : 0;
}

export function highestHz(segments: { stopHz: number }[]): number {
  return segments.length ? Math.max(...segments.map((s) => s.stopHz)) : 0;
}

/** "88 - 108 MHz", or "3 ranges, 88 MHz - 6 GHz" when discontinuous. */
export function describeRange(segments: { startHz: number; stopHz: number }[]): string {
  if (segments.length === 0) {
    return "empty";
  }
  if (segments.length === 1) {
    return `${formatFrequencyShort(segments[0]!.startHz)} - ${formatFrequencyShort(segments[0]!.stopHz)}`;
  }
  return `${segments.length} ranges, ${formatFrequencyShort(lowestHz(segments))} - ${formatFrequencyShort(highestHz(segments))}`;
}

/** A range into a plan, merging with anything it touches, as
 * `SweepPlan::addSegment` does. */
export function addSegment<T extends { startHz: number; stopHz: number }>(segments: T[], segment: T): T[] {
  if (!(segment.stopHz > segment.startHz)) {
    return segments;
  }
  const sorted = [...segments, segment].sort((a, b) => a.startHz - b.startHz);
  const merged: T[] = [];
  for (const candidate of sorted) {
    const last = merged[merged.length - 1];
    if (last && candidate.startHz <= last.stopHz) {
      merged[merged.length - 1] = { ...last, stopHz: Math.max(last.stopHz, candidate.stopHz) };
    } else {
      merged.push({ ...candidate });
    }
  }
  return merged;
}
