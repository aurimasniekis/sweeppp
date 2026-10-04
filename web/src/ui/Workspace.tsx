// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";
import { useEffect, useState } from "react";

import { focused, resolveView, spanWidth } from "../model/layout";
import { refreshMarker, removeMarker } from "../model/markers";
import { kMinLinkBins } from "../protocol/messages";
import { forgetLinkResolution, setLinkResolution } from "../state/actions";
import { readInstrument, sweepRange } from "../state/instrument";
import { session, sessionStore } from "../state/session";
import { refreshContributors } from "../state/contributors";
import { applyTheme, findTheme, themeStore } from "../state/theme";
import { setMarkers, setView, viewStore } from "../state/view";
import { ControlBanner } from "./ControlBanner";
import { HistoryPicker, HistoryView } from "./HistoryView";
import { useOverlayGeneration } from "./overlays";
import { PanelGrid, sweptSpan } from "./PanelGrid";
import { AnalysisPanel } from "./panels/AnalysisPanel";
import { DevicePanel } from "./panels/DevicePanel";
import { MenuPanel } from "./panels/MenuPanel";
import { PerformancePanel } from "./panels/PerformancePanel";
import { StatusBar } from "./StatusBar";
import { toast, Toasts } from "./toasts";
import { TopBar } from "./TopBar";

export function Workspace({ onLoggedOut }: { onLoggedOut: () => void }) {
  const status = useSelector(sessionStore, (s) => s);
  const themeName = useSelector(viewStore, (v) => v.themeName);
  const themesLoaded = useSelector(themeStore, (t) => t.loaded);
  const [history, setHistory] = useState<string | null>(null);
  const overlayGeneration = useOverlayGeneration();

  useEffect(() => {
    if (status.status === "live") {
      refreshContributors(overlayGeneration);
    }
  }, [overlayGeneration, status.status]);

  useEffect(() => {
    session.start();
    const unsubscribe = session.onNotice((notice) => toast(notice.kind, notice.text));
    return () => {
      unsubscribe();
      session.stop();
    };
  }, []);

  useEffect(() => {
    if (themesLoaded) {
      applyTheme(findTheme(themeName));
    }
  }, [themeName, themesLoaded]);

  useEffect(() => {
    if (status.status === "unauthorised") {
      onLoggedOut();
    }
    if (status.status === "live") {
      forgetLinkResolution();
    }
  }, [status.status, onLoggedOut]);

  // Markers read their levels off the live trace, and peak-locked ones move,
  // a few times a second.
  useEffect(() => {
    const timer = window.setInterval(() => {
      const view = viewStore.state;
      if (view.markers.items.length === 0) {
        return;
      }
      const panel = focused(view.layout);
      const reach = spanWidth(resolveView(panel, sweptSpan())) / 100;
      setMarkers((set) => ({ ...set, items: set.items.map((m) => refreshMarker(m, session.traces, reach)) }));
    }, 200);
    return () => window.clearInterval(timer);
  }, []);

  // Network resolution: twice the widest panel's pixels, unless chosen.
  useEffect(() => {
    const timer = window.setInterval(() => {
      if (sessionStore.state.status !== "live") {
        return;
      }
      // Chosen: a count, or -1 for whole frames, which the server spells 0.
      const chosen = viewStore.state.linkBins;
      if (chosen !== 0) {
        setLinkResolution(chosen < 0 ? 0 : chosen);
        return;
      }
      const screen = window.innerWidth * window.devicePixelRatio;
      // Zoomed in, the network has to carry the finer bins being looked at.
      const zoomed = Math.max(
        ...viewStore.state.layout.panels.map((p) => spanWidth(sweptSpan()) / Math.max(spanWidth(resolveView(p, sweptSpan())), 1)),
        1,
      );
      const wanted = Math.round(Math.min(screen * 2 * zoomed, 1 << 20) / 1024) * 1024;
      setLinkResolution(Math.max(wanted, kMinLinkBins));
    }, 1000);
    return () => window.clearInterval(timer);
  }, []);

  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      const target = event.target as HTMLElement;
      if (target.tagName === "INPUT" || target.tagName === "TEXTAREA" || event.ctrlKey || event.metaKey) {
        return;
      }
      if (event.key === "Backspace" || event.key === "Delete") {
        setMarkers((set) => (set.activeId ? removeMarker(set, set.activeId) : set));
      } else if (event.key === "b") {
        setView({ showBands: !viewStore.state.showBands });
      } else if (event.key === "c") {
        setView({ showChannels: !viewStore.state.showChannels });
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, []);

  const actions = {
    sweep: (fromHz: number, toHz: number) => {
      const plan = readInstrument().plan;
      sweepRange({ ...plan, segments: [{ startHz: fromHz, stopHz: toHz, dwellSeconds: 0 }] });
    },
  };

  if (status.status === "refused") {
    return (
      <div className="mx-auto mt-24 max-w-md rounded-lg border border-border bg-panel p-6">
        <h1 className="mb-2 text-lg font-semibold">Not connected</h1>
        <p>{status.error}</p>
        <button className="btn mt-4" onClick={() => location.reload()}>
          Try again
        </button>
      </div>
    );
  }

  if (history) {
    return (
      <>
        <HistoryView name={history} onClose={() => setHistory(null)} />
        <Toasts />
      </>
    );
  }

  return (
    <div className="flex h-full flex-col">
      <TopBar panels={{ device: <DevicePanel />, analysis: <AnalysisPanel />, menu: <MenuPanel /> }} />
      <ControlBanner />
      {status.status === "reconnecting" && (
        <div className="flex shrink-0 items-center gap-3 border-b border-border bg-warning/15 px-3 py-1.5">
          <span>
            Lost the link{status.error ? `: ${status.error}` : ""}. Trying again (attempt {status.attempt}) in{" "}
            {status.retryInSec} s.
          </span>
          <button className="btn" onClick={() => session.retryNow()}>
            Now
          </button>
        </div>
      )}
      {status.status === "connecting" && <div className="px-3 py-1.5 text-dim">Connecting…</div>}
      <PanelGrid actions={actions} />
      <StatusBar
        panels={{ performance: <PerformancePanel />, history: <HistoryPicker onOpen={setHistory} /> }}
      />
      <Toasts />
    </div>
  );
}
