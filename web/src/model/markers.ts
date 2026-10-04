// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import type { TraceStore } from "./traces";

export interface Marker {
  id: number;
  frequencyHz: number;
  /** Re-read from the live trace every frame. */
  levelDb: number;
  visible: boolean;
  /** Follows the strongest signal near it. */
  peakLocked: boolean;
}

export interface MarkerSet {
  items: Marker[];
  /** Zero: nothing selected. */
  activeId: number;
  nextId: number;
}

export const emptyMarkers = (): MarkerSet => ({ items: [], activeId: 0, nextId: 1 });

export function addMarker(set: MarkerSet, hz: number): MarkerSet {
  const marker: Marker = { id: set.nextId, frequencyHz: hz, levelDb: 0, visible: true, peakLocked: false };
  return { items: [...set.items, marker], activeId: marker.id, nextId: set.nextId + 1 };
}

/** Numbering comes back down with the set: M1 after a clear is M1 again. */
export function removeMarker(set: MarkerSet, id: number): MarkerSet {
  const items = set.items.filter((m) => m.id !== id);
  return {
    items,
    activeId: set.activeId === id ? 0 : set.activeId,
    nextId: items.reduce((next, m) => Math.max(next, m.id + 1), 1),
  };
}

export function updateMarker(set: MarkerSet, id: number, change: Partial<Marker>): MarkerSet {
  return { ...set, items: set.items.map((m) => (m.id === id ? { ...m, ...change } : m)) };
}

/** The closest visible marker within `toleranceHz`; the first placed wins a
 * tie, so two at one frequency do not flicker. */
export function nearestMarker(set: MarkerSet, hz: number, toleranceHz: number): Marker | null {
  let best: Marker | null = null;
  for (const marker of set.items) {
    const distance = Math.abs(marker.frequencyHz - hz);
    if (marker.visible && distance <= toleranceHz && (!best || distance < Math.abs(best.frequencyHz - hz))) {
      best = marker;
    }
  }
  return best;
}

/** A marker's level now; a peak-locked one moves to the strongest signal
 * within `reachHz` first. */
export function refreshMarker(marker: Marker, traces: TraceStore, reachHz: number): Marker {
  if (marker.peakLocked) {
    const peak = traces.peakIn(marker.frequencyHz - reachHz, marker.frequencyHz + reachHz);
    return peak ? { ...marker, frequencyHz: peak.hz, levelDb: peak.db } : marker;
  }
  return { ...marker, levelDb: traces.levelAt(marker.frequencyHz) };
}
