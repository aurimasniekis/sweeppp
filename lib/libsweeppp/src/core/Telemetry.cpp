// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/core/Telemetry.hpp"

#include "sweeppp/core/Clock.hpp"

#include <algorithm>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#include <sys/time.h>
#endif

namespace sweeppp {
namespace {

/// Rate over an interval, guarding against a zero or negative dt (a sample()
/// called twice in the same tick would otherwise produce infinity).
double rate(std::uint64_t current, std::uint64_t previous, double dtSeconds) noexcept {
    if (dtSeconds <= 0.0 || current < previous) {
        return 0.0;
    }
    return static_cast<double>(current - previous) / dtSeconds;
}

} // namespace

std::string_view toString(ThrottleReason reason) noexcept {
    switch (reason) {
    case ThrottleReason::None:
        return "none";
    case ThrottleReason::EveryNth:
        return "every-nth";
    case ThrottleReason::CpuLimited:
        return "cpu-limited";
    case ThrottleReason::RingFull:
        return "ring-full";
    case ThrottleReason::PoolExhausted:
        return "pool-exhausted";
    case ThrottleReason::DeviceOverrun:
        return "device-overrun";
    case ThrottleReason::Stopped:
        return "stopped";
    }
    return "unknown";
}

LatencyTracker::Percentiles LatencyTracker::percentiles() const noexcept {
    const std::uint64_t recorded = m_recorded.load(std::memory_order_relaxed);
    if (recorded == 0) {
        return {};
    }

    const auto used = static_cast<std::size_t>(std::min<std::uint64_t>(recorded, kCapacity));
    std::vector<float> values;
    values.reserve(used);
    for (std::size_t i = 0; i < used; ++i) {
        values.push_back(m_samples[i].load(std::memory_order_relaxed));
    }
    std::sort(values.begin(), values.end());

    const auto pick = [&values](double quantile) {
        const auto index = static_cast<std::size_t>(quantile * static_cast<double>(values.size()));
        return values[std::min(index, values.size() - 1)];
    };

    return {.p50 = pick(0.50), .p99 = pick(0.99), .max = values.back()};
}

void StreamCounters::reset() noexcept {
    samplesDelivered.store(0, std::memory_order_relaxed);
    samplesDropped.store(0, std::memory_order_relaxed);
    samplesLostAtSource.store(0, std::memory_order_relaxed);
    bytesDelivered.store(0, std::memory_order_relaxed);
    blocksDelivered.store(0, std::memory_order_relaxed);
    blocksDropped.store(0, std::memory_order_relaxed);
    deviceOverruns.store(0, std::memory_order_relaxed);
    ringFullEvents.store(0, std::memory_order_relaxed);
    poolExhaustedEvents.store(0, std::memory_order_relaxed);
    sequenceGaps.store(0, std::memory_order_relaxed);
    ringFillFraction.store(0.0F, std::memory_order_relaxed);
    // configuredSps and linkCapacityBytesPerSec describe the device, not the
    // run, so they deliberately survive a reset.
}

void ProcessCounters::reset() noexcept {
    fftsComputed.store(0, std::memory_order_relaxed);
    fftsSkipped.store(0, std::memory_order_relaxed);
    samplesProcessed.store(0, std::memory_order_relaxed);
    framesPublished.store(0, std::memory_order_relaxed);
    sweepPassesCompleted.store(0, std::memory_order_relaxed);
    retunes.store(0, std::memory_order_relaxed);
    activeWorkers.store(0, std::memory_order_relaxed);
    workerBusyNs.store(0, std::memory_order_relaxed);
    fftLatency.reset();
}

void RenderCounters::reset() noexcept {
    framesRendered.store(0, std::memory_order_relaxed);
    framesDropped.store(0, std::memory_order_relaxed);
    waterfallLines.store(0, std::memory_order_relaxed);
    sweepSpeedHzPerSec.store(0.0, std::memory_order_relaxed);
}

Telemetry::Telemetry() {
    m_startNs = monotonicNs();
    m_lastSampleNs = m_startNs;
}

void Telemetry::reset() noexcept {
    m_stream.reset();
    m_process.reset();
    m_render.reset();

    m_snapshot = {};
    m_previous = {};

    m_history.measuredSps.clear();
    m_history.linkUtilisation.clear();
    m_history.ringFill.clear();
    m_history.dropRate.clear();
    m_history.processedFraction.clear();
    m_history.fftsPerSec.clear();
    m_history.fftLatencyP99.clear();
    m_history.fps.clear();
    m_history.waterfallLps.clear();
    m_history.cpuPercent.clear();

    m_spsEwma.reset();
    m_bytesEwma.reset();
    m_fftEwma.reset();
    m_fpsEwma.reset();
    m_lpsEwma.reset();
    m_cpuEwma.reset();

    m_startNs = monotonicNs();
    m_lastSampleNs = m_startNs;
}

const TelemetrySnapshot& Telemetry::sample() noexcept {
    const std::uint64_t now = monotonicNs();
    const double dt = nsToSeconds(now - m_lastSampleNs);
    m_lastSampleNs = now;

    // ---- stream ----------------------------------------------------------
    const std::uint64_t delivered = m_stream.samplesDelivered.load(std::memory_order_relaxed);
    const std::uint64_t dropped = m_stream.samplesDropped.load(std::memory_order_relaxed);
    const std::uint64_t lostAtSource = m_stream.samplesLostAtSource.load(std::memory_order_relaxed);
    const std::uint64_t bytes = m_stream.bytesDelivered.load(std::memory_order_relaxed);

    m_spsEwma.push(static_cast<float>(rate(delivered, m_previous.samplesDelivered, dt)));
    m_bytesEwma.push(static_cast<float>(rate(bytes, m_previous.bytesDelivered, dt)));

    StreamStats& stream = m_snapshot.stream;
    stream.configuredSps = m_stream.configuredSps.load(std::memory_order_relaxed);
    stream.measuredSps = static_cast<double>(m_spsEwma.value());
    stream.bytesPerSecIn = static_cast<double>(m_bytesEwma.value());
    stream.linkCapacityBytesPerSec =
        static_cast<double>(m_stream.linkCapacityBytesPerSec.load(std::memory_order_relaxed));
    stream.linkUtilisation = stream.linkCapacityBytesPerSec > 0.0
                                 ? stream.bytesPerSecIn / stream.linkCapacityBytesPerSec
                                 : 0.0;
    stream.samplesDelivered = delivered;
    stream.samplesDropped = dropped;
    stream.samplesLostAtSource = lostAtSource;
    stream.deviceOverruns = m_stream.deviceOverruns.load(std::memory_order_relaxed);
    stream.ringFullEvents = m_stream.ringFullEvents.load(std::memory_order_relaxed);
    stream.poolExhaustedEvents = m_stream.poolExhaustedEvents.load(std::memory_order_relaxed);
    stream.sequenceGaps = m_stream.sequenceGaps.load(std::memory_order_relaxed);
    const std::uint64_t lost = dropped + lostAtSource;
    stream.dropRatePerSec =
        rate(lost, m_previous.samplesDropped + m_previous.samplesLostAtSource, dt);
    // "Of everything the radio produced, what fraction did we lose" -- the
    // question an operator is actually asking, so both kinds of loss count.
    //
    // The denominator is delivered + lostAtSource, not delivered + dropped:
    // dropped samples are a subset of delivered, and adding them would count
    // them twice and understate the loss.
    const std::uint64_t produced = delivered + lostAtSource;
    stream.dropFraction =
        produced > 0 ? static_cast<double>(lost) / static_cast<double>(produced) : 0.0;
    stream.ringFillFraction = m_stream.ringFillFraction.load(std::memory_order_relaxed);

    // ---- processing ------------------------------------------------------
    const std::uint64_t ffts = m_process.fftsComputed.load(std::memory_order_relaxed);
    const std::uint64_t processed = m_process.samplesProcessed.load(std::memory_order_relaxed);
    const std::uint64_t frames = m_process.framesPublished.load(std::memory_order_relaxed);
    const std::uint64_t retunes = m_process.retunes.load(std::memory_order_relaxed);

    m_fftEwma.push(static_cast<float>(rate(ffts, m_previous.fftsComputed, dt)));

    ProcessStats& process = m_snapshot.process;
    process.fftsPerSec = static_cast<double>(m_fftEwma.value());
    process.fftsComputed = ffts;
    process.fftsSkipped = m_process.fftsSkipped.load(std::memory_order_relaxed);
    process.samplesProcessedTotal = processed;
    process.processedFraction =
        delivered > 0 ? static_cast<double>(processed) / static_cast<double>(delivered) : 0.0;
    process.framesPerSec = rate(frames, m_previous.framesPublished, dt);
    process.sweepPassesCompleted = m_process.sweepPassesCompleted.load(std::memory_order_relaxed);
    process.retunesPerSec = rate(retunes, m_previous.retunes, dt);

    const LatencyTracker::Percentiles latency = m_process.fftLatency.percentiles();
    process.fftLatencyP50Us = latency.p50;
    process.fftLatencyP99Us = latency.p99;
    process.fftLatencyMaxUs = latency.max;

    const std::uint32_t workers = m_process.workerCount.load(std::memory_order_relaxed);
    const std::uint64_t busyNs = m_process.workerBusyNs.load(std::memory_order_relaxed);
    process.workerCount = workers;
    // Busy *time* over the interval, not an instantaneous head count. Sampling
    // activeWorkers reads zero almost every time even on a saturated pipeline,
    // because the sample lands in the gap between two blocks.
    process.workerUtilisation =
        workers > 0 && dt > 0.0 && busyNs >= m_previous.workerBusyNs
            ? nsToSeconds(busyNs - m_previous.workerBusyNs) / (dt * static_cast<double>(workers))
            : 0.0;
    process.throttleReason = m_process.throttleReason.load(std::memory_order_relaxed);

    // ---- rendering -------------------------------------------------------
    const std::uint64_t rendered = m_render.framesRendered.load(std::memory_order_relaxed);
    const std::uint64_t lines = m_render.waterfallLines.load(std::memory_order_relaxed);

    m_fpsEwma.push(static_cast<float>(rate(rendered, m_previous.framesRendered, dt)));
    m_lpsEwma.push(static_cast<float>(rate(lines, m_previous.waterfallLines, dt)));

    RenderStats& render = m_snapshot.render;
    render.fps = static_cast<double>(m_fpsEwma.value());
    render.framesDropped = m_render.framesDropped.load(std::memory_order_relaxed);
    render.waterfallLinesPerSec = static_cast<double>(m_lpsEwma.value());
    render.sweepSpeedHzPerSec = m_render.sweepSpeedHzPerSec.load(std::memory_order_relaxed);

    if (const double cpu = sampleProcessCpuPercent(); cpu >= 0.0) {
        m_cpuEwma.push(static_cast<float>(cpu));
    }
    render.cpuPercent = static_cast<double>(m_cpuEwma.value());

    m_snapshot.uptimeSeconds = nsToSeconds(now - m_startNs);

    // ---- histories -------------------------------------------------------
    m_history.measuredSps.push(static_cast<float>(stream.measuredSps));
    m_history.linkUtilisation.push(static_cast<float>(stream.linkUtilisation));
    m_history.ringFill.push(stream.ringFillFraction);
    m_history.dropRate.push(static_cast<float>(stream.dropRatePerSec));
    m_history.processedFraction.push(static_cast<float>(process.processedFraction));
    m_history.fftsPerSec.push(static_cast<float>(process.fftsPerSec));
    m_history.fftLatencyP99.push(process.fftLatencyP99Us);
    m_history.fps.push(static_cast<float>(render.fps));
    m_history.waterfallLps.push(static_cast<float>(render.waterfallLinesPerSec));
    m_history.cpuPercent.push(static_cast<float>(render.cpuPercent));

    m_previous = {.samplesDelivered = delivered,
                  .samplesDropped = dropped,
                  .samplesLostAtSource = lostAtSource,
                  .samplesProcessed = processed,
                  .bytesDelivered = bytes,
                  .fftsComputed = ffts,
                  .framesPublished = frames,
                  .framesRendered = rendered,
                  .waterfallLines = lines,
                  .retunes = retunes,
                  .workerBusyNs = busyNs};

    return m_snapshot;
}

double sampleProcessCpuPercent() noexcept {
#if defined(__APPLE__) || defined(__linux__)
    // rusage gives cumulative CPU time; the caller wants the rate, so the
    // previous reading is kept here rather than in Telemetry -- it is a
    // process-wide quantity and there is only ever one process.
    static std::uint64_t previousCpuNs = 0;
    static std::uint64_t previousWallNs = 0;

    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return -1.0;
    }

    const auto toNs = [](const timeval& tv) {
        return static_cast<std::uint64_t>(tv.tv_sec) * 1'000'000'000ULL +
               static_cast<std::uint64_t>(tv.tv_usec) * 1'000ULL;
    };
    const std::uint64_t cpuNs = toNs(usage.ru_utime) + toNs(usage.ru_stime);
    const std::uint64_t wallNs = monotonicNs();

    double percent = -1.0;
    if (previousWallNs != 0 && wallNs > previousWallNs && cpuNs >= previousCpuNs) {
        percent = 100.0 * static_cast<double>(cpuNs - previousCpuNs) /
                  static_cast<double>(wallNs - previousWallNs);
    }

    previousCpuNs = cpuNs;
    previousWallNs = wallNs;
    return percent;
#else
    return -1.0;
#endif
}

double processCpuSeconds() noexcept {
#if defined(__APPLE__) || defined(__linux__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return -1.0;
    }
    const auto toSeconds = [](const timeval& tv) {
        return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) * 1e-6;
    };
    return toSeconds(usage.ru_utime) + toSeconds(usage.ru_stime);
#else
    return -1.0;
#endif
}

} // namespace sweeppp
