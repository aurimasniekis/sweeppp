// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { type ReactNode, useEffect } from "react";

import { type InstrumentView, rescanSwitchers, setAssignments } from "../../../state/instrument";
import {
  type Antenna,
  type AntennaAssignments,
  describeRange,
  formatFrequencyShort,
  type SdrRxPort,
  totalSpanHz,
} from "../../../protocol/wire";
import { type Choice, Select } from "../../controls";
import {
  antennaFor,
  antennaOnInput,
  assign,
  assignInput,
  assignSwitcher,
  copyAssignments,
  fallbackPort,
  portOfSwitcher,
  rfPathKey,
  setFallbackPort,
  switcherFor,
} from "./assignments";
import { asBool } from "./values";

const kNone = "none";

function describeAntenna(antenna: Antenna): string {
  const gain = `${antenna.gainDbi >= 0 ? "+" : ""}${antenna.gainDbi.toFixed(1)} dBi`;
  return [antenna.category, antenna.type, describeRange(antenna), gain, antenna.needsBiasT ? "bias-T" : ""]
    .filter(Boolean)
    .join(" · ");
}

function Warning({ children }: { children: ReactNode }) {
  return <div className="mt-1 text-xs text-warning">{children}</div>;
}

function Listening() {
  return <div className="mt-1 text-xs text-accent">listening on this one now</div>;
}

/** What is on each connector, and through it, behind any switcher. */
export function Antennas({ instrument }: { instrument: InstrumentView }) {
  const descriptor = instrument.device.descriptor;
  const canControl = instrument.canControl;

  // Switchers come and go on the bus the radio is on, which may not be this
  // machine's, so they are asked for again while the panel is open.
  useEffect(() => {
    if (!canControl || !descriptor) {
      return;
    }
    rescanSwitchers();
    const timer = window.setInterval(rescanSwitchers, 2000);
    return () => window.clearInterval(timer);
  }, [canControl, descriptor !== null]);

  if (!descriptor) {
    return null;
  }

  const locked = !canControl;
  const deviceKey = instrument.device.antennaKey;
  const library = instrument.antennas;
  const assignments = instrument.assignments;
  const ports = descriptor.rxPorts;
  const selected = instrument.values.selectedRxPort;
  const available = instrument.switchers.available;
  const hasBiasTee = descriptor.parameters.some((p) => p.key === "bias_tee");
  const biasTee = { present: hasBiasTee, on: hasBiasTee && asBool(instrument.values.parameters.get("bias_tee")) };

  const edit = (change: (a: AntennaAssignments) => void) => {
    const copy = copyAssignments(assignments);
    change(copy);
    setAssignments(copy);
  };

  const antennaChoices: Choice<string>[] = library.map((a) => ({ value: `a:${a.id}`, label: a.name }));
  const switcherChoices: Choice<string>[] = available.map((info) => ({
    value: `s:${rfPathKey(info)}`,
    label: `${info.label} (switcher)`,
  }));

  // One row even on a radio without ports: the antenna in front of it is as
  // much a fact about the measurement.
  const rows = ports.length > 0 ? ports : [null];

  const plan = instrument.plan;
  const planned = totalSpanHz(plan);
  let covered = 0;
  for (const segment of plan.segments) {
    for (const [startHz, stopHz] of instrument.rfPath.coverage) {
      covered += Math.max(0, Math.min(segment.stopHz, stopHz) - Math.max(segment.startHz, startHz));
    }
  }

  const describePort = (port: SdrRxPort) => {
    const antenna = library.find((a) => a.id === antennaFor(assignments, deviceKey, port.id));
    if (antenna) return `${port.label} (${antenna.name})`;
    if (switcherFor(assignments, deviceKey, port.id)) return `${port.label} (switcher)`;
    return port.label;
  };
  const fallbackId = fallbackPort(assignments, deviceKey);
  const fallback = ports.find((p) => p.id === fallbackId);

  return (
    <>
      <div className="section-title">Antennas</div>
      {rows.map((port, i) => {
        const portId = port?.id ?? "";
        const label = port?.label ?? "Antenna";
        const listening = port !== null && port.id === selected;
        const switcherKey = switcherFor(assignments, deviceKey, portId);

        if (!switcherKey) {
          return (
            <AntennaRow
              key={portId || i}
              label={label}
              port={port}
              assignedId={antennaFor(assignments, deviceKey, portId)}
              library={library}
              choices={[...antennaChoices, ...switcherChoices]}
              biasTee={biasTee}
              disabled={locked}
              onChoose={(v) =>
                edit((a) =>
                  v.startsWith("s:")
                    ? assignSwitcher(a, deviceKey, portId, v.slice(2))
                    : assign(a, deviceKey, portId, v.startsWith("a:") ? v.slice(2) : ""),
                )
              }
            >
              {listening && <Listening />}
            </AntennaRow>
          );
        }

        const view = instrument.switchers.open.find((s) => s.key === switcherKey);
        const choices = [...antennaChoices, ...switcherChoices];
        if (!choices.some((c) => c.value === `s:${switcherKey}`)) {
          choices.push({ value: `s:${switcherKey}`, label: view?.info.label ?? switcherKey });
        }
        return (
          <div key={portId || i} className="py-1">
            <div className="mb-1 text-xs text-dim" title={port?.connector || undefined}>
              {label}
            </div>
            <Select
              value={`s:${switcherKey}`}
              choices={[{ value: kNone, label: "- none -" }, ...choices]}
              disabled={locked}
              onChange={(v) =>
                edit((a) =>
                  v.startsWith("a:")
                    ? assign(a, deviceKey, portId, v.slice(2))
                    : assignSwitcher(a, deviceKey, portId, v.startsWith("s:") ? v.slice(2) : ""),
                )
              }
            />
            {!view ? (
              <Warning>this switcher is not connected; its antennas are not available</Warning>
            ) : (
              <div className="ml-1 mt-1 border-l border-border pl-3">
                {view.inputs.map((input, index) => {
                  const live = instrument.rfPath.legs.some(
                    (leg) => leg.live && leg.switcherKey === switcherKey && leg.route.inputIndex === index,
                  );
                  return (
                    <AntennaRow
                      key={input.id}
                      label={input.label}
                      port={port}
                      assignedId={antennaOnInput(assignments, switcherKey, input.id)}
                      library={library}
                      choices={antennaChoices}
                      biasTee={biasTee}
                      disabled={locked}
                      onChoose={(v) =>
                        edit((a) => assignInput(a, switcherKey, input.id, v.startsWith("a:") ? v.slice(2) : ""))
                      }
                    >
                      {live && <Listening />}
                    </AntennaRow>
                  );
                })}
              </div>
            )}
            {listening && <Listening />}
          </div>
        );
      })}

      {ports.length > 1 && (
        <div className="py-1">
          <div className="mb-1 text-xs text-dim">Uncovered ranges</div>
          <Select
            value={fallback ? `p:${fallback.id}` : kNone}
            choices={[
              { value: kNone, label: "- leave as is -" },
              ...ports.map((p) => ({ value: `p:${p.id}`, label: describePort(p) })),
            ]}
            disabled={locked}
            onChange={(v) => edit((a) => setFallbackPort(a, deviceKey, v.startsWith("p:") ? v.slice(2) : ""))}
          />
          <div className="caption mt-1">Connector for frequencies no assigned antenna covers.</div>
        </div>
      )}

      {plan.segments.length > 0 && (
        <div className="caption py-1">
          covers {formatFrequencyShort(Math.min(covered, planned))} of the planned {formatFrequencyShort(planned)}
        </div>
      )}

      <div className="row">
        <span className="caption">
          {available.length === 0 ? "No switchers found." : `${available.length} switcher${available.length === 1 ? "" : "s"}`}
        </span>
        <button className="btn" disabled={locked} onClick={rescanSwitchers}>
          Rescan switchers
        </button>
      </div>
      {available.map((info) => {
        const key = rfPathKey(info);
        const on = portOfSwitcher(assignments, key);
        const portLabel = on && on[0] === deviceKey ? (ports.find((p) => p.id === on[1])?.label ?? on[1]) : null;
        return (
          <div key={key} className="row text-xs" title={`${key} -- ${info.inputCount} inputs`}>
            <span className="truncate">{info.label}</span>
            <span className="shrink-0 text-dim">{on ? `on ${portLabel ?? "another radio"}` : "not on a connector"}</span>
          </div>
        );
      })}
    </>
  );
}

function AntennaRow({
  label,
  port,
  assignedId,
  library,
  choices,
  biasTee,
  disabled,
  onChoose,
  children,
}: {
  label: string;
  port: SdrRxPort | null;
  assignedId: string;
  library: Antenna[];
  choices: Choice<string>[];
  biasTee: { present: boolean; on: boolean };
  disabled: boolean;
  onChoose: (value: string) => void;
  children?: ReactNode;
}) {
  const assigned = assignedId ? library.find((a) => a.id === assignedId) : undefined;

  let caption: ReactNode;
  if (!assigned) {
    // Kept rather than dropped: restoring the antenna restores the assignment.
    caption = assignedId ? (
      <Warning>antenna '{assignedId}' is missing from the library</Warning>
    ) : (
      <div className="caption mt-1">nothing assigned</div>
    );
  } else {
    // An unpowered active antenna measures its own noise floor, convincingly.
    const reason = !assigned.needsBiasT
      ? null
      : port && !port.biasTee
        ? "this port cannot supply bias-T"
        : !biasTee.present
          ? "this radio cannot supply bias-T"
          : !biasTee.on
            ? "bias-T is switched off"
            : null;
    caption = (
      <>
        <div className="caption mt-1">{describeAntenna(assigned)}</div>
        {reason && <Warning>{reason}</Warning>}
      </>
    );
  }

  return (
    <div className="py-1">
      <div className="mb-1 text-xs text-dim" title={port?.connector || undefined}>
        {label}
      </div>
      <Select
        value={assigned ? `a:${assigned.id}` : kNone}
        choices={[{ value: kNone, label: "- none -" }, ...choices]}
        disabled={disabled}
        onChange={onChoose}
      />
      {caption}
      {children}
    </div>
  );
}
