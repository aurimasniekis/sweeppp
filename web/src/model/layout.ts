// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

/** Panels: how many, where, and which part of the spectrum each shows. A port
 * of ui/PanelLayout, so a layout means the same in both places. */

export type PanelMode = "mirror" | "spans";
export type Arrangement = "single" | "columns" | "rows" | "three" | "grid" | "six" | "nine";

export const kMaxPanels = 9;
const kMinSplit = 0.15;
const kMaxSplit = 0.85;
const kMinThird = 0.1;

export interface Span {
  startHz: number;
  stopHz: number;
}

export const spanValid = (s: Span): boolean => s.stopHz > s.startHz;
export const spanWidth = (s: Span): number => s.stopHz - s.startHz;
export const spanCentre = (s: Span): number => (s.startHz + s.stopHz) / 2;

export interface Splits {
  x: number;
  y: number;
  thirdsX: [number, number];
  thirdsY: [number, number];
}

export interface PanelView {
  id: number;
  /** Zero both ways: fit what is swept. */
  viewStartHz: number;
  viewStopHz: number;
  yMinDb: number;
  yMaxDb: number;
  gradientMinDb: number;
  gradientMaxDb: number;
  waterfallFraction: number;
  waterfallPaused: boolean;
  /** In Spans, the part of the plan this panel shows. */
  segment: Span;
}

export interface PanelLayout {
  mode: PanelMode;
  rowsForTwo: boolean;
  splits: Splits;
  overview: boolean;
  panels: PanelView[];
  focusedId: number;
  nextId: number;
}

export function defaultPanel(id: number): PanelView {
  return {
    id,
    viewStartHz: 0,
    viewStopHz: 0,
    yMinDb: -110,
    yMaxDb: -10,
    gradientMinDb: -75,
    gradientMaxDb: -15,
    waterfallFraction: 0.45,
    waterfallPaused: false,
    segment: { startHz: 0, stopHz: 0 },
  };
}

export function defaultLayout(): PanelLayout {
  return {
    mode: "mirror",
    rowsForTwo: false,
    splits: { x: 0.5, y: 0.5, thirdsX: [1 / 3, 2 / 3], thirdsY: [1 / 3, 2 / 3] },
    overview: true,
    panels: [defaultPanel(1)],
    focusedId: 1,
    nextId: 2,
  };
}

export function focused(layout: PanelLayout): PanelView {
  return layout.panels.find((p) => p.id === layout.focusedId) ?? layout.panels[0]!;
}

/** A copy of `from` as a new panel, or the layout unchanged when it is full. */
export function addPanel(layout: PanelLayout, from: PanelView): PanelLayout {
  if (layout.panels.length >= kMaxPanels) {
    return layout;
  }
  const panel = { ...from, id: layout.nextId, waterfallPaused: false };
  return { ...layout, panels: [...layout.panels, panel], nextId: layout.nextId + 1 };
}

/** The panel removed; never the last one. Ids are not handed back, so a new
 * panel never inherits a removed one's history. */
export function removePanel(layout: PanelLayout, id: number): PanelLayout {
  const index = layout.panels.findIndex((p) => p.id === id);
  if (layout.panels.length <= 1 || index < 0) {
    return layout;
  }
  const panels = layout.panels.filter((p) => p.id !== id);
  const focusedId =
    layout.focusedId === id ? panels[Math.min(index, panels.length - 1)]!.id : layout.focusedId;
  return { ...layout, panels, focusedId };
}

const finite = (v: number, fallback: number) => (Number.isFinite(v) ? v : fallback);
const clamp = (v: number, lo: number, hi: number) => Math.min(Math.max(v, lo), hi);

export function clampSplits(splits: Splits): Splits {
  const defaults = defaultLayout().splits;
  const thirds = (requested: [number, number], fallback: [number, number]): [number, number] => {
    const first = clamp(finite(requested[0], fallback[0]), kMinThird, 1 - kMinThird * 2);
    const second = clamp(finite(requested[1], fallback[1]), first + kMinThird, 1 - kMinThird);
    return [first, second];
  };
  return {
    x: clamp(finite(splits.x, defaults.x), kMinSplit, kMaxSplit),
    y: clamp(finite(splits.y, defaults.y), kMinSplit, kMaxSplit),
    thirdsX: thirds(splits.thirdsX, defaults.thirdsX),
    thirdsY: thirds(splits.thirdsY, defaults.thirdsY),
  };
}

export function arrangementFor(attached: number, rowsForTwo: boolean): Arrangement {
  if (attached <= 1) return "single";
  if (attached === 2) return rowsForTwo ? "rows" : "columns";
  if (attached === 3) return "three";
  if (attached === 4) return "grid";
  return attached <= 6 ? "six" : "nine";
}

export const slotCount: Record<Arrangement, number> = {
  single: 1,
  columns: 2,
  rows: 2,
  three: 3,
  grid: 4,
  six: 6,
  nine: 9,
};
const columnCount: Record<Arrangement, number> = {
  single: 1,
  rows: 1,
  columns: 2,
  three: 2,
  grid: 2,
  six: 3,
  nine: 3,
};
const rowCount: Record<Arrangement, number> = {
  single: 1,
  columns: 1,
  three: 1,
  rows: 2,
  grid: 2,
  six: 2,
  nine: 3,
};

export interface Rect {
  x: number;
  y: number;
  width: number;
  height: number;
}

function cutAxis(
  origin: number,
  length: number,
  parts: number,
  split: number,
  thirds: [number, number],
  gap: number,
): { start: number[]; length: number[] } {
  const inner = Math.max(length - gap * (parts - 1), 0);
  const ends = [inner, inner, inner];
  if (parts === 2) {
    ends[0] = Math.floor(inner * split);
  } else if (parts === 3) {
    ends[0] = Math.floor(inner * thirds[0]);
    ends[1] = Math.floor(inner * thirds[1]);
  }
  const out = { start: [] as number[], length: [] as number[] };
  let before = 0;
  for (let i = 0; i < parts; ++i) {
    out.start.push(origin + before + gap * i);
    out.length.push(ends[i]! - before);
    before = ends[i]!;
  }
  return out;
}

export function arrangePanels(arrangement: Arrangement, area: Rect, requested: Splits, gap: number): Rect[] {
  const splits = clampSplits(requested);
  if (arrangement === "three") {
    const columns = cutAxis(area.x, area.width, 2, splits.x, splits.thirdsX, gap);
    const right = cutAxis(area.y, area.height, 2, splits.y, splits.thirdsY, gap);
    return [
      { x: columns.start[0]!, y: area.y, width: columns.length[0]!, height: area.height },
      { x: columns.start[1]!, y: right.start[0]!, width: columns.length[1]!, height: right.length[0]! },
      { x: columns.start[1]!, y: right.start[1]!, width: columns.length[1]!, height: right.length[1]! },
    ];
  }
  const columns = cutAxis(area.x, area.width, columnCount[arrangement], splits.x, splits.thirdsX, gap);
  const rows = cutAxis(area.y, area.height, rowCount[arrangement], splits.y, splits.thirdsY, gap);
  const rects: Rect[] = [];
  for (let row = 0; row < rowCount[arrangement]; ++row) {
    for (let column = 0; column < columnCount[arrangement]; ++column) {
      rects.push({
        x: columns.start[column]!,
        y: rows.start[row]!,
        width: columns.length[column]!,
        height: rows.length[row]!,
      });
    }
  }
  return rects;
}

export function resolveView(panel: PanelView, fit: Span): Span {
  return panel.viewStopHz > panel.viewStartHz ? { startHz: panel.viewStartHz, stopHz: panel.viewStopHz } : fit;
}

export function zoomAbout(view: Span, anchorHz: number, factor: number): Span {
  return {
    startHz: anchorHz - (anchorHz - view.startHz) * factor,
    stopHz: anchorHz + (view.stopHz - anchorHz) * factor,
  };
}

/** A view kept inside `limits` (when bounded) and above 0 Hz, or null. */
export function clampView(fromHz: number, toHz: number, limits: Span | null): Span | null {
  if (!(toHz > fromHz)) {
    return null;
  }
  if (limits && spanValid(limits)) {
    const width = Math.min(toHz - fromHz, spanWidth(limits));
    if (fromHz < limits.startHz) {
      fromHz = limits.startHz;
      toHz = fromHz + width;
    }
    if (toHz > limits.stopHz) {
      toHz = limits.stopHz;
      fromHz = toHz - width;
    }
    return { startHz: fromHz, stopHz: toHz };
  }
  fromHz = Math.max(fromHz, 0);
  return toHz > fromHz ? { startHz: fromHz, stopHz: toHz } : null;
}

export interface BinSlice {
  first: number;
  count: number;
  startHz: number;
  stopHz: number;
}

/** The bins of a grid whose centres fall in `span`. */
export function segmentBins(gridStartHz: number, binWidthHz: number, binCount: number, span: Span): BinSlice {
  if (binWidthHz <= 0 || binCount === 0 || !spanValid(span)) {
    return { first: 0, count: 0, startHz: 0, stopHz: 0 };
  }
  const edge = (hz: number) => clamp(Math.ceil((hz - gridStartHz) / binWidthHz - 0.5), 0, binCount);
  const first = edge(span.startHz);
  const last = Math.max(edge(span.stopHz), first);
  return {
    first,
    count: last - first,
    startHz: gridStartHz + binWidthHz * first,
    stopHz: gridStartHz + binWidthHz * last,
  };
}

/** Each panel's segment kept where it still overlaps one, the rest handed
 * out in order: panels follow the plan rather than being reset by it. */
export function rebindSegments(bound: Span[], segments: Span[]): { panelSegment: number[]; unclaimed: number[] } {
  const panelSegment = bound.map(() => -1);
  const taken = segments.map(() => false);
  const overlap = (a: Span, b: Span) => Math.min(a.stopHz, b.stopHz) - Math.max(a.startHz, b.startHz);

  bound.forEach((span, p) => {
    if (!spanValid(span)) {
      return;
    }
    let best = 0;
    let choice = -1;
    segments.forEach((segment, s) => {
      const shared = overlap(span, segment);
      if (!taken[s] && shared > best) {
        best = shared;
        choice = s;
      }
    });
    if (choice >= 0) {
      panelSegment[p] = choice;
      taken[choice] = true;
    }
  });

  let next = 0;
  const nextFree = () => {
    while (next < segments.length && taken[next]) {
      ++next;
    }
    return next < segments.length ? next : -1;
  };
  bound.forEach((span, p) => {
    if (panelSegment[p]! >= 0) {
      return;
    }
    if (spanValid(span) && segments.some((s) => overlap(span, s) > 0)) {
      return;
    }
    const free = nextFree();
    if (free >= 0) {
      panelSegment[p] = free;
      taken[free] = true;
    }
  });
  return { panelSegment, unclaimed: segments.flatMap((_, s) => (taken[s] ? [] : [s])) };
}

/** Segments merged into at most `groups` windows, closing the narrowest gaps
 * first: what Mirror panels fit when there are more segments than panels. */
export function groupSegments(segments: Span[], groups: number): Span[] {
  const sorted = segments.filter(spanValid).sort((a, b) => a.startHz - b.startHz);
  const windows: Span[] = [];
  for (const segment of sorted) {
    const last = windows[windows.length - 1];
    if (last && segment.startHz <= last.stopHz) {
      last.stopHz = Math.max(last.stopHz, segment.stopHz);
    } else {
      windows.push({ ...segment });
    }
  }
  if (groups === 0) {
    return [];
  }
  while (windows.length > groups) {
    let narrowest = 0;
    for (let i = 1; i + 1 < windows.length; ++i) {
      const gap = windows[i + 1]!.startHz - windows[i]!.stopHz;
      if (gap < windows[narrowest + 1]!.startHz - windows[narrowest]!.stopHz) {
        narrowest = i;
      }
    }
    windows[narrowest]!.stopHz = windows[narrowest + 1]!.stopHz;
    windows.splice(narrowest + 1, 1);
  }
  return windows;
}

/** Mirror: every panel back on the whole data. Spans: one panel per segment,
 * lowest first, each keeping the levels of the panel that was in its place. */
export function enterMode(layout: PanelLayout, mode: PanelMode, segments: Span[]): PanelLayout {
  if (layout.mode === mode) {
    return layout;
  }
  if (mode === "mirror") {
    return {
      ...layout,
      mode,
      panels: layout.panels.map((p) => ({ ...p, segment: { startHz: 0, stopHz: 0 }, viewStartHz: 0, viewStopHz: 0 })),
    };
  }
  const sorted = segments.filter(spanValid).sort((a, b) => a.startHz - b.startHz);
  const count = clamp(sorted.length, 1, kMaxPanels);
  let next: PanelLayout = { ...layout, mode, panels: layout.panels.slice(0, count) };
  while (next.panels.length < count) {
    next = addPanel(next, focused(next));
  }
  next.panels = next.panels.map((p, i) => ({
    ...p,
    segment: sorted[i] ?? { startHz: 0, stopHz: 0 },
    viewStartHz: 0,
    viewStopHz: 0,
  }));
  if (!next.panels.some((p) => p.id === next.focusedId)) {
    next.focusedId = next.panels[0]!.id;
  }
  return next;
}

/** Spans after the plan changed: each panel follows the segment it overlaps,
 * a panel whose segment merged away closes, and a new segment gets a panel. */
export function rebindSpans(layout: PanelLayout, segments: Span[]): { layout: PanelLayout; dropped: number } {
  if (segments.length === 0) {
    return { layout, dropped: 0 };
  }
  const binding = rebindSegments(
    layout.panels.map((p) => p.segment),
    segments,
  );
  let dropped = 0;
  const kept: PanelView[] = [];
  layout.panels.forEach((p, i) => {
    const index = binding.panelSegment[i]!;
    if (index < 0) {
      ++dropped;
      return;
    }
    const segment = segments[index]!;
    const moved = Math.abs(p.segment.startHz - segment.startHz) > 1 || Math.abs(p.segment.stopHz - segment.stopHz) > 1;
    if (!moved) {
      kept.push(p);
      return;
    }
    // Kept where it was looking when that is still inside, so editing one
    // edge of a segment does not throw away a zoom on the other.
    const inside = p.viewStopHz > p.viewStartHz ? clampView(p.viewStartHz, p.viewStopHz, segment) : null;
    kept.push({ ...p, segment, viewStartHz: inside?.startHz ?? 0, viewStopHz: inside?.stopHz ?? 0 });
  });

  let next: PanelLayout = { ...layout, panels: kept.length > 0 ? kept : layout.panels };
  for (const index of binding.unclaimed) {
    const added = addPanel(next, focused(next));
    if (added === next) {
      break;
    }
    const panel = added.panels[added.panels.length - 1]!;
    added.panels[added.panels.length - 1] = { ...panel, segment: segments[index]!, viewStartHz: 0, viewStopHz: 0 };
    next = added;
  }
  if (!next.panels.some((p) => p.id === next.focusedId)) {
    next = { ...next, focusedId: next.panels[0]!.id };
  }
  return { layout: next, dropped: kept.length > 0 ? dropped : 0 };
}

/** Mirror's counterpart to Spans, for a moment: each panel zoomed onto a
 * range, lowest first, the closest sharing one when there are more ranges. */
export function fitToRanges(layout: PanelLayout, segments: Span[]): PanelLayout {
  const windows = groupSegments(segments, layout.panels.length);
  return {
    ...layout,
    panels: layout.panels.map((p, i) =>
      windows[i] ? { ...p, viewStartHz: windows[i].startHz, viewStopHz: windows[i].stopHz } : p,
    ),
  };
}

/** The panels' rectangles' dividers, each with the split it moves. */
export interface Divider {
  rect: Rect;
  vertical: boolean;
  /** Which fraction a drag moves, and over how many parts of the axis. */
  split: "x" | "y" | "thirdsX0" | "thirdsX1" | "thirdsY0" | "thirdsY1";
  parts: number;
}

export function dividers(arrangement: Arrangement, rects: Rect[], area: Rect, gap: number): Divider[] {
  if (rects.length < 2) {
    return [];
  }
  const out: Divider[] = [];
  const columns = columnCount[arrangement];
  const rows = rowCount[arrangement];
  const verticalParts = arrangement === "three" ? 2 : columns;
  for (let d = 0; d + 1 < verticalParts; ++d) {
    const before = rects[d]!;
    out.push({
      rect: { x: before.x + before.width, y: area.y, width: gap, height: area.height },
      vertical: true,
      split: verticalParts === 2 ? "x" : d === 0 ? "thirdsX0" : "thirdsX1",
      parts: verticalParts,
    });
  }
  if (arrangement === "three") {
    const upper = rects[1]!;
    out.push({
      rect: { x: upper.x, y: upper.y + upper.height, width: upper.width, height: gap },
      vertical: false,
      split: "y",
      parts: 2,
    });
  } else {
    for (let d = 0; d + 1 < rows; ++d) {
      const above = rects[d * columns]!;
      out.push({
        rect: { x: area.x, y: above.y + above.height, width: area.width, height: gap },
        vertical: false,
        split: rows === 2 ? "y" : d === 0 ? "thirdsY0" : "thirdsY1",
        parts: rows,
      });
    }
  }
  return out;
}

/** `splits` with one divider moved by `travel` pixels of an axis `length` long. */
export function moveDivider(splits: Splits, divider: Divider, travel: number, length: number, gap: number): Splits {
  const share = travel / Math.max(length - gap * (divider.parts - 1), 1);
  const next: Splits = { ...splits, thirdsX: [...splits.thirdsX], thirdsY: [...splits.thirdsY] };
  switch (divider.split) {
    case "x":
      next.x += share;
      break;
    case "y":
      next.y += share;
      break;
    case "thirdsX0":
      next.thirdsX[0] += share;
      break;
    case "thirdsX1":
      next.thirdsX[1] += share;
      break;
    case "thirdsY0":
      next.thirdsY[0] += share;
      break;
    case "thirdsY1":
      next.thirdsY[1] += share;
      break;
  }
  return clampSplits(next);
}
