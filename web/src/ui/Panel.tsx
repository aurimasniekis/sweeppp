// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";
import { type PointerEvent as ReactPointerEvent, useEffect, useMemo, useRef, useState } from "react";

import {
  clampView,
  type PanelView,
  removePanel,
  resolveView,
  segmentBins,
  type Span,
  spanCentre,
  spanValid,
  spanWidth,
  zoomAbout,
} from "../model/layout";
import { addMarker, nearestMarker, updateMarker } from "../model/markers";
import type { TraceKind } from "../model/traces";
import { frequency, frequencyShort, level } from "../render/format";
import {
  dbAtY,
  drawSpectrum,
  handleAt,
  type Hit,
  hzAt,
  kAxisLeft,
  overlayKey,
  plotArea,
} from "../render/spectrum";
import { WaterfallRenderer } from "../render/waterfall";
import { hideContribution } from "../state/contributors";
import { session, type WaterfallLine } from "../state/session";
import { bakeColorMap, findTheme, parseHex, themeStore } from "../state/theme";
import { setLayout, setMarkers, setPanel, viewStore } from "../state/view";
import { Icon } from "./Icon";
import { icon } from "./icons";
import { MarkerReadout } from "./MarkerReadout";
import { useOverlays } from "./overlays";

export interface PanelActions {
  /** Sweep [fromHz, toHz] instead of what is swept now. */
  sweep(fromHz: number, toHz: number): void;
}

type Handle = "yMax" | "yMin" | "gradientMax" | "gradientMin";

interface Drag {
  kind: "pan" | "zoom" | "sweep" | "marker" | Handle;
  pointerId: number;
  startX: number;
  startHz: number;
  view: Span;
  markerId: number;
  moved: boolean;
  /** Where a handle was grabbed: its level, and the pointer's height. */
  anchorDb: number;
  anchorY: number;
}

interface Tip {
  x: number;
  y: number;
  lines: { text: string; colour?: string; dim?: boolean }[];
}

const cssColour = ([r, g, b]: number[]) => `rgb(${r! * 255},${g! * 255},${b! * 255})`;

/** What a chip or a bar says when hovered, as the desktop's tooltip does. */
function describeHit(hit: Hit): Tip["lines"] {
  if (hit.more) {
    return hit.overlays.map((o) => ({ text: o.name, colour: cssColour(o.color) }));
  }
  const o = hit.overlays[0]!;
  const extent =
    o.type === "spot" || o.stopHz <= o.startHz
      ? frequencyShort(o.startHz)
      : `${frequencyShort(o.startHz)} - ${frequencyShort(o.stopHz)} (${frequencyShort(o.stopHz - o.startHz)} wide)`;
  const lines: Tip["lines"] = [
    { text: o.name, colour: cssColour(o.color) },
    { text: extent },
    { text: `${o.pluginName} · ${o.type}${o.category ? ` · ${o.category}` : ""}`, dim: true },
  ];
  if (o.description) {
    lines.push({ text: o.description });
  }
  lines.push({
    text: `${o.type === "band" ? "click to mark its extent" : "click for its width"} · ctrl-click to hide`,
    dim: true,
  });
  return lines;
}

/** Fits the canvas's backing store to its box at the screen's density. */
function useCanvasSize(canvas: React.RefObject<HTMLCanvasElement | null>): { width: number; height: number } {
  const [size, setSize] = useState({ width: 0, height: 0 });
  useEffect(() => {
    const element = canvas.current;
    if (!element) {
      return;
    }
    const observer = new ResizeObserver(([entry]) => {
      const box = entry!.contentRect;
      setSize({ width: Math.floor(box.width), height: Math.floor(box.height) });
    });
    observer.observe(element);
    return () => observer.disconnect();
  }, [canvas]);
  return size;
}

export function Panel({
  panel,
  fit,
  limits,
  focused,
  active,
  closable,
  actions,
}: {
  panel: PanelView;
  fit: Span;
  limits: Span;
  focused: boolean;
  /** The one the bar's buttons act on, which carries the marker readout. */
  active: boolean;
  closable: boolean;
  actions: PanelActions;
}) {
  const spectrumRef = useRef<HTMLCanvasElement>(null);
  const waterfallRef = useRef<HTMLCanvasElement>(null);
  const rendererRef = useRef<WaterfallRenderer | null>(null);
  const [waterfallError, setWaterfallError] = useState("");
  const spectrumSize = useCanvasSize(spectrumRef);
  const settings = useSelector(viewStore, (v) => v);
  const themeName = settings.themeName;
  const themes = useSelector(themeStore, (t) => t);
  const theme = findTheme(themeName);
  const view = resolveView(panel, fit);
  const [cursorHz, setCursorHz] = useState<number | null>(null);
  const [selection, setSelection] = useState<{ fromHz: number; toHz: number } | null>(null);
  const [picked, setPicked] = useState("");
  const [tip, setTip] = useState<Tip | null>(null);
  const pointer = useRef<{ x: number; y: number } | null>(null);
  const hits = useRef<Hit[]>([]);
  const drag = useRef<Drag | null>(null);
  const pinch = useRef(new Map<number, number>());
  const overlays = useOverlays(view, settings.showBands, settings.showChannels);

  const waterfallMap = themes.colorMaps.find((m) => m.name === (settings.colorMap || theme?.waterfall.colorMap));
  const waterfallLut = useMemo(
    () => bakeColorMap(waterfallMap ?? themes.colorMaps[0]),
    [waterfallMap, themes.colorMaps],
  );
  const fillMapName = theme?.spectrum.fillColorMap;
  const fillLut = useMemo(
    () => bakeColorMap(themes.colorMaps.find((m) => m.name === fillMapName) ?? themes.colorMaps[0]),
    [fillMapName, themes.colorMaps],
  );

  // Everything the draw needs, read by the animation loop without re-renders.
  const sceneNow = { view, panel, settings, overlays, selection, theme, picked, waterfallLut, fillLut };
  const scene = useRef(sceneNow);
  scene.current = sceneNow;
  const dirty = useRef(true);
  dirty.current = true;

  // The waterfall: a renderer of its own, fed every line.
  useEffect(() => {
    const canvas = waterfallRef.current;
    if (!canvas) {
      return;
    }
    try {
      rendererRef.current = new WaterfallRenderer(canvas);
    } catch (error) {
      setWaterfallError(error instanceof Error ? error.message : String(error));
      return;
    }
    const unsubscribe = session.onLine((line: WaterfallLine) => {
      const renderer = rendererRef.current;
      const current = scene.current;
      if (!renderer || current.panel.waterfallPaused) {
        return;
      }
      const depth = Math.max(current.settings.waterfallLines, (canvas.height || 600) * 1.5);
      if (current.settings.layout.mode === "spans" && spanValid(current.panel.segment)) {
        const slice = segmentBins(line.startHz, line.binWidthHz, line.levels.length, current.panel.segment);
        if (slice.count > 0) {
          renderer.push(line.levels.subarray(slice.first, slice.first + slice.count), slice.startHz, slice.stopHz, depth);
        }
      } else {
        renderer.push(line.levels, line.startHz, line.startHz + line.binWidthHz * line.levels.length, depth);
      }
      dirty.current = true;
    });
    return () => {
      unsubscribe();
      rendererRef.current?.dispose();
      rendererRef.current = null;
    };
  }, []);

  useEffect(() => {
    rendererRef.current?.setColourMap(waterfallLut);
    dirty.current = true;
  }, [waterfallLut]);

  // One loop draws both: the spectrum when the traces or the view moved, the
  // waterfall when a line arrived or the view moved.
  useEffect(() => {
    let frame = 0;
    let drawnGeneration = -1;
    const draw = () => {
      frame = requestAnimationFrame(draw);
      const traces = session.traces;
      if (!dirty.current && traces.generation === drawnGeneration) {
        return;
      }
      dirty.current = false;
      drawnGeneration = traces.generation;
      const current = scene.current;
      const spectrum = spectrumRef.current;
      const dpr = window.devicePixelRatio || 1;
      if (spectrum && spectrum.clientWidth > 0) {
        const width = spectrum.clientWidth;
        const height = spectrum.clientHeight;
        if (spectrum.width !== Math.round(width * dpr) || spectrum.height !== Math.round(height * dpr)) {
          spectrum.width = Math.round(width * dpr);
          spectrum.height = Math.round(height * dpr);
        }
        const ctx = spectrum.getContext("2d")!;
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        const spectrumTheme = current.theme?.spectrum ?? {};
        const colour = (key: string, fallback: string) => {
          const value = spectrumTheme[key];
          return typeof value === "string" ? value : fallback;
        };
        const number = (key: string, fallback: number) => {
          const value = spectrumTheme[key];
          return typeof value === "number" ? value : fallback;
        };
        const chrome = current.theme?.chrome ?? {};
        const holds: TraceKind[] = [];
        if (current.settings.showMaxHold) holds.push("maxHold");
        if (current.settings.showMinHold) holds.push("minHold");
        if (current.settings.showAverage) holds.push("average");
        const config = session.latest?.config;
        const readout = config
          ? `RBW ${frequencyShort(config.rbwHz)}   FFT ${config.fftSize}   ${config.gains
              .map(([name, value]) => `${name} ${value.toFixed(0)}`)
              .join("  ")}`
          : "";
        hits.current = drawSpectrum(ctx, width, height, {
          fromHz: current.view.startHz,
          toHz: current.view.stopHz,
          yMinDb: current.panel.yMinDb,
          yMaxDb: current.panel.yMaxDb,
          gradientMinDb: current.panel.gradientMinDb,
          gradientMaxDb: current.panel.gradientMaxDb,
          traces,
          showGrid: current.settings.showGrid,
          fill: current.settings.fill,
          fillAlpha: number("fillAlpha", 0.55),
          fillLut: current.fillLut,
          waterfallLut: current.waterfallLut,
          traceThickness: number("traceThickness", 1.4),
          points: current.settings.autoPoints ? 0 : current.settings.displayPoints,
          holds,
          markers: current.settings.markers.items,
          activeMarkerId: current.settings.markers.activeId,
          overlays: current.overlays,
          contributionAlpha: number("contributionAlpha", 0.18),
          picked: current.picked,
          pointer: pointer.current,
          readout,
          selection: current.selection,
          colours: {
            background: colour("background", "#000"),
            grid: colour("grid", "#2a2f38"),
            axisText: colour("axisText", "#8b919a"),
            border: typeof chrome.border === "string" ? chrome.border : "#3a3f48",
            accent: typeof chrome.accent === "string" ? chrome.accent : "#3d8bfd",
            live: colour("traceLive", "#4ade80"),
            maxHold: colour("traceMaxHold", "#f87171"),
            minHold: colour("traceMinHold", "#60a5fa"),
            average: colour("traceAverage", "#facc15"),
            marker: colour("marker", "#fbbf24"),
            cursor: colour("cursor", "#ffffff"),
            selection: colour("selection", "rgba(61,139,253,0.25)"),
          },
        });
      }
      const waterfall = waterfallRef.current;
      const renderer = rendererRef.current;
      if (waterfall && renderer && waterfall.clientWidth > 0) {
        const width = Math.round(waterfall.clientWidth * dpr);
        const height = Math.round(waterfall.clientHeight * dpr);
        if (waterfall.width !== width || waterfall.height !== height) {
          waterfall.width = width;
          waterfall.height = height;
        }
        const background = parseHex(
          typeof current.theme?.spectrum.background === "string" ? current.theme.spectrum.background : "#000000",
        );
        renderer.draw({
          fromHz: current.view.startHz,
          toHz: current.view.stopHz,
          gradientMinDb: current.panel.gradientMinDb,
          gradientMaxDb: current.panel.gradientMaxDb,
          background,
          peakDetect: current.settings.waterfallPeakDetect,
        });
      }
    };
    frame = requestAnimationFrame(draw);
    return () => cancelAnimationFrame(frame);
  }, []);

  // ---- gestures --------------------------------------------------------------

  const setView = (span: Span | null) => {
    const clamped = span ? clampView(span.startHz, span.stopHz, limits) : null;
    if (clamped) {
      setPanel(panel.id, { viewStartHz: clamped.startHz, viewStopHz: clamped.stopHz });
    }
  };

  const plotFor = (canvas: HTMLCanvasElement) =>
    canvas === spectrumRef.current
      ? plotArea(canvas.clientWidth, canvas.clientHeight)
      : { x: 0, y: 0, width: canvas.clientWidth, height: canvas.clientHeight };

  const localX = (event: { clientX: number }, canvas: HTMLCanvasElement) =>
    event.clientX - canvas.getBoundingClientRect().left;
  const localY = (event: { clientY: number }, canvas: HTMLCanvasElement) =>
    event.clientY - canvas.getBoundingClientRect().top;

  const hitAt = (x: number, y: number) =>
    hits.current.find((hit) => x >= hit.x0 && x <= hit.x1 && y >= hit.y0 && y <= hit.y1) ?? null;

  const onPointerDown = (event: ReactPointerEvent<HTMLCanvasElement>) => {
    const canvas = event.currentTarget;
    canvas.setPointerCapture(event.pointerId);
    setLayout((layout) => ({ ...layout, focusedId: panel.id }));
    pinch.current.set(event.pointerId, event.clientX);
    if (pinch.current.size > 1) {
      drag.current = null;
      return;
    }
    const plot = plotFor(canvas);
    const x = localX(event, canvas);
    const y = localY(event, canvas);
    const hz = hzAt(plot, view.startHz, view.stopHz, x);
    const base = { pointerId: event.pointerId, startX: x, startHz: hz, view, markerId: 0, moved: false, anchorY: y };

    if (canvas === spectrumRef.current) {
      const handle = handleAt(plot, panel, x, y);
      if (handle) {
        const anchorDb = {
          yMax: panel.yMaxDb,
          yMin: panel.yMinDb,
          gradientMax: panel.gradientMaxDb,
          gradientMin: panel.gradientMinDb,
        }[handle];
        drag.current = { ...base, kind: handle, anchorDb };
        return;
      }
      // A chip or a bar takes the click; the plot around them puts the pick away.
      const hit = hitAt(x, y);
      if (hit) {
        if (!hit.more && (event.ctrlKey || event.metaKey)) {
          hideContribution(hit.overlays[0]!);
        } else if (!hit.more) {
          const key = overlayKey(hit.overlays[0]!);
          setPicked((current) => (current === key ? "" : key));
        }
        drag.current = null;
        return;
      }
      setPicked("");
    }

    const reach = (spanWidth(view) / plot.width) * 8;
    const marker = nearestMarker(viewStore.state.markers, hz, reach);
    const kind: Drag["kind"] = marker
      ? "marker"
      : event.shiftKey && (event.ctrlKey || event.metaKey)
        ? "sweep"
        : event.shiftKey
          ? "zoom"
          : "pan";
    if (marker) {
      setMarkers((set) => ({ ...set, activeId: marker.id }));
    }
    drag.current = { ...base, kind, markerId: marker?.id ?? 0, anchorDb: 0 };
  };

  const onPointerMove = (event: ReactPointerEvent<HTMLCanvasElement>) => {
    const canvas = event.currentTarget;
    const plot = plotFor(canvas);
    const x = localX(event, canvas);
    const y = localY(event, canvas);
    const hz = hzAt(plot, view.startHz, view.stopHz, x);
    setCursorHz(hz);

    if (canvas === spectrumRef.current) {
      pointer.current = { x, y };
      dirty.current = true;
      const hit = drag.current ? null : hitAt(x, y);
      setTip(hit ? { x, y, lines: describeHit(hit) } : null);
      const handle = drag.current?.kind ?? (hit ? null : handleAt(plot, panel, x, y));
      canvas.style.cursor =
        handle === "yMax" || handle === "yMin" || handle === "gradientMax" || handle === "gradientMin"
          ? "ns-resize"
          : hit
            ? "pointer"
            : "";
    }

    // The scale's handles: travelled since the grab, not wherever the
    // pointer is now, so a level moves with it rather than jumping to it.
    const held = drag.current;
    if (held && held.pointerId === event.pointerId) {
      const { yMinDb, yMaxDb, gradientMinDb, gradientMaxDb } = panel;
      const db = held.anchorDb + dbAtY(plot, yMinDb, yMaxDb, y) - dbAtY(plot, yMinDb, yMaxDb, held.anchorY);
      const clamp = (value: number, lo: number, hi: number) => Math.min(Math.max(value, lo), hi);
      switch (held.kind) {
        case "yMax":
          setPanel(panel.id, { yMaxDb: clamp(db, yMinDb + 5, 10) });
          return;
        case "yMin":
          setPanel(panel.id, { yMinDb: clamp(db, -150, yMaxDb - 5) });
          return;
        case "gradientMax":
          setPanel(panel.id, { gradientMaxDb: clamp(db, gradientMinDb + 1, yMaxDb) });
          return;
        case "gradientMin":
          setPanel(panel.id, { gradientMinDb: clamp(db, yMinDb, gradientMaxDb - 1) });
          return;
        default:
          break;
      }
    }

    // Two fingers: the span between them stays under them.
    if (pinch.current.size === 2 && pinch.current.has(event.pointerId)) {
      const before = [...pinch.current.values()];
      pinch.current.set(event.pointerId, event.clientX);
      const after = [...pinch.current.values()];
      const was = Math.abs(before[0]! - before[1]!);
      const now = Math.abs(after[0]! - after[1]!);
      if (was > 10 && now > 10) {
        const left = canvas.getBoundingClientRect().left;
        const anchor = hzAt(plot, view.startHz, view.stopHz, (after[0]! + after[1]!) / 2 - left);
        setView(zoomAbout(view, anchor, was / now));
      }
      return;
    }

    const current = drag.current;
    if (!current || current.pointerId !== event.pointerId) {
      return;
    }
    if (Math.abs(x - current.startX) > 3) {
      current.moved = true;
    }
    if (!current.moved) {
      return;
    }
    const hzPerPixel = spanWidth(current.view) / plot.width;
    switch (current.kind) {
      case "pan": {
        const shift = -(x - current.startX) * hzPerPixel;
        setView({ startHz: current.view.startHz + shift, stopHz: current.view.stopHz + shift });
        break;
      }
      case "zoom":
      case "sweep":
        setSelection({ fromHz: current.startHz, toHz: hz });
        break;
      case "marker":
        setMarkers((set) => updateMarker(set, current.markerId, { frequencyHz: hz }));
        break;
      default:
        break;
    }
  };

  const onPointerUp = (event: ReactPointerEvent<HTMLCanvasElement>) => {
    pinch.current.delete(event.pointerId);
    const current = drag.current;
    drag.current = null;
    setSelection(null);
    if (!current || !current.moved || !selection) {
      return;
    }
    const from = Math.min(selection.fromHz, selection.toHz);
    const to = Math.max(selection.fromHz, selection.toHz);
    if (to - from <= 0) {
      return;
    }
    if (current.kind === "zoom") {
      setView({ startHz: from, stopHz: to });
    } else if (current.kind === "sweep") {
      actions.sweep(from, to);
    }
  };

  const onWheel = (event: React.WheelEvent<HTMLCanvasElement>) => {
    const canvas = event.currentTarget;
    const plot = plotFor(canvas);
    const anchor = hzAt(plot, view.startHz, view.stopHz, localX(event, canvas));
    // A trackpad pinch arrives as a wheel with ctrl held, finer-grained.
    const factor = Math.exp(event.deltaY * (event.ctrlKey ? 0.01 : 0.0015));
    setView(zoomAbout(view, anchor, factor));
  };

  const onDoubleClick = (event: React.MouseEvent<HTMLCanvasElement>) => {
    const canvas = event.currentTarget;
    const hz = hzAt(plotFor(canvas), view.startHz, view.stopHz, localX(event, canvas));
    setMarkers((set) => addMarker(set, hz));
  };

  const canvasHandlers = {
    onPointerDown,
    onPointerMove,
    onPointerUp,
    onPointerCancel: onPointerUp,
    onPointerLeave: () => {
      setCursorHz(null);
      setTip(null);
      pointer.current = null;
      dirty.current = true;
    },
    onWheel,
    onDoubleClick,
  };

  // The divider between the spectrum and the waterfall.
  const bodyRef = useRef<HTMLDivElement>(null);
  const onSplitter = (event: ReactPointerEvent<HTMLDivElement>) => {
    const body = bodyRef.current;
    if (!body) {
      return;
    }
    event.currentTarget.setPointerCapture(event.pointerId);
    const move = (e: PointerEvent) => {
      const box = body.getBoundingClientRect();
      const fraction = 1 - (e.clientY - box.top) / box.height;
      setPanel(panel.id, { waterfallFraction: Math.min(Math.max(fraction, 0.05), 0.95) });
    };
    const up = () => {
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
  };

  const spectrumPlot = plotArea(spectrumSize.width, spectrumSize.height);
  return (
    <div
      className={`flex h-full w-full flex-col overflow-hidden border ${focused ? "border-accent/60" : "border-transparent"} bg-plot`}
    >
      <div className="flex h-7 shrink-0 items-center gap-3 bg-header px-2 text-xs">
        <span className="rounded bg-button px-1.5 text-dim">{panel.id}</span>
        <span className="flex min-w-0 flex-1 justify-center gap-8 text-[13px]">
          <span className="text-dim">
            Start <span className="text-text tabular-nums">{frequency(view.startHz)}</span>
          </span>
          <span className="hidden text-dim sm:inline">
            Center <span className="text-text tabular-nums">{frequency(spanCentre(view))}</span>
          </span>
          <span className="hidden text-dim md:inline">
            Span <span className="text-text tabular-nums">{frequency(spanWidth(view))}</span>
          </span>
          <span className="text-dim">
            Stop <span className="text-text tabular-nums">{frequency(view.stopHz)}</span>
          </span>
        </span>
        {cursorHz !== null && (
          <span className="hidden text-dim tabular-nums lg:inline">
            {frequency(cursorHz)} <span className="text-text">{level(session.traces.levelAt(cursorHz))}</span>
          </span>
        )}
        <span className="flex gap-1">
          <button
            className="rounded px-1.5 hover:bg-button-hover"
            title={panel.waterfallPaused ? "Resume the waterfall" : "Pause the waterfall"}
            onClick={() => setPanel(panel.id, { waterfallPaused: !panel.waterfallPaused })}
          >
            <Icon path={panel.waterfallPaused ? icon.play : icon.pause} />
          </button>
          {closable && (
            <button
              className="rounded px-1.5 hover:bg-button-hover"
              title="Close this panel"
              onClick={() => setLayout((layout) => removePanel(layout, panel.id))}
            >
              <Icon path={icon.close} />
            </button>
          )}
        </span>
      </div>
      <div ref={bodyRef} className="flex min-h-0 flex-1 flex-col">
        <div style={{ flexGrow: 1 - panel.waterfallFraction, flexBasis: 0 }} className="min-h-0">
          <div className="relative h-full w-full">
            <canvas ref={spectrumRef} className="h-full w-full" {...canvasHandlers} />
            {active && <MarkerReadout overlays={overlays} plot={spectrumPlot} />}
            {tip && (
              <div
                className="pointer-events-none absolute z-10 max-w-80 rounded border border-border bg-panel px-2 py-1 text-xs shadow-lg"
                style={{
                  left: Math.min(tip.x + 14, Math.max(spectrumSize.width - 320, 0)),
                  top: tip.y + 16,
                }}
              >
                {tip.lines.map((line, i) => (
                  <div
                    key={i}
                    className={line.dim ? "text-dim" : ""}
                    style={line.colour ? { color: line.colour } : undefined}
                  >
                    {line.text}
                  </div>
                ))}
              </div>
            )}
          </div>
        </div>
        <div className="h-1.5 shrink-0 cursor-row-resize hover:bg-accent/50" onPointerDown={onSplitter} />
        <div style={{ flexGrow: panel.waterfallFraction, flexBasis: 0 }} className="relative min-h-0">
          {waterfallError ? (
            <div className="p-3 text-xs text-warning">Waterfall unavailable: {waterfallError}</div>
          ) : (
            <canvas
              ref={waterfallRef}
              className="absolute top-0 h-full"
              style={{ left: kAxisLeft, width: Math.max(spectrumPlot.width, 1) }}
              {...canvasHandlers}
            />
          )}
        </div>
      </div>
    </div>
  );
}
