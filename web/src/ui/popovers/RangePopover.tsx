// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";
import { useEffect, useState } from "react";

import { parseFrequency } from "../../render/format";
import { sweepRange, useInstrument } from "../../state/instrument";
import {
  addPreset,
  addSegment,
  describeRange,
  highestHz,
  loadBuiltinPresets,
  lowestHz,
  presetList,
  presetsStore,
  removePreset,
  setFavourite,
  type SweepPreset,
} from "../../state/presets";
import { formatFrequencyShort, type SweepSegment } from "../../protocol/wire";
import { CommitField } from "../controls";
import { Icon } from "../Icon";
import { icon } from "../icons";

/** Remembered across openings of the popover, as the desktop keeps it for the
 * session. */
let rememberedMode: "edges" | "centre" = "edges";
let rememberedMultiple = false;

/** MHz as a bare number, or any frequency with its unit: 433.92, 2.4G, 100k. */
function parseMhz(text: string): number | null {
  const trimmed = text.trim();
  if (/^[-+]?\d*\.?\d+$/.test(trimmed)) {
    return Number(trimmed) * 1e6;
  }
  return parseFrequency(trimmed);
}

const mhz = (hz: number) => (hz / 1e6).toFixed(3);

const kStepsMhz = [-100, -10, -1, 1, 10, 100];

/** One frequency in MHz, with steps either side of zero coarse to fine
 * outwards, so the pair for one size sit together. */
function FrequencyRow({
  label,
  hz,
  disabled,
  onChange,
}: {
  label: string;
  hz: number;
  disabled: boolean;
  onChange: (hz: number) => boolean;
}) {
  return (
    <div className="flex items-center gap-1 py-0.5">
      <span className="w-14 shrink-0">{label}</span>
      <div className="w-28 shrink-0">
        <CommitField
          value={mhz(hz)}
          disabled={disabled}
          onCommit={(text) => {
            const parsed = parseMhz(text);
            return parsed !== null && onChange(parsed);
          }}
        />
      </div>
      {kStepsMhz.map((step) => (
        <button
          key={step}
          className="btn h-7 px-1.5 text-xs tabular-nums"
          disabled={disabled}
          onClick={() => onChange(Math.max(0, hz + step * 1e6))}
        >
          {step > 0 ? `+${step}` : step}
        </button>
      ))}
    </div>
  );
}

function PresetRow({
  preset,
  reachable,
  device,
  disabled,
  onLoad,
  onAdd,
}: {
  preset: SweepPreset;
  reachable: boolean;
  device: string;
  disabled: boolean;
  onLoad: (add: boolean) => void;
  onAdd: () => void;
}) {
  const range = describeRange(preset.segments);
  return (
    <div className="flex items-center gap-1 py-0.5">
      <button
        className="btn-icon h-7 w-7"
        title={preset.favourite ? "Remove from favourites" : "Keep at the top of the list"}
        onClick={() => setFavourite(preset.name, !preset.favourite)}
      >
        <Icon path={preset.favourite ? icon.star : icon.starOff} className={preset.favourite ? "text-warning" : "text-dim"} />
      </button>
      <button
        className="flex h-7 min-w-0 flex-1 items-center gap-2 rounded px-1.5 text-left hover:bg-button-hover disabled:opacity-45"
        disabled={disabled || !reachable}
        title={
          reachable
            ? `${preset.name}\n${range}\n\nClick: sweep this\nAlt-click or +: add to the plan`
            : `${preset.name}\n${range}\n\nOutside what ${device} can tune.`
        }
        onClick={(e) => onLoad(e.altKey)}
      >
        <span className="truncate">{preset.name}</span>
        <span className="ml-auto shrink-0 text-xs text-dim tabular-nums">{range}</span>
      </button>
      <button className="btn-icon h-7 w-7" disabled={disabled || !reachable} title="Add to the plan" onClick={onAdd}>
        <Icon path={icon.add} />
      </button>
      <button
        className="btn-icon h-7 w-7"
        title={preset.builtin ? "Hide this built-in preset" : "Delete this preset"}
        onClick={() => removePreset(preset)}
      >
        <Icon path={icon.close} />
      </button>
    </div>
  );
}

/** What is swept: one range or several, the radio's or the antennas' whole
 * reach in one click, and the presets. Edits are sent once a field is
 * committed, so a stop typed below its start on the way is never swept. */
export function RangePopover() {
  const instrument = useInstrument();
  const plan = instrument.plan;
  const disabled = !instrument.canControl;
  const presets = presetList(useSelector(presetsStore, (s) => s));
  const [mode, setMode] = useState(rememberedMode);
  const [multipleWanted, setMultipleWanted] = useState(rememberedMultiple);
  const [presetName, setPresetName] = useState("");
  useEffect(() => loadBuiltinPresets(), []);
  useEffect(() => {
    rememberedMode = mode;
    rememberedMultiple = multipleWanted;
  }, [mode, multipleWanted]);

  const segments = plan.segments;
  const multiple = segments.length > 1 || multipleWanted;
  const device = instrument.device.descriptor?.info;
  const coverage = instrument.rfPath.coverage;

  const apply = (next: { startHz: number; stopHz: number }[]): boolean => {
    if (next.length === 0 || next.some((s) => !(s.stopHz > s.startHz))) {
      return false;
    }
    sweepRange({ ...plan, segments: next.map((s) => ({ dwellSeconds: 0, ...s }) as SweepSegment) });
    return true;
  };
  const setSegment = (index: number, change: Partial<SweepSegment>) =>
    apply(segments.map((s, i) => (i === index ? { ...s, ...change } : s)));

  const single = segments[0] ?? { startHz: 88e6, stopHz: 108e6, dwellSeconds: 0 };
  const centre = (single.startHz + single.stopHz) / 2;
  const span = single.stopHz - single.startHz;
  const total = segments.reduce((sum, s) => sum + Math.max(s.stopHz - s.startHz, 0), 0);
  const outside =
    device !== undefined && (lowestHz(segments) < device.minFrequencyHz || highestHz(segments) > device.maxFrequencyHz);
  const defaultName = `${formatFrequencyShort(lowestHz(segments))} - ${formatFrequencyShort(highestHz(segments))}`;

  const addPresetToPlan = (preset: SweepPreset) => {
    let next = segments.map((s) => ({ startHz: s.startHz, stopHz: s.stopHz }));
    for (const segment of preset.segments) {
      next = addSegment(next, segment);
    }
    setMultipleWanted(next.length > 1);
    apply(next);
  };

  return (
    <div>
      <div className="section-title mt-0">Ranges</div>
      <div className="flex items-center gap-3">
        <label className="flex items-center gap-1.5" title="Sweep several separate bands as one job">
          <input
            type="checkbox"
            className="size-3.5 accent-[var(--color-accent)]"
            checked={multiple}
            disabled={disabled}
            onChange={(e) => {
              setMultipleWanted(e.target.checked);
              // Keep the extent rather than the first row: collapsing should
              // narrow what is swept as little as possible.
              if (!e.target.checked && segments.length > 1) {
                apply([{ startHz: lowestHz(segments), stopHz: highestHz(segments) }]);
              }
            }}
          />
          Multiple ranges
        </label>
        {!multiple && (
          <span className="ml-auto flex gap-3">
            {(["edges", "centre"] as const).map((m) => (
              <label key={m} className="flex items-center gap-1">
                <input
                  type="radio"
                  className="accent-[var(--color-accent)]"
                  checked={mode === m}
                  onChange={() => setMode(m)}
                />
                {m === "edges" ? "Start / stop" : "Centre / span"}
              </label>
            ))}
          </span>
        )}
      </div>

      {!multiple ? (
        <div className="mt-1">
          {mode === "edges" ? (
            <>
              <FrequencyRow label="Start" hz={single.startHz} disabled={disabled} onChange={(hz) => setSegment(0, { startHz: hz })} />
              <FrequencyRow label="Stop" hz={single.stopHz} disabled={disabled} onChange={(hz) => setSegment(0, { stopHz: hz })} />
            </>
          ) : (
            <>
              <FrequencyRow
                label="Centre"
                hz={centre}
                disabled={disabled}
                onChange={(hz) => setSegment(0, { startHz: hz - span / 2, stopHz: hz + span / 2 })}
              />
              <FrequencyRow
                label="Span"
                hz={span}
                disabled={disabled}
                onChange={(hz) => setSegment(0, { startHz: centre - hz / 2, stopHz: centre + hz / 2 })}
              />
            </>
          )}
          {span > 0 ? (
            <p className="caption ml-14 pl-1">
              {mode === "edges"
                ? `centre ${formatFrequencyShort(centre)}, span ${formatFrequencyShort(span)}`
                : `${formatFrequencyShort(single.startHz)} - ${formatFrequencyShort(single.stopHz)}`}
            </p>
          ) : (
            <p className="text-xs text-warning">stop must be above start</p>
          )}
        </div>
      ) : (
        <div className="mt-1">
          {segments.map((segment, i) => (
            <div key={i} className="flex items-center gap-1 py-0.5">
              <span className="w-5 shrink-0 text-dim">{i + 1}</span>
              <div className="w-24 shrink-0">
                <CommitField
                  value={mhz(segment.startHz)}
                  disabled={disabled}
                  onCommit={(text) => {
                    const hz = parseMhz(text);
                    return hz !== null && setSegment(i, { startHz: hz });
                  }}
                />
              </div>
              <span className="text-dim">-</span>
              <div className="w-24 shrink-0">
                <CommitField
                  value={mhz(segment.stopHz)}
                  disabled={disabled}
                  onCommit={(text) => {
                    const hz = parseMhz(text);
                    return hz !== null && setSegment(i, { stopHz: hz });
                  }}
                />
              </div>
              <span className="text-dim">MHz</span>
              <span className="text-xs text-dim tabular-nums">
                ({formatFrequencyShort(Math.max(segment.stopHz - segment.startHz, 0))})
              </span>
              {segments.length > 1 && (
                <button
                  className="btn-icon ml-auto h-7 w-7"
                  disabled={disabled}
                  title="Drop this range from the sweep"
                  onClick={() => apply(segments.filter((_, j) => j !== i))}
                >
                  <Icon path={icon.close} />
                </button>
              )}
            </div>
          ))}
          <button
            className="btn mt-1 w-full"
            disabled={disabled}
            onClick={() => {
              const lastStop = highestHz(segments);
              apply([...segments, { startHz: lastStop, stopHz: lastStop + 100e6 }]);
            }}
          >
            Add range
          </button>
          <p className="caption ml-6">
            {segments.length} ranges, {formatFrequencyShort(total)} covered
          </p>
        </div>
      )}

      {device && (
        <div className="mt-2 flex flex-col gap-1">
          <button
            className="btn w-full"
            disabled={disabled}
            title={`Sweep everything ${device.label} can tune`}
            onClick={() => {
              setMultipleWanted(false);
              apply([{ startHz: device.minFrequencyHz, stopHz: device.maxFrequencyHz }]);
            }}
          >
            Full device range ({formatFrequencyShort(device.minFrequencyHz)} - {formatFrequencyShort(device.maxFrequencyHz)})
          </button>
          {coverage.length > 0 && (
            <button
              className="btn w-full"
              disabled={disabled}
              title="Sweep only what the assigned antennas can hear"
              onClick={() => {
                let next: { startHz: number; stopHz: number }[] = [];
                for (const [startHz, stopHz] of coverage) {
                  next = addSegment(next, { startHz, stopHz });
                }
                setMultipleWanted(next.length > 1);
                apply(next);
              }}
            >
              {coverage.length === 1
                ? `Antenna range (${formatFrequencyShort(coverage[0]![0])} - ${formatFrequencyShort(coverage[0]![1])})`
                : `Antenna ranges (${coverage.length} ranges, ${formatFrequencyShort(coverage.reduce((sum, [a, b]) => sum + b - a, 0))})`}
            </button>
          )}
          {outside && <p className="text-xs text-warning">outside what this radio can tune</p>}
        </div>
      )}

      <div className="section-title">Presets</div>
      <div className="flex gap-1">
        <input
          className="field"
          value={presetName || defaultName}
          onChange={(e) => setPresetName(e.target.value)}
        />
        <button
          className="btn"
          title="Save the ranges above under this name"
          onClick={() => {
            const name = (presetName || defaultName).trim();
            if (name) {
              addPreset(name, segments);
              setPresetName("");
            }
          }}
        >
          Save
        </button>
      </div>
      <div className="mt-1">
        {presets.map((preset) => (
          <PresetRow
            key={`${preset.builtin ? "b" : "u"}:${preset.name}`}
            preset={preset}
            reachable={
              !device ||
              (lowestHz(preset.segments) >= device.minFrequencyHz && highestHz(preset.segments) <= device.maxFrequencyHz)
            }
            device={device?.label ?? "this radio"}
            disabled={disabled}
            onLoad={(add) => {
              if (add) {
                addPresetToPlan(preset);
              } else {
                setMultipleWanted(preset.segments.length > 1);
                apply(preset.segments);
              }
            }}
            onAdd={() => addPresetToPlan(preset)}
          />
        ))}
      </div>
    </div>
  );
}
