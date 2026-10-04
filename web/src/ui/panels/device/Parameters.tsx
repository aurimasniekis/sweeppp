// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { type ReactNode, useState } from "react";

import { parseFrequency } from "../../../render/format";
import { type InstrumentView, setParameter } from "../../../state/instrument";
import { type SdrParameter, SdrParameterType, type SdrValue } from "../../../protocol/wire";
import { type Choice, CommitField, Readout, Select, Slider, Switch, SwitchRow } from "../../controls";
import { applies, asBool, asNumber, asString, formatParameter, needsStop, presetIndex } from "./values";

const kOffList = "__current";
const kStopHint = "Stop the radio to change this.";

interface Row {
  parameter: SdrParameter;
  value: SdrValue;
  /** Refused by the radio while it runs. */
  blocked: boolean;
}

/** Generated from the descriptor alone: there is no per-radio code here. */
export function Parameters({ instrument }: { instrument: InstrumentView }) {
  const descriptor = instrument.device.descriptor;
  if (!descriptor) {
    return null;
  }
  const values = instrument.values.parameters;
  const running = instrument.run.running;
  const sweeping = instrument.run.sweeping;
  const locked = !instrument.canControl;

  // By group rather than declaration order: a driver may append a parameter
  // after the ones it belongs with.
  const groups: { name: string; rows: Row[] }[] = [];
  for (const parameter of descriptor.parameters) {
    const value = values.get(parameter.key);
    if (value === undefined || !applies(parameter, values)) {
      continue;
    }
    const row = { parameter, value, blocked: running && needsStop(parameter, sweeping) };
    const group = groups.find((g) => g.name === parameter.group);
    if (group) {
      group.rows.push(row);
    } else {
      groups.push({ name: parameter.group, rows: [row] });
    }
  }

  return (
    <>
      {groups.map((group) => (
        <div key={group.name}>
          {group.name && <div className="section-title">{group.name}</div>}
          {layout(group.rows, locked)}
        </div>
      ))}
    </>
  );
}

/** Switches two a row, everything else a row of its own. */
function layout(rows: Row[], locked: boolean): ReactNode[] {
  const out: ReactNode[] = [];
  let switches: Row[] = [];
  const flush = () => {
    if (switches.length > 0) {
      out.push(<SwitchPair key={switches[0]!.parameter.key} rows={switches} locked={locked} />);
      switches = [];
    }
  };
  for (const row of rows) {
    if (row.parameter.type === SdrParameterType.Bool && !row.parameter.readOnly) {
      switches.push(row);
      if (switches.length === 2) {
        flush();
      }
      continue;
    }
    flush();
    out.push(<ParameterRow key={row.parameter.key} row={row} locked={locked} />);
  }
  flush();
  return out;
}

function tooltip({ parameter, blocked }: Row): string | undefined {
  const parts = [parameter.description, blocked ? kStopHint : ""].filter(Boolean);
  return parts.length > 0 ? parts.join("\n\n") : undefined;
}

function SwitchPair({ rows, locked }: { rows: Row[]; locked: boolean }) {
  const blocked = rows.filter((r) => r.blocked).map((r) => r.parameter.label);
  return (
    <div className="py-0.5">
      <SwitchRow>
        {rows.map((row) => (
          <div key={row.parameter.key} title={tooltip(row)}>
            <Switch
              label={row.parameter.label}
              checked={asBool(row.value)}
              disabled={locked || row.blocked}
              onChange={(checked) => setParameter(row.parameter.key, checked)}
            />
          </div>
        ))}
      </SwitchRow>
      {blocked.length > 0 && <div className="caption">Stop the radio to change {blocked.join(" and ")}.</div>}
    </div>
  );
}

function ParameterRow({ row, locked }: { row: Row; locked: boolean }) {
  const { parameter, value, blocked } = row;
  if (parameter.readOnly) {
    return (
      <div className="py-1">
        <Readout rows={[[parameter.label, formatParameter(parameter, value), parameter.description || undefined]]} />
      </div>
    );
  }
  const disabled = locked || blocked;
  const tip = tooltip(row);
  const hint = blocked ? <div className="caption mt-1">{kStopHint}</div> : null;

  if (sliderRange(parameter)) {
    return <SliderRow parameter={parameter} value={value} disabled={disabled} tip={tip} hint={hint} />;
  }
  return (
    <div className="py-1">
      <div className="mb-1 text-xs text-dim" title={tip}>
        {parameter.label}
      </div>
      <ParameterControl parameter={parameter} value={value} disabled={disabled} />
      {hint}
    </div>
  );
}

/** Bounded numbers without presets, other than frequencies: a slider across
 * 1 MHz - 6 GHz has no usable step. */
function sliderRange(parameter: SdrParameter): boolean {
  if (parameter.max <= parameter.min) {
    return false;
  }
  if (parameter.type === SdrParameterType.Int) {
    return true;
  }
  return (
    parameter.type === SdrParameterType.Double &&
    parameter.enumValues.length === 0 &&
    parameter.unit !== "Hz" &&
    parameter.unit !== "S/s"
  );
}

function SliderRow({
  parameter,
  value,
  disabled,
  tip,
  hint,
}: {
  parameter: SdrParameter;
  value: SdrValue;
  disabled: boolean;
  tip: string | undefined;
  hint: ReactNode;
}) {
  const integer = parameter.type === SdrParameterType.Int;
  const [dragging, setDragging] = useState<number | null>(null);
  const shown = dragging ?? asNumber(value);
  const step = integer
    ? Math.max(1, Math.round(parameter.step))
    : parameter.step > 0
      ? parameter.step
      : (parameter.max - parameter.min) / 1000;
  const places = integer ? 0 : parameter.step > 0 && parameter.step < 1 ? Math.min(3, Math.ceil(-Math.log10(parameter.step))) : parameter.step === 0 ? 2 : 0;
  return (
    <div className="py-1">
      <div className="mb-1 flex items-baseline justify-between gap-2 text-xs">
        <span className="text-dim" title={tip}>
          {parameter.label}
        </span>
        <span className="tabular-nums">
          {shown.toFixed(places)}
          {parameter.unit && ` ${parameter.unit}`}
        </span>
      </div>
      <Slider
        value={shown}
        min={parameter.min}
        max={parameter.max}
        step={step}
        disabled={disabled}
        onChange={setDragging}
        onCommit={(v) => {
          setDragging(null);
          setParameter(parameter.key, integer ? BigInt(Math.round(v)) : v);
        }}
      />
      {hint}
    </div>
  );
}

function ParameterControl({
  parameter,
  value,
  disabled,
}: {
  parameter: SdrParameter;
  value: SdrValue;
  disabled: boolean;
}) {
  const set = (v: SdrValue) => setParameter(parameter.key, v);

  switch (parameter.type) {
    case SdrParameterType.Enum: {
      const held = asString(value);
      const listed = parameter.enumValues.some((c) => c.value === held && c.value !== "");
      const choices: Choice<string>[] = parameter.enumValues
        .filter((c) => c.value !== "")
        .map((c) => ({ value: c.value, label: c.label }));
      if (!listed) {
        choices.unshift({ value: kOffList, label: formatParameter(parameter, value) });
      }
      return (
        <Select
          value={listed ? held : kOffList}
          choices={choices}
          disabled={disabled}
          onChange={(v) => v !== kOffList && set(v)}
        />
      );
    }

    case SdrParameterType.Double: {
      if (parameter.enumValues.length > 0) {
        // Presets, not constraints: a rate set some other way still shows, as
        // itself, rather than snapping to a neighbour.
        const index = presetIndex(parameter, value);
        const choices: Choice<string>[] = parameter.enumValues.map((c, i) => ({ value: String(i), label: c.label }));
        if (index < 0) {
          choices.unshift({ value: kOffList, label: formatParameter(parameter, value) });
        }
        return (
          <Select
            value={index < 0 ? kOffList : String(index)}
            choices={choices}
            disabled={disabled}
            onChange={(v) => {
              const preset = parameter.enumValues[Number(v)];
              const hz = preset && v !== kOffList ? parseFrequency(preset.value) : null;
              if (hz !== null) {
                set(hz);
              }
            }}
          />
        );
      }
      if (parameter.unit === "Hz" || parameter.unit === "S/s") {
        return (
          <div className="flex items-center gap-2">
            <CommitField
              value={String(parseFloat((asNumber(value) / 1e6).toFixed(6)))}
              disabled={disabled}
              onCommit={(text) => {
                const parsed = parseFrequency(text);
                if (parsed === null) {
                  return false;
                }
                // A bare figure is in mega, as written beside the field.
                set(/^[\s\d._+\-e]*$/i.test(text) ? parsed * 1e6 : parsed);
                return true;
              }}
            />
            <span className="caption shrink-0">M{parameter.unit}</span>
          </div>
        );
      }
      return (
        <NumberField
          value={asNumber(value)}
          unit={parameter.unit}
          disabled={disabled}
          onCommit={(n) => set(n)}
        />
      );
    }

    case SdrParameterType.Int:
      return (
        <NumberField
          value={Math.round(asNumber(value))}
          unit={parameter.unit}
          disabled={disabled}
          onCommit={(n) => set(BigInt(Math.round(n)))}
        />
      );

    case SdrParameterType.String:
      return (
        <CommitField
          value={asString(value)}
          inputMode="text"
          disabled={disabled}
          onCommit={(text) => {
            set(text);
            return true;
          }}
        />
      );

    default:
      return null;
  }
}

function NumberField({
  value,
  unit,
  disabled,
  onCommit,
}: {
  value: number;
  unit: string;
  disabled: boolean;
  onCommit: (n: number) => void;
}) {
  return (
    <div className="flex items-center gap-2">
      <CommitField
        value={String(value)}
        disabled={disabled}
        onCommit={(text) => {
          const parsed = parseFrequency(text);
          if (parsed === null) {
            return false;
          }
          onCommit(parsed);
          return true;
        }}
      />
      {unit && <span className="caption shrink-0">{unit}</span>}
    </div>
  );
}
