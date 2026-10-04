// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";

import { focused } from "../../../model/layout";
import { session } from "../../../state/session";
import { setPanel, setView, viewStore } from "../../../state/view";
import { CommitField, Field, Segmented, Select, Slider, Switch, SwitchRow } from "../../controls";
import { kFillChoices } from "../../popovers/ChartPopover";

const kFloorDb = -150;
const kCeilingDb = 10;

const kReadoutPlaces = [
  "Off",
  "Top left",
  "Top centre",
  "Top right",
  "Middle left",
  "Middle right",
  "Bottom left",
  "Bottom centre",
  "Bottom right",
];

/** Two dB fields, committed only while the bottom stays under the top. */
function DbRange({
  label,
  min,
  max,
  onChange,
}: {
  label: string;
  min: number;
  max: number;
  onChange: (min: number, max: number) => void;
}) {
  const commit = (text: string, which: "min" | "max"): boolean => {
    const value = Number(text.replace(/\s*db\s*$/i, ""));
    if (!Number.isFinite(value) || value < kFloorDb || value > kCeilingDb) {
      return false;
    }
    const nextMin = which === "min" ? value : min;
    const nextMax = which === "max" ? value : max;
    if (nextMax <= nextMin) {
      return false;
    }
    onChange(nextMin, nextMax);
    return true;
  };
  return (
    <div className="contents">
      <div className="caption">{label}</div>
      <CommitField value={min.toFixed(0)} onCommit={(t) => commit(t, "min")} />
      <span className="caption">–</span>
      <CommitField value={max.toFixed(0)} onCommit={(t) => commit(t, "max")} />
      <span className="caption">dB</span>
    </div>
  );
}

export function DisplaySection() {
  const v = useSelector(viewStore, (s) => s);
  const panel = focused(v.layout);
  const pointsExponent = Math.round(Math.log2(Math.max(64, v.displayPoints)));
  return (
    <div>
      <SwitchRow>
        <Switch label="Grid" checked={v.showGrid} onChange={(on) => setView({ showGrid: on })} />
        <Switch label="Bands (B)" checked={v.showBands} onChange={(on) => setView({ showBands: on })} />
        <Switch label="Channels (C)" checked={v.showChannels} onChange={(on) => setView({ showChannels: on })} />
        <Switch label="Auto points" checked={v.autoPoints} onChange={(on) => setView({ autoPoints: on })} />
        <Switch
          label="Waterfall peaks"
          checked={v.waterfallPeakDetect}
          onChange={(on) => setView({ waterfallPeakDetect: on })}
        />
      </SwitchRow>
      {v.autoPoints ? (
        <p className="caption">Display points follow the plot width.</p>
      ) : (
        <Field label={`Display points ${v.displayPoints}`}>
          <Slider
            value={pointsExponent}
            min={6}
            max={13}
            onChange={(x) => setView({ displayPoints: 2 ** x })}
          />
        </Field>
      )}

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
      </SwitchRow>
      {v.showAverage && (
        <Field label={`Average over ${v.averageWindow} frames`}>
          <Slider value={v.averageWindow} min={2} max={256} onChange={(x) => setView({ averageWindow: x })} />
        </Field>
      )}
      <button className="btn mt-1 w-full" onClick={() => session.traces.resetHolds()}>
        Reset holds
      </button>
      <Field label="Under trace" hint="Solid: the trace's colour. Gradient: coloured by level.">
        <Segmented value={v.fill} choices={kFillChoices} onChange={(fill) => setView({ fill })} />
      </Field>

      <div className="section-title">Levels, panel {panel.id}</div>
      <div className="grid grid-cols-[auto_1fr_auto_1fr_auto] items-center gap-1">
        <DbRange
          label="Y range"
          min={panel.yMinDb}
          max={panel.yMaxDb}
          onChange={(yMinDb, yMaxDb) => setPanel(panel.id, { yMinDb, yMaxDb })}
        />
        <DbRange
          label="Gradient"
          min={panel.gradientMinDb}
          max={panel.gradientMaxDb}
          onChange={(gradientMinDb, gradientMaxDb) => setPanel(panel.id, { gradientMinDb, gradientMaxDb })}
        />
      </div>

      <div className="section-title">Markers</div>
      <Field label="Readout" hint="Where the selected marker's readout card is drawn.">
        <Select
          value={String(Math.min(Math.max(v.markerReadout, 0), 8))}
          choices={kReadoutPlaces.map((label, i) => ({ value: String(i), label }))}
          onChange={(x) => setView({ markerReadout: Number(x) })}
        />
      </Field>
    </div>
  );
}
