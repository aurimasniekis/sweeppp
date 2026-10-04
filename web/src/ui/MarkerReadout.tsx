// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";

import { frequency, level } from "../render/format";
import type { Overlay, Plot } from "../render/spectrum";
import { session } from "../state/session";
import { viewStore } from "../state/view";

/** Where the card sits, by the readout setting: off, then the eight places
 * around the plot. */
const kAnchors: [number, number][] = [
  [0, 0],
  [0, 0],
  [0.5, 0],
  [1, 0],
  [0, 0.5],
  [1, 0.5],
  [0, 1],
  [0.5, 1],
  [1, 1],
];

/** The selected marker read out on the plot: where it is, what is there, and
 * against the marker before it, the span between them and its peak. */
export function MarkerReadout({ overlays, plot }: { overlays: Overlay[]; plot: Plot }) {
  const markers = useSelector(viewStore, (v) => v.markers);
  const place = useSelector(viewStore, (v) => v.markerReadout);
  const selected = markers.items.find((m) => m.id === markers.activeId);
  if (place <= 0 || !selected || !selected.visible) {
    return null;
  }
  const index = markers.items.indexOf(selected);
  let other = null;
  for (let step = 1; step < markers.items.length; ++step) {
    const candidate = markers.items[(index - step + markers.items.length) % markers.items.length]!;
    if (candidate.visible) {
      other = candidate;
      break;
    }
  }
  const what = overlays.find((o) => o.startHz <= selected.frequencyHz && o.stopHz >= selected.frequencyHz);
  const [ax, ay] = kAnchors[Math.min(place, 8)]!;
  const rows: [string, string][] = [];
  if (other) {
    const start = Math.min(selected.frequencyHz, other.frequencyHz);
    const stop = Math.max(selected.frequencyHz, other.frequencyHz);
    const peak = session.traces.peakIn(start, stop);
    const measured = selected.levelDb > -190 && other.levelDb > -190;
    rows.push(
      ["Start", frequency(start)],
      ["End", frequency(stop)],
      ["Centre", frequency((start + stop) / 2)],
      ["Span", frequency(stop - start)],
      ["Delta", measured ? `${(selected.levelDb - other.levelDb).toFixed(1)} dB` : "-"],
    );
    if (peak) {
      rows.push(["Peak", `${frequency(peak.hz)} at ${level(peak.db)}`]);
    }
  }
  return (
    <div
      className="pointer-events-none absolute w-64 rounded border border-border bg-panel/90 p-1.5 text-xs tabular-nums"
      style={{
        left: plot.x + 10 + (plot.width - 20 - 256) * ax,
        top: plot.y + 10 + Math.max(plot.height - 20 - (other ? 150 : 50), 0) * ay,
      }}
    >
      <div>
        M{selected.id} <span className="float-right">{frequency(selected.frequencyHz)}</span>
      </div>
      <div>{level(selected.levelDb)}</div>
      <div className="truncate" style={what ? { color: `rgb(${what.color.slice(0, 3).map((c) => Math.round(c * 255)).join(",")})` } : undefined}>
        {what ? what.name : "-"}
      </div>
      {other && (
        <div className="mt-1 border-t border-border pt-1">
          <div className="text-dim">vs M{other.id}</div>
          {rows.map(([label, value]) => (
            <div key={label} className="flex justify-between">
              <span className="text-dim">{label}</span>
              <span>{value}</span>
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
