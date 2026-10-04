// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { readFileSync } from "node:fs";
import { join } from "node:path";

import { describe, expect, it } from "vitest";

import { decodeClients, decodeControl, decodeMessage, decodeRecordings, decodeState, msg, section } from "./messages";
import { Metadata, value, ValueType } from "./metadata";
import { kStreamHeaderBytes, RecordFramer, RecordType } from "./records";
import * as wire from "./wire";
import {
  type Antenna,
  type BenchmarkStatus,
  type DeviceDescriptor,
  type FftBackendInfo,
  type FftBenchmarkConfig,
  type InstrumentNotice,
  type PipelineConfig,
  type ProcessStats,
  type RfLegView,
  type ScheduleSummary,
  type SdrHealthReading,
  type StreamStats,
  type SwitcherView,
  type SweepPlan,
  FftBackendType,
  FftPlanQuality,
  FftSizeConstraint,
  kNoInput,
  NoticeKind,
  OverlapResolution,
  PortStrategy,
  SdrParameterType,
  SweepMode,
  ThrottleMode,
  ThrottleReason,
  WindowType,
} from "./wire";

const directory = process.env.SWEEPPP_WEB_FIXTURES;

const antenna: Antenna = {
  id: "d190",
  name: "Diamond D-190",
  category: "Wideband",
  type: "discone",
  startHz: 100e6,
  stopHz: 1.5e9,
  gainDbi: 2.15,
  needsBiasT: true,
  notes: "on the roof",
  builtin: true,
};

const device: DeviceDescriptor = {
  info: {
    driver: "hackrf",
    id: "0000000000000000a06063c8234e925f",
    label: "HackRF One r9",
    serial: "a06063c8234e925f",
    hardwareRevision: "r9",
    firmware: { version: "2024.02.1", knownLatest: "2024.02.1", aheadOfDriver: false },
    fpga: { version: "", knownLatest: "1.0", aheadOfDriver: true },
    minFrequencyHz: 1e6,
    maxFrequencyHz: 6e9,
    minSampleRate: 2e6,
    maxSampleRate: 20e6,
    linkCapacityBytesPerSec: 40_000_000,
    linkDescription: "USB 2.0 High-Speed",
  },
  parameters: [
    {
      key: "lna_gain",
      label: "LNA gain",
      group: "Gain",
      type: SdrParameterType.Int,
      unit: "dB",
      min: 0,
      max: 40,
      step: 8,
      enumValues: [],
      readOnly: false,
      gridAffecting: false,
      calibrationAffecting: true,
      requiresStop: false,
      description: "IF amplifier",
      defaultValue: 16n,
      appliesWhenKey: "gain_mode",
      appliesWhenValues: ["manual", "hybrid"],
    },
    {
      key: "sample_rate",
      label: "Sample rate",
      group: "Tuning",
      type: SdrParameterType.Enum,
      unit: "S/s",
      min: 2e6,
      max: 20e6,
      step: 0,
      enumValues: [
        { value: "20000000", label: "20 MS/s", description: "" },
        { value: "10000000", label: "10 MS/s", description: "half" },
      ],
      readOnly: true,
      gridAffecting: true,
      calibrationAffecting: false,
      requiresStop: true,
      description: "",
      defaultValue: 20e6,
      appliesWhenKey: "",
      appliesWhenValues: [],
    },
    {
      key: "amp",
      label: "RF amp",
      group: "Gain",
      type: SdrParameterType.Bool,
      unit: "",
      min: 0,
      max: 0,
      step: 0,
      enumValues: [],
      readOnly: false,
      gridAffecting: false,
      calibrationAffecting: true,
      requiresStop: false,
      description: "",
      defaultValue: true,
      appliesWhenKey: "",
      appliesWhenValues: [],
    },
  ],
  rxPorts: [
    {
      id: "rx1",
      label: "RX1",
      connector: "SMA (J1)",
      minHz: 70e6,
      maxHz: 6e9,
      biasTee: true,
      requiresStop: true,
      switchSeconds: 0.25,
    },
  ],
  supportedSampleRates: [2e6, 8e6, 20e6],
};

const plan: SweepPlan = {
  name: "ISM",
  segments: [
    { startHz: 2.4e9, stopHz: 2.5e9, dwellSeconds: 0 },
    { startHz: 433e6, stopHz: 435e6, dwellSeconds: 0.01 },
  ],
  mode: SweepMode.Detail,
  rbwHz: 12_345.678,
  sampleRate: 10e6,
  usableBandwidthFraction: 0.6,
  stepOverlap: 0.1,
  dcGuardFraction: 0,
  dwellSeconds: 0.002,
  averageCount: 8,
  window: WindowType.Kaiser,
  windowBeta: 12.5,
  fftOverlap: 0.5,
  overlapResolution: OverlapResolution.Mean,
  continuous: false,
  antennaRouting: true,
  portStrategy: PortStrategy.FewestSwitches,
};

const pipeline: PipelineConfig = {
  fftSize: 16384,
  window: WindowType.FlatTop,
  windowBeta: 3,
  overlap: 0.75,
  workerCount: 6,
  throttleMode: ThrottleMode.EveryNth,
  everyNth: 3,
  averageCount: 4,
  planQuality: FftPlanQuality.Thorough,
  targetFrameRate: 30,
  dbfsToDbmOffset: -12.5,
};

const schedule: ScheduleSummary = {
  stepCount: 42,
  fftSize: 2048,
  actualRbwHz: 9765.625,
  gridStartHz: 433e6,
  gridBinWidthHz: 4882.8125,
  gridBinCount: 424_000,
  estimatedPassSeconds: 0.37,
  estimatedSweepRateHzPerSec: 5.4e9,
  retuneOverheadFraction: 0.31,
  unroutedHz: [
    [433e6, 435e6],
    [2.45e9, 2.5e9],
  ],
  portSwitches: 2,
};

const backend: FftBackendInfo = {
  name: "vdsp",
  displayName: "Accelerate vDSP",
  description: "Apple's own",
  capabilities: {
    type: FftBackendType.Gpu,
    minSize: 16,
    maxSize: 1 << 20,
    sizeConstraint: FftSizeConstraint.PowerOfTwo,
    supportsBatch: true,
    supportsInPlace: false,
    threadSafeExecute: true,
    threadSafePlanning: true,
  },
  available: false,
  unavailableReason: "not on this machine",
  isSuggestedDefault: true,
};

const switcher: SwitcherView = {
  key: "minicircuits:12345",
  info: {
    driver: "minicircuits",
    id: "12345",
    label: "Mini-Circuits USB-1SP8T-63H",
    model: "USB-1SP8T-63H",
    serial: "12345",
    inputCount: 8,
    requiresStop: true,
    switchSeconds: 0.001,
  },
  inputs: [
    { id: "in1", label: "J1", minHz: 1e6, maxHz: 6e9 },
    { id: "in2", label: "J2", minHz: 0, maxHz: 0 },
  ],
  selectedInput: 1,
};

const leg: RfLegView = {
  route: {
    portIndex: 1,
    id: "rx2",
    inputIndex: 3,
    startHz: 100e6,
    stopHz: 1.5e9,
    gainDbi: 2.15,
    switchSeconds: 0.25,
    inputSwitchSeconds: 0.001,
  },
  switcherKey: "minicircuits:12345",
  antenna,
  portLabel: "RX2 via J4",
  live: true,
};

const reading: SdrHealthReading = {
  label: "Temperature",
  value: "41.3 °C",
  numeric: Math.fround(41.3),
  minimum: Math.fround(-10),
  maximum: Math.fround(85.5),
  alarm: true,
};

const streamStats: StreamStats = {
  configuredSps: 20e6,
  measuredSps: 19_999_871.5,
  bytesPerSecIn: 40e6,
  linkCapacityBytesPerSec: 60e6,
  linkUtilisation: 0.66,
  samplesDelivered: 2n ** 60n + 7n,
  samplesDropped: 12n,
  samplesLostAtSource: 3n,
  deviceOverruns: 1n,
  ringFullEvents: 2n,
  poolExhaustedEvents: 4n,
  sequenceGaps: 2n ** 64n - 1n,
  dropRatePerSec: 0.5,
  dropFraction: 1e-6,
  ringFillFraction: Math.fround(0.3),
};

const processStats: ProcessStats = {
  fftsPerSec: 12_000.5,
  fftsComputed: 9_007_199_254_740_993n,
  fftsSkipped: 17n,
  samplesProcessedTotal: 2n ** 63n,
  processedFraction: 0.98,
  framesPerSec: 59.9,
  sweepPassesCompleted: 1234n,
  retunesPerSec: 310,
  fftLatencyP50Us: Math.fround(12.3),
  fftLatencyP99Us: Math.fround(45.6),
  fftLatencyMaxUs: Math.fround(789.1),
  workerUtilisation: 0.42,
  workerCount: 6,
  throttleReason: ThrottleReason.CpuLimited,
  sweepSpeedHzPerSec: 4.2e9,
};

const benchmarkConfig: FftBenchmarkConfig = {
  sizes: [256, 1024, 8192],
  threadCounts: [1, 4],
  quality: FftPlanQuality.Fast,
  secondsPerSample: 0.2,
  minRuns: 4,
  maxRuns: 1000,
  warmupRuns: 2,
};

const benchmark: BenchmarkStatus = {
  running: true,
  complete: false,
  stepsDone: 3,
  stepsTotal: 8,
  currentStep: "vdsp at 4096",
  elapsedSeconds: 1.25,
  results: [
    {
      backend: "vdsp",
      displayName: "Accelerate vDSP",
      samples: [
        {
          size: 4096,
          threads: 4,
          planSeconds: 0.001,
          p50Seconds: 2e-6,
          p99Seconds: 5e-6,
          maxSeconds: 1e-4,
          throughputPerSecond: 1.9e6,
          cpuCores: 3.9,
          runs: 50_000,
          skipped: "",
        },
        {
          size: 1 << 20,
          threads: 1,
          planSeconds: 0,
          p50Seconds: 0,
          p99Seconds: 0,
          maxSeconds: 0,
          throughputPerSecond: 0,
          cpuCores: -1,
          runs: 0,
          skipped: "too large",
        },
      ],
      error: "",
    },
    { backend: "cufft", displayName: "cuFFT", samples: [], error: "no device" },
  ],
};

const notice: InstrumentNotice = { kind: NoticeKind.Condition, text: "link saturated" };

/** Every encoder with its decoder, each with a value nothing defaults to. */
const pairs: { name: string; encode: (v: never) => Metadata; decode: (m: Metadata) => unknown; sample: unknown }[] = [
  { name: "device", encode: wire.encodeDevice, decode: wire.decodeDevice, sample: device },
  { name: "info", encode: wire.encodeInfo, decode: wire.decodeInfo, sample: device.info },
  { name: "parameter", encode: wire.encodeParameter, decode: wire.decodeParameter, sample: device.parameters[1] },
  { name: "rxPort", encode: wire.encodeRxPort, decode: wire.decodeRxPort, sample: device.rxPorts[0] },
  { name: "plan", encode: wire.encodePlan, decode: wire.decodePlan, sample: plan },
  { name: "segment", encode: wire.encodeSegment, decode: wire.decodeSegment, sample: plan.segments[1] },
  { name: "pipeline", encode: wire.encodePipeline, decode: wire.decodePipeline, sample: pipeline },
  {
    name: "correctionSettings",
    encode: wire.encodeCorrectionSettings,
    decode: wire.decodeCorrectionSettings,
    sample: { dcRemoval: false, flatten: true, spurMask: false, autoSpurs: true },
  },
  {
    name: "correctionSummary",
    encode: wire.encodeCorrectionSummary,
    decode: wire.decodeCorrectionSummary,
    sample: {
      present: true,
      floorPoints: 4096,
      spurs: 5,
      automaticSpurs: 2,
      learnedAt: "2026-10-04T09:00:00Z",
      floorStaleReason: "gain changed",
    },
  },
  { name: "schedule", encode: wire.encodeSchedule, decode: wire.decodeSchedule, sample: schedule },
  {
    name: "engineStats",
    encode: wire.encodeEngineStats,
    decode: wire.decodeEngineStats,
    sample: {
      stitched: 2n ** 64n - 2n,
      unsettled: 3n,
      unattributed: 4n,
      tooShort: 5n,
      lastPassCoverage: 0.995,
      measuredSweepRateHzPerSec: 3.3e9,
      passCount: 2n ** 53n + 1n,
    },
  },
  { name: "backend", encode: wire.encodeBackend, decode: wire.decodeBackend, sample: backend },
  { name: "backends", encode: wire.encodeBackends, decode: wire.decodeBackends, sample: [backend, backend] },
  { name: "antenna", encode: wire.encodeAntenna, decode: wire.decodeAntenna, sample: antenna },
  { name: "antennas", encode: wire.encodeAntennas, decode: wire.decodeAntennas, sample: [antenna] },
  {
    name: "assignments",
    encode: wire.encodeAssignments,
    decode: wire.decodeAssignments,
    sample: {
      entries: [
        { device: "hackrf:abc", port: "rx1", switcher: "", input: "", antenna: "d190" },
        { device: "", port: "", switcher: "minicircuits:12345", input: "in2", antenna: "yagi" },
      ],
      fallbackPorts: [["hackrf:abc", "rx2"]],
    },
  },
  { name: "switcherView", encode: wire.encodeSwitcherView, decode: wire.decodeSwitcherView, sample: switcher },
  { name: "switcherViews", encode: wire.encodeSwitcherViews, decode: wire.decodeSwitcherViews, sample: [switcher] },
  {
    name: "switcherInfos",
    encode: wire.encodeSwitcherInfos,
    decode: wire.decodeSwitcherInfos,
    sample: [switcher.info, { ...switcher.info, id: "2" }],
  },
  { name: "route", encode: wire.encodeRoute, decode: wire.decodeRoute, sample: { ...leg.route, inputIndex: kNoInput } },
  { name: "rfLeg", encode: wire.encodeRfLeg, decode: wire.decodeRfLeg, sample: leg },
  { name: "rfLegs", encode: wire.encodeRfLegs, decode: wire.decodeRfLegs, sample: [leg, leg] },
  { name: "ranges", encode: wire.encodeRanges, decode: wire.decodeRanges, sample: schedule.unroutedHz },
  { name: "health", encode: wire.encodeHealth, decode: wire.decodeHealth, sample: [reading] },
  { name: "streamStats", encode: wire.encodeStreamStats, decode: wire.decodeStreamStats, sample: streamStats },
  { name: "processStats", encode: wire.encodeProcessStats, decode: wire.decodeProcessStats, sample: processStats },
  {
    name: "benchmarkConfig",
    encode: wire.encodeBenchmarkConfig,
    decode: wire.decodeBenchmarkConfig,
    sample: benchmarkConfig,
  },
  {
    name: "benchmarkStatus",
    encode: wire.encodeBenchmarkStatus,
    decode: wire.decodeBenchmarkStatus,
    sample: benchmark,
  },
  { name: "notice", encode: wire.encodeNotice, decode: wire.decodeNotice, sample: notice },
  {
    name: "deviceSection",
    encode: wire.encodeDeviceSection,
    decode: wire.decodeDeviceSection,
    sample: {
      descriptor: device,
      antennaKey: "hackrf:a06063c8234e925f",
      profileDriver: "hackrf",
      profileId: "a06063c8234e925f",
      displayLabel: "HackRF One on pi",
    },
  },
  {
    name: "deviceSection without a radio",
    encode: wire.encodeDeviceSection,
    decode: wire.decodeDeviceSection,
    sample: { descriptor: null, antennaKey: "", profileDriver: "", profileId: "", displayLabel: "none" },
  },
  {
    name: "valuesSection",
    encode: wire.encodeValuesSection,
    decode: wire.decodeValuesSection,
    sample: {
      parameters: new Map<string, wire.SdrValue>([
        ["amp", false],
        ["lna_gain", -24n],
        ["sample_rate", 20e6],
        ["gain_mode", "manual"],
      ]),
      selectedRxPort: "rx1",
    },
  },
  {
    name: "runSection",
    encode: wire.encodeRunSection,
    decode: wire.decodeRunSection,
    sample: {
      running: true,
      sweeping: true,
      startGeneration: 2n ** 63n + 5n,
      engine: {
        stitched: 1n,
        unsettled: 2n,
        unattributed: 3n,
        tooShort: 4n,
        lastPassCoverage: 1,
        measuredSweepRateHzPerSec: 2,
        passCount: 6n,
      },
    },
  },
  {
    name: "backendsSection",
    encode: wire.encodeBackendsSection,
    decode: wire.decodeBackendsSection,
    sample: { backends: [backend], current: "vdsp" },
  },
  {
    name: "correctionsSection",
    encode: wire.encodeCorrectionsSection,
    decode: wire.decodeCorrectionsSection,
    sample: {
      settings: { dcRemoval: false, flatten: false, spurMask: false, autoSpurs: true },
      summary: { present: true, floorPoints: 1, spurs: 2, automaticSpurs: 3, learnedAt: "x", floorStaleReason: "" },
    },
  },
  {
    name: "learningSection",
    encode: wire.encodeLearningSection,
    decode: wire.decodeLearningSection,
    sample: { active: true, label: "learning the floor" },
  },
  {
    name: "switchersSection",
    encode: wire.encodeSwitchersSection,
    decode: wire.decodeSwitchersSection,
    sample: { open: [switcher], available: [switcher.info] },
  },
  {
    name: "rfPathSection",
    encode: wire.encodeRfPathSection,
    decode: wire.decodeRfPathSection,
    sample: { legs: [leg], coverage: [[100e6, 1.5e9]] },
  },
  { name: "linkSection", encode: wire.encodeLinkSection, decode: wire.decodeLinkSection, sample: { maxBins: 4096 } },
  {
    name: "recordingsSection",
    encode: wire.encodeRecordingsSection,
    decode: decodeRecordings,
    sample: {
      available: true,
      active: true,
      current: "now.sweeps",
      lines: 1200,
      bytes: 5_000_000,
      files: [{ name: "then.sweeps", bytes: 1024, modifiedWallNs: 1_759_568_400_000_000_000n }],
    },
  },
  {
    name: "controlSection",
    encode: wire.encodeControlSection,
    decode: decodeControl,
    sample: { shared: true, you: false, held: true, controller: "bench", controllerKind: "desktop" },
  },
  {
    name: "clientsSection",
    encode: wire.encodeClientsSection,
    decode: decodeClients,
    sample: [
      { id: 1, name: "bench", kind: "desktop", address: "10.0.0.2:5000", controls: true, you: false },
      { id: 2, name: "browser", kind: "web", address: "10.0.0.3:5001", controls: false, you: true },
    ],
  },
];

describe("the wire codec", () => {
  it.each(pairs)("round-trips $name", ({ encode, decode, sample }) => {
    const encoded = encode(sample as never);
    expect(decode(encoded)).toEqual(sample);
    const bytes = encoded.toBytes();
    const reread = Metadata.fromBytes(bytes);
    expect(encode(decode(reread) as never).toBytes()).toEqual(bytes);
  });

  it("round-trips every SdrValue alternative with its own tag", () => {
    for (const v of [true, false, 0n, -5n, 2n ** 63n - 1n, 0, 20e6, -0.5, "", "manual"] as wire.SdrValue[]) {
      expect(wire.decodeValue(wire.encodeValue(v))).toBe(v);
    }
    expect(wire.encodeValue(20e6).type).toBe(ValueType.Float);
    expect(wire.encodeValue(20n).type).toBe(ValueType.Int);
    expect(wire.decodeValue(value.bytes(new Uint8Array(1)))).toBe(0n);
  });

  it("decodes an empty hash to the C++ defaults", () => {
    const empty = new Metadata();
    expect(wire.decodePlan(empty)).toEqual(wire.defaultSweepPlan());
    expect(wire.decodePipeline(empty)).toEqual(wire.defaultPipelineConfig());
    expect(wire.decodeCorrectionSettings(empty)).toEqual(wire.defaultCorrectionSettings());
    expect(wire.decodeBackend(empty).capabilities).toEqual(wire.defaultFftCapabilities());
    expect(wire.decodeBenchmarkConfig(empty)).toEqual(wire.defaultFftBenchmarkConfig());
    expect(wire.decodeRoute(empty).inputIndex).toBe(kNoInput);
    expect(wire.decodeBenchmarkSample(empty).threads).toBe(1);
    expect(wire.decodeParameter(empty).defaultValue).toBe(0n);
    expect(wire.decodeParameter(empty).type).toBe(SdrParameterType.Double);
    expect(wire.decodeProcessStats(empty).throttleReason).toBe(ThrottleReason.Stopped);
    expect(wire.decodeNotice(empty).kind).toBe(NoticeKind.Info);
    expect(wire.decodeDeviceSection(empty).descriptor).toBeNull();
  });

  it("takes the fallback for an enum out of range, a u32 out of range, or a value of another type", () => {
    const m = wire.encodePlan(plan).setInt("window", 6).setInt("mode", -1).setInt("averageCount", 2n ** 32n);
    m.setString("rbwHz", "100k");
    const decoded = wire.decodePlan(m);
    expect(decoded.window).toBe(WindowType.Hann);
    expect(decoded.mode).toBe(SweepMode.Fast);
    expect(decoded.averageCount).toBe(1);
    expect(decoded.rbwHz).toBe(100e3);
    expect(decoded.portStrategy).toBe(PortStrategy.FewestSwitches);
  });

  it("bounds a benchmark config as the server does", () => {
    const m = wire
      .encodeBenchmarkConfig(benchmarkConfig)
      .set("sizes", value.array(ValueType.Int, [value.int(0), value.int(-4), value.int(512)]))
      .set("threadCounts", value.array(ValueType.Int, Array.from({ length: 10 }, (_, i) => value.int(i + 1))))
      .setFloat("secondsPerSample", 600)
      .setInt("minRuns", 50)
      .setInt("maxRuns", 10);
    const decoded = wire.decodeBenchmarkConfig(m);
    expect(decoded.sizes).toEqual([512]);
    expect(decoded.threadCounts).toEqual([1, 2, 3, 4, 5, 6, 7, 8]);
    expect(decoded.secondsPerSample).toBe(60);
    expect(decoded.maxRuns).toBe(50);

    const floats = wire
      .encodeBenchmarkConfig(benchmarkConfig)
      .set("sizes", value.array(ValueType.Float, [value.float(1024)]))
      .set("threadCounts", value.array(ValueType.Int, []))
      .setFloat("secondsPerSample", 0);
    const fallback = wire.decodeBenchmarkConfig(floats);
    expect(fallback.sizes).toEqual(wire.defaultFftBenchmarkConfig().sizes);
    expect(fallback.threadCounts).toEqual([1]);
    expect(fallback.secondsPerSample).toBe(0.001);
  });

  it("skips array elements of the wrong type", () => {
    const m = new Metadata()
      .set("items", value.array(ValueType.Hash, [value.hash(wire.encodeAntenna(antenna))]))
      .set("appliesWhenValues", value.array(ValueType.Int, [value.int(1)]))
      .set("supportedSampleRates", value.array(ValueType.Int, [value.int(1)]));
    expect(wire.decodeAntennas(m)).toEqual([antenna]);
    expect(wire.decodeParameter(m).appliesWhenValues).toEqual([]);
    expect(wire.decodeDevice(m).supportedSampleRates).toEqual([]);
  });

  it("rounds f32 fields as C++ stores them", () => {
    const encoded = wire.encodeHealth([{ ...reading, numeric: 0.1 }]);
    expect(wire.decodeHealth(encoded)[0]!.numeric).toBe(Math.fround(0.1));
  });

  it("answers the plan and antenna helpers as C++ does", () => {
    expect(wire.lowestHz(plan)).toBe(433e6);
    expect(wire.highestHz(plan)).toBe(2.5e9);
    expect(wire.totalSpanHz(plan)).toBeCloseTo(102e6);
    expect(wire.lowestHz(wire.defaultSweepPlan())).toBe(0);
    expect(wire.highestHz(wire.defaultSweepPlan())).toBe(0);
    expect(wire.antennaCovers(antenna, 1.5e9)).toBe(true);
    expect(wire.antennaCovers(antenna, 90e6, 200e6)).toBe(false);
    expect(wire.describeRange(antenna)).toBe("100 MHz - 1.5 GHz");
    expect(wire.describeRange({ ...antenna, startHz: 25e6, stopHz: 6e9 })).toBe("25 MHz - 6 GHz");
    expect(wire.describeRange({ ...antenna, stopHz: 0 })).toBe("no range");
    expect(wire.formatFrequencyShort(433.92e6)).toBe("433.9 MHz");
    expect(wire.formatFrequencyShort(999.96e6)).toBe("1000 MHz");
    expect(wire.formatFrequencyShort(12_345e9)).toBe("1.234e+04 GHz");
    expect(wire.formatFrequencyShort(0)).toBe("0 Hz");
    expect(wire.formatFrequencyShort(1.5)).toBe("1.5 Hz");
    expect(wire.formatFrequencyShort(1.0625e9)).toBe("1.062 GHz");
    expect(wire.formatFrequencyShort(1.0675e9)).toBe("1.067 GHz");
    expect(wire.formatFrequencyShort(0.000123456)).toBe("0.0001235 Hz");
    expect(wire.formatFrequencyShort(0.0000123456)).toBe("1.235e-05 Hz");
    expect(wire.formatFrequencyShort(-2.4e9)).toBe("-2.4 GHz");
  });
});

/** Every state section, by name, with the decoder and encoder that should
 * reproduce it byte for byte. */
const sectionCodecs: Record<string, { decode: (m: Metadata) => unknown; encode: (v: never) => Metadata }> = {
  [section.device]: { decode: wire.decodeDeviceSection, encode: wire.encodeDeviceSection },
  [section.values]: { decode: wire.decodeValuesSection, encode: wire.encodeValuesSection },
  [section.run]: { decode: wire.decodeRunSection, encode: wire.encodeRunSection },
  [section.plan]: { decode: wire.decodePlan, encode: wire.encodePlan },
  [section.schedule]: { decode: wire.decodeSchedule, encode: wire.encodeSchedule },
  [section.pipeline]: { decode: wire.decodePipeline, encode: wire.encodePipeline },
  [section.backends]: { decode: wire.decodeBackendsSection, encode: wire.encodeBackendsSection },
  [section.corrections]: { decode: wire.decodeCorrectionsSection, encode: wire.encodeCorrectionsSection },
  [section.learning]: { decode: wire.decodeLearningSection, encode: wire.encodeLearningSection },
  [section.antennas]: { decode: wire.decodeAntennas, encode: wire.encodeAntennas },
  [section.assignments]: { decode: wire.decodeAssignments, encode: wire.encodeAssignments },
  [section.switchers]: { decode: wire.decodeSwitchersSection, encode: wire.encodeSwitchersSection },
  [section.rfPath]: { decode: wire.decodeRfPathSection, encode: wire.encodeRfPathSection },
  [section.link]: { decode: wire.decodeLinkSection, encode: wire.encodeLinkSection },
  [section.benchmark]: { decode: wire.decodeBenchmarkStatus, encode: wire.encodeBenchmarkStatus },
  [section.recordings]: { decode: decodeRecordings, encode: wire.encodeRecordingsSection },
  [section.control]: { decode: decodeControl, encode: wire.encodeControlSection },
  [section.clients]: { decode: decodeClients, encode: wire.encodeClientsSection },
  [section.overlays]: { decode: wire.decodeOverlaysSection, encode: wire.encodeOverlaysSection },
};

function hex(bytes: Uint8Array): string {
  return [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
}

describe.skipIf(!directory)("the wire codec against what the C++ server sent", () => {
  it("re-encodes every section of every state byte for byte", () => {
    const stream = new Uint8Array(readFileSync(join(directory!, "stream.bin")));
    const framer = new RecordFramer();
    framer.feed(stream.subarray(kStreamHeaderBytes));
    const seen = new Set<string>();
    let states = 0;
    for (let record = framer.next(); record; record = framer.next()) {
      if (record.type !== RecordType.PluginData) {
        continue;
      }
      const message = decodeMessage(record);
      if (message.name !== msg.state) {
        continue;
      }
      ++states;
      for (const [name, body] of decodeState(message.body).sections.entries) {
        const codec = sectionCodecs[name];
        expect(codec, `section '${name}'`).toBeDefined();
        expect(body.type, `section '${name}'`).toBe(ValueType.Hash);
        if (!codec || body.type !== ValueType.Hash) {
          continue;
        }
        expect(hex(codec.encode(codec.decode(body.value) as never).toBytes()), `section '${name}'`).toBe(
          hex(body.value.toBytes()),
        );
        seen.add(name);
      }
    }
    expect(states).toBeGreaterThan(0);
    expect([...seen].sort()).toEqual(Object.keys(sectionCodecs).sort());
  });
});
