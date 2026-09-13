// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/SpscRing.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sweeppp {

/// Why the pipeline is not processing every sample it receives.
///
/// At 100 MS/s it is not physically possible to FFT everything, so the
/// question is never "is anything being skipped" but "which decision is
/// causing it". Making that an explicit, displayable value is the difference
/// between an operator trusting the instrument and not.
enum class ThrottleReason : std::uint8_t {
    None,          ///< Every delivered sample is being processed.
    EveryNth,      ///< Operator chose ProcessEveryNth(n).
    CpuLimited,    ///< Auto mode: workers cannot keep up, skipping to stay live.
    RingFull,      ///< Consumer behind; blocks dropped at the ring.
    PoolExhausted, ///< No free blocks; dropped at acquisition.
    DeviceOverrun, ///< The radio itself dropped samples before we saw them.
    Stopped,
};

[[nodiscard]] std::string_view toString(ThrottleReason reason) noexcept;

/// Fixed-length ring of recent values, for the sparklines beside each readout.
template <std::size_t N>
class RollingHistory {
public:
    static constexpr std::size_t kCapacity = N;

    void push(float value) noexcept {
        m_values[m_next] = value;
        m_next = (m_next + 1) % N;
        if (m_count < N) {
            ++m_count;
        }
    }

    void clear() noexcept {
        m_values.fill(0.0F);
        m_next = 0;
        m_count = 0;
    }

    [[nodiscard]] std::size_t count() const noexcept { return m_count; }

    /// Oldest-first, so a plot reads left-to-right in time.
    [[nodiscard]] float at(std::size_t i) const noexcept {
        const std::size_t start = m_count < N ? 0 : m_next;
        return m_values[(start + i) % N];
    }

    [[nodiscard]] float latest() const noexcept {
        return m_count == 0 ? 0.0F : m_values[(m_next + N - 1) % N];
    }

    [[nodiscard]] float max() const noexcept {
        float result = 0.0F;
        for (std::size_t i = 0; i < m_count; ++i) {
            result = std::max(result, at(i));
        }
        return result;
    }

    [[nodiscard]] float mean() const noexcept {
        if (m_count == 0) {
            return 0.0F;
        }
        float total = 0.0F;
        for (std::size_t i = 0; i < m_count; ++i) {
            total += at(i);
        }
        return total / static_cast<float>(m_count);
    }

private:
    std::array<float, N> m_values{};
    std::size_t m_next = 0;
    std::size_t m_count = 0;
};

/// Exponentially weighted moving average.
///
/// Raw per-interval rates jitter enough to be unreadable; smoothing is what
/// makes a rate display usable. The unsmoothed value is still available for
/// anything that needs the instantaneous figure.
class Ewma {
public:
    explicit Ewma(float alpha = 0.3F) noexcept : m_alpha(alpha) {}

    void push(float value) noexcept {
        if (!m_primed) {
            m_value = value;
            m_primed = true;
        } else {
            m_value += m_alpha * (value - m_value);
        }
    }

    [[nodiscard]] float value() const noexcept { return m_value; }
    void reset() noexcept {
        m_value = 0.0F;
        m_primed = false;
    }

private:
    float m_alpha;
    float m_value = 0.0F;
    bool m_primed = false;
};

/// Latency percentiles from a bounded ring of recent observations.
///
/// Writers are FFT workers on the hot path, so recording is a relaxed store
/// into a slot chosen by a relaxed fetch_add -- two workers can race for a
/// slot and one observation is lost. That is acceptable: this drives a p50/p99
/// readout sampled a few times a second, not an accounting total.
class LatencyTracker {
public:
    static constexpr std::size_t kCapacity = 1024;

    void record(float microseconds) noexcept {
        const std::size_t slot = m_next.fetch_add(1, std::memory_order_relaxed) % kCapacity;
        m_samples[slot].store(microseconds, std::memory_order_relaxed);
        m_recorded.fetch_add(1, std::memory_order_relaxed);
    }

    struct Percentiles {
        float p50 = 0.0F;
        float p99 = 0.0F;
        float max = 0.0F;
    };

    /// Copies the ring and sorts it. Called by the telemetry sampler only.
    [[nodiscard]] Percentiles percentiles() const noexcept;

    void reset() noexcept {
        for (auto& sample : m_samples) {
            sample.store(0.0F, std::memory_order_relaxed);
        }
        m_next.store(0, std::memory_order_relaxed);
        m_recorded.store(0, std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<float>, kCapacity> m_samples{};
    std::atomic<std::size_t> m_next{0};
    std::atomic<std::uint64_t> m_recorded{0};
};

// ---------------------------------------------------------------------------
// Hot-path counters. Written by acquisition, workers and the renderer; only
// ever read by the sampler. All relaxed -- these are counters, not
// synchronisation.
// ---------------------------------------------------------------------------

struct alignas(kCacheLineSize) StreamCounters {
    std::atomic<std::uint64_t> samplesDelivered{0};
    /// Samples that reached us and were then discarded -- ring full, throttled,
    /// settling after a retune, or still queued at shutdown.
    std::atomic<std::uint64_t> samplesDropped{0};
    /// Samples the radio produced that were never handed over at all, because
    /// no pooled block was free or the device reported an overrun.
    ///
    /// Counted apart from samplesDropped because the two are different
    /// diagnoses -- "the host could not keep up with the bus" against "the host
    /// threw away what it was given" -- and because keeping them apart is what
    /// makes delivered == processed + dropped an exact identity rather than an
    /// approximation. Operator-visible loss is the sum of both.
    std::atomic<std::uint64_t> samplesLostAtSource{0};
    std::atomic<std::uint64_t> bytesDelivered{0};
    std::atomic<std::uint64_t> blocksDelivered{0};
    std::atomic<std::uint64_t> blocksDropped{0};
    std::atomic<std::uint64_t> deviceOverruns{0};
    std::atomic<std::uint64_t> ringFullEvents{0};
    std::atomic<std::uint64_t> poolExhaustedEvents{0};
    /// Gaps found in block sequence numbers. HackRF reports no overrun status,
    /// so host-side gap detection is the only overrun signal it has.
    std::atomic<std::uint64_t> sequenceGaps{0};

    std::atomic<double> configuredSps{0.0};
    std::atomic<std::uint64_t> linkCapacityBytesPerSec{0};
    std::atomic<float> ringFillFraction{0.0F};

    void reset() noexcept;
};

struct alignas(kCacheLineSize) ProcessCounters {
    std::atomic<std::uint64_t> fftsComputed{0};
    std::atomic<std::uint64_t> fftsSkipped{0};
    std::atomic<std::uint64_t> samplesProcessed{0};
    std::atomic<std::uint64_t> framesPublished{0};
    std::atomic<std::uint64_t> sweepPassesCompleted{0};
    std::atomic<std::uint64_t> retunes{0};
    std::atomic<std::uint32_t> activeWorkers{0};
    std::atomic<std::uint32_t> workerCount{0};
    /// Total nanoseconds workers spent inside processBlock, summed across all
    /// of them. Utilisation is the delta of this over (workers * elapsed) --
    /// an instantaneous activeWorkers reading is almost always zero even on a
    /// fully loaded pipeline, because the sample lands between blocks.
    std::atomic<std::uint64_t> workerBusyNs{0};
    std::atomic<ThrottleReason> throttleReason{ThrottleReason::Stopped};

    LatencyTracker fftLatency;

    void reset() noexcept;
};

struct alignas(kCacheLineSize) RenderCounters {
    std::atomic<std::uint64_t> framesRendered{0};
    std::atomic<std::uint64_t> framesDropped{0};
    std::atomic<std::uint64_t> waterfallLines{0};
    std::atomic<double> sweepSpeedHzPerSec{0.0};

    void reset() noexcept;
};

// ---------------------------------------------------------------------------
// Sampled snapshots. Plain values -- what the UI actually draws.
// ---------------------------------------------------------------------------

struct StreamStats {
    double configuredSps = 0.0;
    double measuredSps = 0.0;
    double bytesPerSecIn = 0.0;
    double linkCapacityBytesPerSec = 0.0;
    /// bytesPerSecIn / linkCapacity. Above ~0.8 the USB link is the limit, and
    /// the operator needs to know that before blaming the FFT settings.
    double linkUtilisation = 0.0;

    std::uint64_t samplesDelivered = 0;
    std::uint64_t samplesDropped = 0;
    std::uint64_t samplesLostAtSource = 0;
    std::uint64_t deviceOverruns = 0;
    std::uint64_t ringFullEvents = 0;
    std::uint64_t poolExhaustedEvents = 0;
    std::uint64_t sequenceGaps = 0;

    double dropRatePerSec = 0.0;
    double dropFraction = 0.0;
    float ringFillFraction = 0.0F;
};

struct ProcessStats {
    double fftsPerSec = 0.0;
    std::uint64_t fftsComputed = 0;
    std::uint64_t fftsSkipped = 0;
    /// Running total of samples that reached an FFT. Together with
    /// samplesDropped this must exactly equal samplesDelivered -- the
    /// reconciliation that proves nothing is lost unaccounted.
    std::uint64_t samplesProcessedTotal = 0;
    /// Fraction of delivered samples that actually reached an FFT. The single
    /// number that answers "am I seeing everything?".
    double processedFraction = 0.0;
    double framesPerSec = 0.0;
    std::uint64_t sweepPassesCompleted = 0;
    double retunesPerSec = 0.0;

    float fftLatencyP50Us = 0.0F;
    float fftLatencyP99Us = 0.0F;
    float fftLatencyMaxUs = 0.0F;

    double workerUtilisation = 0.0;
    std::uint32_t workerCount = 0;
    ThrottleReason throttleReason = ThrottleReason::Stopped;
};

struct RenderStats {
    double fps = 0.0;
    std::uint64_t framesDropped = 0;
    double waterfallLinesPerSec = 0.0;
    double sweepSpeedHzPerSec = 0.0;
    double cpuPercent = 0.0;
};

struct TelemetrySnapshot {
    StreamStats stream;
    ProcessStats process;
    RenderStats render;
    double uptimeSeconds = 0.0;
};

/// Owns the counters and turns them into rates.
///
/// `sample()` is expected roughly 4x/second from the UI thread; every rate is
/// computed against the real elapsed time since the previous call, so an
/// irregular cadence does not distort the numbers.
class Telemetry {
public:
    static constexpr std::size_t kHistoryLength = 240; // ~60 s at 4 Hz

    Telemetry();

    StreamCounters& stream() noexcept { return m_stream; }
    ProcessCounters& process() noexcept { return m_process; }
    RenderCounters& render() noexcept { return m_render; }

    const StreamCounters& stream() const noexcept { return m_stream; }
    const ProcessCounters& process() const noexcept { return m_process; }
    const RenderCounters& render() const noexcept { return m_render; }

    /// Recomputes rates and appends to the sparkline histories.
    const TelemetrySnapshot& sample() noexcept;

    [[nodiscard]] const TelemetrySnapshot& snapshot() const noexcept { return m_snapshot; }

    /// Zeroes counters and histories. Called when a device starts, so figures
    /// always describe the current run rather than the session.
    void reset() noexcept;

    struct History {
        RollingHistory<kHistoryLength> measuredSps;
        RollingHistory<kHistoryLength> linkUtilisation;
        RollingHistory<kHistoryLength> ringFill;
        RollingHistory<kHistoryLength> dropRate;
        RollingHistory<kHistoryLength> processedFraction;
        RollingHistory<kHistoryLength> fftsPerSec;
        RollingHistory<kHistoryLength> fftLatencyP99;
        RollingHistory<kHistoryLength> fps;
        RollingHistory<kHistoryLength> waterfallLps;
        RollingHistory<kHistoryLength> cpuPercent;
    };

    [[nodiscard]] const History& history() const noexcept { return m_history; }

private:
    struct Previous {
        std::uint64_t samplesDelivered = 0;
        std::uint64_t samplesDropped = 0;
        std::uint64_t samplesLostAtSource = 0;
        std::uint64_t samplesProcessed = 0;
        std::uint64_t bytesDelivered = 0;
        std::uint64_t fftsComputed = 0;
        std::uint64_t framesPublished = 0;
        std::uint64_t framesRendered = 0;
        std::uint64_t waterfallLines = 0;
        std::uint64_t retunes = 0;
        std::uint64_t workerBusyNs = 0;
    };

    StreamCounters m_stream;
    ProcessCounters m_process;
    RenderCounters m_render;

    TelemetrySnapshot m_snapshot;
    History m_history;
    Previous m_previous;

    Ewma m_spsEwma{0.35F};
    Ewma m_bytesEwma{0.35F};
    Ewma m_fftEwma{0.35F};
    Ewma m_fpsEwma{0.35F};
    Ewma m_lpsEwma{0.35F};
    Ewma m_cpuEwma{0.25F};

    std::uint64_t m_startNs = 0;
    std::uint64_t m_lastSampleNs = 0;
};

/// Process CPU usage as a percentage of one core, measured between calls.
/// Returns a negative value where the platform does not provide it.
[[nodiscard]] double sampleProcessCpuPercent() noexcept;

/// Cumulative CPU time this process has consumed, user plus system, across
/// every thread. Negative where the platform does not provide it.
///
/// The raw counter rather than a rate, because a caller timing a bounded piece
/// of work needs both ends of the interval and cannot use one that resets
/// itself on every read.
[[nodiscard]] double processCpuSeconds() noexcept;

} // namespace sweeppp
