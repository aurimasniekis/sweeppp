// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";
import { useState } from "react";

import { focused, resolveView, spanCentre } from "../../model/layout";
import { addMarker, emptyMarkers, type MarkerSet, removeMarker, updateMarker } from "../../model/markers";
import { frequency, level, parseFrequency } from "../../render/format";
import { session } from "../../state/session";
import { setMarkers, viewStore } from "../../state/view";
import { sweptSpan } from "../PanelGrid";
import { Icon } from "../Icon";
import { icon } from "../icons";

const kPresetKey = "sweeppp.markerPresets";

function loadPresets(): Record<string, number[]> {
  try {
    return JSON.parse(localStorage.getItem(kPresetKey) ?? "{}") as Record<string, number[]>;
  } catch {
    return {};
  }
}

/** Markers and what is under them; presets kept in this browser. */
export function MarkersPopover() {
  const markers = useSelector(viewStore, (v) => v.markers);
  const layout = useSelector(viewStore, (v) => v.layout);
  const [presets, setPresets] = useState(loadPresets);
  const [presetName, setPresetName] = useState("");
  const centre = spanCentre(resolveView(focused(layout), sweptSpan()));

  const savePresets = (next: Record<string, number[]>) => {
    setPresets(next);
    try {
      localStorage.setItem(kPresetKey, JSON.stringify(next));
    } catch {
      // Not remembered, that is all.
    }
  };

  const peakSearch = () => {
    const view = resolveView(focused(layout), sweptSpan());
    const peak = session.traces.peakIn(view.startHz, view.stopHz);
    if (peak) {
      setMarkers((set) => {
        const active = set.items.find((m) => m.id === set.activeId);
        return active ? updateMarker(set, active.id, { frequencyHz: peak.hz }) : addMarker(set, peak.hz);
      });
    }
  };

  return (
    <div>
      <div className="flex flex-wrap gap-1">
        <button className="btn" onClick={() => setMarkers((set) => addMarker(set, centre))}>
          Add
        </button>
        <button className="btn" onClick={peakSearch} title="Put the selected marker on the strongest signal in view">
          Peak
        </button>
        <button className="btn" disabled={markers.items.length === 0} onClick={() => setMarkers(() => emptyMarkers())}>
          Clear all
        </button>
      </div>
      <p className="caption mt-1">Double-click the plot to add one; drag a marker to move it.</p>
      <table className="mt-2 w-full text-xs">
        <thead className="text-dim">
          <tr>
            <th className="text-left font-normal">#</th>
            <th className="text-left font-normal">Frequency</th>
            <th className="text-right font-normal">Level</th>
            <th className="text-right font-normal">Δ</th>
            <th />
          </tr>
        </thead>
        <tbody>
          {markers.items.map((m, i) => {
            const reference = markers.items[0];
            return (
              <tr
                key={m.id}
                className={m.id === markers.activeId ? "text-accent" : ""}
                onClick={() => setMarkers((set) => ({ ...set, activeId: m.id }))}
              >
                <td>M{m.id}</td>
                <td>
                  <input
                    className="w-28 bg-transparent tabular-nums outline-none focus:text-text"
                    defaultValue={frequency(m.frequencyHz, 6)}
                    key={m.frequencyHz}
                    onBlur={(e) => {
                      const hz = parseFrequency(e.target.value.replace(/\s*Hz$/i, ""));
                      if (hz !== null) {
                        setMarkers((set) => updateMarker(set, m.id, { frequencyHz: hz }));
                      }
                    }}
                  />
                </td>
                <td className="text-right tabular-nums">{level(m.levelDb)}</td>
                <td className="text-right tabular-nums text-dim">
                  {i > 0 && reference ? `${frequency(m.frequencyHz - reference.frequencyHz)}` : ""}
                </td>
                <td className="text-right">
                  <button
                    className="px-1"
                    title={m.peakLocked ? "Stop following the peak" : "Follow the strongest signal nearby"}
                    onClick={() => setMarkers((set) => updateMarker(set, m.id, { peakLocked: !m.peakLocked }))}
                  >
                    {m.peakLocked ? "◆" : "◇"}
                  </button>
                  <button className="px-1" onClick={() => setMarkers((set) => removeMarker(set, m.id))}>
                    <Icon path={icon.close} />
                  </button>
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
      <div className="section-title">Presets</div>
      <div className="flex gap-1">
        <input
          className="field"
          placeholder="Name"
          value={presetName}
          onChange={(e) => setPresetName(e.target.value)}
        />
        <button
          className="btn"
          disabled={!presetName || markers.items.length === 0}
          onClick={() => savePresets({ ...presets, [presetName]: markers.items.map((m) => m.frequencyHz) })}
        >
          Save
        </button>
      </div>
      {Object.entries(presets).map(([name, frequencies]) => (
        <div key={name} className="row">
          <button
            className="truncate text-left hover:text-accent"
            onClick={() =>
              setMarkers(() => frequencies.reduce<MarkerSet>((set, hz) => addMarker(set, hz), emptyMarkers()))
            }
          >
            {name} <span className="text-dim">({frequencies.length})</span>
          </button>
          <button
            className="px-1 text-dim"
            onClick={() => {
              const next = { ...presets };
              delete next[name];
              savePresets(next);
            }}
          >
            <Icon path={icon.close} />
          </button>
        </div>
      ))}
    </div>
  );
}
