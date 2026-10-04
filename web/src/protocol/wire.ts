// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import type { ConnectedClient, ControlState, Notice, ServerRecordings } from "./messages";
import { Metadata, type Value, value, ValueType } from "./metadata";

/** The instrument's value types as typed metadata. Mirrors remote/WireCodec:
 * the same keys, the same value types and the same fallbacks, so what one end
 * encodes the other re-encodes to the same bytes. */

export { decodeNotice, NoticeKind, type NoticeKindId } from "./messages";
export type { Notice as InstrumentNotice } from "./messages";

// ---- enums, numbered as their C++ declarations ---------------------------------

export const SdrParameterType = { Bool: 0, Int: 1, Double: 2, Enum: 3, String: 4 } as const;
export type SdrParameterTypeId = (typeof SdrParameterType)[keyof typeof SdrParameterType];

export const SweepMode = { Fast: 0, Detail: 1 } as const;
export type SweepModeId = (typeof SweepMode)[keyof typeof SweepMode];

export const WindowType = {
  Rectangular: 0,
  Hann: 1,
  Hamming: 2,
  BlackmanHarris: 3,
  FlatTop: 4,
  Kaiser: 5,
} as const;
export type WindowTypeId = (typeof WindowType)[keyof typeof WindowType];

export const OverlapResolution = { Best: 0, Max: 1, Mean: 2 } as const;
export type OverlapResolutionId = (typeof OverlapResolution)[keyof typeof OverlapResolution];

export const PortStrategy = { TightestFit: 0, PortOrder: 1, HighestGain: 2, FewestSwitches: 3 } as const;
export type PortStrategyId = (typeof PortStrategy)[keyof typeof PortStrategy];

export const ThrottleMode = { EveryNth: 0, Auto: 1, AllSamples: 2 } as const;
export type ThrottleModeId = (typeof ThrottleMode)[keyof typeof ThrottleMode];

export const FftPlanQuality = { Fast: 0, Balanced: 1, Thorough: 2 } as const;
export type FftPlanQualityId = (typeof FftPlanQuality)[keyof typeof FftPlanQuality];

export const FftBackendType = { Cpu: 0, Gpu: 1 } as const;
export type FftBackendTypeId = (typeof FftBackendType)[keyof typeof FftBackendType];

export const FftSizeConstraint = { PowerOfTwo: 0, Any: 1 } as const;
export type FftSizeConstraintId = (typeof FftSizeConstraint)[keyof typeof FftSizeConstraint];

export const ThrottleReason = {
  None: 0,
  EveryNth: 1,
  CpuLimited: 2,
  RingFull: 3,
  PoolExhausted: 4,
  DeviceOverrun: 5,
  Stopped: 6,
} as const;
export type ThrottleReasonId = (typeof ThrottleReason)[keyof typeof ThrottleReason];

/** C++'s `kNoInput` is `size_t(-1)`. Sizes here are read signed, so its
 * all-ones crosses as -1 and re-encodes to the same bytes. */
export const kNoInput = -1;

// ---- the structs ----------------------------------------------------------------

/** `std::variant<bool, int64_t, double, std::string>`: an int is a bigint and a
 * double a number, so the two stay apart even when the double is whole. */
export type SdrValue = boolean | bigint | number | string;

export interface VersionReport {
  version: string;
  knownLatest: string;
  aheadOfDriver: boolean;
}

export interface SdrDeviceInfo {
  driver: string;
  id: string;
  label: string;
  serial: string;
  hardwareRevision: string;
  firmware: VersionReport;
  fpga: VersionReport;
  minFrequencyHz: number;
  maxFrequencyHz: number;
  minSampleRate: number;
  maxSampleRate: number;
  linkCapacityBytesPerSec: number;
  linkDescription: string;
}

export interface SdrEnumValue {
  value: string;
  label: string;
  description: string;
}

export interface SdrParameter {
  key: string;
  label: string;
  group: string;
  type: SdrParameterTypeId;
  unit: string;
  min: number;
  max: number;
  step: number;
  enumValues: SdrEnumValue[];
  readOnly: boolean;
  gridAffecting: boolean;
  calibrationAffecting: boolean;
  requiresStop: boolean;
  description: string;
  defaultValue: SdrValue;
  appliesWhenKey: string;
  appliesWhenValues: string[];
}

export interface SdrRxPort {
  id: string;
  label: string;
  connector: string;
  minHz: number;
  maxHz: number;
  biasTee: boolean;
  requiresStop: boolean;
  switchSeconds: number;
}

export interface DeviceDescriptor {
  info: SdrDeviceInfo;
  parameters: SdrParameter[];
  rxPorts: SdrRxPort[];
  supportedSampleRates: number[];
}

export interface SweepSegment {
  startHz: number;
  stopHz: number;
  dwellSeconds: number;
}

export interface SweepPlan {
  name: string;
  segments: SweepSegment[];
  mode: SweepModeId;
  rbwHz: number;
  sampleRate: number;
  usableBandwidthFraction: number;
  stepOverlap: number;
  dcGuardFraction: number;
  dwellSeconds: number;
  averageCount: number;
  window: WindowTypeId;
  windowBeta: number;
  fftOverlap: number;
  overlapResolution: OverlapResolutionId;
  continuous: boolean;
  antennaRouting: boolean;
  portStrategy: PortStrategyId;
}

export interface PipelineConfig {
  fftSize: number;
  window: WindowTypeId;
  windowBeta: number;
  overlap: number;
  workerCount: number;
  throttleMode: ThrottleModeId;
  everyNth: number;
  averageCount: number;
  planQuality: FftPlanQualityId;
  targetFrameRate: number;
  dbfsToDbmOffset: number;
}

export interface CorrectionSettings {
  dcRemoval: boolean;
  flatten: boolean;
  spurMask: boolean;
  autoSpurs: boolean;
}

export interface CorrectionSummary {
  present: boolean;
  floorPoints: number;
  spurs: number;
  automaticSpurs: number;
  learnedAt: string;
  floorStaleReason: string;
}

/** A frequency range in Hz, start then stop. */
export type HzRange = [startHz: number, stopHz: number];

export interface ScheduleSummary {
  stepCount: number;
  fftSize: number;
  actualRbwHz: number;
  gridStartHz: number;
  gridBinWidthHz: number;
  gridBinCount: number;
  estimatedPassSeconds: number;
  estimatedSweepRateHzPerSec: number;
  retuneOverheadFraction: number;
  unroutedHz: HzRange[];
  portSwitches: number;
}

export interface EngineStats {
  stitched: bigint;
  unsettled: bigint;
  unattributed: bigint;
  tooShort: bigint;
  lastPassCoverage: number;
  measuredSweepRateHzPerSec: number;
  passCount: bigint;
}

export interface FftCapabilities {
  type: FftBackendTypeId;
  minSize: number;
  maxSize: number;
  sizeConstraint: FftSizeConstraintId;
  supportsBatch: boolean;
  supportsInPlace: boolean;
  threadSafeExecute: boolean;
  threadSafePlanning: boolean;
}

export interface FftBackendInfo {
  name: string;
  displayName: string;
  description: string;
  capabilities: FftCapabilities;
  available: boolean;
  unavailableReason: string;
  isSuggestedDefault: boolean;
}

export interface Antenna {
  id: string;
  name: string;
  category: string;
  type: string;
  startHz: number;
  stopHz: number;
  gainDbi: number;
  needsBiasT: boolean;
  notes: string;
  builtin: boolean;
}

export interface AntennaAssignment {
  device: string;
  port: string;
  switcher: string;
  input: string;
  antenna: string;
}

export interface AntennaAssignments {
  entries: AntennaAssignment[];
  /** Device key, then port id. */
  fallbackPorts: [device: string, port: string][];
}

export interface RfPathInfo {
  driver: string;
  id: string;
  label: string;
  model: string;
  serial: string;
  inputCount: number;
  requiresStop: boolean;
  switchSeconds: number;
}

export interface RfPathInput {
  id: string;
  label: string;
  minHz: number;
  maxHz: number;
}

export interface SwitcherView {
  key: string;
  info: RfPathInfo;
  inputs: RfPathInput[];
  selectedInput: number;
}

export interface RoutePort {
  portIndex: number;
  id: string;
  inputIndex: number;
  startHz: number;
  stopHz: number;
  gainDbi: number;
  switchSeconds: number;
  inputSwitchSeconds: number;
}

export interface RfLegView {
  route: RoutePort;
  switcherKey: string;
  antenna: Antenna;
  portLabel: string;
  live: boolean;
}

/** `numeric`, `minimum` and `maximum` are floats in C++, held here already
 * rounded to f32. */
export interface SdrHealthReading {
  label: string;
  value: string;
  numeric: number;
  minimum: number;
  maximum: number;
  alarm: boolean;
}

export interface StreamStats {
  configuredSps: number;
  measuredSps: number;
  bytesPerSecIn: number;
  linkCapacityBytesPerSec: number;
  linkUtilisation: number;
  samplesDelivered: bigint;
  samplesDropped: bigint;
  samplesLostAtSource: bigint;
  deviceOverruns: bigint;
  ringFullEvents: bigint;
  poolExhaustedEvents: bigint;
  sequenceGaps: bigint;
  dropRatePerSec: number;
  dropFraction: number;
  ringFillFraction: number;
}

export interface ProcessStats {
  fftsPerSec: number;
  fftsComputed: bigint;
  fftsSkipped: bigint;
  samplesProcessedTotal: bigint;
  processedFraction: number;
  framesPerSec: number;
  sweepPassesCompleted: bigint;
  retunesPerSec: number;
  fftLatencyP50Us: number;
  fftLatencyP99Us: number;
  fftLatencyMaxUs: number;
  workerUtilisation: number;
  workerCount: number;
  throttleReason: ThrottleReasonId;
  sweepSpeedHzPerSec: number;
}

export interface FftBenchmarkConfig {
  sizes: number[];
  threadCounts: number[];
  quality: FftPlanQualityId;
  secondsPerSample: number;
  minRuns: number;
  maxRuns: number;
  warmupRuns: number;
}

export interface FftBenchmarkSample {
  size: number;
  threads: number;
  planSeconds: number;
  p50Seconds: number;
  p99Seconds: number;
  maxSeconds: number;
  throughputPerSecond: number;
  cpuCores: number;
  runs: number;
  skipped: string;
}

export interface FftBenchmarkEntry {
  backend: string;
  displayName: string;
  samples: FftBenchmarkSample[];
  error: string;
}

export interface BenchmarkStatus {
  running: boolean;
  complete: boolean;
  stepsDone: number;
  stepsTotal: number;
  currentStep: string;
  elapsedSeconds: number;
  results: FftBenchmarkEntry[];
}

// ---- defaults the decoders fall back to -----------------------------------------

export function defaultSweepPlan(): SweepPlan {
  return {
    name: "",
    segments: [],
    mode: SweepMode.Fast,
    rbwHz: 100e3,
    sampleRate: 20e6,
    usableBandwidthFraction: 0.75,
    stepOverlap: 0.05,
    dcGuardFraction: 0.05,
    dwellSeconds: 0,
    averageCount: 1,
    window: WindowType.Hann,
    windowBeta: 8.6,
    fftOverlap: 0,
    overlapResolution: OverlapResolution.Best,
    continuous: true,
    antennaRouting: false,
    portStrategy: PortStrategy.TightestFit,
  };
}

export function defaultPipelineConfig(): PipelineConfig {
  return {
    fftSize: 4096,
    window: WindowType.Hann,
    windowBeta: 8.6,
    overlap: 0,
    workerCount: 0,
    throttleMode: ThrottleMode.Auto,
    everyNth: 1,
    averageCount: 1,
    planQuality: FftPlanQuality.Balanced,
    targetFrameRate: 60,
    dbfsToDbmOffset: 0,
  };
}

export function defaultCorrectionSettings(): CorrectionSettings {
  return { dcRemoval: true, flatten: true, spurMask: true, autoSpurs: false };
}

export function defaultFftCapabilities(): FftCapabilities {
  return {
    type: FftBackendType.Cpu,
    minSize: 2,
    maxSize: 1 << 24,
    sizeConstraint: FftSizeConstraint.Any,
    supportsBatch: false,
    supportsInPlace: true,
    threadSafeExecute: false,
    threadSafePlanning: false,
  };
}

/** `defaultFftBenchmarkConfig()`, not the struct's member defaults: the
 * decoder falls back to this one, size ladder included. */
export function defaultFftBenchmarkConfig(): FftBenchmarkConfig {
  return {
    sizes: [1024, 4096, 16384, 65536],
    threadCounts: [1],
    quality: FftPlanQuality.Balanced,
    secondsPerSample: 0.35,
    minRuns: 16,
    maxRuns: 2_000_000,
    warmupRuns: 8,
  };
}

// ---- small helpers beside the structs --------------------------------------------

/** Lowest start across the segments, or 0 with none. */
export function lowestHz(plan: SweepPlan): number {
  return plan.segments.length === 0 ? 0 : Math.min(...plan.segments.map((s) => s.startHz));
}

/** Highest stop across the segments, or 0 with none. */
export function highestHz(plan: SweepPlan): number {
  return plan.segments.length === 0 ? 0 : Math.max(...plan.segments.map((s) => s.stopHz));
}

export function totalSpanHz(plan: SweepPlan): number {
  return plan.segments.reduce((total, s) => total + (s.stopHz - s.startHz), 0);
}

/** Closed at both ends, and the whole span: as `Antenna::covers`. */
export function antennaCovers(antenna: Antenna, fromHz: number, toHz = fromHz): boolean {
  return antenna.stopHz > antenna.startHz && fromHz >= antenna.startHz && toHz <= antenna.stopHz;
}

/** printf's `%.4g`: four significant digits, trailing zeros dropped.
 *
 * Rounded from the exact decimal expansion rather than with `toPrecision`,
 * which sends an exact tie away from zero where printf sends it to even:
 * 1.0625 GHz is "1.062 GHz" in C++. */
function generalFormat4(v: number): string {
  const precision = 4;
  if (!Number.isFinite(v)) {
    return Number.isNaN(v) ? "nan" : v > 0 ? "inf" : "-inf";
  }
  const sign = v < 0 || Object.is(v, -0) ? "-" : "";
  const abs = Math.abs(v);
  if (abs === 0) {
    return `${sign}0`;
  }
  // Below 1e21 a hundred places hold every double's expansion that could tie.
  const [whole = "", fraction = ""] = abs >= 1e21 ? [BigInt(abs).toString()] : abs.toFixed(100).split(".");
  const raw = `${whole}${fraction}`;
  const first = raw.search(/[1-9]/);
  let exponent = whole.length - 1 - first;
  const digits = raw.slice(first);
  let kept = digits.slice(0, precision).padEnd(precision, "0");
  const rest = digits.slice(precision);
  const next = rest[0] ?? "0";
  const odd = Number(kept[precision - 1]) % 2 === 1;
  if (next > "5" || (next === "5" && (/[1-9]/.test(rest.slice(1)) || odd))) {
    kept = (BigInt(kept) + 1n).toString();
    if (kept.length > precision) {
      kept = kept.slice(0, precision);
      ++exponent;
    }
  }
  const trim = (text: string) => text.replace(/0+$/, "");
  if (exponent < -4 || exponent >= precision) {
    const decimals = trim(kept.slice(1));
    const power = `${exponent < 0 ? "-" : "+"}${String(Math.abs(exponent)).padStart(2, "0")}`;
    return `${sign}${kept[0]}${decimals ? `.${decimals}` : ""}e${power}`;
  }
  const integer = exponent >= 0 ? kept.slice(0, exponent + 1) : "0";
  const decimals = trim(exponent >= 0 ? kept.slice(exponent + 1) : "0".repeat(-exponent - 1) + kept);
  return `${sign}${integer}${decimals ? `.${decimals}` : ""}`;
}

/** "2.4 GHz", "433.9 MHz": as `formatFrequencyShort`. */
export function formatFrequencyShort(hz: number): string {
  const magnitude = Math.abs(hz);
  if (magnitude >= 1e9) {
    return `${generalFormat4(hz / 1e9)} GHz`;
  }
  if (magnitude >= 1e6) {
    return `${generalFormat4(hz / 1e6)} MHz`;
  }
  if (magnitude >= 1e3) {
    return `${generalFormat4(hz / 1e3)} kHz`;
  }
  return `${generalFormat4(hz)} Hz`;
}

/** "10 MHz - 1.5 GHz" */
export function describeRange(antenna: Antenna): string {
  if (antenna.stopHz <= antenna.startHz) {
    return "no range";
  }
  return `${formatFrequencyShort(antenna.startHz)} - ${formatFrequencyShort(antenna.stopHz)}`;
}

// ---- codec helpers, as WireCodec.cpp's -------------------------------------------

function setU64(out: Metadata, key: string, v: bigint): void {
  out.setInt(key, BigInt.asIntN(64, v));
}

function getU64(m: Metadata, key: string, fallback = 0n): bigint {
  return BigInt.asUintN(64, m.getBigInt(key, BigInt.asIntN(64, fallback)));
}

function getU32(m: Metadata, key: string, fallback = 0): number {
  const v = m.getBigInt(key, BigInt(fallback));
  return v < 0n || v > 0xffffffffn ? fallback : Number(v);
}

function getSize(m: Metadata, key: string, fallback = 0): number {
  return m.getInt(key, fallback);
}

function getF32(m: Metadata, key: string, fallback = 0): number {
  return Math.fround(m.getFloat(key, fallback));
}

/** `count` values from zero; anything else is the fallback. */
function getEnum<E extends number>(m: Metadata, key: string, fallback: E, count: number): E {
  const v = m.getBigInt(key, BigInt(fallback));
  return v >= 0n && v < BigInt(count) ? (Number(v) as E) : fallback;
}

function hashArray<T>(items: readonly T[], encode: (item: T) => Metadata): Value {
  return value.array(
    ValueType.Hash,
    items.map((item) => value.hash(encode(item))),
  );
}

/** The hashes in the array under `key`, decoded; any other element is skipped. */
function hashList<T>(m: Metadata, key: string, decode: (m: Metadata) => T): T[] {
  return m.getHashes(key).map(decode);
}

function stringArray(items: readonly string[]): Value {
  return value.array(
    ValueType.String,
    items.map((item) => value.string(item)),
  );
}

function stringList(m: Metadata, key: string): string[] {
  return m.getArray(key).flatMap((v) => (v.type === ValueType.String ? [v.value] : []));
}

function floatArray(items: readonly number[]): Value {
  return value.array(
    ValueType.Float,
    items.map((item) => value.float(item)),
  );
}

function floatList(m: Metadata, key: string): number[] {
  const v = m.find(key);
  if (v?.type !== ValueType.Array || v.elementType !== ValueType.Float) {
    return [];
  }
  return v.value.map((element) => (element.type === ValueType.Float ? element.value : 0));
}

function sizeArray(items: readonly number[]): Value {
  return value.array(
    ValueType.Int,
    items.map((item) => value.int(item)),
  );
}

/** Positive integers under `key`, at most `limit` of them. */
function sizeList(m: Metadata, key: string, limit: number): number[] {
  const v = m.find(key);
  if (v?.type !== ValueType.Array || v.elementType !== ValueType.Int) {
    return [];
  }
  const out: number[] = [];
  for (const element of v.value) {
    if (out.length >= limit) {
      break;
    }
    if (element.type === ValueType.Int && element.value > 0n) {
      out.push(Number(element.value));
    }
  }
  return out;
}

// ---- values -----------------------------------------------------------------------

export function encodeValue(v: SdrValue): Value {
  switch (typeof v) {
    case "boolean":
      return value.bool(v);
    case "bigint":
      return value.int(v);
    case "number":
      return value.float(v);
    default:
      return value.string(v);
  }
}

export function decodeValue(v: Value): SdrValue {
  switch (v.type) {
    case ValueType.Bool:
    case ValueType.Int:
    case ValueType.Float:
    case ValueType.String:
      return v.value;
    default:
      return 0n;
  }
}

// ---- single entries -----------------------------------------------------------------

export function encodeVersionReport(report: VersionReport): Metadata {
  return new Metadata()
    .setString("version", report.version)
    .setString("knownLatest", report.knownLatest)
    .setBool("aheadOfDriver", report.aheadOfDriver);
}

export function decodeVersionReport(m: Metadata): VersionReport {
  return {
    version: m.getString("version"),
    knownLatest: m.getString("knownLatest"),
    aheadOfDriver: m.getBool("aheadOfDriver"),
  };
}

export function encodeInfo(info: SdrDeviceInfo): Metadata {
  return new Metadata()
    .setString("driver", info.driver)
    .setString("id", info.id)
    .setString("label", info.label)
    .setString("serial", info.serial)
    .setString("hardwareRevision", info.hardwareRevision)
    .setHash("firmware", encodeVersionReport(info.firmware))
    .setHash("fpga", encodeVersionReport(info.fpga))
    .setFloat("minFrequencyHz", info.minFrequencyHz)
    .setFloat("maxFrequencyHz", info.maxFrequencyHz)
    .setFloat("minSampleRate", info.minSampleRate)
    .setFloat("maxSampleRate", info.maxSampleRate)
    .setInt("linkCapacityBytesPerSec", info.linkCapacityBytesPerSec)
    .setString("linkDescription", info.linkDescription);
}

export function decodeInfo(m: Metadata): SdrDeviceInfo {
  return {
    driver: m.getString("driver"),
    id: m.getString("id"),
    label: m.getString("label"),
    serial: m.getString("serial"),
    hardwareRevision: m.getString("hardwareRevision"),
    firmware: decodeVersionReport(m.getHash("firmware")),
    fpga: decodeVersionReport(m.getHash("fpga")),
    minFrequencyHz: m.getFloat("minFrequencyHz"),
    maxFrequencyHz: m.getFloat("maxFrequencyHz"),
    minSampleRate: m.getFloat("minSampleRate"),
    maxSampleRate: m.getFloat("maxSampleRate"),
    linkCapacityBytesPerSec: getSize(m, "linkCapacityBytesPerSec"),
    linkDescription: m.getString("linkDescription"),
  };
}

export function encodeEnumValue(v: SdrEnumValue): Metadata {
  return new Metadata()
    .setString("value", v.value)
    .setString("label", v.label)
    .setString("description", v.description);
}

export function decodeEnumValue(m: Metadata): SdrEnumValue {
  return {
    value: m.getString("value"),
    label: m.getString("label"),
    description: m.getString("description"),
  };
}

export function encodeParameter(parameter: SdrParameter): Metadata {
  return new Metadata()
    .setString("key", parameter.key)
    .setString("label", parameter.label)
    .setString("group", parameter.group)
    .setInt("type", parameter.type)
    .setString("unit", parameter.unit)
    .setFloat("min", parameter.min)
    .setFloat("max", parameter.max)
    .setFloat("step", parameter.step)
    .set("enumValues", hashArray(parameter.enumValues, encodeEnumValue))
    .setBool("readOnly", parameter.readOnly)
    .setBool("gridAffecting", parameter.gridAffecting)
    .setBool("calibrationAffecting", parameter.calibrationAffecting)
    .setBool("requiresStop", parameter.requiresStop)
    .setString("description", parameter.description)
    .set("defaultValue", encodeValue(parameter.defaultValue))
    .setString("appliesWhenKey", parameter.appliesWhenKey)
    .set("appliesWhenValues", stringArray(parameter.appliesWhenValues));
}

export function decodeParameter(m: Metadata): SdrParameter {
  const defaultValue = m.find("defaultValue");
  return {
    key: m.getString("key"),
    label: m.getString("label"),
    group: m.getString("group"),
    type: getEnum<SdrParameterTypeId>(m, "type", SdrParameterType.Double, 5),
    unit: m.getString("unit"),
    min: m.getFloat("min"),
    max: m.getFloat("max"),
    step: m.getFloat("step"),
    enumValues: hashList(m, "enumValues", decodeEnumValue),
    readOnly: m.getBool("readOnly"),
    gridAffecting: m.getBool("gridAffecting"),
    calibrationAffecting: m.getBool("calibrationAffecting"),
    requiresStop: m.getBool("requiresStop"),
    description: m.getString("description"),
    defaultValue: defaultValue ? decodeValue(defaultValue) : 0n,
    appliesWhenKey: m.getString("appliesWhenKey"),
    appliesWhenValues: stringList(m, "appliesWhenValues"),
  };
}

export function encodeRxPort(port: SdrRxPort): Metadata {
  return new Metadata()
    .setString("id", port.id)
    .setString("label", port.label)
    .setString("connector", port.connector)
    .setFloat("minHz", port.minHz)
    .setFloat("maxHz", port.maxHz)
    .setBool("biasTee", port.biasTee)
    .setBool("requiresStop", port.requiresStop)
    .setFloat("switchSeconds", port.switchSeconds);
}

export function decodeRxPort(m: Metadata): SdrRxPort {
  return {
    id: m.getString("id"),
    label: m.getString("label"),
    connector: m.getString("connector"),
    minHz: m.getFloat("minHz"),
    maxHz: m.getFloat("maxHz"),
    biasTee: m.getBool("biasTee"),
    requiresStop: m.getBool("requiresStop"),
    switchSeconds: m.getFloat("switchSeconds"),
  };
}

export function encodeSegment(segment: SweepSegment): Metadata {
  return new Metadata()
    .setFloat("startHz", segment.startHz)
    .setFloat("stopHz", segment.stopHz)
    .setFloat("dwellSeconds", segment.dwellSeconds);
}

export function decodeSegment(m: Metadata): SweepSegment {
  return {
    startHz: m.getFloat("startHz"),
    stopHz: m.getFloat("stopHz"),
    dwellSeconds: m.getFloat("dwellSeconds"),
  };
}

export function encodeRange(range: HzRange): Metadata {
  return new Metadata().setFloat("startHz", range[0]).setFloat("stopHz", range[1]);
}

export function decodeRange(m: Metadata): HzRange {
  return [m.getFloat("startHz"), m.getFloat("stopHz")];
}

export function encodeBackend(info: FftBackendInfo): Metadata {
  const capabilities = new Metadata()
    .setInt("type", info.capabilities.type)
    .setInt("minSize", info.capabilities.minSize)
    .setInt("maxSize", info.capabilities.maxSize)
    .setInt("sizeConstraint", info.capabilities.sizeConstraint)
    .setBool("supportsBatch", info.capabilities.supportsBatch)
    .setBool("supportsInPlace", info.capabilities.supportsInPlace)
    .setBool("threadSafeExecute", info.capabilities.threadSafeExecute)
    .setBool("threadSafePlanning", info.capabilities.threadSafePlanning);
  return new Metadata()
    .setString("name", info.name)
    .setString("displayName", info.displayName)
    .setString("description", info.description)
    .setHash("capabilities", capabilities)
    .setBool("available", info.available)
    .setString("unavailableReason", info.unavailableReason)
    .setBool("isSuggestedDefault", info.isSuggestedDefault);
}

export function decodeBackend(m: Metadata): FftBackendInfo {
  const capabilities = m.getHash("capabilities");
  const defaults = defaultFftCapabilities();
  return {
    name: m.getString("name"),
    displayName: m.getString("displayName"),
    description: m.getString("description"),
    capabilities: {
      type: getEnum(capabilities, "type", defaults.type, 2),
      minSize: getSize(capabilities, "minSize", defaults.minSize),
      maxSize: getSize(capabilities, "maxSize", defaults.maxSize),
      sizeConstraint: getEnum(capabilities, "sizeConstraint", defaults.sizeConstraint, 2),
      supportsBatch: capabilities.getBool("supportsBatch", defaults.supportsBatch),
      supportsInPlace: capabilities.getBool("supportsInPlace", defaults.supportsInPlace),
      threadSafeExecute: capabilities.getBool("threadSafeExecute", defaults.threadSafeExecute),
      threadSafePlanning: capabilities.getBool("threadSafePlanning", defaults.threadSafePlanning),
    },
    available: m.getBool("available"),
    unavailableReason: m.getString("unavailableReason"),
    isSuggestedDefault: m.getBool("isSuggestedDefault"),
  };
}

export function encodeAntenna(antenna: Antenna): Metadata {
  return new Metadata()
    .setString("id", antenna.id)
    .setString("name", antenna.name)
    .setString("category", antenna.category)
    .setString("type", antenna.type)
    .setFloat("startHz", antenna.startHz)
    .setFloat("stopHz", antenna.stopHz)
    .setFloat("gainDbi", antenna.gainDbi)
    .setBool("needsBiasT", antenna.needsBiasT)
    .setString("notes", antenna.notes)
    .setBool("builtin", antenna.builtin);
}

export function decodeAntenna(m: Metadata): Antenna {
  return {
    id: m.getString("id"),
    name: m.getString("name"),
    category: m.getString("category"),
    type: m.getString("type"),
    startHz: m.getFloat("startHz"),
    stopHz: m.getFloat("stopHz"),
    gainDbi: m.getFloat("gainDbi"),
    needsBiasT: m.getBool("needsBiasT"),
    notes: m.getString("notes"),
    builtin: m.getBool("builtin"),
  };
}

export function encodeAssignment(assignment: AntennaAssignment): Metadata {
  return new Metadata()
    .setString("device", assignment.device)
    .setString("port", assignment.port)
    .setString("switcher", assignment.switcher)
    .setString("input", assignment.input)
    .setString("antenna", assignment.antenna);
}

export function decodeAssignment(m: Metadata): AntennaAssignment {
  return {
    device: m.getString("device"),
    port: m.getString("port"),
    switcher: m.getString("switcher"),
    input: m.getString("input"),
    antenna: m.getString("antenna"),
  };
}

export function encodeFallback(fallback: [device: string, port: string]): Metadata {
  return new Metadata().setString("device", fallback[0]).setString("port", fallback[1]);
}

export function decodeFallback(m: Metadata): [device: string, port: string] {
  return [m.getString("device"), m.getString("port")];
}

export function encodeSwitcherInfo(info: RfPathInfo): Metadata {
  return new Metadata()
    .setString("driver", info.driver)
    .setString("id", info.id)
    .setString("label", info.label)
    .setString("model", info.model)
    .setString("serial", info.serial)
    .setInt("inputCount", info.inputCount)
    .setBool("requiresStop", info.requiresStop)
    .setFloat("switchSeconds", info.switchSeconds);
}

export function decodeSwitcherInfo(m: Metadata): RfPathInfo {
  return {
    driver: m.getString("driver"),
    id: m.getString("id"),
    label: m.getString("label"),
    model: m.getString("model"),
    serial: m.getString("serial"),
    inputCount: getU32(m, "inputCount"),
    requiresStop: m.getBool("requiresStop"),
    switchSeconds: m.getFloat("switchSeconds"),
  };
}

export function encodeSwitcherInput(input: RfPathInput): Metadata {
  return new Metadata()
    .setString("id", input.id)
    .setString("label", input.label)
    .setFloat("minHz", input.minHz)
    .setFloat("maxHz", input.maxHz);
}

export function decodeSwitcherInput(m: Metadata): RfPathInput {
  return {
    id: m.getString("id"),
    label: m.getString("label"),
    minHz: m.getFloat("minHz"),
    maxHz: m.getFloat("maxHz"),
  };
}

export function encodeSwitcherView(view: SwitcherView): Metadata {
  return new Metadata()
    .setString("key", view.key)
    .setHash("info", encodeSwitcherInfo(view.info))
    .set("inputs", hashArray(view.inputs, encodeSwitcherInput))
    .setInt("selectedInput", view.selectedInput);
}

export function decodeSwitcherView(m: Metadata): SwitcherView {
  return {
    key: m.getString("key"),
    info: decodeSwitcherInfo(m.getHash("info")),
    inputs: hashList(m, "inputs", decodeSwitcherInput),
    selectedInput: getU32(m, "selectedInput"),
  };
}

export function encodeRoute(route: RoutePort): Metadata {
  return new Metadata()
    .setInt("portIndex", route.portIndex)
    .setString("id", route.id)
    .setInt("inputIndex", route.inputIndex)
    .setFloat("startHz", route.startHz)
    .setFloat("stopHz", route.stopHz)
    .setFloat("gainDbi", route.gainDbi)
    .setFloat("switchSeconds", route.switchSeconds)
    .setFloat("inputSwitchSeconds", route.inputSwitchSeconds);
}

export function decodeRoute(m: Metadata): RoutePort {
  return {
    portIndex: getSize(m, "portIndex"),
    id: m.getString("id"),
    inputIndex: getSize(m, "inputIndex", kNoInput),
    startHz: m.getFloat("startHz"),
    stopHz: m.getFloat("stopHz"),
    gainDbi: m.getFloat("gainDbi"),
    switchSeconds: m.getFloat("switchSeconds"),
    inputSwitchSeconds: m.getFloat("inputSwitchSeconds"),
  };
}

export function encodeRfLeg(leg: RfLegView): Metadata {
  return new Metadata()
    .setHash("route", encodeRoute(leg.route))
    .setString("switcherKey", leg.switcherKey)
    .setHash("antenna", encodeAntenna(leg.antenna))
    .setString("portLabel", leg.portLabel)
    .setBool("live", leg.live);
}

export function decodeRfLeg(m: Metadata): RfLegView {
  return {
    route: decodeRoute(m.getHash("route")),
    switcherKey: m.getString("switcherKey"),
    antenna: decodeAntenna(m.getHash("antenna")),
    portLabel: m.getString("portLabel"),
    live: m.getBool("live"),
  };
}

export function encodeReading(reading: SdrHealthReading): Metadata {
  return new Metadata()
    .setString("label", reading.label)
    .setString("value", reading.value)
    .setFloat("numeric", Math.fround(reading.numeric))
    .setFloat("minimum", Math.fround(reading.minimum))
    .setFloat("maximum", Math.fround(reading.maximum))
    .setBool("alarm", reading.alarm);
}

export function decodeReading(m: Metadata): SdrHealthReading {
  return {
    label: m.getString("label"),
    value: m.getString("value"),
    numeric: getF32(m, "numeric"),
    minimum: getF32(m, "minimum"),
    maximum: getF32(m, "maximum"),
    alarm: m.getBool("alarm"),
  };
}

export function encodeBenchmarkSample(sample: FftBenchmarkSample): Metadata {
  return new Metadata()
    .setInt("size", sample.size)
    .setInt("threads", sample.threads)
    .setFloat("planSeconds", sample.planSeconds)
    .setFloat("p50Seconds", sample.p50Seconds)
    .setFloat("p99Seconds", sample.p99Seconds)
    .setFloat("maxSeconds", sample.maxSeconds)
    .setFloat("throughputPerSecond", sample.throughputPerSecond)
    .setFloat("cpuCores", sample.cpuCores)
    .setInt("runs", sample.runs)
    .setString("skipped", sample.skipped);
}

export function decodeBenchmarkSample(m: Metadata): FftBenchmarkSample {
  return {
    size: getSize(m, "size"),
    threads: getSize(m, "threads", 1),
    planSeconds: m.getFloat("planSeconds"),
    p50Seconds: m.getFloat("p50Seconds"),
    p99Seconds: m.getFloat("p99Seconds"),
    maxSeconds: m.getFloat("maxSeconds"),
    throughputPerSecond: m.getFloat("throughputPerSecond"),
    cpuCores: m.getFloat("cpuCores"),
    runs: getSize(m, "runs"),
    skipped: m.getString("skipped"),
  };
}

export function encodeBenchmarkEntry(entry: FftBenchmarkEntry): Metadata {
  return new Metadata()
    .setString("backend", entry.backend)
    .setString("displayName", entry.displayName)
    .set("samples", hashArray(entry.samples, encodeBenchmarkSample))
    .setString("error", entry.error);
}

export function decodeBenchmarkEntry(m: Metadata): FftBenchmarkEntry {
  return {
    backend: m.getString("backend"),
    displayName: m.getString("displayName"),
    samples: hashList(m, "samples", decodeBenchmarkSample),
    error: m.getString("error"),
  };
}

// ---- the radio --------------------------------------------------------------------

export function encodeDevice(device: DeviceDescriptor): Metadata {
  return new Metadata()
    .setHash("info", encodeInfo(device.info))
    .set("parameters", hashArray(device.parameters, encodeParameter))
    .set("rxPorts", hashArray(device.rxPorts, encodeRxPort))
    .set("supportedSampleRates", floatArray(device.supportedSampleRates));
}

export function decodeDevice(m: Metadata): DeviceDescriptor {
  return {
    info: decodeInfo(m.getHash("info")),
    parameters: hashList(m, "parameters", decodeParameter),
    rxPorts: hashList(m, "rxPorts", decodeRxPort),
    supportedSampleRates: floatList(m, "supportedSampleRates"),
  };
}

// ---- running ----------------------------------------------------------------------

export function encodePlan(plan: SweepPlan): Metadata {
  return new Metadata()
    .setString("name", plan.name)
    .set("segments", hashArray(plan.segments, encodeSegment))
    .setInt("mode", plan.mode)
    .setFloat("rbwHz", plan.rbwHz)
    .setFloat("sampleRate", plan.sampleRate)
    .setFloat("usableBandwidthFraction", plan.usableBandwidthFraction)
    .setFloat("stepOverlap", plan.stepOverlap)
    .setFloat("dcGuardFraction", plan.dcGuardFraction)
    .setFloat("dwellSeconds", plan.dwellSeconds)
    .setInt("averageCount", plan.averageCount)
    .setInt("window", plan.window)
    .setFloat("windowBeta", plan.windowBeta)
    .setFloat("fftOverlap", plan.fftOverlap)
    .setInt("overlapResolution", plan.overlapResolution)
    .setBool("continuous", plan.continuous)
    .setBool("antennaRouting", plan.antennaRouting)
    .setInt("portStrategy", plan.portStrategy);
}

export function decodePlan(m: Metadata): SweepPlan {
  const defaults = defaultSweepPlan();
  return {
    name: m.getString("name"),
    segments: hashList(m, "segments", decodeSegment),
    mode: getEnum(m, "mode", defaults.mode, 2),
    rbwHz: m.getFloat("rbwHz", defaults.rbwHz),
    sampleRate: m.getFloat("sampleRate", defaults.sampleRate),
    usableBandwidthFraction: m.getFloat("usableBandwidthFraction", defaults.usableBandwidthFraction),
    stepOverlap: m.getFloat("stepOverlap", defaults.stepOverlap),
    dcGuardFraction: m.getFloat("dcGuardFraction", defaults.dcGuardFraction),
    dwellSeconds: m.getFloat("dwellSeconds", defaults.dwellSeconds),
    averageCount: getU32(m, "averageCount", defaults.averageCount),
    window: getEnum(m, "window", defaults.window, 6),
    windowBeta: m.getFloat("windowBeta", defaults.windowBeta),
    fftOverlap: m.getFloat("fftOverlap", defaults.fftOverlap),
    overlapResolution: getEnum(m, "overlapResolution", defaults.overlapResolution, 3),
    continuous: m.getBool("continuous", defaults.continuous),
    antennaRouting: m.getBool("antennaRouting", defaults.antennaRouting),
    portStrategy: getEnum(m, "portStrategy", defaults.portStrategy, 4),
  };
}

export function encodePipeline(config: PipelineConfig): Metadata {
  return new Metadata()
    .setInt("fftSize", config.fftSize)
    .setInt("window", config.window)
    .setFloat("windowBeta", config.windowBeta)
    .setFloat("overlap", config.overlap)
    .setInt("workerCount", config.workerCount)
    .setInt("throttleMode", config.throttleMode)
    .setInt("everyNth", config.everyNth)
    .setInt("averageCount", config.averageCount)
    .setInt("planQuality", config.planQuality)
    .setFloat("targetFrameRate", config.targetFrameRate)
    .setFloat("dbfsToDbmOffset", config.dbfsToDbmOffset);
}

export function decodePipeline(m: Metadata): PipelineConfig {
  const defaults = defaultPipelineConfig();
  return {
    fftSize: getU32(m, "fftSize", defaults.fftSize),
    window: getEnum(m, "window", defaults.window, 6),
    windowBeta: m.getFloat("windowBeta", defaults.windowBeta),
    overlap: m.getFloat("overlap", defaults.overlap),
    workerCount: getU32(m, "workerCount", defaults.workerCount),
    throttleMode: getEnum(m, "throttleMode", defaults.throttleMode, 3),
    everyNth: getU32(m, "everyNth", defaults.everyNth),
    averageCount: getU32(m, "averageCount", defaults.averageCount),
    planQuality: getEnum(m, "planQuality", defaults.planQuality, 3),
    targetFrameRate: m.getFloat("targetFrameRate", defaults.targetFrameRate),
    dbfsToDbmOffset: m.getFloat("dbfsToDbmOffset", defaults.dbfsToDbmOffset),
  };
}

export function encodeSchedule(schedule: ScheduleSummary): Metadata {
  return new Metadata()
    .setInt("stepCount", schedule.stepCount)
    .setInt("fftSize", schedule.fftSize)
    .setFloat("actualRbwHz", schedule.actualRbwHz)
    .setFloat("gridStartHz", schedule.gridStartHz)
    .setFloat("gridBinWidthHz", schedule.gridBinWidthHz)
    .setInt("gridBinCount", schedule.gridBinCount)
    .setFloat("estimatedPassSeconds", schedule.estimatedPassSeconds)
    .setFloat("estimatedSweepRateHzPerSec", schedule.estimatedSweepRateHzPerSec)
    .setFloat("retuneOverheadFraction", schedule.retuneOverheadFraction)
    .set("unroutedHz", hashArray(schedule.unroutedHz, encodeRange))
    .setInt("portSwitches", schedule.portSwitches);
}

export function decodeSchedule(m: Metadata): ScheduleSummary {
  return {
    stepCount: getSize(m, "stepCount"),
    fftSize: getU32(m, "fftSize"),
    actualRbwHz: m.getFloat("actualRbwHz"),
    gridStartHz: m.getFloat("gridStartHz"),
    gridBinWidthHz: m.getFloat("gridBinWidthHz"),
    gridBinCount: getSize(m, "gridBinCount"),
    estimatedPassSeconds: m.getFloat("estimatedPassSeconds"),
    estimatedSweepRateHzPerSec: m.getFloat("estimatedSweepRateHzPerSec"),
    retuneOverheadFraction: m.getFloat("retuneOverheadFraction"),
    unroutedHz: hashList(m, "unroutedHz", decodeRange),
    portSwitches: getU32(m, "portSwitches"),
  };
}

export function encodeEngineStats(stats: EngineStats): Metadata {
  const out = new Metadata();
  setU64(out, "stitched", stats.stitched);
  setU64(out, "unsettled", stats.unsettled);
  setU64(out, "unattributed", stats.unattributed);
  setU64(out, "tooShort", stats.tooShort);
  out.setFloat("lastPassCoverage", stats.lastPassCoverage);
  out.setFloat("measuredSweepRateHzPerSec", stats.measuredSweepRateHzPerSec);
  setU64(out, "passCount", stats.passCount);
  return out;
}

export function decodeEngineStats(m: Metadata): EngineStats {
  return {
    stitched: getU64(m, "stitched"),
    unsettled: getU64(m, "unsettled"),
    unattributed: getU64(m, "unattributed"),
    tooShort: getU64(m, "tooShort"),
    lastPassCoverage: m.getFloat("lastPassCoverage"),
    measuredSweepRateHzPerSec: m.getFloat("measuredSweepRateHzPerSec"),
    passCount: getU64(m, "passCount"),
  };
}

export function encodeBackends(backends: readonly FftBackendInfo[]): Metadata {
  return new Metadata().set("items", hashArray(backends, encodeBackend));
}

export function decodeBackends(m: Metadata): FftBackendInfo[] {
  return hashList(m, "items", decodeBackend);
}

// ---- corrections ------------------------------------------------------------------

export function encodeCorrectionSettings(settings: CorrectionSettings): Metadata {
  return new Metadata()
    .setBool("dcRemoval", settings.dcRemoval)
    .setBool("flatten", settings.flatten)
    .setBool("spurMask", settings.spurMask)
    .setBool("autoSpurs", settings.autoSpurs);
}

export function decodeCorrectionSettings(m: Metadata): CorrectionSettings {
  const defaults = defaultCorrectionSettings();
  return {
    dcRemoval: m.getBool("dcRemoval", defaults.dcRemoval),
    flatten: m.getBool("flatten", defaults.flatten),
    spurMask: m.getBool("spurMask", defaults.spurMask),
    autoSpurs: m.getBool("autoSpurs", defaults.autoSpurs),
  };
}

export function encodeCorrectionSummary(summary: CorrectionSummary): Metadata {
  return new Metadata()
    .setBool("present", summary.present)
    .setInt("floorPoints", summary.floorPoints)
    .setInt("spurs", summary.spurs)
    .setInt("automaticSpurs", summary.automaticSpurs)
    .setString("learnedAt", summary.learnedAt)
    .setString("floorStaleReason", summary.floorStaleReason);
}

export function decodeCorrectionSummary(m: Metadata): CorrectionSummary {
  return {
    present: m.getBool("present"),
    floorPoints: getSize(m, "floorPoints"),
    spurs: getSize(m, "spurs"),
    automaticSpurs: getSize(m, "automaticSpurs"),
    learnedAt: m.getString("learnedAt"),
    floorStaleReason: m.getString("floorStaleReason"),
  };
}

// ---- antennas ---------------------------------------------------------------------

/** The whole library, shipped entries included and marked as such. */
export function encodeAntennas(antennas: readonly Antenna[]): Metadata {
  return new Metadata().set("items", hashArray(antennas, encodeAntenna));
}

export function decodeAntennas(m: Metadata): Antenna[] {
  return hashList(m, "items", decodeAntenna);
}

export function encodeAssignments(assignments: AntennaAssignments): Metadata {
  return new Metadata()
    .set("entries", hashArray(assignments.entries, encodeAssignment))
    .set("fallbackPorts", hashArray(assignments.fallbackPorts, encodeFallback));
}

export function decodeAssignments(m: Metadata): AntennaAssignments {
  return {
    entries: hashList(m, "entries", decodeAssignment),
    fallbackPorts: hashList(m, "fallbackPorts", decodeFallback),
  };
}

export function encodeSwitcherViews(views: readonly SwitcherView[]): Metadata {
  return new Metadata().set("items", hashArray(views, encodeSwitcherView));
}

export function decodeSwitcherViews(m: Metadata): SwitcherView[] {
  return hashList(m, "items", decodeSwitcherView);
}

export function encodeSwitcherInfos(infos: readonly RfPathInfo[]): Metadata {
  return new Metadata().set("items", hashArray(infos, encodeSwitcherInfo));
}

export function decodeSwitcherInfos(m: Metadata): RfPathInfo[] {
  return hashList(m, "items", decodeSwitcherInfo);
}

export function encodeRfLegs(legs: readonly RfLegView[]): Metadata {
  return new Metadata().set("items", hashArray(legs, encodeRfLeg));
}

export function decodeRfLegs(m: Metadata): RfLegView[] {
  return hashList(m, "items", decodeRfLeg);
}

export function encodeRanges(ranges: readonly HzRange[]): Metadata {
  return new Metadata().set("items", hashArray(ranges, encodeRange));
}

export function decodeRanges(m: Metadata): HzRange[] {
  return hashList(m, "items", decodeRange);
}

// ---- telemetry --------------------------------------------------------------------

export function encodeHealth(readings: readonly SdrHealthReading[]): Metadata {
  return new Metadata().set("items", hashArray(readings, encodeReading));
}

export function decodeHealth(m: Metadata): SdrHealthReading[] {
  return hashList(m, "items", decodeReading);
}

export function encodeStreamStats(stats: StreamStats): Metadata {
  const out = new Metadata()
    .setFloat("configuredSps", stats.configuredSps)
    .setFloat("measuredSps", stats.measuredSps)
    .setFloat("bytesPerSecIn", stats.bytesPerSecIn)
    .setFloat("linkCapacityBytesPerSec", stats.linkCapacityBytesPerSec)
    .setFloat("linkUtilisation", stats.linkUtilisation);
  setU64(out, "samplesDelivered", stats.samplesDelivered);
  setU64(out, "samplesDropped", stats.samplesDropped);
  setU64(out, "samplesLostAtSource", stats.samplesLostAtSource);
  setU64(out, "deviceOverruns", stats.deviceOverruns);
  setU64(out, "ringFullEvents", stats.ringFullEvents);
  setU64(out, "poolExhaustedEvents", stats.poolExhaustedEvents);
  setU64(out, "sequenceGaps", stats.sequenceGaps);
  return out
    .setFloat("dropRatePerSec", stats.dropRatePerSec)
    .setFloat("dropFraction", stats.dropFraction)
    .setFloat("ringFillFraction", Math.fround(stats.ringFillFraction));
}

export function decodeStreamStats(m: Metadata): StreamStats {
  return {
    configuredSps: m.getFloat("configuredSps"),
    measuredSps: m.getFloat("measuredSps"),
    bytesPerSecIn: m.getFloat("bytesPerSecIn"),
    linkCapacityBytesPerSec: m.getFloat("linkCapacityBytesPerSec"),
    linkUtilisation: m.getFloat("linkUtilisation"),
    samplesDelivered: getU64(m, "samplesDelivered"),
    samplesDropped: getU64(m, "samplesDropped"),
    samplesLostAtSource: getU64(m, "samplesLostAtSource"),
    deviceOverruns: getU64(m, "deviceOverruns"),
    ringFullEvents: getU64(m, "ringFullEvents"),
    poolExhaustedEvents: getU64(m, "poolExhaustedEvents"),
    sequenceGaps: getU64(m, "sequenceGaps"),
    dropRatePerSec: m.getFloat("dropRatePerSec"),
    dropFraction: m.getFloat("dropFraction"),
    ringFillFraction: getF32(m, "ringFillFraction"),
  };
}

export function encodeProcessStats(stats: ProcessStats): Metadata {
  const out = new Metadata().setFloat("fftsPerSec", stats.fftsPerSec);
  setU64(out, "fftsComputed", stats.fftsComputed);
  setU64(out, "fftsSkipped", stats.fftsSkipped);
  setU64(out, "samplesProcessedTotal", stats.samplesProcessedTotal);
  out.setFloat("processedFraction", stats.processedFraction).setFloat("framesPerSec", stats.framesPerSec);
  setU64(out, "sweepPassesCompleted", stats.sweepPassesCompleted);
  return out
    .setFloat("retunesPerSec", stats.retunesPerSec)
    .setFloat("fftLatencyP50Us", Math.fround(stats.fftLatencyP50Us))
    .setFloat("fftLatencyP99Us", Math.fround(stats.fftLatencyP99Us))
    .setFloat("fftLatencyMaxUs", Math.fround(stats.fftLatencyMaxUs))
    .setFloat("workerUtilisation", stats.workerUtilisation)
    .setInt("workerCount", stats.workerCount)
    .setInt("throttleReason", stats.throttleReason)
    .setFloat("sweepSpeedHzPerSec", stats.sweepSpeedHzPerSec);
}

export function decodeProcessStats(m: Metadata): ProcessStats {
  return {
    fftsPerSec: m.getFloat("fftsPerSec"),
    fftsComputed: getU64(m, "fftsComputed"),
    fftsSkipped: getU64(m, "fftsSkipped"),
    samplesProcessedTotal: getU64(m, "samplesProcessedTotal"),
    processedFraction: m.getFloat("processedFraction"),
    framesPerSec: m.getFloat("framesPerSec"),
    sweepPassesCompleted: getU64(m, "sweepPassesCompleted"),
    retunesPerSec: m.getFloat("retunesPerSec"),
    fftLatencyP50Us: getF32(m, "fftLatencyP50Us"),
    fftLatencyP99Us: getF32(m, "fftLatencyP99Us"),
    fftLatencyMaxUs: getF32(m, "fftLatencyMaxUs"),
    workerUtilisation: m.getFloat("workerUtilisation"),
    workerCount: getU32(m, "workerCount"),
    throttleReason: getEnum<ThrottleReasonId>(m, "throttleReason", ThrottleReason.Stopped, 7),
    sweepSpeedHzPerSec: m.getFloat("sweepSpeedHzPerSec"),
  };
}

// ---- the benchmark ----------------------------------------------------------------

export function encodeBenchmarkConfig(config: FftBenchmarkConfig): Metadata {
  return new Metadata()
    .set("sizes", sizeArray(config.sizes))
    .set("threadCounts", sizeArray(config.threadCounts))
    .setInt("quality", config.quality)
    .setFloat("secondsPerSample", config.secondsPerSample)
    .setInt("minRuns", config.minRuns)
    .setInt("maxRuns", config.maxRuns)
    .setInt("warmupRuns", config.warmupRuns);
}

/** Bounded as the server bounds it, since it runs what this says on its own
 * cores: so a config the server would change is not shown unchanged here. */
export function decodeBenchmarkConfig(m: Metadata): FftBenchmarkConfig {
  const kMaxSizes = 64;
  const kMaxThreadCounts = 8;
  const defaults = defaultFftBenchmarkConfig();
  const sizes = sizeList(m, "sizes", kMaxSizes);
  const threadCounts = sizeList(m, "threadCounts", kMaxThreadCounts);
  const seconds = m.getFloat("secondsPerSample", defaults.secondsPerSample);
  const minRuns = getSize(m, "minRuns", defaults.minRuns);
  const maxRuns = getSize(m, "maxRuns", defaults.maxRuns);
  // std::max over size_t: compared unsigned, so a negative read is the larger.
  const maxIsLarger = BigInt.asUintN(64, BigInt(maxRuns)) >= BigInt.asUintN(64, BigInt(minRuns));
  return {
    sizes: sizes.length > 0 ? sizes : defaults.sizes,
    threadCounts: threadCounts.length > 0 ? threadCounts : [1],
    quality: getEnum(m, "quality", defaults.quality, 3),
    secondsPerSample: seconds < 0.001 ? 0.001 : 60 < seconds ? 60 : seconds,
    minRuns,
    maxRuns: maxIsLarger ? maxRuns : minRuns,
    warmupRuns: getSize(m, "warmupRuns", defaults.warmupRuns),
  };
}

export function encodeBenchmarkStatus(status: BenchmarkStatus): Metadata {
  return new Metadata()
    .setBool("running", status.running)
    .setBool("complete", status.complete)
    .setInt("stepsDone", status.stepsDone)
    .setInt("stepsTotal", status.stepsTotal)
    .setString("currentStep", status.currentStep)
    .setFloat("elapsedSeconds", status.elapsedSeconds)
    .set("results", hashArray(status.results, encodeBenchmarkEntry));
}

export function decodeBenchmarkStatus(m: Metadata): BenchmarkStatus {
  return {
    running: m.getBool("running"),
    complete: m.getBool("complete"),
    stepsDone: getSize(m, "stepsDone"),
    stepsTotal: getSize(m, "stepsTotal"),
    currentStep: m.getString("currentStep"),
    elapsedSeconds: m.getFloat("elapsedSeconds"),
    results: hashList(m, "results", decodeBenchmarkEntry),
  };
}

export function encodeNotice(notice: Notice): Metadata {
  return new Metadata().setInt("kind", notice.kind).setString("text", notice.text);
}

// ---- state sections, as RemoteServer assembles them ------------------------------------

export interface DeviceSection {
  /** Null when no radio is open: the section's `present` is false. */
  descriptor: DeviceDescriptor | null;
  antennaKey: string;
  profileDriver: string;
  profileId: string;
  displayLabel: string;
}

export function encodeDeviceSection(section: DeviceSection): Metadata {
  const out = new Metadata().setBool("present", section.descriptor !== null);
  if (section.descriptor !== null) {
    out.setHash("descriptor", encodeDevice(section.descriptor));
  }
  return out
    .setString("antennaKey", section.antennaKey)
    .setString("profileDriver", section.profileDriver)
    .setString("profileId", section.profileId)
    .setString("displayLabel", section.displayLabel);
}

export function decodeDeviceSection(m: Metadata): DeviceSection {
  return {
    descriptor: m.getBool("present") ? decodeDevice(m.getHash("descriptor")) : null,
    antennaKey: m.getString("antennaKey"),
    profileDriver: m.getString("profileDriver"),
    profileId: m.getString("profileId"),
    displayLabel: m.getString("displayLabel"),
  };
}

export interface ValuesSection {
  /** Every parameter the radio reports a value for, by key. */
  parameters: Map<string, SdrValue>;
  selectedRxPort: string;
}

export function encodeValuesSection(section: ValuesSection): Metadata {
  const parameters = new Metadata();
  for (const [key, v] of section.parameters) {
    parameters.set(key, encodeValue(v));
  }
  return new Metadata().setHash("parameters", parameters).setString("selectedRxPort", section.selectedRxPort);
}

export function decodeValuesSection(m: Metadata): ValuesSection {
  const parameters = new Map<string, SdrValue>();
  for (const [key, v] of m.getHash("parameters").entries) {
    parameters.set(key, decodeValue(v));
  }
  return { parameters, selectedRxPort: m.getString("selectedRxPort") };
}

export interface RunSection {
  running: boolean;
  sweeping: boolean;
  startGeneration: bigint;
  engine: EngineStats;
}

export function encodeRunSection(section: RunSection): Metadata {
  const out = new Metadata().setBool("running", section.running).setBool("sweeping", section.sweeping);
  setU64(out, "startGeneration", section.startGeneration);
  return out.setHash("engine", encodeEngineStats(section.engine));
}

export function decodeRunSection(m: Metadata): RunSection {
  return {
    running: m.getBool("running"),
    sweeping: m.getBool("sweeping"),
    startGeneration: getU64(m, "startGeneration"),
    engine: decodeEngineStats(m.getHash("engine")),
  };
}

export interface BackendsSection {
  backends: FftBackendInfo[];
  current: string;
}

export function encodeBackendsSection(section: BackendsSection): Metadata {
  return encodeBackends(section.backends).setString("current", section.current);
}

export function decodeBackendsSection(m: Metadata): BackendsSection {
  return { backends: decodeBackends(m), current: m.getString("current") };
}

export interface CorrectionsSection {
  settings: CorrectionSettings;
  summary: CorrectionSummary;
}

export function encodeCorrectionsSection(section: CorrectionsSection): Metadata {
  return new Metadata()
    .setHash("settings", encodeCorrectionSettings(section.settings))
    .setHash("summary", encodeCorrectionSummary(section.summary));
}

export function decodeCorrectionsSection(m: Metadata): CorrectionsSection {
  return {
    settings: decodeCorrectionSettings(m.getHash("settings")),
    summary: decodeCorrectionSummary(m.getHash("summary")),
  };
}

export interface LearningSection {
  active: boolean;
  label: string;
}

export function encodeLearningSection(section: LearningSection): Metadata {
  return new Metadata().setBool("active", section.active).setString("label", section.label);
}

export function decodeLearningSection(m: Metadata): LearningSection {
  return { active: m.getBool("active"), label: m.getString("label") };
}

export interface SwitchersSection {
  open: SwitcherView[];
  available: RfPathInfo[];
}

export function encodeSwitchersSection(section: SwitchersSection): Metadata {
  return new Metadata()
    .setHash("open", encodeSwitcherViews(section.open))
    .setHash("available", encodeSwitcherInfos(section.available));
}

export function decodeSwitchersSection(m: Metadata): SwitchersSection {
  return {
    open: decodeSwitcherViews(m.getHash("open")),
    available: decodeSwitcherInfos(m.getHash("available")),
  };
}

export interface RfPathSection {
  legs: RfLegView[];
  coverage: HzRange[];
}

export function encodeRfPathSection(section: RfPathSection): Metadata {
  return new Metadata()
    .setHash("legs", encodeRfLegs(section.legs))
    .setHash("coverage", encodeRanges(section.coverage));
}

export function decodeRfPathSection(m: Metadata): RfPathSection {
  return { legs: decodeRfLegs(m.getHash("legs")), coverage: decodeRanges(m.getHash("coverage")) };
}

export interface OverlaysSection {
  generation: bigint;
}

export function encodeOverlaysSection(section: OverlaysSection): Metadata {
  return new Metadata().setInt("generation", section.generation);
}

export function decodeOverlaysSection(m: Metadata): OverlaysSection {
  return { generation: m.getBigInt("generation") };
}

export interface LinkSection {
  /** Zero sends frames whole. */
  maxBins: number;
}

export function encodeLinkSection(section: LinkSection): Metadata {
  return new Metadata().setInt("maxBins", section.maxBins);
}

export function decodeLinkSection(m: Metadata): LinkSection {
  return { maxBins: Number(BigInt.asUintN(32, m.getBigInt("maxBins"))) };
}

/** The encoders for what messages.ts decodes: `ServerRecordings::toMetadata`,
 * `ControlState::toMetadata` and `encodeClients`. */
export function encodeRecordingsSection(recordings: ServerRecordings): Metadata {
  return new Metadata()
    .setBool("available", recordings.available)
    .setBool("active", recordings.active)
    .setString("current", recordings.current)
    .setInt("lines", recordings.lines)
    .setInt("bytes", recordings.bytes)
    .set(
      "files",
      hashArray(recordings.files, (file) =>
        new Metadata()
          .setString("name", file.name)
          .setInt("bytes", file.bytes)
          .setInt("modifiedWallNs", file.modifiedWallNs),
      ),
    );
}

export function encodeControlSection(control: ControlState): Metadata {
  return new Metadata()
    .setBool("shared", control.shared)
    .setBool("you", control.you)
    .setBool("held", control.held)
    .setString("controller", control.controller)
    .setString("controllerKind", control.controllerKind);
}

export function encodeClientsSection(clients: readonly ConnectedClient[]): Metadata {
  return new Metadata().set(
    "list",
    hashArray(clients, (client) =>
      new Metadata()
        .setInt("id", client.id)
        .setString("name", client.name)
        .setString("kind", client.kind)
        .setString("address", client.address)
        .setBool("controls", client.controls)
        .setBool("you", client.you),
    ),
  );
}
