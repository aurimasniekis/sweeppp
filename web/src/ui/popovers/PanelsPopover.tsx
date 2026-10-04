// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";

import {
  addPanel,
  defaultLayout,
  defaultPanel,
  enterMode,
  fitToRanges,
  focused,
  kMaxPanels,
  type PanelLayout,
} from "../../model/layout";
import { useInstrument } from "../../state/instrument";
import { planSegments } from "../PanelGrid";
import { setLayout, viewStore } from "../../state/view";
import { Segmented, Switch } from "../controls";
import { Icon } from "../Icon";
import { icon } from "../icons";

/** The arrangements the desktop offers, with its glyphs. */
const kArrangements = [
  { count: 1, rows: false, glyph: icon.layoutSingle, title: "One panel" },
  { count: 2, rows: false, glyph: icon.layoutColumns, title: "Two, side by side" },
  { count: 2, rows: true, glyph: icon.layoutRows, title: "Two, stacked" },
  { count: 3, rows: false, glyph: icon.layoutThree, title: "Three: one wide, two stacked" },
  { count: 4, rows: false, glyph: icon.layoutGrid, title: "Four, in a grid" },
  { count: 6, rows: false, glyph: icon.layoutSix, title: "Six: three across, two down" },
  { count: 9, rows: false, glyph: icon.layoutNine, title: "Nine: three by three" },
];

/** How many panels, laid out how, and whether they mirror or split the plan. */
export function PanelsPopover() {
  const layout = useSelector(viewStore, (v) => v.layout);
  const sweeping = useInstrument().run.sweeping;
  const setCount = (count: number) =>
    setLayout((current) => {
      let next: PanelLayout = current;
      while (next.panels.length < count) {
        next = addPanel(next, { ...focused(next), viewStartHz: 0, viewStopHz: 0, segment: defaultPanel(0).segment });
      }
      if (next.panels.length > count) {
        const panels = next.panels.slice(0, count);
        next = {
          ...next,
          panels,
          focusedId: panels.some((p) => p.id === next.focusedId) ? next.focusedId : panels[0]!.id,
        };
      }
      return next;
    });
  return (
    <div>
      <div className="section-title">Layout</div>
      <div className="flex flex-wrap gap-1">
        {kArrangements.map((a) => {
          const selected = layout.panels.length === a.count && (a.count !== 2 || layout.rowsForTwo === a.rows);
          return (
            <button
              key={a.title}
              className={`btn-icon ${selected ? "border-accent bg-accent/25" : ""}`}
              title={a.title}
              disabled={a.count > kMaxPanels}
              onClick={() => {
                setCount(a.count);
                if (a.count === 2) {
                  setLayout((l) => ({ ...l, rowsForTwo: a.rows }));
                }
              }}
            >
              <Icon path={a.glyph} />
            </button>
          );
        })}
      </div>
      <div className="section-title">Panels show</div>
      <Segmented
        value={layout.mode}
        choices={[
          { value: "mirror", label: "Mirror" },
          { value: "spans", label: "Spans" },
        ]}
        onChange={(mode) => setLayout((l) => enterMode(l, mode, planSegments()))}
      />
      <p className="caption mt-2">
        {layout.mode === "mirror"
          ? "Every panel shows the sweep, each zoomed on its own."
          : "One panel per range of the plan; add ranges in the range popup or shift+drag on the strip."}
      </p>
      {layout.mode === "mirror" && (
        <button
          className="btn mt-1 w-full"
          disabled={!sweeping || planSegments().length === 0}
          title="Zoom each panel onto a swept range, lowest first. More ranges than panels: the closest share one."
          onClick={() => setLayout((l) => fitToRanges(l, planSegments()))}
        >
          Fit to ranges
        </button>
      )}
      <div className="mt-2 flex items-center justify-between gap-2">
        <Switch
          label="Overview strip"
          checked={layout.overview}
          disabled={layout.mode !== "spans"}
          onChange={(on) => setLayout((l) => ({ ...l, overview: on }))}
        />
        <button
          className="btn"
          title="Even out the splits between panels and inside each"
          onClick={() =>
            setLayout((l) => ({
              ...l,
              splits: defaultLayout().splits,
              panels: l.panels.map((p) => ({ ...p, waterfallFraction: defaultPanel(0).waterfallFraction })),
            }))
          }
        >
          Reset splits
        </button>
      </div>
    </div>
  );
}
