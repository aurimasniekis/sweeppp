// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";
import { type PointerEvent as ReactPointerEvent, useEffect, useRef, useState } from "react";

import {
  arrangementFor,
  arrangePanels,
  type Divider,
  dividers,
  groupSegments,
  moveDivider,
  rebindSpans,
  type Span,
  spanValid,
} from "../model/layout";
import { NoticeKind, section } from "../protocol/messages";
import { session, sessionStore } from "../state/session";
import { setLayout, viewStore } from "../state/view";
import { OverviewStrip } from "./OverviewStrip";
import { Panel, type PanelActions } from "./Panel";
import { toast } from "./toasts";

/** Between panels: wide enough to grab. */
const kGap = 6;

/** The plan's segments, as spans. */
export function planSegments(): Span[] {
  return session
    .section(section.plan)
    .getHashes("segments")
    .map((s) => ({ startHz: s.getFloat("startHz"), stopHz: s.getFloat("stopHz") }))
    .filter(spanValid);
}

/** What is swept: the grid the frames arrive on, or the plan before any have. */
export function sweptSpan(): Span {
  const traces = session.traces;
  if (traces.binCount > 0) {
    return { startHz: traces.startHz, stopHz: traces.stopHz };
  }
  const segments = planSegments();
  if (segments.length > 0) {
    return {
      startHz: Math.min(...segments.map((s) => s.startHz)),
      stopHz: Math.max(...segments.map((s) => s.stopHz)),
    };
  }
  return { startHz: 0, stopHz: 1 };
}

export function PanelGrid({ actions }: { actions: PanelActions }) {
  const layout = useSelector(viewStore, (v) => v.layout);
  useSelector(sessionStore, (s) => s.sectionsVersion);
  const area = useRef<HTMLDivElement>(null);
  const [size, setSize] = useState({ width: 0, height: 0 });
  // The grid's span changes as frames arrive; follow it a few times a second.
  const [, setTick] = useState(0);
  useEffect(() => {
    const timer = window.setInterval(() => setTick((t) => t + 1), 500);
    return () => window.clearInterval(timer);
  }, []);

  useEffect(() => {
    const element = area.current;
    if (!element) {
      return;
    }
    const observer = new ResizeObserver(([entry]) =>
      setSize({ width: entry!.contentRect.width, height: entry!.contentRect.height }),
    );
    observer.observe(element);
    return () => observer.disconnect();
  }, []);

  // Spans: panels follow the plan -- each keeps the segment it overlaps, a
  // merged-away one closes, a new segment gets a panel.
  const segments = planSegments();
  const segmentKey = segments.map((s) => `${s.startHz}-${s.stopHz}`).join(",");
  useEffect(() => {
    if (layout.mode !== "spans" || segments.length === 0) {
      return;
    }
    const { layout: next, dropped } = rebindSpans(viewStore.state.layout, segments);
    if (next !== viewStore.state.layout && JSON.stringify(next) !== JSON.stringify(viewStore.state.layout)) {
      setLayout(() => next);
    }
    if (dropped > 0) {
      toast(NoticeKind.Info, dropped === 1 ? "Segments merged; one panel closed" : `Segments merged; ${dropped} panels closed`);
    }
  }, [layout.mode, segmentKey]);

  // A divider dragged: the share of the axis the pointer travelled.
  const dragging = useRef<{ divider: Divider; last: number } | null>(null);
  const onDividerDown = (divider: Divider) => (event: ReactPointerEvent<HTMLDivElement>) => {
    event.currentTarget.setPointerCapture(event.pointerId);
    dragging.current = { divider, last: divider.vertical ? event.clientX : event.clientY };
  };
  const onDividerMove = (event: ReactPointerEvent<HTMLDivElement>) => {
    const drag = dragging.current;
    if (!drag) {
      return;
    }
    const at = drag.divider.vertical ? event.clientX : event.clientY;
    const travel = at - drag.last;
    drag.last = at;
    const length = drag.divider.vertical ? size.width : size.height;
    setLayout((current) => ({ ...current, splits: moveDivider(current.splits, drag.divider, travel, length, kGap) }));
  };

  const swept = sweptSpan();
  const mirrorWindows = groupSegments(segments, layout.panels.length);
  const arrangement = arrangementFor(layout.panels.length, layout.rowsForTwo);
  const areaRect = { x: 0, y: 0, width: size.width, height: size.height };
  const gap = layout.panels.length > 1 ? kGap : 0;
  const rects = arrangePanels(arrangement, areaRect, layout.splits, gap);

  return (
    <div className="flex min-h-0 flex-1 flex-col bg-plot">
      {layout.mode === "spans" && layout.overview && <OverviewStrip segments={segments} />}
      <div ref={area} data-snapshot-root className="relative min-h-0 flex-1 bg-plot">
        {layout.panels.map((panel, i) => {
          const rect = rects[i];
          if (!rect) {
            return null;
          }
          const fit =
            layout.mode === "spans" && spanValid(panel.segment)
              ? panel.segment
              : layout.panels.length > 1 && mirrorWindows.length === layout.panels.length
                ? mirrorWindows[i]!
                : swept;
          return (
            <div
              key={panel.id}
              className="absolute"
              style={{ left: rect.x, top: rect.y, width: rect.width, height: rect.height }}
            >
              <Panel
                panel={panel}
                fit={fit}
                limits={swept}
                focused={panel.id === layout.focusedId && layout.panels.length > 1}
                active={panel.id === layout.focusedId}
                closable={layout.panels.length > 1}
                actions={actions}
              />
            </div>
          );
        })}
        {dividers(arrangement, rects, areaRect, gap).map((divider, i) => (
          <div
            key={i}
            className={`absolute z-10 hover:bg-accent/50 ${divider.vertical ? "cursor-col-resize" : "cursor-row-resize"}`}
            style={{ left: divider.rect.x, top: divider.rect.y, width: divider.rect.width, height: divider.rect.height }}
            onPointerDown={onDividerDown(divider)}
            onPointerMove={onDividerMove}
            onPointerUp={() => (dragging.current = null)}
            onPointerCancel={() => (dragging.current = null)}
          />
        ))}
      </div>
    </div>
  );
}
