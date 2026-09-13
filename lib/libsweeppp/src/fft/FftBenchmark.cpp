// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/fft/FftBenchmark.hpp"

#include "sweeppp/core/Clock.hpp"
#include "sweeppp/core/Telemetry.hpp"
#include "sweeppp/fft/FftBackendManager.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <format>
#include <memory>
#include <thread>
#include <vector>

namespace sweeppp {
namespace {

/// The same signal for every backend and every run, so the only thing varying
/// between columns is the transform.
///
/// Non-trivial on purpose. A buffer of zeros or a single tone lets a
/// denormal-flushing CPU take a different path than real IQ would, and the
/// benchmark would then measure something no sweep ever runs.
std::vector<std::complex<float>> makeInput(std::size_t size) {
    std::vector<std::complex<float>> input(size);
    for (std::size_t i = 0; i < size; ++i) {
        const double t = static_cast<double>(i);
        input[i] = {static_cast<float>(std::sin(0.017 * t) + 0.25 * std::cos(0.311 * t)),
                    static_cast<float>(std::cos(0.023 * t) - 0.5 * std::sin(0.907 * t))};
    }
    return input;
}

[[nodiscard]] double percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(sorted.size() - 1));
    return sorted[std::min(index, sorted.size() - 1)];
}

/// One measurement: `threads` workers sharing `plan`, each on its own buffers,
/// all timing themselves, for about `budgetSeconds`.
///
/// Per-thread buffers because that is the arrangement `threadSafeExecute`
/// describes and the one the pipeline uses. Sharing an output buffer would
/// measure cache-line ping-pong rather than the transform.
void measure(IFftPlan& plan, std::size_t size, std::size_t threads,
             const FftBenchmarkConfig& config, FftBenchmarkSample& sample) {
    // Started together, so a fast thread cannot finish its whole budget before
    // a slow one begins and turn a concurrency measurement back into a serial
    // one.
    std::atomic<bool> go{false};
    std::atomic<std::size_t> ready{0};

    std::vector<std::vector<double>> timings(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    const std::uint64_t budgetNs =
        static_cast<std::uint64_t>(std::max(config.secondsPerSample, 0.0) * 1e9);

    for (std::size_t t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            const std::vector<std::complex<float>> input = makeInput(size);
            std::vector<std::complex<float>> output(size);

            for (std::size_t run = 0; run < config.warmupRuns; ++run) {
                plan.execute(input.data(), output.data());
            }

            std::vector<double>& mine = timings[t];
            mine.reserve(config.minRuns);

            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            const std::uint64_t deadline = monotonicNs() + budgetNs;
            while (mine.size() < config.maxRuns) {
                const std::uint64_t started = monotonicNs();
                plan.execute(input.data(), output.data());
                const std::uint64_t ended = monotonicNs();
                mine.push_back(nsToSeconds(ended - started));

                if (mine.size() >= config.minRuns && ended >= deadline) {
                    break;
                }
            }
        });
    }

    while (ready.load() < threads) {
        std::this_thread::yield();
    }

    // CPU and wall span the measured window only: the warm-up and the thread
    // spawn are outside it, so `cpuCores` is the backend's own appetite rather
    // than this function's.
    const double cpuBefore = processCpuSeconds();
    const std::uint64_t wallBefore = monotonicNs();
    go.store(true, std::memory_order_release);

    for (std::thread& worker : workers) {
        worker.join();
    }

    const std::uint64_t wallAfter = monotonicNs();
    const double cpuAfter = processCpuSeconds();
    const double wallSeconds = nsToSeconds(wallAfter - wallBefore);

    std::vector<double> all;
    for (const std::vector<double>& mine : timings) {
        all.insert(all.end(), mine.begin(), mine.end());
    }
    std::ranges::sort(all);

    sample.runs = all.size();
    sample.p50Seconds = percentile(all, 0.50);
    sample.p99Seconds = percentile(all, 0.99);
    sample.maxSeconds = all.empty() ? 0.0 : all.back();
    sample.throughputPerSecond =
        wallSeconds > 0.0 ? static_cast<double>(all.size()) / wallSeconds : 0.0;
    sample.cpuCores = (cpuBefore >= 0.0 && cpuAfter >= 0.0 && wallSeconds > 0.0)
                          ? (cpuAfter - cpuBefore) / wallSeconds
                          : -1.0;
}

} // namespace

FftBenchmarkConfig defaultFftBenchmarkConfig() {
    return FftBenchmarkConfig{
        .sizes = {1024, 4096, 16384, 65536},
        .threadCounts = {1},
        .quality = FftPlanQuality::Balanced,
        .secondsPerSample = 0.35,
        .minRuns = 16,
        .maxRuns = 2'000'000,
        .warmupRuns = 8,
    };
}

std::size_t fftBenchmarkStepCount(const FftBenchmarkConfig& config,
                                  std::size_t backendCount) noexcept {
    return backendCount * config.sizes.size() *
           std::max<std::size_t>(config.threadCounts.size(), 1);
}

FftBenchmarkEntry benchmarkFftBackend(IFftBackend& backend, const FftBenchmarkConfig& config,
                                      const FftBenchmarkProgress& progress) {
    FftBenchmarkEntry entry{
        .backend = std::string(backend.name()),
        .displayName = backend.displayName(),
        .samples = {},
        .error = {},
    };

    std::vector<std::size_t> threadCounts = config.threadCounts;
    if (threadCounts.empty()) {
        threadCounts.push_back(1);
    }

    for (const std::size_t size : config.sizes) {
        // Planned once per size and shared across every thread count, which is
        // both faster and a truer model: the pipeline builds one plan and
        // hands it to all its workers.
        std::unique_ptr<IFftPlan> plan;
        std::string planError;
        double planSeconds = 0.0;

        if (!backend.supportsSize(size)) {
            // Not an error. A backend that only does powers of two is a
            // legitimate answer to "which is faster here", and the blank cell
            // with its reason is part of the comparison.
            planError = std::format("{} does not accept size {}", backend.name(), size);
        } else {
            const std::uint64_t planStarted = monotonicNs();
            auto created = createPlanSerialised(backend, {.size = size, .quality = config.quality});
            planSeconds = nsToSeconds(monotonicNs() - planStarted);
            if (created) {
                plan = std::move(*created);
            } else {
                planError = created.error().message();
            }
        }

        for (const std::size_t threads : threadCounts) {
            if (progress && !progress(backend.name(), size, threads)) {
                return entry;
            }

            FftBenchmarkSample sample{.size = size,
                                      .threads = std::max<std::size_t>(threads, 1),
                                      .planSeconds = planSeconds,
                                      .p50Seconds = 0.0,
                                      .p99Seconds = 0.0,
                                      .maxSeconds = 0.0,
                                      .throughputPerSecond = 0.0,
                                      .cpuCores = -1.0,
                                      .runs = 0,
                                      .skipped = planError};

            if (plan != nullptr) {
                measure(*plan, size, sample.threads, config, sample);
            }
            entry.samples.push_back(std::move(sample));
        }
    }

    return entry;
}

std::vector<FftBenchmarkEntry> benchmarkFftBackends(const FftBenchmarkConfig& config,
                                                    const FftBenchmarkProgress& progress) {
    std::vector<FftBenchmarkEntry> results;

    for (const FftBackendInfo& info : FftBackendManager::instance().enumerate()) {
        if (!info.available) {
            continue;
        }

        auto acquired = FftBackendManager::instance().acquire(info.name);
        if (!acquired) {
            // A backend can fail on first use -- that is what lazy
            // instantiation is for -- and the registry demotes it when it
            // does. Recorded here so the panel shows the reason rather than a
            // backend that silently vanished between listing and measuring.
            results.push_back(FftBenchmarkEntry{.backend = info.name,
                                                .displayName = info.displayName,
                                                .samples = {},
                                                .error = acquired.error().message()});
            continue;
        }

        results.push_back(benchmarkFftBackend(**acquired, config, progress));
    }

    return results;
}

} // namespace sweeppp
