// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";
import { useState } from "react";

import { count, duration, frequencyField, frequencyShort, parseFrequency } from "../../render/format";
import {
  applyPipelineConfig,
  applySweepPlan,
  type InstrumentView,
  setFftBackend,
  setSweeping,
  useInstrument,
} from "../../state/instrument";
import { setView, viewStore } from "../../state/view";
import {
  type OverlapResolutionId,
  type PipelineConfig,
  type PortStrategyId,
  SweepMode,
  type SweepModeId,
  type SweepPlan,
  ThrottleMode,
  type ThrottleModeId,
  WindowType,
  type WindowTypeId,
} from "../../protocol/wire";
import { CommitField, Field, Readout, Segmented, Select, Switch, SwitchRow } from "../controls";
import { CorrectionsBlock } from "./analysis/CorrectionsBlock";
import {
  kFftSizes,
  listedFftSize,
  overlapResolutionChoices,
  portStrategyChoices,
  snapFftSize,
  throttleChoices,
  windowChoices,
  windowDescriptions,
  windowEnbw,
  withMode,
} from "./analysis/model";
import { TuneSlider } from "./analysis/TuneSlider";

const kAdvancedKey = "sweeppp.analysisAdvanced";

function loadAdvanced(): boolean {
  try {
    return localStorage.getItem(kAdvancedKey) === "1";
  } catch {
    return false;
  }
}

function saveAdvanced(advanced: boolean): void {
  try {
    localStorage.setItem(kAdvancedKey, advanced ? "1" : "0");
  } catch {
    // Not remembered.
  }
}

const fraction2 = (x: number) => x.toFixed(2);

/** The rate a fixed tune runs at: the radio's own, there being no plan. */
function fixedSampleRate(instrument: InstrumentView): number {
  if (!instrument.device.descriptor) {
    return 20e6;
  }
  const rate = instrument.values.parameters.get("sample_rate");
  return typeof rate === "number" || typeof rate === "bigint" ? Number(rate) : 20e6;
}

function rateLabel(hz: number): string {
  return `${parseFloat((hz / 1e6).toPrecision(6))} MS/s`;
}

/** Resolution, window, stitching, throughput and corrections: the desktop's
 * Analysis section. Plan edits while sweeping, pipeline edits at a fixed
 * tune; the window, overlap and averages go to both, as there. */
export function AnalysisPanel() {
  const instrument = useInstrument();
  const linkBins = useSelector(viewStore, (s) => s.linkBins);
  const [advanced, setAdvanced] = useState(loadAdvanced);
  const disabled = !instrument.canControl;
  const plan = instrument.plan;
  const config = instrument.pipeline;
  const schedule = instrument.schedule;
  const sweeping = instrument.run.sweeping;

  const sampleRate = sweeping ? plan.sampleRate : fixedSampleRate(instrument);
  const enbw = windowEnbw(config.window, config.windowBeta);
  const activePoints = sweeping ? schedule.fftSize : config.fftSize;
  const rbwFor = (points: number) => (sampleRate * enbw) / Math.max(points, 1);

  const editPlan = (change: Partial<SweepPlan>) => applySweepPlan({ ...plan, ...change });
  const editConfig = (change: Partial<PipelineConfig>) => applyPipelineConfig({ ...config, ...change });
  const editBoth = (planChange: Partial<SweepPlan>, configChange: Partial<PipelineConfig>) => {
    editPlan(planChange);
    editConfig(configChange);
  };

  return (
    <div>
      <div className="flex justify-end">
        <Segmented
          value={advanced ? "advanced" : "simple"}
          choices={[
            { value: "simple", label: "Simple" },
            { value: "advanced", label: "Advanced" },
          ]}
          onChange={(v) => {
            setAdvanced(v === "advanced");
            saveAdvanced(v === "advanced");
          }}
        />
      </div>

      <div className="section-title">Sweep</div>
      <SwitchRow>
        <div title={"Off: stay at one centre frequency.\nOn: step across the planned range."}>
          <Switch label="Sweep the planned range" checked={sweeping} disabled={disabled} onChange={setSweeping} />
        </div>
        {sweeping && (
          <div title="Switch connectors so each band goes through the antenna that covers it. Assign antennas in the device panel.">
            <Switch
              label="Route by antenna"
              checked={plan.antennaRouting}
              disabled={disabled}
              onChange={(on) => editPlan({ antennaRouting: on })}
            />
          </div>
        )}
      </SwitchRow>
      {sweeping && <SweepBlock instrument={instrument} disabled={disabled} />}

      <div className="section-title">Resolution</div>
      {sweeping && <SampleRateField instrument={instrument} disabled={disabled} />}
      {!advanced ? (
        <>
          <Field label="RBW" hint="Narrower resolves more detail but sweeps slower.">
            <CommitField
              value={frequencyField(sweeping ? plan.rbwHz : rbwFor(config.fftSize))}
              disabled={disabled}
              onCommit={(text) => {
                const hz = parseFrequency(text);
                if (hz === null || !(hz > 0)) {
                  return false;
                }
                const rbwHz = Math.max(hz, 1);
                if (sweeping) {
                  editPlan({ rbwHz });
                } else {
                  editConfig({ fftSize: snapFftSize((sampleRate * enbw) / rbwHz) });
                }
                return true;
              }}
            />
          </Field>
          <p className="caption">{count(activePoints)} points per transform</p>
        </>
      ) : (
        <>
          <Field label="FFT points" hint="RBW = sample rate × window ENBW / points">
            <Select
              value={`${listedFftSize(activePoints)}`}
              choices={kFftSizes.map((n) => ({ value: `${n}`, label: `${count(n)}  ·  ${frequencyShort(rbwFor(n))}` }))}
              disabled={disabled}
              onChange={(v) => {
                const points = Number(v);
                // The planner derives the size from the RBW, so a size is
                // asked for as the RBW that produces it.
                if (sweeping) {
                  editPlan({ rbwHz: rbwFor(points) });
                } else {
                  editConfig({ fftSize: points });
                }
              }}
            />
          </Field>
          <p className="caption">{frequencyShort(rbwFor(activePoints))} resolution bandwidth</p>
        </>
      )}

      {advanced && (
        <AdvancedBlock instrument={instrument} disabled={disabled} editPlan={editPlan} editConfig={editConfig} editBoth={editBoth} />
      )}

      <CorrectionsBlock instrument={instrument} disabled={disabled} />

      <div className="section-title">Network</div>
      <Field
        label="Network resolution"
        hint="How finely the spectrum crosses the network. Fewer bins keep a slow link live; each bin shown is the strongest of those it covers. Recording on the server keeps full resolution."
      >
        <Select
          value={`${linkBins}`}
          choices={[
            { value: "0", label: "Auto (follows the screen)" },
            { value: "-1", label: "Full" },
            { value: "262144", label: "262 144 bins" },
            { value: "65536", label: "65 536 bins" },
            { value: "16384", label: "16 384 bins" },
            ...([0, -1, 262144, 65536, 16384].includes(linkBins)
              ? []
              : [{ value: `${linkBins}`, label: `${count(linkBins)} bins` }]),
          ]}
          onChange={(v) => setView({ linkBins: Number(v) })}
        />
      </Field>
      <p className="caption">
        Now {instrument.link.maxBins > 0 ? `${count(instrument.link.maxBins)} bins` : "whole frames"}
      </p>

      {sweeping && schedule.stepCount > 0 && <Predicted instrument={instrument} />}
    </div>
  );
}

function SweepBlock({ instrument, disabled }: { instrument: InstrumentView; disabled: boolean }) {
  const plan = instrument.plan;
  const schedule = instrument.schedule;
  const portCount = instrument.device.descriptor?.rxPorts.length ?? 0;
  const fallback =
    instrument.device.descriptor !== null
      ? (instrument.assignments.fallbackPorts.find(([device]) => device === instrument.device.antennaKey)?.[1] ?? "")
      : "";
  const unrouted = schedule.unroutedHz.map(([from, to]) => `${frequencyShort(from)} - ${frequencyShort(to)}`).join(", ");
  return (
    <>
      <Field label="Priority" hint="Fast: one FFT per step. Detail: more averaging and overlap, slower.">
        <Segmented<`${SweepModeId}`>
          value={`${plan.mode}`}
          choices={[
            { value: `${SweepMode.Fast}`, label: "Fast" },
            { value: `${SweepMode.Detail}`, label: "Detail" },
          ]}
          disabled={disabled}
          onChange={(v) => applySweepPlan(withMode(plan, Number(v) as SweepModeId))}
        />
      </Field>
      {plan.antennaRouting && portCount > 1 && (
        <Field
          label="When both cover it"
          hint="Tightest fit: the narrowest-range antenna. Port order: the first connector. Most gain: the highest-gain antenna. Fewest switches: stay on a connector while it covers the band."
        >
          <Select
            value={`${plan.portStrategy}`}
            choices={portStrategyChoices}
            disabled={disabled}
            onChange={(v) => applySweepPlan({ ...plan, portStrategy: Number(v) as PortStrategyId })}
          />
        </Field>
      )}
      {plan.antennaRouting && unrouted && (
        <p className="caption text-warning">
          {fallback
            ? `no assigned antenna covers ${unrouted} — swept on ${fallback}`
            : `no assigned antenna covers ${unrouted} — swept through whatever is connected`}
        </p>
      )}
      {plan.antennaRouting && schedule.portSwitches > 0 && (
        <p className="caption">
          {schedule.portSwitches} port change{schedule.portSwitches === 1 ? "" : "s"} per pass
        </p>
      )}
    </>
  );
}

/** The plan's step width. The desktop sets it through the radio's sample
 * rate parameter, which while sweeping lands in the plan; this is that. */
function SampleRateField({ instrument, disabled }: { instrument: InstrumentView; disabled: boolean }) {
  const plan = instrument.plan;
  const rates = [...(instrument.device.descriptor?.supportedSampleRates ?? [])].sort((a, b) => a - b);
  if (rates.length === 0) {
    return <Readout rows={[["Sample rate", rateLabel(plan.sampleRate)]]} />;
  }
  if (!rates.includes(plan.sampleRate)) {
    rates.push(plan.sampleRate);
    rates.sort((a, b) => a - b);
  }
  return (
    <Field label="Sample rate" hint="Each step's width: faster covers the span in fewer retunes.">
      <Select
        value={`${plan.sampleRate}`}
        choices={rates.map((r) => ({ value: `${r}`, label: rateLabel(r) }))}
        disabled={disabled}
        onChange={(v) => applySweepPlan({ ...plan, sampleRate: Number(v) })}
      />
    </Field>
  );
}

function AdvancedBlock({
  instrument,
  disabled,
  editPlan,
  editConfig,
  editBoth,
}: {
  instrument: InstrumentView;
  disabled: boolean;
  editPlan: (change: Partial<SweepPlan>) => void;
  editConfig: (change: Partial<PipelineConfig>) => void;
  editBoth: (planChange: Partial<SweepPlan>, configChange: Partial<PipelineConfig>) => void;
}) {
  const plan = instrument.plan;
  const config = instrument.pipeline;
  const sweeping = instrument.run.sweeping;
  const backends = instrument.backends.backends;
  const current = instrument.backends.current;
  const available = backends.filter((b) => b.available);
  const unavailable = backends.filter((b) => !b.available);
  const backendChoices = available.map((b) => ({ value: b.name, label: b.displayName || b.name }));
  if (current && !backendChoices.some((c) => c.value === current)) {
    backendChoices.unshift({ value: current, label: backends.find((b) => b.name === current)?.displayName || current });
  }

  return (
    <>
      <div className="section-title">Window</div>
      <Field label="Window" hint={windowDescriptions[config.window]}>
        <Select
          value={`${config.window}`}
          choices={windowChoices}
          disabled={disabled}
          onChange={(v) => {
            const window = Number(v) as WindowTypeId;
            editBoth({ window }, { window });
          }}
        />
      </Field>
      {config.window === WindowType.Kaiser && (
        <TuneSlider
          label="Kaiser beta"
          value={config.windowBeta}
          format={(x) => x.toFixed(1)}
          min={0}
          max={20}
          step={0.1}
          disabled={disabled}
          onCommit={(windowBeta) => editBoth({ windowBeta }, { windowBeta })}
        />
      )}
      <TuneSlider
        label="Overlap"
        value={sweeping ? plan.fftOverlap : config.overlap}
        format={fraction2}
        min={0}
        max={0.9}
        step={0.05}
        hint="Fraction of samples shared between transforms. Higher costs more CPU."
        disabled={disabled}
        onCommit={(x) => editBoth({ fftOverlap: x }, { overlap: x })}
      />
      <TuneSlider
        label="Frame rate"
        value={config.targetFrameRate}
        format={(x) => `${x.toFixed(0)} fps`}
        min={5}
        max={144}
        step={1}
        hint="Display refresh cap. Does not limit what is measured."
        disabled={disabled}
        onCommit={(targetFrameRate) => editConfig({ targetFrameRate })}
      />
      <TuneSlider
        label="Averages"
        value={sweeping ? plan.averageCount : config.averageCount}
        format={(x) => `${x}`}
        min={1}
        max={64}
        step={1}
        hint="Lowers the noise floor; blunts short bursts."
        disabled={disabled}
        onCommit={(averageCount) => editBoth({ averageCount }, { averageCount })}
      />

      {sweeping && (
        <>
          <div className="section-title">Stitching</div>
          <TuneSlider
            label="Usable band"
            value={plan.usableBandwidthFraction}
            format={fraction2}
            min={0.4}
            max={1}
            step={0.01}
            hint="Fraction of each step kept, cropping the filter roll-off."
            disabled={disabled}
            onCommit={(usableBandwidthFraction) => editPlan({ usableBandwidthFraction })}
          />
          <TuneSlider
            label="Step overlap"
            value={plan.stepOverlap}
            format={fraction2}
            min={0}
            max={0.5}
            step={0.01}
            hint="Extra overlap so tuning error leaves no gaps between steps."
            disabled={disabled}
            onCommit={(stepOverlap) => editPlan({ stepOverlap })}
          />
          <TuneSlider
            label="LO guard"
            value={plan.dcGuardFraction}
            format={(x) => x.toFixed(3)}
            min={0}
            max={0.2}
            step={0.005}
            hint="Discards the LO leak at each step's centre. Roughly halves the sweep rate; zero leaves evenly spaced LO peaks. Once corrections are learned below, zero takes the speed back with the peaks removed."
            disabled={disabled}
            onCommit={(dcGuardFraction) => editPlan({ dcGuardFraction })}
          />
          <TuneSlider
            label="Dwell"
            value={plan.dwellSeconds}
            format={(x) => (x > 0 ? duration(x) : "none")}
            min={0}
            max={0.05}
            step={0.0005}
            hint="Time at each step beyond the retune settle."
            disabled={disabled}
            onCommit={(dwellSeconds) => editPlan({ dwellSeconds })}
          />
          <Field
            label="Where steps overlap"
            hint="Best: the step that measured it furthest from its LO and band edge. Max: the louder, which prints a peak at every step centre. Mean: their average."
          >
            <Select
              value={`${plan.overlapResolution}`}
              choices={overlapResolutionChoices}
              disabled={disabled}
              onChange={(v) => editPlan({ overlapResolution: Number(v) as OverlapResolutionId })}
            />
          </Field>
          <div title="Off: stop after one pass.">
            <Switch
              label="Continuous"
              checked={plan.continuous}
              disabled={disabled}
              onChange={(continuous) => editPlan({ continuous })}
            />
          </div>
        </>
      )}

      <div className="section-title">Throughput</div>
      <Field label="Backend" hint="FFT implementation. Switching restarts acquisition.">
        <Select
          value={current}
          choices={backendChoices}
          disabled={disabled || backendChoices.length === 0}
          onChange={(name) => {
            if (name !== current) {
              setFftBackend(name);
            }
          }}
        />
      </Field>
      {unavailable.map((b) => (
        <p key={b.name} className="caption">
          {b.displayName || b.name} unavailable: {b.unavailableReason}
        </p>
      ))}
      <Field label="Throttle" hint="Every Nth: process 1 in N buffers. Auto: as much as the CPU allows. All samples: never skip.">
        <Select
          value={`${config.throttleMode}`}
          choices={throttleChoices}
          disabled={disabled}
          onChange={(v) => editConfig({ throttleMode: Number(v) as ThrottleModeId })}
        />
      </Field>
      {config.throttleMode === ThrottleMode.EveryNth && (
        <TuneSlider
          label="N"
          value={config.everyNth}
          format={(x) => `${x}`}
          min={1}
          max={64}
          step={1}
          disabled={disabled}
          onCommit={(everyNth) => editConfig({ everyNth })}
        />
      )}
      <TuneSlider
        label="Workers"
        value={config.workerCount}
        format={(x) => (x === 0 ? "auto" : `${x}`)}
        min={0}
        max={16}
        step={1}
        hint="Auto: cores minus two."
        disabled={disabled}
        onCommit={(workerCount) => editConfig({ workerCount })}
      />
    </>
  );
}

function Predicted({ instrument }: { instrument: InstrumentView }) {
  const schedule = instrument.schedule;
  const link = instrument.link.maxBins;
  const rows: [string, string, string?][] = [
    ["steps", count(schedule.stepCount)],
    ["FFT size", count(schedule.fftSize)],
    ["actual RBW", frequencyShort(schedule.actualRbwHz)],
    ["pass time", duration(schedule.estimatedPassSeconds)],
    ["sweep rate", `${(schedule.estimatedSweepRateHzPerSec / 1e6).toFixed(1)} MHz/s`],
    [
      "retune cost",
      `${(schedule.retuneOverheadFraction * 100).toFixed(0)}% of the pass`,
      "Share of the pass spent retuning rather than measuring.",
    ],
  ];
  if (link > 0 && schedule.gridBinCount > link) {
    const group = Math.ceil(schedule.gridBinCount / link);
    rows.push([
      "over the network",
      `${count(Math.ceil(schedule.gridBinCount / group))} bins of ${frequencyShort(schedule.gridBinWidthHz * group)}`,
      "What crosses the network after Network resolution reduces it.",
    ]);
  }
  return (
    <>
      <div className="section-title">Predicted</div>
      <Readout rows={rows} />
    </>
  );
}
