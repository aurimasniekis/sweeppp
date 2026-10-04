// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";
import { useMemo } from "react";

import {
  decodeClients,
  decodeControl,
  decodeRecordings,
  op,
  section,
} from "../protocol/messages";
import { Metadata } from "../protocol/metadata";
import {
  type AntennaAssignments,
  type CorrectionSettings,
  decodeAntennas,
  decodeAssignments,
  decodeBackendsSection,
  decodeBenchmarkStatus,
  decodeCorrectionsSection,
  decodeDeviceSection,
  decodeLearningSection,
  decodeLinkSection,
  decodePipeline,
  decodePlan,
  decodeRfPathSection,
  decodeRunSection,
  decodeSchedule,
  decodeSwitchersSection,
  decodeValuesSection,
  encodeAntennas,
  encodeAssignments,
  encodeBenchmarkConfig,
  encodeCorrectionSettings,
  encodePipeline,
  encodePlan,
  encodeValue,
  type Antenna,
  type FftBenchmarkConfig,
  type PipelineConfig,
  type SdrValue,
  type SweepPlan,
} from "../protocol/wire";
import { patched, send } from "./actions";
import { session, sessionStore } from "./session";

/** The instrument as the server last described it, decoded. */
export function readInstrument() {
  const s = (name: string) => session.section(name);
  return {
    device: decodeDeviceSection(s(section.device)),
    values: decodeValuesSection(s(section.values)),
    run: decodeRunSection(s(section.run)),
    plan: decodePlan(s(section.plan)),
    schedule: decodeSchedule(s(section.schedule)),
    pipeline: decodePipeline(s(section.pipeline)),
    backends: decodeBackendsSection(s(section.backends)),
    corrections: decodeCorrectionsSection(s(section.corrections)),
    learning: decodeLearningSection(s(section.learning)),
    antennas: decodeAntennas(s(section.antennas)),
    assignments: decodeAssignments(s(section.assignments)),
    switchers: decodeSwitchersSection(s(section.switchers)),
    rfPath: decodeRfPathSection(s(section.rfPath)),
    link: decodeLinkSection(s(section.link)),
    benchmark: decodeBenchmarkStatus(s(section.benchmark)),
    recordings: decodeRecordings(s(section.recordings)),
    control: decodeControl(s(section.control)),
    clients: decodeClients(s(section.clients)),
    /** Whether this page may change anything. */
    canControl: session.remote?.canControl ?? false,
  };
}

export type InstrumentView = ReturnType<typeof readInstrument>;

/** The instrument, re-read whenever a section changes. */
export function useInstrument(): InstrumentView {
  const version = useSelector(sessionStore, (s) => s.sectionsVersion);
  return useMemo(readInstrument, [version]);
}

// ---- edits: each lands here at once and goes to the server ---------------------

export function applySweepPlan(plan: SweepPlan): void {
  send(op.applySweepPlan, new Metadata().setHash("plan", encodePlan(plan)), (sections) =>
    sections.set(section.plan, encodePlan(plan)),
  );
}

/** Sweeps `plan` whether or not the radio was sweeping: one restart. */
export function sweepRange(plan: SweepPlan): void {
  send(op.sweepRange, new Metadata().setHash("plan", encodePlan(plan)), (sections) => {
    sections.set(section.plan, encodePlan(plan));
    patched(section.run, (m) => m.setBool("sweeping", true))(sections);
  });
}

export function setSweeping(enabled: boolean): void {
  send(op.setSweeping, new Metadata().setBool("enabled", enabled), patched(section.run, (m) => m.setBool("sweeping", enabled)));
}

export function applyPipelineConfig(config: PipelineConfig): void {
  send(op.applyPipelineConfig, new Metadata().setHash("config", encodePipeline(config)), (sections) =>
    sections.set(section.pipeline, encodePipeline(config)),
  );
}

export function setFftBackend(name: string): void {
  send(op.setFftBackend, new Metadata().setString("name", name), patched(section.backends, (m) => m.setString("current", name)));
}

export function setParameter(key: string, value: SdrValue): void {
  send(
    op.setParameter,
    new Metadata().setString("key", key).set("value", encodeValue(value)),
    patched(section.values, (m) => {
      const parameters = m.getHash("parameters");
      parameters.set(key, encodeValue(value));
      m.setHash("parameters", parameters);
    }),
  );
}

export function setCorrectionSettings(settings: CorrectionSettings): void {
  send(
    op.setCorrectionSettings,
    new Metadata().setHash("settings", encodeCorrectionSettings(settings)),
    patched(section.corrections, (m) => m.setHash("settings", encodeCorrectionSettings(settings))),
  );
}

export function startLearning(): void {
  send(op.startLearning, new Metadata(), patched(section.learning, (m) => m.setBool("active", true)));
}

export function cancelLearning(): void {
  send(op.cancelLearning, new Metadata(), patched(section.learning, (m) => m.setBool("active", false)));
}

export function clearAutoSpurs(): void {
  send(op.clearAutoSpurs);
}

export function clearCorrections(): void {
  send(op.clearCorrections);
}

/** The operator's own antennas; the shipped ones stay as they are. */
export function setUserAntennas(antennas: Antenna[]): void {
  const own = antennas.map((a) => ({ ...a, builtin: false }));
  send(op.setUserAntennas, new Metadata().setHash("antennas", encodeAntennas(own)), (sections) => {
    const builtin = decodeAntennas(session.section(section.antennas)).filter(
      (a) => a.builtin && !own.some((o) => o.id === a.id),
    );
    sections.set(section.antennas, encodeAntennas([...builtin, ...own]));
  });
}

export function setAssignments(assignments: AntennaAssignments): void {
  send(op.setAssignments, new Metadata().setHash("assignments", encodeAssignments(assignments)), (sections) =>
    sections.set(section.assignments, encodeAssignments(assignments)),
  );
}

export function rescanSwitchers(): void {
  send(op.rescanSwitchers);
}

export function resetTelemetry(): void {
  send(op.resetTelemetry);
}

export function startBenchmark(config: FftBenchmarkConfig): void {
  send(
    op.startBenchmark,
    new Metadata().setHash("config", encodeBenchmarkConfig(config)),
    patched(section.benchmark, (m) => m.setBool("running", true).setBool("complete", false)),
  );
}

export function cancelBenchmark(): void {
  send(op.cancelBenchmark);
}

export function startRecording(maxBins: number): void {
  send(op.startRecording, new Metadata().setInt("maxBins", maxBins));
}

export function stopRecording(): void {
  send(op.stopRecording);
}

export function deleteRecording(name: string): void {
  send(op.deleteRecording, new Metadata().setString("name", name));
}
