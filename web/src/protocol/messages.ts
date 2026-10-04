// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { ByteReader, ByteWriter, ProtocolError } from "./bytes";
import { kMaxMetadataDepth, Metadata, value, ValueType } from "./metadata";
import { appendRecord, RecordType, type StreamRecord } from "./records";

/** The remote protocol: control messages as PluginData records under one
 * plugin id, each body a metadata hash. Mirrors remote/Protocol.hpp. */

export const kPluginId = "org.sweeppp.remote";
export const kProtocolVersion = 2;
export const kMaxServerRecordBytes = 16 * 1024 * 1024;
export const kPingIntervalMs = 1000;
export const kSilenceTimeoutMs = 10_000;
export const kMinLinkBins = 1024;

export const msg = {
  hello: "hello",
  welcome: "welcome",
  refused: "refused",
  command: "command",
  reply: "reply",
  state: "state",
  notice: "notice",
  frame: "frame",
  ping: "ping",
  pong: "pong",
  bye: "bye",
  chunk: "chunk",
  history: "history",
} as const;

export const refusal = {
  busy: "busy",
  version: "version",
  limit: "limit",
  protocol: "protocol",
  shutdown: "shutdown",
} as const;

export const clientKind = { desktop: "desktop", web: "web" } as const;

export const op = {
  start: "start",
  stop: "stop",
  restart: "restart",
  setSweeping: "setSweeping",
  applySweepPlan: "applySweepPlan",
  sweepRange: "sweepRange",
  applyPipelineConfig: "applyPipelineConfig",
  setFftBackend: "setFftBackend",
  setParameter: "setParameter",
  resetTelemetry: "resetTelemetry",
  setCorrectionSettings: "setCorrectionSettings",
  startLearning: "startLearning",
  cancelLearning: "cancelLearning",
  clearAutoSpurs: "clearAutoSpurs",
  clearCorrections: "clearCorrections",
  setUserAntennas: "setUserAntennas",
  setAssignments: "setAssignments",
  rescanSwitchers: "rescanSwitchers",
  setLinkResolution: "setLinkResolution",
  startBenchmark: "startBenchmark",
  cancelBenchmark: "cancelBenchmark",
  startRecording: "startRecording",
  stopRecording: "stopRecording",
  deleteRecording: "deleteRecording",
  fetchRecording: "fetchRecording",
  takeControl: "takeControl",
  releaseControl: "releaseControl",
  historyOpen: "historyOpen",
  historyQuery: "historyQuery",
  historyClose: "historyClose",
  setContributorShown: "setContributorShown",
  setContributorOrder: "setContributorOrder",
  selectContributorDataset: "selectContributorDataset",
  toggleContributorRow: "toggleContributorRow",
  hideContribution: "hideContribution",
} as const;
export type Op = (typeof op)[keyof typeof op];

export const section = {
  device: "device",
  values: "values",
  run: "run",
  plan: "plan",
  schedule: "schedule",
  pipeline: "pipeline",
  backends: "backends",
  corrections: "corrections",
  learning: "learning",
  antennas: "antennas",
  assignments: "assignments",
  switchers: "switchers",
  rfPath: "rfPath",
  link: "link",
  benchmark: "benchmark",
  recordings: "recordings",
  control: "control",
  clients: "clients",
  /** A counter that moves when the server's band and channel labels change. */
  overlays: "overlays",
} as const;
export type Section = (typeof section)[keyof typeof section];

/** The sections a command can change: the server sends them again with its
 * acknowledgement, and the client keeps its own copy until then. */
export function sectionsTouchedBy(name: string): readonly Section[] {
  switch (name) {
    case op.start:
    case op.stop:
    case op.restart:
    case op.setSweeping:
      return [section.run, section.plan, section.schedule];
    case op.applySweepPlan:
    case op.sweepRange:
      return [section.plan, section.schedule, section.run];
    case op.applyPipelineConfig:
      return [section.pipeline];
    case op.setFftBackend:
      return [section.backends];
    case op.setParameter:
      return [section.values, section.plan, section.schedule, section.run];
    case op.setCorrectionSettings:
    case op.startLearning:
    case op.cancelLearning:
    case op.clearAutoSpurs:
    case op.clearCorrections:
      return [section.corrections, section.learning];
    case op.setUserAntennas:
      return [section.antennas, section.rfPath];
    case op.setAssignments:
      return [section.assignments, section.rfPath, section.plan, section.schedule];
    case op.rescanSwitchers:
      return [section.switchers, section.rfPath];
    case op.setLinkResolution:
      return [section.link];
    case op.startBenchmark:
    case op.cancelBenchmark:
      return [section.benchmark];
    case op.startRecording:
    case op.stopRecording:
    case op.deleteRecording:
      return [section.recordings];
    case op.takeControl:
    case op.releaseControl:
      return [section.control, section.clients];
    default:
      return [];
  }
}

/** What a client that does not control a shared server may still send. */
export function viewerMay(name: string): boolean {
  return (
    name === op.setLinkResolution ||
    name === op.fetchRecording ||
    name === op.takeControl ||
    name === op.releaseControl ||
    name === op.historyOpen ||
    name === op.historyQuery ||
    name === op.historyClose
  );
}

// ---- the envelope ---------------------------------------------------------------

export interface Message {
  name: string;
  monotonicNs: bigint;
  body: Metadata;
}

/** Appends a whole PluginData record carrying `body` as message `name`. */
export function appendMessage(out: ByteWriter, name: string, body: Metadata, monotonicNs = 0n): void {
  const encodedBody = body.toBytes();
  const payload = new ByteWriter()
    .string(kPluginId)
    .string(name)
    .u32(kProtocolVersion)
    .u64(monotonicNs)
    .u32(encodedBody.length)
    .bytes(encodedBody)
    .finish();
  appendRecord(out, RecordType.PluginData, payload);
}

/** The record as a control message. The caller checks it is PluginData. */
export function decodeMessage(record: StreamRecord): Message {
  const in_ = new ByteReader(record.payload);
  const pluginId = in_.string();
  const name = in_.string();
  const schemaVersion = in_.u32();
  const monotonicNs = in_.u64();
  const bodyBytes = in_.u32();
  if (bodyBytes > in_.remaining) {
    throw new ProtocolError(`plugin record claims ${bodyBytes} bytes of body but only ${in_.remaining} remain`);
  }
  if (pluginId !== kPluginId) {
    throw new ProtocolError(`a record for '${pluginId}' on the control stream`);
  }
  if (schemaVersion !== kProtocolVersion) {
    throw new ProtocolError(`protocol version ${schemaVersion}, this page speaks ${kProtocolVersion}`);
  }
  const body = Metadata.decode(new ByteReader(in_.bytes(bodyBytes)), kMaxMetadataDepth);
  return { name, monotonicNs, body };
}

// ---- messages --------------------------------------------------------------------

export interface Hello {
  protocolVersion: number;
  software: string;
  kind: string;
  name: string;
  clientId: string;
}

export function encodeHello(hello: Hello): Metadata {
  return new Metadata()
    .setInt("protocolVersion", hello.protocolVersion)
    .setString("software", hello.software)
    .setString("kind", hello.kind)
    .setString("name", hello.name)
    .setString("clientId", hello.clientId);
}

export interface Welcome {
  serverName: string;
  shared: boolean;
}

export function decodeWelcome(m: Metadata): Welcome {
  return { serverName: m.getString("serverName"), shared: m.getBool("shared") };
}

export interface Refused {
  reason: string;
  message: string;
}

export function decodeRefused(m: Metadata): Refused {
  return { reason: m.getString("reason"), message: m.getString("message") };
}

export interface Command {
  seq: bigint;
  op: string;
  args: Metadata;
}

export function encodeCommand(command: Command): Metadata {
  return new Metadata()
    .setInt("seq", command.seq)
    .setString("op", command.op)
    .setHash("args", command.args);
}

export interface Reply {
  seq: bigint;
  ok: boolean;
  code: number;
  message: string;
}

export function decodeReply(m: Metadata): Reply {
  return {
    seq: m.getBigInt("seq"),
    ok: m.getBool("ok"),
    code: m.getInt("code"),
    message: m.getString("message"),
  };
}

export interface State {
  ackSeq: bigint;
  sections: Metadata;
}

export function decodeState(m: Metadata): State {
  return { ackSeq: m.getBigInt("ackSeq"), sections: m.getHash("sections") };
}

export interface FrameCommit {
  segmentId: number;
  line: number;
  sequence: bigint;
  hostTimeNs: bigint;
  wallTimeNs: bigint;
  deviceTimeNs: bigint;
  sweepPass: bigint;
  sweepStep: number;
  passComplete: boolean;
  averageCount: number;
  clippedFraction: number;
  /** Kept on the server while this client was away, and sent on its return. */
  replayed: boolean;
}

/** A u32 field, or the fallback for anything out of range. */
function u32(m: Metadata, key: string, fallback = 0): number {
  const v = m.getBigInt(key, BigInt(fallback));
  return v < 0n || v > 0xffffffffn ? fallback : Number(v);
}

export function decodeFrameCommit(m: Metadata): FrameCommit {
  return {
    segmentId: u32(m, "segmentId"),
    line: u32(m, "line"),
    sequence: BigInt.asUintN(64, m.getBigInt("sequence")),
    hostTimeNs: BigInt.asUintN(64, m.getBigInt("hostTimeNs")),
    wallTimeNs: BigInt.asUintN(64, m.getBigInt("wallTimeNs")),
    deviceTimeNs: BigInt.asUintN(64, m.getBigInt("deviceTimeNs")),
    sweepPass: BigInt.asUintN(64, m.getBigInt("sweepPass")),
    sweepStep: u32(m, "sweepStep"),
    passComplete: m.getBool("passComplete"),
    averageCount: u32(m, "averageCount", 1),
    clippedFraction: m.getFloat("clippedFraction"),
    replayed: m.getBool("replayed"),
  };
}

export function encodePing(id: bigint, clientNs: bigint): Metadata {
  return new Metadata().setInt("id", id).setInt("clientNs", clientNs);
}

export interface Pong {
  id: bigint;
  clientNs: bigint;
  serverNs: bigint;
  serverWallNs: bigint;
}

export function decodePong(m: Metadata): Pong {
  return {
    id: BigInt.asUintN(64, m.getBigInt("id")),
    clientNs: BigInt.asUintN(64, m.getBigInt("clientNs")),
    serverNs: BigInt.asUintN(64, m.getBigInt("serverNs")),
    serverWallNs: BigInt.asUintN(64, m.getBigInt("serverWallNs")),
  };
}

export function encodeBye(reason: string): Metadata {
  return new Metadata().setString("reason", reason);
}

export const NoticeKind = {
  Info: 0,
  Success: 1,
  Warning: 2,
  Error: 3,
  Condition: 4,
  ClearCondition: 5,
} as const;
export type NoticeKindId = (typeof NoticeKind)[keyof typeof NoticeKind];

export interface Notice {
  kind: NoticeKindId;
  text: string;
}

export function decodeNotice(m: Metadata): Notice {
  const kind = m.getInt("kind");
  return {
    kind: (kind >= 0 && kind < 6 ? kind : NoticeKind.Info) as NoticeKindId,
    text: m.getString("text"),
  };
}

export interface ControlState {
  shared: boolean;
  you: boolean;
  held: boolean;
  controller: string;
  controllerKind: string;
}

export function decodeControl(m: Metadata): ControlState {
  return {
    shared: m.getBool("shared"),
    you: m.getBool("you"),
    held: m.getBool("held"),
    controller: m.getString("controller"),
    controllerKind: m.getString("controllerKind"),
  };
}

export interface ConnectedClient {
  id: number;
  name: string;
  kind: string;
  address: string;
  controls: boolean;
  you: boolean;
}

export function decodeClients(m: Metadata): ConnectedClient[] {
  return m.getHashes("list").map((row) => ({
    id: unsigned(row, "id"),
    name: row.getString("name"),
    kind: row.getString("kind"),
    address: row.getString("address"),
    controls: row.getBool("controls"),
    you: row.getBool("you"),
  }));
}

export interface RecordingFile {
  name: string;
  bytes: number;
  modifiedWallNs: bigint;
}

export interface ServerRecordings {
  available: boolean;
  active: boolean;
  current: string;
  lines: number;
  bytes: number;
  files: RecordingFile[];
}

/** A u64 field as a number: exact up to 2^53, which no count here reaches. */
function unsigned(m: Metadata, key: string): number {
  return Number(BigInt.asUintN(64, m.getBigInt(key)));
}

export function decodeRecordings(m: Metadata): ServerRecordings {
  return {
    available: m.getBool("available"),
    active: m.getBool("active"),
    current: m.getString("current"),
    lines: unsigned(m, "lines"),
    bytes: unsigned(m, "bytes"),
    files: m.getHashes("files").map((row) => ({
      name: row.getString("name"),
      bytes: unsigned(row, "bytes"),
      modifiedWallNs: BigInt.asUintN(64, row.getBigInt("modifiedWallNs")),
    })),
  };
}

/** The body of a Telemetry record: the server's figures, kept as metadata
 * for the panels to read. */
export function decodeTelemetry(record: StreamRecord): Metadata {
  return Metadata.fromBytes(record.payload, kMaxMetadataDepth);
}

export { value, ValueType };
