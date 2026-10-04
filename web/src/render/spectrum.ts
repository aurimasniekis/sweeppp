// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import type { Marker } from "../model/markers";
import type { TraceKind, TraceStore } from "../model/traces";
import { isMeasuredDb } from "../protocol/mirror";
import { frequencyShort, niceStep } from "./format";

/** The left gutter for dB labels and the bottom one for frequencies, in CSS
 * pixels. The waterfall under a spectrum lines up with what is between. */
export const kAxisLeft = 62;
export const kAxisBottom = 38;

/** The waterfall's gradient bar, right of the plot. */
export const kGradientBarWidth = 18;
const kGradientBarGap = 8;
const kAxisTop = 8;
const kAxisRight = kGradientBarGap + kGradientBarWidth + 4;

/** The dB scale's handles, inside the plot's left edge. */
export const kHandleSize = 10;
const kHandleInset = 3;

const kFont = "12px system-ui, sans-serif";
const kLineHeight = 15;
const kOverlayTextTop = 6;

export interface Overlay {
  plugin: string;
  pluginName: string;
  name: string;
  type: string;
  description: string;
  category: string;
  startHz: number;
  stopHz: number;
  color: [number, number, number, number];
}

export function overlayKey(overlay: Overlay): string {
  return `${overlay.plugin}|${overlay.type}|${overlay.name}|${overlay.startHz}|${overlay.stopHz}`;
}

export type FillStyle = "none" | "solid" | "gradient";

export interface SpectrumColours {
  background: string;
  grid: string;
  axisText: string;
  border: string;
  accent: string;
  live: string;
  maxHold: string;
  minHold: string;
  average: string;
  marker: string;
  cursor: string;
  selection: string;
}

export interface SpectrumScene {
  fromHz: number;
  toHz: number;
  yMinDb: number;
  yMaxDb: number;
  gradientMinDb: number;
  gradientMaxDb: number;
  traces: TraceStore;
  showGrid: boolean;
  fill: FillStyle;
  fillAlpha: number;
  /** 256 RGBA entries the gradient fill is coloured from. */
  fillLut: Uint8Array;
  /** The waterfall's, for the bar beside the plot. */
  waterfallLut: Uint8Array;
  traceThickness: number;
  /** Points across the plot; zero puts one on every pixel. */
  points: number;
  holds: TraceKind[];
  markers: Marker[];
  activeMarkerId: number;
  /** In rank order, highest first. */
  overlays: Overlay[];
  contributionAlpha: number;
  /** The overlay clicked to show its extent, by `overlayKey`. */
  picked: string;
  /** Where the pointer is, for hover outlines. */
  pointer: { x: number; y: number } | null;
  /** RBW, FFT and gains; empty until a frame has arrived. */
  readout: string;
  /** A range being dragged out, in Hz. */
  selection: { fromHz: number; toHz: number } | null;
  colours: SpectrumColours;
}

/** Something on the plot the pointer can hover or click. */
export interface Hit {
  x0: number;
  y0: number;
  x1: number;
  y1: number;
  /** One for a chip or a bar, every one gathered for an overflow chip. */
  overlays: Overlay[];
  more: boolean;
}

export interface Plot {
  x: number;
  y: number;
  width: number;
  height: number;
}

export function plotArea(width: number, height: number): Plot {
  return {
    x: kAxisLeft,
    y: kAxisTop,
    width: Math.max(width - kAxisLeft - kAxisRight, 1),
    height: Math.max(height - kAxisBottom - kAxisTop, 1),
  };
}

/** The gradient bar's left edge. */
export function gradientBarX(plot: Plot): number {
  return plot.x + plot.width + kGradientBarGap;
}

export function hzAt(plot: Plot, fromHz: number, toHz: number, x: number): number {
  return fromHz + ((x - plot.x) / plot.width) * (toHz - fromHz);
}

export function xAt(plot: Plot, fromHz: number, toHz: number, hz: number): number {
  return plot.x + ((hz - fromHz) / (toHz - fromHz)) * plot.width;
}

export function yAtDb(plot: Plot, yMinDb: number, yMaxDb: number, db: number): number {
  return plot.y + ((yMaxDb - db) / (yMaxDb - yMinDb)) * plot.height;
}

export function dbAtY(plot: Plot, yMinDb: number, yMaxDb: number, y: number): number {
  return yMaxDb - ((y - plot.y) / plot.height) * (yMaxDb - yMinDb);
}

/** Display points: the plot's width, or the fixed count the settings ask for. */
export function displayPoints(plot: Plot, points: number): number {
  return points > 0 ? Math.min(Math.max(points, 64), 16384) : Math.max(Math.floor(plot.width), 1);
}

const css = ([r, g, b]: number[], alpha: number) =>
  `rgba(${Math.round(r! * 255)},${Math.round(g! * 255)},${Math.round(b! * 255)},${alpha})`;

/** Dark text on a light chip, light on a dark one. */
function chipText([r, g, b]: number[]): string {
  return 0.2126 * r! + 0.7152 * g! + 0.0722 * b! > 0.6 ? "#101010" : "#ffffff";
}

function lutColour(lut: Uint8Array, t: number, alpha: number): string {
  const i = Math.round(Math.min(Math.max(t, 0), 1) * 255) * 4;
  return `rgba(${lut[i]},${lut[i + 1]},${lut[i + 2]},${alpha})`;
}

function parseCss(colour: string): [number, number, number] {
  const hex = colour.replace(/^#/, "");
  if (/^[0-9a-f]{6}/i.test(hex)) {
    return [0, 2, 4].map((at) => parseInt(hex.slice(at, at + 2), 16) / 255) as [number, number, number];
  }
  return [1, 1, 1];
}

const kChipGap = 2;
const kChipPadX = 3;
const kChipPadY = 1;
const kWidthBar = 3;
const kChipRowGap = kWidthBar + 3;
const kMaxChipRows = 8;
const kMaxChipFraction = 0.45;
const kMinBandWidth = 7;

interface Slot {
  start: number;
  end: number;
}

const fits = (row: Slot[], start: number, end: number, gap: number) =>
  row.every((slot) => !(start < slot.end + gap && slot.start - gap < end));

/** Bands as bars the width of the allocation and channels as named chips,
 * stacked from the top, as the desktop draws them. Returns what can be
 * hovered. */
function drawContributions(ctx: CanvasRenderingContext2D, plot: Plot, scene: SpectrumScene): Hit[] {
  const hits: Hit[] = [];
  const { fromHz, toHz, pointer } = scene;
  const left = plot.x;
  const right = plot.x + plot.width;
  const top = plot.y;
  const bottom = plot.y + plot.height;
  const x = (hz: number) => xAt(plot, fromHz, toHz, hz);
  const hovering = (x0: number, y0: number, x1: number, y1: number) =>
    pointer !== null && pointer.x >= x0 && pointer.x <= x1 && pointer.y >= y0 && pointer.y <= y1;

  // Widest first, so a channel is not buried by the allocation holding it.
  const byWidth = [...scene.overlays].sort((a, b) => b.stopHz - b.startHz - (a.stopHz - a.startHz));

  // Only the picked one is painted over the trace.
  for (const overlay of byWidth) {
    if (overlayKey(overlay) !== scene.picked) {
      continue;
    }
    if (overlay.type === "band") {
      const x0 = Math.max(x(overlay.startHz), left);
      const x1 = Math.min(x(overlay.stopHz), right);
      if (x1 > x0) {
        ctx.fillStyle = css(overlay.color, scene.contributionAlpha || 0.18);
        ctx.fillRect(x0, top, x1 - x0, plot.height);
        ctx.strokeStyle = css(overlay.color, 0.65);
        ctx.beginPath();
        if (overlay.startHz > fromHz) {
          ctx.moveTo(Math.round(x0) + 0.5, top);
          ctx.lineTo(Math.round(x0) + 0.5, bottom);
        }
        if (overlay.stopHz < toHz) {
          ctx.moveTo(Math.round(x1) - 0.5, top);
          ctx.lineTo(Math.round(x1) - 0.5, bottom);
        }
        ctx.stroke();
      }
    } else {
      const centre = x((overlay.startHz + overlay.stopHz) / 2);
      if (centre >= left && centre <= right) {
        ctx.strokeStyle = css(overlay.color, 0.65);
        ctx.beginPath();
        ctx.moveTo(Math.round(centre) + 0.5, top);
        ctx.lineTo(Math.round(centre) + 0.5, bottom);
        ctx.stroke();
      }
    }
  }

  const firstRow = top + kOverlayTextTop + kLineHeight + 4;
  const height = kLineHeight + kChipPadY * 2;
  const rowStep = height + kChipRowGap;
  const budget = bottom * kMaxChipFraction + top * (1 - kMaxChipFraction) - firstRow;
  const maxRows = Math.min(kMaxChipRows, Math.floor(Math.max(budget, rowStep) / rowStep));

  ctx.textBaseline = "top";
  ctx.textAlign = "left";

  // Channels in rank order, the picked one first whatever its rank.
  const channels = scene.overlays.filter((o) => o.type !== "band");
  channels.sort((a, b) => Number(overlayKey(b) === scene.picked) - Number(overlayKey(a) === scene.picked));
  const rows: Slot[][] = [];
  const overflow: Overlay[] = [];
  for (const overlay of channels) {
    const lo = Math.max(x(overlay.startHz), left);
    const hi = Math.min(x(overlay.stopHz), right);
    if (!overlay.name || hi < lo) {
      continue;
    }
    const at = Math.min(Math.max(x((overlay.startHz + overlay.stopHz) / 2), lo), hi);
    const half = ctx.measureText(overlay.name).width / 2 + kChipPadX;
    if (half * 2 > plot.width) {
      continue;
    }
    const start = Math.min(Math.max(at - half, left), right - half * 2);
    const end = start + half * 2;
    let row = rows.findIndex((slots) => fits(slots, start, end, kChipGap));
    if (row < 0) {
      if (rows.length >= maxRows) {
        overflow.push(overlay);
        continue;
      }
      rows.push([]);
      row = rows.length - 1;
    }
    rows[row]!.push({ start, end });

    const y = firstRow + row * rowStep;
    const hovered = hovering(start, y, end, y + height);
    const selected = overlayKey(overlay) === scene.picked;
    if (hovered && !selected) {
      ctx.strokeStyle = css(overlay.color, 0.45);
      ctx.beginPath();
      ctx.moveTo(Math.round(at) + 0.5, y + height);
      ctx.lineTo(Math.round(at) + 0.5, bottom);
      ctx.stroke();
    }
    ctx.fillStyle = css(overlay.color, hovered || selected ? 1 : 0.95);
    ctx.beginPath();
    ctx.roundRect(start, y, end - start, height, 2);
    ctx.fill();
    if (hovered || selected) {
      ctx.strokeStyle = `rgba(255,255,255,${selected ? 1 : 0.63})`;
      ctx.stroke();
    }
    ctx.fillStyle = chipText(overlay.color);
    ctx.fillText(overlay.name, start + kChipPadX, y + kChipPadY + 1);
    if (selected && overlay.stopHz > overlay.startHz) {
      const w0 = Math.max(x(overlay.startHz), left);
      const w1 = Math.min(x(overlay.stopHz), right);
      ctx.fillStyle = css(overlay.color, 0.95);
      ctx.fillRect(w0, y + height + 1, Math.max(w1 - w0, 1), kWidthBar);
    }
    hits.push({ x0: start, y0: y, x1: end, y1: y + height, overlays: [overlay], more: false });
  }

  // What did not fit, one "…" per cluster.
  if (overflow.length > 0) {
    const half = ctx.measureText("…").width / 2 + kChipPadX;
    const y = firstRow + rows.length * rowStep;
    const centreOf = (o: Overlay) => x((o.startHz + o.stopHz) / 2);
    const byX = [...overflow].sort((a, b) => centreOf(a) - centreOf(b));
    for (let first = 0; first < byX.length; ) {
      const startX = centreOf(byX[first]!);
      let last = first;
      while (last + 1 < byX.length && centreOf(byX[last + 1]!) - startX <= half * 2 + kChipGap) {
        ++last;
      }
      const centre = Math.min(Math.max((startX + centreOf(byX[last]!)) / 2, left + half), right - half);
      const colour = byX[first]!.color;
      ctx.fillStyle = css(colour, 0.95);
      ctx.beginPath();
      ctx.roundRect(centre - half, y, half * 2, height, 2);
      ctx.fill();
      ctx.fillStyle = chipText(colour);
      ctx.fillText("…", centre - half + kChipPadX, y + kChipPadY + 1);
      hits.push({
        x0: centre - half,
        y0: y,
        x1: centre + half,
        y1: y + height,
        overlays: byX.slice(first, last + 1),
        more: true,
      });
      first = last + 1;
    }
  }

  // Allocations under the stack: no gap between bars, since an allocation
  // table is contiguous; only nested ones take a row of their own.
  const bandTop = firstRow + (rows.length + (overflow.length > 0 ? 1 : 0)) * rowStep;
  const bandRows: Slot[][] = [];
  for (const overlay of byWidth) {
    if (overlay.type !== "band") {
      continue;
    }
    const x0 = Math.max(x(overlay.startHz), left);
    const x1 = Math.min(x(overlay.stopHz), right);
    if (x1 - x0 < kMinBandWidth) {
      continue;
    }
    let row = bandRows.findIndex((slots) => fits(slots, x0, x1, 0));
    if (row < 0) {
      if (bandRows.length >= kMaxChipRows) {
        continue;
      }
      bandRows.push([]);
      row = bandRows.length - 1;
    }
    bandRows[row]!.push({ start: x0, end: x1 });

    const y = bandTop + row * rowStep;
    const hovered = hovering(x0 + 0.5, y, x1 - 0.5, y + height);
    const selected = overlayKey(overlay) === scene.picked;
    ctx.fillStyle = css(overlay.color, hovered || selected ? 1 : 0.92);
    ctx.beginPath();
    ctx.roundRect(x0 + 0.5, y, x1 - x0 - 1, height, 2);
    ctx.fill();
    if (hovered || selected) {
      ctx.strokeStyle = `rgba(255,255,255,${selected ? 1 : 0.63})`;
      ctx.stroke();
    }
    if (overlay.name) {
      ctx.save();
      ctx.beginPath();
      ctx.rect(x0 + 1.5, y, x1 - x0 - 3, height);
      ctx.clip();
      ctx.fillStyle = chipText(overlay.color);
      ctx.textAlign = "center";
      ctx.fillText(overlay.name, (x0 + x1) / 2, y + kChipPadY + 1);
      ctx.restore();
    }
    hits.push({ x0, y0: y, x1, y1: y + height, overlays: [overlay], more: false });
  }
  return hits;
}

/** One trace as the desktop draws it: each column a bar from its weakest to
 * its strongest bin, the strongest joined across, so a one-bin carrier is
 * never thinned away by zooming out. */
function drawTrace(
  ctx: CanvasRenderingContext2D,
  plot: Plot,
  levels: { minimum: Float32Array; maximum: Float32Array },
  yAt: (db: number) => number,
  colour: string,
  thickness: number,
): void {
  const columns = levels.maximum.length;
  const columnWidth = plot.width / columns;
  ctx.strokeStyle = colour;
  ctx.lineWidth = thickness;
  ctx.lineJoin = "round";
  ctx.beginPath();
  let previous = false;
  for (let i = 0; i < columns; ++i) {
    const high = levels.maximum[i]!;
    if (!isMeasuredDb(high)) {
      previous = false;
      continue;
    }
    const x = plot.x + (i + 0.5) * columnWidth;
    const top = yAt(high);
    const bottom = yAt(levels.minimum[i]!);
    if (previous) {
      ctx.lineTo(x, top);
    } else {
      ctx.moveTo(x, top);
    }
    if (bottom - top > 1) {
      ctx.lineTo(x, bottom);
      ctx.moveTo(x, top);
    }
    previous = true;
  }
  ctx.stroke();
  ctx.lineWidth = 1;
}

/** Draws one spectrum, sized in CSS pixels on a canvas scaled by `dpr`. */
export function drawSpectrum(
  ctx: CanvasRenderingContext2D,
  width: number,
  height: number,
  scene: SpectrumScene,
): Hit[] {
  const plot = plotArea(width, height);
  const { fromHz, toHz, yMinDb, yMaxDb, colours } = scene;
  const yAt = (db: number) => yAtDb(plot, yMinDb, yMaxDb, db);
  const span = toHz - fromHz;
  const bottom = plot.y + plot.height;

  ctx.fillStyle = colours.background;
  ctx.fillRect(0, 0, width, height);
  ctx.font = kFont;
  ctx.lineWidth = 1;

  // The grid and both axes, labelled as the desktop's are: dB down the left,
  // MHz along the bottom.
  const dbStep = niceStep(((yMaxDb - yMinDb) / plot.height) * 40);
  ctx.strokeStyle = colours.grid;
  ctx.fillStyle = colours.axisText;
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";
  for (let db = Math.ceil(yMinDb / dbStep) * dbStep; db <= yMaxDb + 1e-9; db += dbStep) {
    const y = Math.round(yAt(db)) + 0.5;
    if (scene.showGrid) {
      ctx.beginPath();
      ctx.moveTo(plot.x, y);
      ctx.lineTo(plot.x + plot.width, y);
      ctx.stroke();
    }
    ctx.fillText(`${Math.round(db * 100) / 100}`, plot.x - 6, y);
  }
  if (span > 0) {
    const hzStep = niceStep((span / plot.width) * 120);
    const decimals = Math.max(0, Math.ceil(-Math.log10(hzStep / 1e6) - 1e-9));
    ctx.textAlign = "center";
    ctx.textBaseline = "top";
    for (let hz = Math.ceil(fromHz / hzStep) * hzStep; hz <= toHz; hz += hzStep) {
      const x = Math.round(xAt(plot, fromHz, toHz, hz)) + 0.5;
      if (scene.showGrid) {
        ctx.beginPath();
        ctx.moveTo(x, plot.y);
        ctx.lineTo(x, bottom);
        ctx.stroke();
      }
      ctx.fillText((hz / 1e6).toFixed(decimals), x, bottom + 4);
    }
  }
  ctx.textAlign = "center";
  ctx.textBaseline = "bottom";
  ctx.fillText("Frequency (MHz)", plot.x + plot.width / 2, height - 2);
  ctx.save();
  ctx.translate(13, plot.y + plot.height / 2);
  ctx.rotate(-Math.PI / 2);
  ctx.textBaseline = "middle";
  ctx.fillText("dBFS", 0, 0);
  ctx.restore();
  ctx.strokeStyle = colours.grid;
  ctx.strokeRect(plot.x - 0.5, plot.y - 0.5, plot.width + 1, plot.height + 1);

  ctx.save();
  ctx.beginPath();
  ctx.rect(plot.x, plot.y, plot.width, plot.height);
  ctx.clip();

  const hits = drawContributions(ctx, plot, scene);

  const pixels = displayPoints(plot, scene.points);
  if (scene.traces.binCount > 0 && span > 0) {
    // Holds under the live trace, which is what is being watched.
    const holdColour: Record<TraceKind, string> = {
      live: colours.live,
      maxHold: colours.maxHold,
      minHold: colours.minHold,
      average: colours.average,
    };
    for (const kind of ["minHold", "average", "maxHold"] as TraceKind[]) {
      if (scene.holds.includes(kind) && scene.traces.trace(kind).length > 0) {
        const hold = scene.traces.envelope(kind, fromHz, toHz, pixels);
        drawTrace(ctx, plot, hold, yAt, holdColour[kind], scene.traceThickness);
      }
    }

    const live = scene.traces.envelope("live", fromHz, toHz, pixels);
    if (scene.fill !== "none") {
      // A column per point, each coloured by its own level when graded.
      const columnWidth = plot.width / pixels;
      const solid = scene.fill === "solid";
      if (solid) {
        ctx.fillStyle = css(parseCss(colours.live), scene.fillAlpha);
        ctx.beginPath();
      }
      const gradientSpan = scene.gradientMaxDb - scene.gradientMinDb;
      for (let i = 0; i < pixels; ++i) {
        const level = live.maximum[i]!;
        if (!isMeasuredDb(level)) {
          continue;
        }
        const x0 = plot.x + i * columnWidth;
        const y = yAt(level);
        if (solid) {
          ctx.rect(x0, y, columnWidth + 1, bottom - y);
        } else {
          ctx.fillStyle = lutColour(
            scene.fillLut,
            gradientSpan > 0 ? (level - scene.gradientMinDb) / gradientSpan : 0,
            scene.fillAlpha,
          );
          ctx.fillRect(x0, y, columnWidth + 1, bottom - y);
        }
      }
      if (solid) {
        ctx.fill();
      }
    }
    drawTrace(ctx, plot, live, yAt, colours.live, scene.traceThickness);
  }

  if (scene.selection) {
    const x0 = xAt(plot, fromHz, toHz, Math.min(scene.selection.fromHz, scene.selection.toHz));
    const x1 = xAt(plot, fromHz, toHz, Math.max(scene.selection.fromHz, scene.selection.toHz));
    ctx.fillStyle = colours.selection;
    ctx.fillRect(x0, plot.y, x1 - x0, plot.height);
  }

  // Markers: each stroke over a backing in the plot's own colour, so it reads
  // over whatever it crosses; the selected one in the cursor colour.
  const backing = css(parseCss(colours.background), 0.75);
  ctx.textAlign = "left";
  ctx.textBaseline = "top";
  for (const marker of scene.markers) {
    if (!marker.visible || marker.frequencyHz < fromHz || marker.frequencyHz > toHz) {
      continue;
    }
    const x = xAt(plot, fromHz, toHz, marker.frequencyHz);
    const selected = marker.id === scene.activeMarkerId;
    const colour = selected ? colours.cursor : colours.marker;
    const weight = selected ? 1.8 : 1.2;
    for (const [style, w] of [
      [backing, weight + 2],
      [colour, weight],
    ] as [string, number][]) {
      ctx.strokeStyle = style;
      ctx.lineWidth = w;
      ctx.beginPath();
      ctx.moveTo(x, plot.y);
      ctx.lineTo(x, bottom);
      ctx.stroke();
    }
    ctx.lineWidth = 1;
    if (isMeasuredDb(marker.levelDb)) {
      const y = yAt(marker.levelDb);
      ctx.fillStyle = backing;
      ctx.beginPath();
      ctx.arc(x, y, 5.5, 0, Math.PI * 2);
      ctx.fill();
      ctx.fillStyle = colour;
      ctx.beginPath();
      ctx.arc(x, y, 4, 0, Math.PI * 2);
      ctx.fill();
    }
    const label = `M${marker.id} ${frequencyShort(marker.frequencyHz)}`;
    const textWidth = ctx.measureText(label).width;
    const chipX = x - textWidth / 2 - 4;
    const chipY = bottom - kLineHeight - 4;
    ctx.fillStyle = colour;
    ctx.beginPath();
    ctx.roundRect(chipX, chipY, textWidth + 8, kLineHeight + 4, 3);
    ctx.fill();
    ctx.strokeStyle = backing;
    ctx.stroke();
    ctx.fillStyle = chipText(parseCss(colour));
    ctx.fillText(label, chipX + 4, chipY + 3);
  }

  // The readout row: what the frame was measured with, and what one drawn
  // column integrates over.
  if (scene.readout) {
    ctx.fillStyle = colours.axisText;
    ctx.textBaseline = "top";
    ctx.textAlign = "left";
    ctx.fillText(scene.readout, plot.x + 8, plot.y + kOverlayTextTop);
    ctx.textAlign = "right";
    ctx.fillText(
      `VBW ${frequencyShort(span / pixels)}   Points ${pixels}`,
      plot.x + plot.width - 8,
      plot.y + kOverlayTextTop,
    );
  }
  ctx.restore();

  // The dB scale's two handles, dragged to stretch either end.
  ctx.fillStyle = colours.accent;
  for (const db of [yMaxDb, yMinDb]) {
    ctx.beginPath();
    ctx.roundRect(plot.x + kHandleInset, yAt(db) - kHandleSize / 2, kHandleSize * 1.6, kHandleSize, 2);
    ctx.fill();
  }

  // The waterfall's gradient beside the plot, its handles at the levels the
  // waterfall's colours run between.
  const barX = gradientBarX(plot);
  const slices = 64;
  for (let i = 0; i < slices; ++i) {
    const t0 = i / slices;
    const t1 = (i + 1) / slices;
    ctx.fillStyle = lutColour(scene.waterfallLut, (t0 + t1) / 2, 1);
    ctx.fillRect(barX, plot.y + plot.height * (1 - t1), kGradientBarWidth, plot.height / slices + 0.5);
  }
  ctx.strokeStyle = colours.border;
  ctx.strokeRect(barX + 0.5, plot.y + 0.5, kGradientBarWidth - 1, plot.height - 1);
  ctx.fillStyle = colours.accent;
  for (const db of [scene.gradientMaxDb, scene.gradientMinDb]) {
    const y = plot.y + plot.height * (1 - Math.min(Math.max((db - yMinDb) / (yMaxDb - yMinDb), 0), 1));
    ctx.beginPath();
    ctx.moveTo(barX - 6, y);
    ctx.lineTo(barX, y - 5);
    ctx.lineTo(barX, y + 5);
    ctx.closePath();
    ctx.fill();
  }
  return hits;
}

/** Which of the scale's handles is under (x, y), if any. */
export function handleAt(
  plot: Plot,
  scene: { yMinDb: number; yMaxDb: number; gradientMinDb: number; gradientMaxDb: number },
  x: number,
  y: number,
): "yMax" | "yMin" | "gradientMax" | "gradientMin" | null {
  const yAt = (db: number) => yAtDb(plot, scene.yMinDb, scene.yMaxDb, db);
  if (x >= plot.x + kHandleInset && x <= plot.x + kHandleInset + kHandleSize * 2.4) {
    if (Math.abs(y - yAt(scene.yMaxDb)) <= kHandleSize) return "yMax";
    if (Math.abs(y - yAt(scene.yMinDb)) <= kHandleSize) return "yMin";
  }
  const barX = gradientBarX(plot);
  if (x >= barX - 8 && x <= barX + kGradientBarWidth && y >= plot.y && y <= plot.y + plot.height) {
    const clamp = (db: number) => Math.min(Math.max(db, scene.yMinDb), scene.yMaxDb);
    return Math.abs(y - yAt(clamp(scene.gradientMaxDb))) < Math.abs(y - yAt(clamp(scene.gradientMinDb)))
      ? "gradientMax"
      : "gradientMin";
  }
  return null;
}
