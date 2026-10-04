// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";

import { focused } from "../../model/layout";
import { session } from "../../state/session";
import { themeStore } from "../../state/theme";
import { setPanel, setView, viewStore } from "../../state/view";
import { Field, Select, Slider, Switch } from "../controls";

/** The waterfall: pause, history, gradient and colours. */
export function WaterfallPopover() {
  const v = useSelector(viewStore, (s) => s);
  const maps = useSelector(themeStore, (t) => t.colorMaps);
  const panel = focused(v.layout);
  const autoLevel = () => {
    // Floor to the strongest signal: the quietest and loudest tenth of what
    // is on screen now, a little beyond either end.
    const live = [...session.traces.trace("live")].filter((x) => x > -190).sort((a, b) => a - b);
    if (live.length === 0) {
      return;
    }
    const floor = live[Math.floor(live.length * 0.1)]!;
    const peak = live[Math.floor(live.length * 0.999)]!;
    setPanel(panel.id, { gradientMinDb: Math.round(floor - 3), gradientMaxDb: Math.round(Math.max(peak + 3, floor + 10)) });
  };
  return (
    <div>
      <Switch
        label="Paused"
        checked={panel.waterfallPaused}
        onChange={(on) => setPanel(panel.id, { waterfallPaused: on })}
      />
      <div className="section-title">Gradient, panel {panel.id}</div>
      <Field label={`Top ${panel.gradientMaxDb.toFixed(0)} dBFS`}>
        <Slider
          value={panel.gradientMaxDb}
          min={-140}
          max={10}
          onChange={(x) => setPanel(panel.id, { gradientMaxDb: Math.max(x, panel.gradientMinDb + 5) })}
        />
      </Field>
      <Field label={`Bottom ${panel.gradientMinDb.toFixed(0)} dBFS`}>
        <Slider
          value={panel.gradientMinDb}
          min={-150}
          max={0}
          onChange={(x) => setPanel(panel.id, { gradientMinDb: Math.min(x, panel.gradientMaxDb - 5) })}
        />
      </Field>
      <button className="btn w-full" onClick={autoLevel} title="Fit the gradient to what is on screen now">
        Fit to the signal
      </button>
      <Field label="Colours">
        <Select
          value={v.colorMap || "(theme)"}
          choices={[{ value: "(theme)", label: "The theme's" }, ...maps.map((m) => ({ value: m.name, label: m.name }))]}
          onChange={(name) => setView({ colorMap: name === "(theme)" ? "" : name })}
        />
      </Field>
      <Field label={`History ${v.waterfallLines} lines`} hint="Kept in this browser's graphics memory.">
        <Slider
          value={v.waterfallLines}
          min={256}
          max={8192}
          step={256}
          onChange={(x) => setView({ waterfallLines: x })}
        />
      </Field>
    </div>
  );
}
