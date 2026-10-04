// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";
import { type PointerEvent as ReactPointerEvent, useEffect, useRef, useState } from "react";

import { clampView, focused, type Span, spanValid, spanWidth, zoomAbout } from "../model/layout";
import { isMeasuredDb } from "../protocol/mirror";
import { frequency } from "../render/format";
import { readInstrument, sweepRange } from "../state/instrument";
import { addSegment } from "../state/presets";
import { session } from "../state/session";
import { findTheme } from "../state/theme";
import { setLayout, viewStore } from "../state/view";

export const kOverviewHeight = 44;

const same = (a: Span, b: Span) => Math.abs(a.startHz - b.startHz) <= 1 && Math.abs(a.stopHz - b.stopHz) <= 1;

function hex(colour: unknown, alpha: number, fallback: string): string {
  if (typeof colour !== "string" || !/^#[0-9a-f]{6}/i.test(colour)) {
    return fallback;
  }
  const [r, g, b] = [1, 3, 5].map((at) => parseInt(colour.slice(at, at + 2), 16));
  return `rgba(${r},${g},${b},${alpha})`;
}

/** The whole reach above the Spans panels: every segment, numbered by the
 * panel showing it, under a coarse live trace. Click a segment to focus its
 * panel or to move the focused one onto it; shift+drag adds a segment. */
export function OverviewStrip({ segments }: { segments: Span[] }) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const layout = useSelector(viewStore, (v) => v.layout);
  const themeName = useSelector(viewStore, (v) => v.themeName);
  const [zoom, setZoom] = useState<Span | null>(null);
  const [tip, setTip] = useState("");
  const selecting = useRef<{ fromX: number; toX: number } | null>(null);

  const device = readInstrument().device.descriptor?.info;
  let full: Span = device ? { startHz: Math.max(0, device.minFrequencyHz), stopHz: device.maxFrequencyHz } : { startHz: 0, stopHz: 0 };
  if (!spanValid(full) && segments.length > 0) {
    const lo = Math.min(...segments.map((s) => s.startHz));
    const hi = Math.max(...segments.map((s) => s.stopHz));
    const margin = Math.max((hi - lo) * 0.05, 1e6);
    full = { startHz: Math.max(0, lo - margin), stopHz: hi + margin };
  }
  const range = (zoom && clampView(zoom.startHz, zoom.stopHz, full)) || full;

  const scene = useRef({ range, segments, layout, themeName });
  scene.current = { range, segments, layout, themeName };
  const dirty = useRef(true);
  dirty.current = true;

  useEffect(() => {
    let frame = 0;
    let drawn = -1;
    const draw = () => {
      frame = requestAnimationFrame(draw);
      const element = canvas.current;
      if (!element || element.clientWidth === 0) {
        return;
      }
      if (!dirty.current && session.traces.generation === drawn) {
        return;
      }
      dirty.current = false;
      drawn = session.traces.generation;
      const { range: r, segments: segs, layout: l, themeName: name } = scene.current;
      const theme = findTheme(name);
      const chrome = theme?.chrome ?? {};
      const spectrum = theme?.spectrum ?? {};
      const dpr = window.devicePixelRatio || 1;
      const width = element.clientWidth;
      const height = element.clientHeight;
      if (element.width !== Math.round(width * dpr) || element.height !== Math.round(height * dpr)) {
        element.width = Math.round(width * dpr);
        element.height = Math.round(height * dpr);
      }
      const ctx = element.getContext("2d")!;
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      ctx.fillStyle = typeof spectrum.background === "string" ? spectrum.background : "#000";
      ctx.fillRect(0, 0, width, height);
      if (!spanValid(r)) {
        return;
      }
      const x = (hz: number) => ((hz - r.startHz) / spanWidth(r)) * width;
      const focusedId = focused(l).id;

      ctx.font = "11px system-ui, sans-serif";
      ctx.textBaseline = "top";
      for (const segment of segs) {
        const x0 = Math.max(x(segment.startHz), 0);
        const x1 = Math.min(x(segment.stopHz), width);
        if (x1 <= x0) {
          continue;
        }
        const index = l.panels.findIndex((p) => same(p.segment, segment));
        const shown = index >= 0;
        const tint = shown && l.panels[index]!.id === focusedId ? chrome.accent : shown ? chrome.text : chrome.textDim;
        const w = Math.max(x1 - x0, 2);
        ctx.fillStyle = hex(tint, shown ? 0.18 : 0.08, "rgba(128,128,128,0.15)");
        ctx.fillRect(x0, 0, w, height);
        ctx.strokeStyle = hex(tint, shown ? 0.8 : 0.4, "rgba(128,128,128,0.6)");
        ctx.strokeRect(x0 + 0.5, 0.5, w - 1, height - 1);
        const label = `${index + 1}`;
        if (shown && w > ctx.measureText(label).width + 6) {
          ctx.fillStyle = hex(tint, 1, "#ccc");
          ctx.fillText(label, x0 + 3, 2);
        }
      }

      // A coarse live trace, one column per pixel.
      const panel = focused(l);
      const columns = Math.max(Math.floor(width), 1);
      const envelope = session.traces.envelope("live", r.startHz, r.stopHz, columns);
      const yAt = (db: number) =>
        Math.min(Math.max(((panel.yMaxDb - db) / (panel.yMaxDb - panel.yMinDb)) * height, 0), height);
      ctx.fillStyle = hex(spectrum.traceLive, 0.55, "rgba(74,222,128,0.55)");
      for (let i = 0; i < columns; ++i) {
        const level = envelope.maximum[i]!;
        if (isMeasuredDb(level)) {
          const y = yAt(level);
          ctx.fillRect((i * width) / columns, y, Math.max(width / columns, 1), height - y);
        }
      }

      if (selecting.current) {
        const from = Math.min(selecting.current.fromX, selecting.current.toX);
        const to = Math.max(selecting.current.fromX, selecting.current.toX);
        ctx.fillStyle = hex(chrome.ok, 0.25, "rgba(74,222,128,0.25)");
        ctx.fillRect(from, 0, to - from, height);
      }
      ctx.strokeStyle = typeof chrome.border === "string" ? chrome.border : "#444";
      ctx.strokeRect(0.5, 0.5, width - 1, height - 1);
    };
    frame = requestAnimationFrame(draw);
    return () => cancelAnimationFrame(frame);
  }, []);

  const hzAt = (clientX: number) => {
    const box = canvas.current!.getBoundingClientRect();
    return range.startHz + ((clientX - box.left) / box.width) * spanWidth(range);
  };
  const localX = (clientX: number) => clientX - canvas.current!.getBoundingClientRect().left;
  const segmentAt = (hz: number) => {
    const slack = (spanWidth(range) / Math.max(canvas.current?.clientWidth ?? 1, 1)) * 2;
    return segments.find((s) => hz >= s.startHz - slack && hz <= s.stopHz + slack) ?? null;
  };

  const onPointerDown = (event: ReactPointerEvent<HTMLCanvasElement>) => {
    event.currentTarget.setPointerCapture(event.pointerId);
    if (event.shiftKey) {
      const at = localX(event.clientX);
      selecting.current = { fromX: at, toX: at };
      return;
    }
    const segment = segmentAt(hzAt(event.clientX));
    if (!segment) {
      return;
    }
    setLayout((current) => {
      const index = current.panels.findIndex((p) => same(p.segment, segment));
      if (index >= 0) {
        return { ...current, focusedId: current.panels[index]!.id };
      }
      // No panel shows it: the focused one moves onto it.
      return {
        ...current,
        panels: current.panels.map((p) =>
          p.id === current.focusedId ? { ...p, segment, viewStartHz: 0, viewStopHz: 0 } : p,
        ),
      };
    });
  };

  const onPointerMove = (event: ReactPointerEvent<HTMLCanvasElement>) => {
    if (selecting.current) {
      selecting.current.toX = Math.min(Math.max(localX(event.clientX), 0), canvas.current!.clientWidth);
      dirty.current = true;
      return;
    }
    const hz = hzAt(event.clientX);
    const segment = segmentAt(hz);
    const shown = segment && layout.panels.some((p) => same(p.segment, segment));
    setTip(
      segment
        ? `${frequency(segment.startHz)} - ${frequency(segment.stopHz)}\n${frequency(spanWidth(segment))}\nClick: ${shown ? "focus its panel" : "show it in the focused panel"}\nShift+drag: add a segment`
        : `${frequency(hz)}\nShift+drag: add a segment\nWheel: zoom, double-click: reset`,
    );
  };

  const onPointerUp = () => {
    const selection = selecting.current;
    selecting.current = null;
    dirty.current = true;
    if (!selection || Math.abs(selection.toX - selection.fromX) < 4) {
      return;
    }
    const box = canvas.current!.getBoundingClientRect();
    const from = hzAt(box.left + Math.min(selection.fromX, selection.toX));
    const to = hzAt(box.left + Math.max(selection.fromX, selection.toX));
    const plan = readInstrument().plan;
    let next = plan.segments.map((s) => ({ ...s }));
    next = addSegment(next, { startHz: from, stopHz: to, dwellSeconds: 0 });
    sweepRange({ ...plan, segments: next });
  };

  return (
    <canvas
      ref={canvas}
      className="block w-full shrink-0 cursor-pointer"
      style={{ height: kOverviewHeight }}
      title={tip}
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={onPointerUp}
      onPointerCancel={onPointerUp}
      onDoubleClick={() => setZoom(null)}
      onWheel={(event) => {
        const factor = Math.pow(0.8, Math.max(Math.min(-event.deltaY / 100, 2.5), -2.5));
        const zoomed = zoomAbout(range, hzAt(event.clientX), factor);
        setZoom(clampView(zoomed.startHz, zoomed.stopHz, full));
      }}
    />
  );
}
