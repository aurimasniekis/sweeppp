// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { Store } from "@tanstack/react-store";

import { defaultLayout, type PanelLayout, type PanelView } from "../model/layout";
import { emptyMarkers, type MarkerSet } from "../model/markers";
import type { FillStyle } from "../render/spectrum";

/** How this page draws, kept in this browser: the desktop's ViewSettings. */
export interface ViewState {
  layout: PanelLayout;
  autoPoints: boolean;
  displayPoints: number;
  smoothing: number;
  showMaxHold: boolean;
  maxHoldDecayDbPerSec: number;
  showMinHold: boolean;
  showAverage: boolean;
  averageWindow: number;
  /** Under the live trace: its own colour, or coloured by level. */
  fill: FillStyle;
  showGrid: boolean;
  showBands: boolean;
  showChannels: boolean;
  waterfallPeakDetect: boolean;
  waterfallLines: number;
  markers: MarkerSet;
  markerReadout: number;
  themeName: string;
  colorMap: string;
  /** Bins a frame is reduced to before it crosses the network: zero follows
   * the screen, -1 sends frames whole. */
  linkBins: number;
}

export function defaultView(): ViewState {
  return {
    layout: defaultLayout(),
    autoPoints: true,
    displayPoints: 1024,
    smoothing: 0,
    showMaxHold: false,
    maxHoldDecayDbPerSec: 0,
    showMinHold: false,
    showAverage: false,
    averageWindow: 16,
    fill: "solid",
    showGrid: true,
    showBands: true,
    showChannels: true,
    waterfallPeakDetect: false,
    waterfallLines: 2048,
    markers: emptyMarkers(),
    markerReadout: 8,
    themeName: "Dark",
    colorMap: "",
    linkBins: 0,
  };
}

const kStorageKey = "sweeppp.view";

/** What was saved, laid over the defaults: a field added since keeps its
 * default rather than coming back undefined. */
function load(): ViewState {
  const defaults = defaultView();
  try {
    const saved = localStorage.getItem(kStorageKey);
    if (!saved) {
      return defaults;
    }
    const parsed = JSON.parse(saved) as Partial<ViewState>;
    return {
      ...defaults,
      ...parsed,
      layout: { ...defaults.layout, ...parsed.layout },
      markers: { ...defaults.markers, ...parsed.markers },
    };
  } catch {
    return defaults;
  }
}

export const viewStore = new Store<ViewState>(load());

let saveTimer: number | undefined;
viewStore.subscribe((state) => {
  window.clearTimeout(saveTimer);
  saveTimer = window.setTimeout(() => {
    try {
      localStorage.setItem(kStorageKey, JSON.stringify(state));
    } catch {
      // A private window or a full quota: the view simply is not remembered.
    }
  }, 300);
});

export function setView(change: Partial<ViewState>): void {
  viewStore.setState((state) => ({ ...state, ...change }));
}

export function setLayout(change: (layout: PanelLayout) => PanelLayout): void {
  viewStore.setState((state) => ({ ...state, layout: change(state.layout) }));
}

export function setPanel(id: number, change: Partial<PanelView>): void {
  setLayout((layout) => ({
    ...layout,
    panels: layout.panels.map((p) => (p.id === id ? { ...p, ...change } : p)),
  }));
}

export function setMarkers(change: (markers: MarkerSet) => MarkerSet): void {
  viewStore.setState((state) => ({ ...state, markers: change(state.markers) }));
}
