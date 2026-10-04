// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";
import { useCallback, useEffect, useRef, useState } from "react";

import type { Metadata } from "../../../protocol/metadata";
import {
  decodeHealth,
  decodeProcessStats,
  decodeStreamStats,
  type ProcessStats,
  type SdrHealthReading,
  type StreamStats,
} from "../../../protocol/wire";
import { byteRate, bytes, count, duration, frequencyShort } from "../../../render/format";
import { resetTelemetry, useInstrument } from "../../../state/instrument";
import { session, sessionStore } from "../../../state/session";
import { Bar, Histories, SectionTitle, Stat, Stats } from "./parts";

const kThrottleNames = ["none", "every-nth", "cpu-limited", "ring-full", "pool-exhausted", "device-overrun", "stopped"];

const kThrottleHint =
  "every-nth: 1/N coverage requested\n" +
  "cpu-limited: workers cannot keep up\n" +
  "ring-full / pool-exhausted: lost before a worker\n" +
  "device-overrun: dropped by the radio";

const kAccent = "var(--color-accent)";
const kOk = "var(--color-ok)";
const kWarning = "var(--color-warning)";
const kDanger = "var(--color-danger)";

interface ServerLink {
  framesSent: bigint;
  passesCoalesced: bigint;
  partialsCoalesced: bigint;
  eventsDropped: bigint;
  encodeNs: bigint;
}

/** The machine the server runs on, as `HostStats` reports it. */
interface ServerHost {
  system: string;
  cores: number;
  cpuPercent: number;
  processCpuPercent: number;
  memoryTotalBytes: number;
  memoryUsedBytes: number;
  processMemoryBytes: number;
  load1: number;
  uptimeSeconds: number;
  temperatureC: number | null;
  diskTotalBytes: number;
  diskFreeBytes: number;
}

function decodeServerHost(m: Metadata): ServerHost | null {
  if (m.size === 0) {
    return null;
  }
  const bytes = (key: string) => Number(BigInt.asUintN(64, m.getBigInt(key)));
  return {
    system: m.getString("system"),
    cores: m.getInt("cores"),
    cpuPercent: m.getFloat("cpuPercent", -1),
    processCpuPercent: m.getFloat("processCpuPercent", -1),
    memoryTotalBytes: bytes("memoryTotalBytes"),
    memoryUsedBytes: bytes("memoryUsedBytes"),
    processMemoryBytes: bytes("processMemoryBytes"),
    load1: m.getFloat("load1", -1),
    uptimeSeconds: m.getFloat("uptimeSeconds", -1),
    temperatureC: m.has("temperatureC") ? m.getFloat("temperatureC") : null,
    diskTotalBytes: bytes("diskTotalBytes"),
    diskFreeBytes: bytes("diskFreeBytes"),
  };
}

interface Snapshot {
  host: ServerHost | null;
  stream: StreamStats;
  process: ProcessStats;
  health: SdrHealthReading[];
  link: ServerLink;
  framesSentPerSec: number;
  roundTripMs: number;
  bytesPerSec: number;
  bytesReceived: number;
  framesPerSec: number;
  linesPerSec: number;
}

const u64 = (m: Metadata, key: string) => BigInt.asUintN(64, m.getBigInt(key));

function decodeServerLink(m: Metadata): ServerLink {
  return {
    framesSent: u64(m, "framesSent"),
    passesCoalesced: u64(m, "passesCoalesced"),
    partialsCoalesced: u64(m, "partialsCoalesced"),
    eventsDropped: u64(m, "eventsDropped"),
    encodeNs: u64(m, "encodeNs"),
  };
}

interface Previous {
  report: Metadata;
  monotonicNs: bigint;
  framesSent: bigint;
  framesSentPerSec: number;
}

/** The newest report, polled, with the recent history of its rates. */
function useTelemetry() {
  const [histories] = useState(() => new Histories());
  const previous = useRef<Previous | null>(null);
  const [snapshot, setSnapshot] = useState<Snapshot | null>(null);

  const poll = useCallback(() => {
    const report = session.telemetry;
    const stream = decodeStreamStats(report.getHash("stream"));
    const process = decodeProcessStats(report.getHash("process"));
    const health = decodeHealth(report.getHash("health"));
    const link = decodeServerLink(report.getHash("link"));
    const host = decodeServerHost(report.getHash("host"));
    const client = sessionStore.state;
    const bytesReceived = session.remote?.link.bytesReceived ?? 0;

    const before = previous.current;
    let framesSentPerSec = before?.framesSentPerSec ?? 0;
    const monotonicNs = u64(report, "monotonicNs");
    if (report !== before?.report && monotonicNs > 0n) {
      if (before && monotonicNs > before.monotonicNs) {
        // Counters that went back were reset, or belong to a new connection.
        framesSentPerSec =
          link.framesSent >= before.framesSent
            ? (Number(link.framesSent - before.framesSent) * 1e9) / Number(monotonicNs - before.monotonicNs)
            : 0;
      }
      previous.current = { report, monotonicNs, framesSent: link.framesSent, framesSentPerSec };

      histories.push("measuredSps", stream.measuredSps);
      histories.push("dropRate", stream.dropRatePerSec);
      histories.push("fftsPerSec", process.fftsPerSec);
      histories.push("fftLatencyP99", process.fftLatencyP99Us);
      histories.push("workerBusy", process.workerUtilisation * 100);
      histories.push("roundTrip", client.roundTripMs);
      histories.push("receiving", client.bytesPerSec);
      histories.push("framesSent", framesSentPerSec);
      histories.push("frames", client.framesPerSec);
      histories.push("lines", client.linesPerSec);
      if (host) {
        histories.push("serverCpu", Math.max(host.cpuPercent, 0));
        if (host.memoryTotalBytes > 0) {
          histories.push("serverMemory", (100 * host.memoryUsedBytes) / host.memoryTotalBytes);
        }
        if (host.temperatureC !== null) {
          histories.push("serverTemperature", host.temperatureC);
        }
      }
      for (const reading of health) {
        histories.push(`health:${reading.label}`, reading.numeric);
      }
    }

    setSnapshot({
      host,
      stream,
      process,
      health,
      link,
      framesSentPerSec,
      roundTripMs: client.roundTripMs,
      bytesPerSec: client.bytesPerSec,
      bytesReceived,
      framesPerSec: client.framesPerSec,
      linesPerSec: client.linesPerSec,
    });
  }, [histories]);

  useEffect(() => {
    poll();
    const timer = window.setInterval(poll, 250);
    return () => window.clearInterval(timer);
  }, [poll]);

  const reset = () => {
    resetTelemetry();
    histories.clear();
    previous.current = null;
    poll();
  };

  return { snapshot, histories, reset };
}

const n = (value: bigint) => count(Number(value));
const share = (part: bigint, total: bigint) => (total > 0n ? (Number(part) * 100) / Number(total) : 0);

/** The machine at the other end, which nobody is sitting at. */
function ServerSection({ host, histories }: { host: ServerHost; histories: Histories }) {
  const hot = host.temperatureC !== null && host.temperatureC >= 80;
  return (
    <>
      <SectionTitle title="Server" />
      <Stats>
        {host.system && <Stat label="System" value={`${host.system}, ${host.cores} cores`} />}
        {host.cpuPercent >= 0 && (
          <Stat
            label="CPU"
            value={`${host.cpuPercent.toFixed(0)}% of all cores`}
            trend={{ values: histories.get("serverCpu"), max: 100, color: kWarning }}
          />
        )}
        {(host.processCpuPercent >= 0 || host.processMemoryBytes > 0) && (
          <Stat
            label="sweeppp-cli"
            value={`${Math.max(host.processCpuPercent, 0).toFixed(0)}% of one core, ${bytes(host.processMemoryBytes)}`}
          />
        )}
        {host.memoryTotalBytes > 0 && (
          <Stat
            label="Memory"
            value={`${bytes(host.memoryUsedBytes)} of ${bytes(host.memoryTotalBytes)} (${((100 * host.memoryUsedBytes) / host.memoryTotalBytes).toFixed(0)}%)`}
            trend={{ values: histories.get("serverMemory"), max: 100, color: kAccent }}
          />
        )}
        {host.temperatureC !== null && (
          <Stat
            label="Temperature"
            value={`${host.temperatureC.toFixed(1)} °C`}
            className={hot ? "text-danger" : ""}
            trend={{
              values: histories.get("serverTemperature"),
              min: 20,
              max: Math.max(histories.max("serverTemperature", 90), 90),
              color: kDanger,
            }}
          />
        )}
        {host.load1 >= 0 && <Stat label="Load" value={host.load1.toFixed(2)} />}
        {host.diskTotalBytes > 0 && (
          <Stat label="Recordings disk" value={`${bytes(host.diskFreeBytes)} free of ${bytes(host.diskTotalBytes)}`} />
        )}
        {host.uptimeSeconds >= 0 && <Stat label="Up" value={duration(host.uptimeSeconds)} />}
      </Stats>
    </>
  );
}

export function PerformanceTab() {
  const instrument = useInstrument();
  const serverName = useSelector(sessionStore, (s) => s.serverName);
  const { snapshot, histories, reset } = useTelemetry();
  if (!snapshot) {
    return null;
  }
  const { stream, process, health, link } = snapshot;
  const engine = instrument.run.engine;
  const engineTotal = engine.stitched + engine.unsettled + engine.unattributed + engine.tooShort;
  const utilisation = stream.linkUtilisation;
  const processed = process.processedFraction;

  return (
    <div>
      <SectionTitle title="Link" />
      <p className="caption mb-1">Input and processing below are measured on {serverName || "the server"}.</p>
      <Stats>
        <Stat label="Server" value={location.host} />
        <Stat
          label="Round trip"
          value={`${snapshot.roundTripMs.toFixed(1)} ms`}
          trend={{ values: histories.get("roundTrip"), max: histories.max("roundTrip", 1), color: kWarning }}
        />
        <Stat
          label="Receiving"
          value={byteRate(snapshot.bytesPerSec)}
          trend={{ values: histories.get("receiving"), max: histories.max("receiving", 1), color: kAccent }}
        />
        <Stat label="Received" value={bytes(snapshot.bytesReceived)} hint="Since this page connected." />
        <Stat
          label="Frames sent"
          value={`${n(link.framesSent)} (${snapshot.framesSentPerSec.toFixed(0)}/s)`}
          trend={{ values: histories.get("framesSent"), max: histories.max("framesSent", 1), color: kAccent }}
        />
        <Stat
          label="Passes merged"
          value={n(link.passesCoalesced)}
          hint="Merged into a later one: the link had not taken it yet."
        />
        <Stat
          label="Partials merged"
          value={n(link.partialsCoalesced)}
          hint="Merged into a later one: the link had not taken it yet."
        />
        <Stat label="Events dropped" value={n(link.eventsDropped)} />
        {link.framesSent > 0n && (
          <Stat
            label="Server encoding"
            value={`${(Number(link.encodeNs) / 1e6 / Number(link.framesSent)).toFixed(2)} ms a frame`}
          />
        )}
      </Stats>

      {snapshot.host && <ServerSection host={snapshot.host} histories={histories} />}

      <SectionTitle title="Input">
        <button
          className="btn h-5 px-1.5 text-[11px] font-normal normal-case tracking-normal"
          title="Reset the counters and graphs"
          disabled={!instrument.canControl}
          onClick={reset}
        >
          Reset
        </button>
      </SectionTitle>
      {stream.linkCapacityBytesPerSec > 0 && (
        <Bar
          fraction={utilisation}
          color={utilisation > 0.9 ? kDanger : utilisation > 0.7 ? kWarning : kOk}
          label={`${byteRate(stream.bytesPerSecIn)} of ${byteRate(stream.linkCapacityBytesPerSec)} (${(utilisation * 100).toFixed(0)}%)`}
        />
      )}
      <Bar fraction={stream.ringFillFraction} label={`ring ${(stream.ringFillFraction * 100).toFixed(0)}% full`} />
      <Stats>
        <Stat
          label="Sample rate"
          value={`${frequencyShort(stream.measuredSps)} / ${frequencyShort(stream.configuredSps)}`}
          trend={{ values: histories.get("measuredSps"), max: stream.configuredSps * 1.2 || 1, color: kAccent }}
        />
        <Stat
          label="Samples dropped"
          value={`${n(stream.samplesDropped + stream.samplesLostAtSource)} (${(stream.dropFraction * 100).toFixed(3)}%)`}
          trend={{ values: histories.get("dropRate"), max: histories.max("dropRate", 1), color: kDanger }}
        />
        <Stat label="discarded" indent value={n(stream.samplesDropped)} hint="Taken, but the host could not process it." />
        <Stat label="never taken" indent value={n(stream.samplesLostAtSource)} hint="The host could not accept it." />
        <Stat label="Device overruns" value={n(stream.deviceOverruns)} />
        <Stat label="Ring full" value={n(stream.ringFullEvents)} />
        <Stat label="Pool exhausted" value={n(stream.poolExhaustedEvents)} />
        <Stat label="Sequence gaps" value={n(stream.sequenceGaps)} />
      </Stats>

      {health.length > 0 && (
        <>
          <SectionTitle title="Device" />
          <Stats>
            {health.map((reading) => (
              <Stat
                key={reading.label}
                label={reading.label}
                value={reading.value}
                className={reading.alarm ? "text-danger" : ""}
                trend={
                  reading.maximum > reading.minimum
                    ? {
                        values: histories.get(`health:${reading.label}`),
                        min: reading.minimum,
                        max: reading.maximum,
                        color: "var(--color-accent-hover)",
                      }
                    : undefined
                }
              />
            ))}
          </Stats>
        </>
      )}

      <SectionTitle title="Processing" />
      <Bar
        fraction={processed}
        color={processed > 0.99 ? kOk : processed > 0.5 ? kWarning : kDanger}
        label={`${(processed * 100).toFixed(1)}% of delivered samples processed`}
      />
      <Stats>
        <Stat label="Throttle" value={kThrottleNames[process.throttleReason] ?? "-"} hint={kThrottleHint} />
        <Stat
          label="FFT rate"
          value={`${process.fftsPerSec.toFixed(0)}/s`}
          trend={{ values: histories.get("fftsPerSec"), max: histories.max("fftsPerSec", 1), color: kAccent }}
        />
        <Stat
          label="FFT latency"
          value={`p50 ${process.fftLatencyP50Us.toFixed(0)} µs | p99 ${process.fftLatencyP99Us.toFixed(0)} µs`}
          hint={`max ${process.fftLatencyMaxUs.toFixed(0)} µs`}
          trend={{ values: histories.get("fftLatencyP99"), max: histories.max("fftLatencyP99", 1), color: kWarning }}
        />
        <Stat
          label="Workers"
          value={`${process.workerCount} (${(process.workerUtilisation * 100).toFixed(0)}% busy)`}
          trend={{ values: histories.get("workerBusy"), max: 100, color: kWarning }}
        />
        <Stat label="FFTs computed" value={n(process.fftsComputed)} />
        <Stat label="FFTs skipped" value={n(process.fftsSkipped)} />
        <Stat label="Sweep passes" value={n(process.sweepPassesCompleted)} />
        <Stat label="Retunes" value={`${process.retunesPerSec.toFixed(0)}/s`} />
      </Stats>

      <SectionTitle title="Sweep" />
      <Stats>
        {/* The engine numbers passes from 1, the one in progress included. */}
        <Stat label="Passes" value={n(engine.passCount > 0n ? engine.passCount - 1n : 0n)} />
        <Stat label="Measured sweep rate" value={`${(engine.measuredSweepRateHzPerSec / 1e6).toFixed(1)} MHz/s`} />
        <Stat
          label="Last pass measured"
          value={`${(engine.lastPassCoverage * 100).toFixed(1)}% of the span`}
          hint="Bins the newest pass measured; the rest show older passes."
        />
        <Stat
          label="Step frames stitched"
          value={`${n(engine.stitched)} (${share(engine.stitched, engineTotal).toFixed(1)}%)`}
        />
        <Stat
          label="discarded unsettled"
          indent
          value={`${n(engine.unsettled)} (${share(engine.unsettled, engineTotal).toFixed(1)}%)`}
          hint="Taken while the tuner was still settling."
        />
        <Stat
          label="unattributable"
          indent
          value={`${n(engine.unattributed)} (${share(engine.unattributed, engineTotal).toFixed(1)}%)`}
          hint="Centre matched no planned step."
        />
        <Stat
          label="too few bins"
          indent
          value={`${n(engine.tooShort)} (${share(engine.tooShort, engineTotal).toFixed(1)}%)`}
        />
      </Stats>

      <SectionTitle title="Output" />
      <Stats>
        <Stat
          label="Frames"
          value={`${snapshot.framesPerSec.toFixed(0)}/s`}
          hint="Spectrum frames this page took."
          trend={{ values: histories.get("frames"), max: histories.max("frames", 1), color: kOk }}
        />
        <Stat
          label="Waterfall"
          value={`${snapshot.linesPerSec.toFixed(0)} lines/s`}
          hint="Lines this page added."
          trend={{ values: histories.get("lines"), max: histories.max("lines", 1), color: kAccent }}
        />
        <Stat label="Sweep speed" value={`${(process.sweepSpeedHzPerSec / 1e6).toFixed(1)} MHz/s`} />
      </Stats>
    </div>
  );
}
