// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";

import { focused } from "../../model/layout";
import { session } from "../../state/session";
import { setPanel, setView, viewStore } from "../../state/view";
import type { FillStyle } from "../../render/spectrum";
import { Field, Segmented, Slider, Switch, SwitchRow } from "../controls";

export const kFillChoices: { value: FillStyle; label: string }[] = [
  { value: "none", label: "None" },
  { value: "solid", label: "Solid" },
  { value: "gradient", label: "Gradient" },
];

/** The spectrum display: traces, fill, levels. */
export function ChartPopover() {
  const v = useSelector(viewStore, (s) => s);
  const panel = focused(v.layout);
  return (
    <div>
      <div className="section-title">Traces</div>
      <SwitchRow>
        <Switch
          label="Max hold"
          checked={v.showMaxHold}
          onChange={(on) => {
            session.traces.resetHolds();
            setView({ showMaxHold: on });
          }}
        />
        <Switch label="Min hold" checked={v.showMinHold} onChange={(on) => setView({ showMinHold: on })} />
        <Switch label="Average" checked={v.showAverage} onChange={(on) => setView({ showAverage: on })} />
        <Switch label="Grid" checked={v.showGrid} onChange={(on) => setView({ showGrid: on })} />
      </SwitchRow>
      <button className="btn mt-1 w-full" onClick={() => session.traces.resetHolds()}>
        Clear holds
      </button>
      <Field label="Under trace" hint="Solid: the trace's colour. Gradient: coloured by level.">
        <Segmented value={v.fill} choices={kFillChoices} onChange={(fill) => setView({ fill })} />
      </Field>
      <Field label={`Smoothing ${(v.smoothing * 100).toFixed(0)}%`} hint="Steadier, at the cost of blunting transients.">
        <Slider value={v.smoothing} min={0} max={0.95} step={0.05} onChange={(x) => setView({ smoothing: x })} />
      </Field>
      {v.showMaxHold && (
        <Field label={`Max hold decay ${v.maxHoldDecayDbPerSec.toFixed(0)} dB/s`}>
          <Slider
            value={v.maxHoldDecayDbPerSec}
            min={0}
            max={60}
            onChange={(x) => setView({ maxHoldDecayDbPerSec: x })}
          />
        </Field>
      )}
      {v.showAverage && (
        <Field label={`Average over ${v.averageWindow} frames`}>
          <Slider value={v.averageWindow} min={2} max={256} onChange={(x) => setView({ averageWindow: x })} />
        </Field>
      )}
      <div className="section-title">Levels, panel {panel.id}</div>
      <Field label={`Top ${panel.yMaxDb.toFixed(0)} dBFS`}>
        <Slider
          value={panel.yMaxDb}
          min={-140}
          max={10}
          onChange={(x) => setPanel(panel.id, { yMaxDb: Math.max(x, panel.yMinDb + 5) })}
        />
      </Field>
      <Field label={`Bottom ${panel.yMinDb.toFixed(0)} dBFS`}>
        <Slider
          value={panel.yMinDb}
          min={-150}
          max={0}
          onChange={(x) => setPanel(panel.id, { yMinDb: Math.min(x, panel.yMaxDb - 5) })}
        />
      </Field>
    </div>
  );
}
