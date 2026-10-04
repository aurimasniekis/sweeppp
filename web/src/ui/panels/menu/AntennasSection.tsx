// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";
import { type ReactNode, useState } from "react";

import { type Antenna, describeRange } from "../../../protocol/wire";
import { frequencyField, parseFrequency } from "../../../render/format";
import { setUserAntennas, useInstrument } from "../../../state/instrument";
import { sessionStore } from "../../../state/session";
import { Switch } from "../../controls";
import { ConfirmButton } from "./Section";
import { Icon } from "../../Icon";
import { icon } from "../../icons";

/** "Wideband · discone · 25 MHz - 1.3 GHz · +2.0 dBi · bias-T" */
function describeAntenna(antenna: Antenna): string {
  return [
    antenna.category,
    antenna.type,
    describeRange(antenna),
    `${antenna.gainDbi >= 0 ? "+" : ""}${antenna.gainDbi.toFixed(1)} dBi`,
    antenna.needsBiasT ? "bias-T" : "",
  ]
    .filter((part) => part !== "")
    .join(" · ");
}

/** As the library's `slugify`: lower-case runs of letters and digits, joined
 * by single dashes. */
function slugify(name: string): string {
  return name
    .toLowerCase()
    .split(/[^a-z0-9]+/)
    .filter((part) => part !== "")
    .join("-");
}

/** As `AntennaLibrary::makeId`: the name's slug, numbered past any taken. */
function makeId(name: string, taken: readonly Antenna[]): string {
  const base = slugify(name) || "antenna";
  const free = (id: string) => !taken.some((a) => a.id === id);
  if (free(base)) {
    return base;
  }
  for (let suffix = 2; suffix < 1000; ++suffix) {
    if (free(`${base}-${suffix}`)) {
      return `${base}-${suffix}`;
    }
  }
  return base;
}

interface Draft {
  /** Empty for a new entry. An edited one keeps its id: assignments store it. */
  id: string;
  name: string;
  category: string;
  type: string;
  start: string;
  stop: string;
  gain: string;
  needsBiasT: boolean;
  notes: string;
}

function draftOf(antenna: Antenna | null): Draft {
  return {
    id: antenna?.id ?? "",
    name: antenna?.name ?? "",
    category: antenna?.category ?? "",
    type: antenna?.type ?? "",
    start: antenna && antenna.startHz > 0 ? frequencyField(antenna.startHz) : "",
    stop: antenna && antenna.stopHz > 0 ? frequencyField(antenna.stopHz) : "",
    gain: antenna ? String(antenna.gainDbi) : "0",
    needsBiasT: antenna?.needsBiasT ?? false,
    notes: antenna?.notes ?? "",
  };
}

function Labelled({ label, children }: { label: string; children: ReactNode }) {
  return (
    <label className="contents">
      <span className="caption">{label}</span>
      {children}
    </label>
  );
}

function Editor({ draft, all, onDone }: { draft: Draft; all: readonly Antenna[]; onDone: () => void }) {
  const [d, setD] = useState(draft);
  const edit = (change: Partial<Draft>) => setD((current) => ({ ...current, ...change }));

  const startHz = parseFrequency(d.start);
  const stopHz = parseFrequency(d.stop);
  const gainDbi = Number(d.gain);
  const problem = !d.name.trim()
    ? "a name is required"
    : startHz === null || stopHz === null
      ? "start and stop need a frequency, as 88M or 1.2G"
      : startHz <= 0 || stopHz <= startHz
        ? "stop must be above start"
        : d.gain.trim() === "" || !Number.isFinite(gainDbi)
          ? "gain needs a number"
          : "";

  const save = () => {
    if (problem || startHz === null || stopHz === null) {
      return;
    }
    // A copy of a shipped entry keeps its id, so it shadows the original
    // rather than sitting beside it.
    const own = all.filter((a) => !a.builtin);
    const name = d.name.trim();
    const id = d.id || makeId(name, all);
    const antenna: Antenna = {
      id,
      name,
      category: d.category.trim(),
      type: d.type.trim(),
      startHz,
      stopHz,
      gainDbi,
      needsBiasT: d.needsBiasT,
      notes: d.notes.trim(),
      builtin: false,
    };
    const index = own.findIndex((a) => a.id === id);
    setUserAntennas(index >= 0 ? own.map((a, i) => (i === index ? antenna : a)) : [...own, antenna]);
    onDone();
  };

  return (
    <div>
      <div className="grid grid-cols-[auto_1fr] items-center gap-x-2 gap-y-1">
        <Labelled label="Name">
          <input className="field" value={d.name} placeholder="Diamond D-190" onChange={(e) => edit({ name: e.target.value })} />
        </Labelled>
        <Labelled label="Category">
          <input className="field" value={d.category} placeholder="Wideband, ADS-B, GPS" onChange={(e) => edit({ category: e.target.value })} />
        </Labelled>
        <Labelled label="Type">
          <input className="field" value={d.type} placeholder="discone, yagi, whip" onChange={(e) => edit({ type: e.target.value })} />
        </Labelled>
        <span className="caption" title="Usable range. A routed sweep sends only these frequencies through it.">
          Range
        </span>
        <div className="grid grid-cols-[1fr_auto_1fr] items-center gap-1">
          <input className="field" value={d.start} placeholder="25M" inputMode="decimal" onChange={(e) => edit({ start: e.target.value })} />
          <span className="caption">–</span>
          <input className="field" value={d.stop} placeholder="1.3G" inputMode="decimal" onChange={(e) => edit({ stop: e.target.value })} />
        </div>
        <Labelled label="Gain dBi">
          <input className="field" value={d.gain} inputMode="decimal" onChange={(e) => edit({ gain: e.target.value })} />
        </Labelled>
        <span />
        <Switch label="Needs bias-T" checked={d.needsBiasT} onChange={(on) => edit({ needsBiasT: on })} />
        <Labelled label="Notes">
          <input className="field" value={d.notes} onChange={(e) => edit({ notes: e.target.value })} />
        </Labelled>
      </div>
      {problem && <p className="mt-1 text-xs text-warning">{problem}</p>}
      <div className="mt-2 grid grid-cols-2 gap-1">
        <button className="btn" disabled={problem !== ""} onClick={save}>
          Save
        </button>
        <button className="btn" onClick={onDone}>
          Cancel
        </button>
      </div>
    </div>
  );
}

export function AntennasSection() {
  const instrument = useInstrument();
  const serverName = useSelector(sessionStore, (s) => s.serverName);
  const [editing, setEditing] = useState<Draft | null>(null);
  const antennas = instrument.antennas;
  const disabled = !instrument.canControl;

  if (editing && !disabled) {
    return <Editor draft={editing} all={antennas} onDone={() => setEditing(null)} />;
  }

  const remove = (id: string) => setUserAntennas(antennas.filter((a) => !a.builtin && a.id !== id));

  return (
    <div>
      <p className="caption mb-1">
        {disabled ? "Only the client in control can edit these." : "Assign these to connectors in the device panel."}
      </p>
      {antennas.length === 0 && <p className="caption">No antennas yet.</p>}
      {antennas.map((antenna) => (
        <div key={antenna.id} className="flex items-start gap-1 border-b border-separator py-1 last:border-b-0">
          <div className="min-w-0 flex-1">
            <div className="truncate">{antenna.name}</div>
            <div className="caption pl-3">{describeAntenna(antenna)}</div>
          </div>
          <button
            className="btn-icon h-7 w-7"
            disabled={disabled}
            title={antenna.builtin ? "Edit a copy" : "Edit this antenna"}
            onClick={() => setEditing(draftOf(antenna))}
          >
            <Icon path={icon.edit} />
          </button>
          <ConfirmButton
            className="btn-icon h-7 w-7"
            disabled={disabled || antenna.builtin}
            title={antenna.builtin ? "Built-in antennas cannot be deleted" : "Delete this antenna"}
            prompt="Delete?"
            onConfirm={() => remove(antenna.id)}
          >
            <Icon path={icon.delete} />
          </ConfirmButton>
        </div>
      ))}
      <button className="btn mt-1.5 w-full" disabled={disabled} onClick={() => setEditing(draftOf(null))}>
        Add antenna
      </button>
      {serverName && <p className="caption mt-1">Stored on {serverName}</p>}
    </div>
  );
}
