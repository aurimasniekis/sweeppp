// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { Store, useSelector } from "@tanstack/react-store";

import {
  decodeProcessStats,
  defaultFftBenchmarkConfig,
  type FftBenchmarkConfig,
  type FftBenchmarkEntry,
  type FftBenchmarkSample,
  FftPlanQuality,
  type FftPlanQualityId,
  WindowType,
  type WindowTypeId,
} from "../../../protocol/wire";
import { count, frequencyShort } from "../../../render/format";
import {
  cancelBenchmark,
  type InstrumentView,
  setFftBackend,
  startBenchmark,
  useInstrument,
} from "../../../state/instrument";
import { session, sessionStore } from "../../../state/session";
import { type Choice, CommitField, Field, Select, Switch, SwitchRow } from "../../controls";
import { Bar, SectionTitle } from "./parts";

const kDepthSeconds = [0.1, 0.35, 1.0];

/** Tops of the size ladder: far enough to reach the sizes that give fine
 * resolution bandwidths, without charging every quick check for the slow
 * planners at a million points. */
const kLadderTops = [65536, 262144, 1048576];

type Metric = "p50" | "p99" | "max" | "throughput" | "cpu" | "plan";

const kMetrics: Choice<Metric>[] = [
  { value: "p50", label: "Per transform (p50)" },
  { value: "p99", label: "Tail (p99)" },
  { value: "max", label: "Worst (max)" },
  { value: "throughput", label: "Throughput" },
  { value: "cpu", label: "CPU cores" },
  { value: "plan", label: "Plan time" },
];

const kQualities: Choice<string>[] = [
  { value: String(FftPlanQuality.Fast), label: "Fast" },
  { value: String(FftPlanQuality.Balanced), label: "Balanced" },
  { value: String(FftPlanQuality.Thorough), label: "Thorough" },
];

interface BenchmarkChoices {
  depth: number;
  oneThread: boolean;
  allWorkers: boolean;
  top: number;
  metric: Metric;
  quality: FftPlanQualityId;
  minRuns: number;
  maxRuns: number;
  warmupRuns: number;
}

const defaults = defaultFftBenchmarkConfig();

/** Kept across the popover closing. */
const choicesStore = new Store<BenchmarkChoices>({
  depth: 1,
  oneThread: true,
  allWorkers: true,
  top: 0,
  metric: "p50",
  quality: defaults.quality,
  minRuns: defaults.minRuns,
  maxRuns: defaults.maxRuns,
  warmupRuns: defaults.warmupRuns,
});

const choose = (patch: Partial<BenchmarkChoices>) => choicesStore.setState((s) => ({ ...s, ...patch }));

/** Transforms are microseconds and planners hundreds of milliseconds: each in
 * its own unit, so the two stay comparable at a glance. */
function benchTime(seconds: number): string {
  if (seconds >= 1) return `${seconds.toFixed(2)} s`;
  if (seconds >= 1e-3) return `${(seconds * 1e3).toFixed(1)} ms`;
  return `${(seconds * 1e6).toFixed(2)} µs`;
}

/** Powers of four from 1024 to `top`. */
function ladder(top: number): number[] {
  const sizes: number[] = [];
  for (let size = 1024; size <= top; size *= 4) {
    sizes.push(size);
  }
  return sizes;
}

function besselI0(x: number): number {
  let sum = 1;
  let term = 1;
  const halfXSquared = (x * x) / 4;
  for (let k = 1; k < 64; ++k) {
    term *= halfXSquared / (k * k);
    sum += term;
    if (term < sum * 1e-17) {
      break;
    }
  }
  return sum;
}

/** Measured from the window itself, as the server does, so RBW reads as the
 * pipeline's own figure. */
function windowEnbw(type: WindowTypeId, beta: number): number {
  const size = 1024;
  const denominator = size - 1;
  const cosineSum: Partial<Record<WindowTypeId, number[]>> = {
    [WindowType.Hann]: [0.5, 0.5],
    [WindowType.Hamming]: [0.54, 0.46],
    [WindowType.BlackmanHarris]: [0.35875, 0.48829, 0.14128, 0.01168],
    [WindowType.FlatTop]: [0.21557895, 0.41663158, 0.277263158, 0.083578947, 0.006947368],
  };
  const a = cosineSum[type];
  let sum = 0;
  let sumSquares = 0;
  const i0Beta = besselI0(beta);
  for (let n = 0; n < size; ++n) {
    let w = 1;
    if (a) {
      const phase = (2 * Math.PI * n) / denominator;
      w = a.reduce((acc, c, i) => acc + (i % 2 === 0 ? 1 : -1) * c * Math.cos(i * phase), 0);
    } else if (type === WindowType.Kaiser) {
      const ratio = (2 * n) / denominator - 1;
      w = besselI0(beta * Math.sqrt(Math.max(0, 1 - ratio * ratio))) / i0Beta;
    }
    w = Math.fround(w);
    sum += w;
    sumSquares += w * w;
  }
  return sum === 0 ? 1 : (size * sumSquares) / (sum * sum);
}

function metricValue(sample: FftBenchmarkSample, metric: Metric): number {
  switch (metric) {
    case "p50":
      return sample.p50Seconds;
    case "p99":
      return sample.p99Seconds;
    case "max":
      return sample.maxSeconds;
    case "throughput":
      return sample.throughputPerSecond;
    case "cpu":
      return sample.cpuCores;
    case "plan":
      return sample.planSeconds;
  }
}

/** Fewer cores is not better if they bought throughput, so CPU has no winner. */
const lowerIsBetter = (metric: Metric) => metric !== "throughput" && metric !== "cpu";

function formatMetric(sample: FftBenchmarkSample, metric: Metric): string {
  const value = metricValue(sample, metric);
  if (metric === "throughput") return `${value.toFixed(0)}/s`;
  if (metric === "cpu") return value < 0 ? "-" : value.toFixed(2);
  return benchTime(value);
}

function sampleAt(entry: FftBenchmarkEntry, size: number, threads: number): FftBenchmarkSample | undefined {
  return entry.samples.find((s) => s.size === size && s.threads === threads);
}

/** Every (size, threads) any backend reported, ascending: what actually ran,
 * a cancelled run included. */
function benchmarkRows(entries: FftBenchmarkEntry[]): [number, number][] {
  const keys = new Map<string, [number, number]>();
  for (const entry of entries) {
    for (const s of entry.samples) {
      keys.set(`${s.size}:${s.threads}`, [s.size, s.threads]);
    }
  }
  return [...keys.values()].sort((a, b) => a[0] - b[0] || a[1] - b[1]);
}

function fastestAt(entries: FftBenchmarkEntry[], size: number, threads: number): FftBenchmarkEntry | undefined {
  let winner: FftBenchmarkEntry | undefined;
  let best = 0;
  for (const entry of entries) {
    const sample = sampleAt(entry, size, threads);
    if (sample && !sample.skipped && sample.throughputPerSecond > best) {
      best = sample.throughputPerSecond;
      winner = entry;
    }
  }
  return winner;
}

function sampleRateOf(instrument: InstrumentView): number {
  if (instrument.run.sweeping) {
    return instrument.plan.sampleRate;
  }
  const rate = instrument.values.parameters.get("sample_rate");
  return instrument.device.descriptor && (typeof rate === "number" || typeof rate === "bigint") ? Number(rate) : 20e6;
}

function buildConfig(choices: BenchmarkChoices, fftSize: number, workers: number): FftBenchmarkConfig {
  // The configured size always, wherever the ladder stops: it is the one this
  // installation runs.
  const sizes = [...new Set([...ladder(kLadderTops[choices.top] ?? kLadderTops[0]!), fftSize])].sort((a, b) => a - b);
  const threadCounts: number[] = [];
  if (choices.oneThread) threadCounts.push(1);
  if (choices.allWorkers && workers > 1) threadCounts.push(workers);
  return {
    sizes,
    threadCounts: threadCounts.length > 0 ? threadCounts : [1],
    quality: choices.quality,
    secondsPerSample: kDepthSeconds[choices.depth] ?? kDepthSeconds[1]!,
    minRuns: choices.minRuns,
    maxRuns: Math.max(choices.maxRuns, choices.minRuns),
    warmupRuns: choices.warmupRuns,
  };
}

const parseCount = (text: string, minimum: number): number | null => {
  const value = Number(text.trim().replaceAll("_", "").replaceAll(" ", ""));
  return Number.isInteger(value) && value >= minimum ? value : null;
};

export function BenchmarkTab() {
  const instrument = useInstrument();
  const choices = useSelector(choicesStore, (s) => s);
  const serverName = useSelector(sessionStore, (s) => s.serverName);
  const status = instrument.benchmark;
  const running = status.running;
  const watching = !instrument.canControl;
  const pipeline = instrument.pipeline;

  const workers =
    pipeline.workerCount > 0
      ? pipeline.workerCount
      : Math.max(decodeProcessStats(session.telemetry.getHash("process")).workerCount, 1);
  const sampleRate = sampleRateOf(instrument);
  const enbw = windowEnbw(pipeline.window, pipeline.windowBeta);
  const rbwFor = (size: number) => (size === 0 ? 0 : (sampleRate * enbw) / size);

  const config = buildConfig(choices, pipeline.fftSize, workers);
  const available = instrument.backends.backends.filter((b) => b.available).length;
  const steps = available * config.sizes.length * Math.max(config.threadCounts.length, 1);

  const setCount = (key: "minRuns" | "maxRuns" | "warmupRuns", minimum: number) => (text: string) => {
    const value = parseCount(text, minimum);
    if (value === null) {
      return false;
    }
    choose({ [key]: value });
    return true;
  };

  return (
    <div>
      <p className="caption mb-1">Times every available backend on {serverName || "the server"}, where the transforms run.</p>

      <div className="grid grid-cols-2 gap-x-3">
        <Field label="Depth" hint="Time spent on each measurement.">
          <Select
            value={String(choices.depth)}
            choices={[
              { value: "0", label: "Quick" },
              { value: "1", label: "Normal" },
              { value: "2", label: "Thorough" },
            ]}
            onChange={(v) => choose({ depth: Number(v) })}
            disabled={running}
          />
        </Field>
        <Field label="Sizes" hint="Largest FFT size to measure.">
          <Select
            value={String(choices.top)}
            choices={kLadderTops.map((top, i) => ({
              value: String(i),
              label: `to ${count(top)} (${frequencyShort(rbwFor(top))})`,
            }))}
            onChange={(v) => choose({ top: Number(v) })}
            disabled={running}
          />
        </Field>
      </div>

      <Field label="Threads">
        <SwitchRow>
          <Switch
            label="1"
            checked={choices.oneThread}
            disabled={running}
            onChange={(on) => choose(on ? { oneThread: true } : { oneThread: false, allWorkers: true })}
          />
          <Switch
            label={`${workers} workers`}
            checked={choices.allWorkers}
            disabled={running}
            onChange={(on) => choose(on ? { allWorkers: true } : { allWorkers: false, oneThread: true })}
          />
        </SwitchRow>
      </Field>

      <details className="py-1">
        <summary className="cursor-pointer text-xs text-dim select-none">Runs and planning</summary>
        <div className="grid grid-cols-2 gap-x-3">
          <Field label="Plan quality">
            <Select
              value={String(choices.quality)}
              choices={kQualities}
              onChange={(v) => choose({ quality: Number(v) as FftPlanQualityId })}
              disabled={running}
            />
          </Field>
          <Field label="Warmup runs">
            <CommitField
              value={String(choices.warmupRuns)}
              onCommit={setCount("warmupRuns", 0)}
              disabled={running}
              inputMode="numeric"
            />
          </Field>
          <Field label="Min runs">
            <CommitField
              value={String(choices.minRuns)}
              onCommit={setCount("minRuns", 1)}
              disabled={running}
              inputMode="numeric"
            />
          </Field>
          <Field label="Max runs">
            <CommitField
              value={String(choices.maxRuns)}
              onCommit={setCount("maxRuns", 1)}
              disabled={running}
              inputMode="numeric"
            />
          </Field>
        </div>
      </details>

      <div className="mt-1 flex items-center gap-2">
        <button className="btn" disabled={running || watching} onClick={() => startBenchmark(config)}>
          {running ? "Running…" : "Run benchmark"}
        </button>
        {running && (
          <button className="btn" disabled={watching} onClick={cancelBenchmark}>
            Cancel
          </button>
        )}
        <span className="caption">
          {running
            ? benchTime(status.elapsedSeconds)
            : `${steps} measurements, about ${benchTime(steps * config.secondsPerSample)} plus planning`}
        </span>
      </div>

      {running && (
        <Bar
          fraction={status.stepsTotal === 0 ? 0 : status.stepsDone / status.stepsTotal}
          label={status.currentStep || `${status.stepsDone} / ${status.stepsTotal}`}
        />
      )}

      {instrument.run.running && (
        <p className="mt-1 text-xs text-warning">A sweep is running and will skew the results.</p>
      )}

      {status.results.length === 0 ? (
        <p className="caption mt-2">{running ? "Measuring…" : "No results yet."}</p>
      ) : (
        <Results
          results={status.results}
          metric={choices.metric}
          rbwFor={rbwFor}
          sampleRate={sampleRate}
          fftSize={pipeline.fftSize}
          currentBackend={instrument.backends.current}
          watching={watching}
        />
      )}
    </div>
  );
}

function Results({
  results,
  metric,
  rbwFor,
  sampleRate,
  fftSize,
  currentBackend,
  watching,
}: {
  results: FftBenchmarkEntry[];
  metric: Metric;
  rbwFor: (size: number) => number;
  sampleRate: number;
  fftSize: number;
  currentBackend: string;
  watching: boolean;
}) {
  const rows = benchmarkRows(results);
  const widest = Math.max(1, ...rows.map(([, threads]) => threads));
  // Plans have no thread axis: the pipeline builds one and shares it.
  const perThread = metric !== "plan";
  const shown = perThread ? rows : rows.filter(([size], i) => i === 0 || rows[i - 1]![0] !== size);
  const rbwHint = `at ${frequencyShort(sampleRate)} and the current window`;
  const forThisSize = fastestAt(results, fftSize, widest);

  return (
    <>
      <SectionTitle title="Results" />
      <Field label="Show">
        <Select value={metric} choices={kMetrics} onChange={(m) => choose({ metric: m })} />
      </Field>
      <div className="overflow-x-auto">
        <table className="w-full border-collapse text-xs tabular-nums">
          <thead>
            <tr className="text-dim">
              <th className="border border-border px-1.5 py-1 text-left font-normal">Size</th>
              <th className="border border-border px-1.5 py-1 text-left font-normal">RBW</th>
              {perThread && <th className="border border-border px-1.5 py-1 text-left font-normal">Threads</th>}
              {results.map((entry) => (
                <th
                  key={entry.backend}
                  className="border border-border px-1.5 py-1 text-left font-normal"
                  title={entry.displayName}
                >
                  {entry.backend}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {shown.map(([size, threads]) => {
              // The row's best, so the eye finds the winner without comparing.
              let best = 0;
              if (metric !== "cpu") {
                for (const entry of results) {
                  const sample = sampleAt(entry, size, threads);
                  const value = sample && !sample.skipped ? metricValue(sample, metric) : 0;
                  if (value > 0 && (best === 0 || (lowerIsBetter(metric) ? value < best : value > best))) {
                    best = value;
                  }
                }
              }
              return (
                <tr key={`${size}:${threads}`} className="even:bg-header">
                  <td className="border border-border px-1.5 py-0.5">{count(size)}</td>
                  <td className="border border-border px-1.5 py-0.5" title={rbwHint}>
                    {frequencyShort(rbwFor(size))}
                  </td>
                  {perThread && <td className="border border-border px-1.5 py-0.5">{threads}</td>}
                  {results.map((entry) => {
                    const sample = sampleAt(entry, size, threads);
                    if (!sample || sample.skipped) {
                      return (
                        <td key={entry.backend} className="border border-border px-1.5 py-0.5 text-dim" title={sample?.skipped}>
                          -
                        </td>
                      );
                    }
                    const value = metricValue(sample, metric);
                    const winner = best > 0 && value === best;
                    const factor = lowerIsBetter(metric) ? value / best : best / value;
                    return (
                      <td
                        key={entry.backend}
                        className={`whitespace-nowrap border border-border px-1.5 py-0.5 ${winner ? "text-ok" : ""}`}
                        title={
                          `p50 ${benchTime(sample.p50Seconds)} | p99 ${benchTime(sample.p99Seconds)} | max ${benchTime(sample.maxSeconds)}\n` +
                          `${sample.throughputPerSecond.toFixed(0)} FFT/s | ${sample.cpuCores.toFixed(2)} cores | ${sample.runs} runs\n` +
                          `plan ${benchTime(sample.planSeconds)}`
                        }
                      >
                        {formatMetric(sample, metric)}
                        {!winner && best > 0 && value > 0 && <span className="text-dim"> {factor.toFixed(2)}x</span>}
                      </td>
                    );
                  })}
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>

      {results
        .filter((entry) => entry.error)
        .map((entry) => (
          <p key={entry.backend} className="mt-1 text-xs text-danger">
            {entry.backend}: {entry.error}
          </p>
        ))}

      <SectionTitle title="Verdict" />
      {results.length < 2 ? (
        <p className="caption">Only one backend is installed; there is nothing to compare.</p>
      ) : (
        <>
          <p className="caption">At {widest} threads, the arrangement a sweep uses.</p>
          <ul className="mt-1 list-disc pl-5 text-xs">
            {rows
              .filter(([, threads]) => threads === widest)
              .map(([size, threads]) => {
                const winner = fastestAt(results, size, threads);
                const best = winner && sampleAt(winner, size, threads);
                const runnerUp = Math.max(
                  0,
                  ...results
                    .filter((entry) => entry !== winner)
                    .map((entry) => sampleAt(entry, size, threads))
                    .filter((sample) => sample && !sample.skipped)
                    .map((sample) => sample!.throughputPerSecond),
                );
                if (!winner || !best || runnerUp <= 0) {
                  return null;
                }
                return (
                  <li key={size}>
                    {count(size)} points ({frequencyShort(rbwFor(size))} RBW): {winner.backend}, by{" "}
                    {(best.throughputPerSecond / runnerUp).toFixed(2)}x
                  </li>
                );
              })}
          </ul>
        </>
      )}

      {forThisSize && (
        <div className="mt-2 flex items-center gap-2">
          {currentBackend === forThisSize.backend ? (
            <span className="caption">
              {forThisSize.backend} is already selected, and wins at the configured size of {fftSize}.
            </span>
          ) : (
            <>
              <button className="btn" disabled={watching} onClick={() => setFftBackend(forThisSize.backend)}>
                Use {forThisSize.backend}
              </button>
              <span className="caption">fastest at the configured size of {fftSize}</span>
            </>
          )}
        </div>
      )}
    </>
  );
}
