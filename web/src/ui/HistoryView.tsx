// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";
import { useEffect, useRef, useState } from "react";

import { focused } from "../model/layout";
import { isMeasuredDb } from "../protocol/mirror";
import { frequency, frequencyShort, niceStep } from "../render/format";
import { kAxisLeft } from "../render/spectrum";
import {
  closeHistory,
  type HistorySummary,
  type HistoryTile,
  openHistory,
  queryHistory,
  tileDb,
} from "../state/history";
import { useInstrument } from "../state/instrument";
import { bakeColorMap, findTheme, themeStore } from "../state/theme";
import { viewStore } from "../state/view";

interface Window {
  fromNs: bigint;
  toNs: bigint;
  fromHz: number;
  toHz: number;
}

const kSpectrumHeight = 150;
const kAxisBottom = 20;

/** Per screen pixel, the strongest level any tile puts there: lines spread
 * over the rows their time covers, bins over the columns of their band. */
function rasterise(tiles: HistoryTile[], window: Window, width: number, height: number): Float32Array {
  const pixels = new Float32Array(width * height).fill(-Infinity);
  const span = Number(window.toNs - window.fromNs);
  const yAt = (ns: bigint) => ((1 - Number(ns - window.fromNs) / span) * height) | 0;
  const xAt = (hz: number) => (((hz - window.fromHz) / (window.toHz - window.fromHz)) * width) | 0;
  for (const tile of tiles) {
    const lineNs = tile.lines > 1 ? (tile.lastLineNs - tile.firstLineNs) / BigInt(tile.lines - 1) : 1n;
    for (let line = 0; line < tile.lines; ++line) {
      const t = tile.firstLineNs + lineNs * BigInt(line);
      const top = Math.max(yAt(t + lineNs), 0);
      const bottom = Math.min(Math.max(yAt(t), top + 1), height);
      if (bottom <= 0 || top >= height) {
        continue;
      }
      for (let bin = 0; bin < tile.bins; ++bin) {
        const left = Math.max(xAt(tile.startHz + tile.binWidthHz * bin), 0);
        const right = Math.min(Math.max(xAt(tile.startHz + tile.binWidthHz * (bin + 1)), left + 1), width);
        if (right <= 0 || left >= width) {
          continue;
        }
        const db = tileDb(tile, line, bin);
        if (!isMeasuredDb(db)) {
          continue;
        }
        for (let y = top; y < bottom; ++y) {
          const row = y * width;
          for (let x = left; x < right; ++x) {
            if (db > pixels[row + x]!) {
              pixels[row + x] = db;
            }
          }
        }
      }
    }
  }
  return pixels;
}

/** A recording on the server, read in place: a waterfall over its time, and
 * the spectrum at the line under the cursor. */
export function HistoryView({ name, onClose }: { name: string; onClose: () => void }) {
  const [summary, setSummary] = useState<HistorySummary | null>(null);
  const [error, setError] = useState("");
  const [window_, setWindow] = useState<Window | null>(null);
  const [tiles, setTiles] = useState<HistoryTile[]>([]);
  const [loading, setLoading] = useState(false);
  const [cursor, setCursor] = useState<{ ns: bigint; hz: number } | null>(null);
  // The live panel's gradient: the colour map was chosen for the levels it shows.
  const [gradient, setGradient] = useState(() => {
    const panel = focused(viewStore.state.layout);
    return { min: panel.gradientMinDb, max: panel.gradientMaxDb };
  });
  const area = useRef<HTMLDivElement>(null);
  const waterfall = useRef<HTMLCanvasElement>(null);
  const spectrum = useRef<HTMLCanvasElement>(null);
  const [size, setSize] = useState({ width: 0, height: 0 });
  const themeName = useSelector(viewStore, (v) => v.themeName);
  const colorMapName = useSelector(viewStore, (v) => v.colorMap);
  const maps = useSelector(themeStore, (t) => t.colorMaps);

  useEffect(() => {
    let handle = 0;
    let cancelled = false;
    openHistory(name)
      .then((opened) => {
        handle = opened.handle;
        if (cancelled) {
          closeHistory(handle);
          return;
        }
        setSummary(opened);
        setWindow({
          fromNs: opened.firstLineNs,
          toNs: opened.lastLineNs > opened.firstLineNs ? opened.lastLineNs : opened.firstLineNs + 1n,
          fromHz: opened.lowestHz,
          toHz: opened.highestHz > opened.lowestHz ? opened.highestHz : opened.lowestHz + 1,
        });
      })
      .catch((e: unknown) => setError(e instanceof Error ? e.message : String(e)));
    return () => {
      cancelled = true;
      if (handle) {
        closeHistory(handle);
      }
    };
  }, [name]);

  useEffect(() => {
    const element = area.current;
    if (!element) {
      return;
    }
    const observer = new ResizeObserver(([entry]) =>
      setSize({ width: Math.floor(entry!.contentRect.width), height: Math.floor(entry!.contentRect.height) }),
    );
    observer.observe(element);
    return () => observer.disconnect();
  }, []);

  const plotWidth = Math.max(size.width - kAxisLeft - 70, 1);
  const waterfallHeight = Math.max(size.height - kSpectrumHeight - kAxisBottom - 8, 1);

  // Asked for again, at the screen's resolution, whenever the window moves.
  useEffect(() => {
    if (!summary || !window_ || plotWidth < 2 || waterfallHeight < 2) {
      return;
    }
    const timer = setTimeout(() => {
      setLoading(true);
      queryHistory(summary.handle, {
        fromNs: window_.fromNs,
        toNs: window_.toNs,
        fromHz: window_.fromHz,
        toHz: window_.toHz,
        lines: Math.min(Math.round(waterfallHeight * devicePixelRatio), 2048),
        bins: Math.min(Math.round(plotWidth * devicePixelRatio), 4096),
      })
        .then((got) => {
          setTiles(got);
          setError("");
        })
        .catch((e: unknown) => setError(e instanceof Error ? e.message : String(e)))
        .finally(() => setLoading(false));
    }, 200);
    return () => clearTimeout(timer);
  }, [summary, window_, plotWidth, waterfallHeight]);

  // The waterfall image.
  useEffect(() => {
    const canvas = waterfall.current;
    if (!canvas || !window_) {
      return;
    }
    const width = Math.max(Math.round(plotWidth * devicePixelRatio), 1);
    const height = Math.max(Math.round(waterfallHeight * devicePixelRatio), 1);
    canvas.width = width;
    canvas.height = height;
    const ctx = canvas.getContext("2d")!;
    const pixels = rasterise(tiles, window_, width, height);
    const lut = bakeColorMap(
      maps.find((m) => m.name === (colorMapName || findTheme(themeName)?.waterfall.colorMap)) ?? maps[0],
    );
    const image = ctx.createImageData(width, height);
    const span = Math.max(gradient.max - gradient.min, 0.1);
    for (let i = 0; i < pixels.length; ++i) {
      const db = pixels[i]!;
      if (db === -Infinity) {
        image.data[i * 4 + 3] = 255;
        continue;
      }
      const index = Math.min(Math.max(Math.round(((db - gradient.min) / span) * 255), 0), 255) * 4;
      image.data[i * 4] = lut[index]!;
      image.data[i * 4 + 1] = lut[index + 1]!;
      image.data[i * 4 + 2] = lut[index + 2]!;
      image.data[i * 4 + 3] = 255;
    }
    ctx.putImageData(image, 0, 0);
  }, [tiles, window_, plotWidth, waterfallHeight, gradient, maps, colorMapName, themeName]);

  // The spectrum at the cursor's line, or the newest in view.
  useEffect(() => {
    const canvas = spectrum.current;
    if (!canvas || !window_) {
      return;
    }
    const dpr = devicePixelRatio;
    const width = size.width;
    canvas.width = Math.round(width * dpr);
    canvas.height = Math.round(kSpectrumHeight * dpr);
    const ctx = canvas.getContext("2d")!;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    const theme = findTheme(themeName)?.spectrum ?? {};
    const colour = (key: string, fallback: string) => (typeof theme[key] === "string" ? (theme[key] as string) : fallback);
    ctx.fillStyle = colour("background", "#000");
    ctx.fillRect(0, 0, width, kSpectrumHeight);
    const at = cursor?.ns ?? window_.toNs;
    const columns = new Float32Array(Math.max(Math.floor(plotWidth), 1)).fill(-Infinity);
    for (const tile of tiles) {
      if (at < tile.firstLineNs || at > tile.lastLineNs + (tile.lastLineNs - tile.firstLineNs) / BigInt(Math.max(tile.lines, 1))) {
        continue;
      }
      const lineNs = tile.lines > 1 ? Number(tile.lastLineNs - tile.firstLineNs) / (tile.lines - 1) : 1;
      const line = Math.min(Math.max(Math.round(Number(at - tile.firstLineNs) / lineNs), 0), tile.lines - 1);
      for (let bin = 0; bin < tile.bins; ++bin) {
        const hz = tile.startHz + tile.binWidthHz * (bin + 0.5);
        const x = Math.floor(((hz - window_.fromHz) / (window_.toHz - window_.fromHz)) * columns.length);
        const db = tileDb(tile, line, bin);
        if (x >= 0 && x < columns.length && isMeasuredDb(db) && db > columns[x]!) {
          columns[x] = db;
        }
      }
    }
    const yMin = gradient.min - 20;
    const yMax = gradient.max + 10;
    const yAt = (db: number) => 4 + ((yMax - db) / (yMax - yMin)) * (kSpectrumHeight - 8);
    ctx.strokeStyle = colour("grid", "#2a2f38");
    ctx.fillStyle = colour("axisText", "#8b919a");
    ctx.font = "11px system-ui, sans-serif";
    ctx.textAlign = "right";
    ctx.textBaseline = "middle";
    const step = niceStep((yMax - yMin) / 4);
    for (let db = Math.ceil(yMin / step) * step; db <= yMax; db += step) {
      const y = Math.round(yAt(db)) + 0.5;
      ctx.beginPath();
      ctx.moveTo(kAxisLeft, y);
      ctx.lineTo(kAxisLeft + plotWidth, y);
      ctx.stroke();
      ctx.fillText(`${db}`, kAxisLeft - 6, y);
    }
    ctx.strokeStyle = colour("traceLive", "#4ade80");
    ctx.beginPath();
    let drawing = false;
    columns.forEach((db, x) => {
      if (db === -Infinity) {
        drawing = false;
        return;
      }
      if (drawing) {
        ctx.lineTo(kAxisLeft + x + 0.5, yAt(db));
      } else {
        ctx.moveTo(kAxisLeft + x + 0.5, yAt(db));
        drawing = true;
      }
    });
    ctx.stroke();
  }, [tiles, cursor, window_, size.width, plotWidth, gradient, themeName]);

  if (error && !summary) {
    return (
      <div className="flex h-full flex-col items-center justify-center gap-3">
        <p className="text-warning">{error}</p>
        <button className="btn" onClick={onClose}>
          Back
        </button>
      </div>
    );
  }

  const zoomTime = (factor: number, anchor: bigint) => {
    if (!window_ || !summary) return;
    const width = Number(window_.toNs - window_.fromNs) * factor;
    const total = Number(summary.lastLineNs - summary.firstLineNs);
    const clamped = Math.min(Math.max(width, 1e6), Math.max(total, 1e6));
    const share = Number(anchor - window_.fromNs) / Number(window_.toNs - window_.fromNs);
    let from = anchor - BigInt(Math.round(clamped * share));
    from = from < summary.firstLineNs ? summary.firstLineNs : from;
    let to = from + BigInt(Math.round(clamped));
    if (to > summary.lastLineNs) {
      to = summary.lastLineNs;
      from = to - BigInt(Math.round(clamped));
      from = from < summary.firstLineNs ? summary.firstLineNs : from;
    }
    setWindow({ ...window_, fromNs: from, toNs: to });
  };

  const zoomFrequency = (factor: number, anchor: number) => {
    if (!window_ || !summary) return;
    const lo = summary.lowestHz;
    const hi = summary.highestHz;
    const width = Math.min((window_.toHz - window_.fromHz) * factor, hi - lo);
    let from = anchor - (anchor - window_.fromHz) * (width / (window_.toHz - window_.fromHz));
    from = Math.min(Math.max(from, lo), hi - width);
    setWindow({ ...window_, fromHz: from, toHz: from + width });
  };

  const fromCanvas = (event: React.MouseEvent<HTMLCanvasElement>) => {
    if (!window_) return null;
    const box = event.currentTarget.getBoundingClientRect();
    const fx = (event.clientX - box.left) / box.width;
    const fy = (event.clientY - box.top) / box.height;
    return {
      hz: window_.fromHz + fx * (window_.toHz - window_.fromHz),
      ns: window_.toNs - BigInt(Math.round(fy * Number(window_.toNs - window_.fromNs))),
    };
  };

  const elapsed = (ns: bigint) => (summary ? Number(ns - summary.firstLineNs) / 1e9 : 0);
  const scrollbar =
    summary && window_
      ? {
          top: 1 - elapsed(window_.toNs) / Math.max(elapsed(summary.lastLineNs), 1e-9),
          height: (elapsed(window_.toNs) - elapsed(window_.fromNs)) / Math.max(elapsed(summary.lastLineNs), 1e-9),
        }
      : null;

  return (
    <div className="flex h-full flex-col">
      <div className="flex h-11 shrink-0 items-center gap-3 border-b border-border bg-header px-3">
        <button className="btn" onClick={onClose}>
          ← Live
        </button>
        <span className="truncate font-semibold">{name}</span>
        {summary && (
          <span className="hidden text-dim md:inline">
            {frequencyShort(summary.lowestHz)} – {frequencyShort(summary.highestHz)} ·{" "}
            {elapsed(summary.lastLineNs).toFixed(1)} s · {summary.totalLines} lines
          </span>
        )}
        {loading && <span className="text-dim">Loading…</span>}
        {error && <span className="text-warning">{error}</span>}
        <span className="ml-auto flex items-center gap-2 text-xs text-dim">
          Gradient
          <input
            className="field w-16"
            type="number"
            value={gradient.min}
            onChange={(e) => setGradient((g) => ({ ...g, min: Math.min(Number(e.target.value), g.max - 5) }))}
          />
          <input
            className="field w-16"
            type="number"
            value={gradient.max}
            onChange={(e) => setGradient((g) => ({ ...g, max: Math.max(Number(e.target.value), g.min + 5) }))}
          />
          <button
            className="btn"
            onClick={() =>
              summary &&
              setWindow({
                fromNs: summary.firstLineNs,
                toNs: summary.lastLineNs,
                fromHz: summary.lowestHz,
                toHz: summary.highestHz,
              })
            }
          >
            Whole recording
          </button>
        </span>
      </div>
      <div ref={area} className="relative min-h-0 flex-1 bg-plot">
        <canvas
          ref={waterfall}
          className="absolute top-0 cursor-crosshair"
          style={{ left: kAxisLeft, width: plotWidth, height: waterfallHeight }}
          onMouseMove={(e) => setCursor(fromCanvas(e))}
          onMouseLeave={() => setCursor(null)}
          onWheel={(e) => {
            const at = fromCanvas(e);
            if (!at) return;
            const factor = Math.exp(e.deltaY * 0.0015);
            if (e.shiftKey) {
              zoomTime(factor, at.ns);
            } else {
              zoomFrequency(factor, at.hz);
            }
          }}
        />
        {window_ && (
          <div className="absolute text-[11px] text-axis" style={{ left: kAxisLeft, top: waterfallHeight + 2, width: plotWidth }}>
            {Array.from({ length: 6 }, (_, i) => {
              const hz = window_.fromHz + ((window_.toHz - window_.fromHz) * i) / 5;
              return (
                <span key={i} className="absolute -translate-x-1/2" style={{ left: `${(i / 5) * 100}%` }}>
                  {frequencyShort(hz)}
                </span>
              );
            })}
          </div>
        )}
        {window_ &&
          Array.from({ length: 5 }, (_, i) => {
            const ns = window_.toNs - (window_.toNs - window_.fromNs) * BigInt(i) / 4n;
            return (
              <span
                key={i}
                className="absolute w-10 -translate-y-1/2 text-right text-[11px] text-axis"
                style={{ left: 2, top: (i / 4) * waterfallHeight }}
              >
                {elapsed(ns).toFixed(1)}s
              </span>
            );
          })}
        {scrollbar && (
          <div
            className="absolute w-3 rounded bg-button"
            style={{ left: kAxisLeft + plotWidth + 30, top: 0, height: waterfallHeight }}
            onPointerDown={(e) => {
              if (!summary || !window_) return;
              const box = e.currentTarget.getBoundingClientRect();
              const share = 1 - (e.clientY - box.top) / box.height;
              const total = summary.lastLineNs - summary.firstLineNs;
              const width = window_.toNs - window_.fromNs;
              let to = summary.firstLineNs + BigInt(Math.round(Number(total) * share)) + width / 2n;
              to = to > summary.lastLineNs ? summary.lastLineNs : to;
              let from = to - width;
              if (from < summary.firstLineNs) {
                from = summary.firstLineNs;
                to = from + width;
              }
              setWindow({ ...window_, fromNs: from, toNs: to });
            }}
          >
            <div
              className="absolute w-full rounded bg-accent/70"
              style={{ top: `${scrollbar.top * 100}%`, height: `${Math.max(scrollbar.height * 100, 2)}%` }}
            />
          </div>
        )}
        <canvas
          ref={spectrum}
          className="absolute left-0 w-full"
          style={{ top: waterfallHeight + kAxisBottom + 8, height: kSpectrumHeight }}
        />
        {cursor && (
          <div className="pointer-events-none absolute right-3 top-2 rounded bg-panel/90 px-2 py-1 text-xs">
            {frequency(cursor.hz)} · {elapsed(cursor.ns).toFixed(2)} s
          </div>
        )}
      </div>
      <p className="caption shrink-0 border-t border-border bg-header px-3 py-1">
        Wheel: zoom in frequency · Shift+wheel: zoom in time · the bar on the right moves through time
      </p>
    </div>
  );
}

/** The server's recordings, to pick one to read. */
export function HistoryPicker({ onOpen }: { onOpen: (name: string) => void }) {
  const { recordings } = useInstrument();
  const files = recordings.files.filter((f) => !(recordings.active && f.name === recordings.current));
  if (!recordings.available) {
    return <p className="caption">This server has nowhere to record to.</p>;
  }
  if (files.length === 0) {
    return <p className="caption">Nothing recorded on the server yet. Record from the device panel.</p>;
  }
  return (
    <div>
      {files.map((file) => (
        <button
          key={file.name}
          className="block w-full truncate rounded px-2 py-1.5 text-left hover:bg-button-hover"
          onClick={() => onOpen(file.name)}
        >
          {file.name}
        </button>
      ))}
    </div>
  );
}
