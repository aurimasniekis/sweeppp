// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/fft/IFftBackend.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace sweeppp {

/// Comparing FFT backends on the machine that will run them.
///
/// Which backend is fastest is a property of the CPU, not of the project, so it
/// cannot be answered once in a document.
///
/// **It also cannot be answered by one thread.** The pipeline shares a single
/// plan across every worker, and backends do not rank the same way under that
/// as they do alone: one that copies its input into scratch is nearly free on
/// an idle machine and expensive when eight workers are competing for the same
/// cache. Measuring at one thread and calling it the answer produced a
/// recommendation that the pipeline then contradicted, which is why
/// `threadCounts` exists and why throughput is reported beside per-transform
/// time -- at N threads those are different questions and only the first is
/// the one a sweep pays.
///
/// Deliberately here rather than in the GUI: it is arithmetic over
/// `IFftBackend` with no interface in it, so the panel, the CLI and a test can
/// all ask the same question and get the same numbers.

/// One backend, at one size, at one thread count.
struct FftBenchmarkSample {
    std::size_t size = 0;
    std::size_t threads = 1;

    /// Building the plan, at the configured quality. Independent of the thread
    /// count -- the pipeline plans once and shares it -- so the same figure
    /// repeats across a size's rows.
    double planSeconds = 0.0;

    /// Per transform, from the timings the worker threads took themselves.
    ///
    /// p99 earns its place: two backends can agree on the median and differ
    /// sevenfold in the tail, and it is the tail an operator sees as the
    /// display hitching rather than as a slower average.
    double p50Seconds = 0.0;
    double p99Seconds = 0.0;
    double maxSeconds = 0.0;

    /// Transforms per second that `threads` workers achieved together.
    ///
    /// Not `threads / p50`: that assumes perfect scaling, which is exactly the
    /// assumption being tested. This is the figure the pipeline gets.
    double throughputPerSecond = 0.0;

    /// Process CPU seconds burned per wall second during the measurement.
    ///
    /// Above `threads` means the backend is threading internally -- worth
    /// knowing, because a backend that quietly uses every core wins a
    /// benchmark and then contends with the rest of the pipeline. Negative
    /// where the platform does not report CPU time.
    double cpuCores = 0.0;

    /// Transforms actually timed, across all threads.
    std::size_t runs = 0;

    /// Why this size was not measured. Empty when it was.
    std::string skipped;
};

struct FftBenchmarkEntry {
    std::string backend; ///< Registry name, the same string `--fft-backend` takes.
    std::string displayName;
    std::vector<FftBenchmarkSample> samples;

    /// Why the backend produced nothing at all -- it could not be acquired,
    /// typically. Kept rather than dropped, for the same reason the registry
    /// lists unavailable backends with their reason.
    std::string error;
};

struct FftBenchmarkConfig {
    std::vector<std::size_t> sizes;

    /// Thread counts to measure at. One entry of 1 gives the old
    /// single-threaded comparison; add the pipeline's worker count to get the
    /// one that predicts a sweep.
    std::vector<std::size_t> threadCounts{1};

    /// Balanced by default, because that is what the pipeline asks for and so
    /// what the plan column should reflect.
    FftPlanQuality quality = FftPlanQuality::Balanced;

    /// How long to keep timing each point.
    ///
    /// A budget rather than a fixed run count, because a count that gives a
    /// stable median at 1024 is 60x too little work at 65536 -- which is how a
    /// benchmark comes to disagree with itself between runs.
    double secondsPerSample = 0.35;

    /// Floor and ceiling on the budget, per thread. The floor keeps a fast
    /// size honest when the clock is coarse; the ceiling stops a slow one
    /// running away.
    std::size_t minRuns = 16;
    std::size_t maxRuns = 2'000'000;

    /// Discarded. Enough to fault in the buffers and let the CPU reach a
    /// steady clock, and no more.
    std::size_t warmupRuns = 8;
};

/// Sizes worth comparing by default, at one thread. Callers wanting the
/// figure that predicts their own pipeline should add its worker count to
/// `threadCounts`.
[[nodiscard]] FftBenchmarkConfig defaultFftBenchmarkConfig();

/// Called before each measurement. Return false to abandon the run, which is
/// what makes a benchmark cancellable from a UI without a thread being killed
/// from outside.
using FftBenchmarkProgress =
    std::function<bool(std::string_view backend, std::size_t size, std::size_t threads)>;

/// Times one backend. Never fails: a size the backend refuses becomes a
/// skipped sample with the reason, because "this backend cannot do 3000
/// points" is a comparison result and not an error.
[[nodiscard]] FftBenchmarkEntry benchmarkFftBackend(IFftBackend& backend,
                                                    const FftBenchmarkConfig& config,
                                                    const FftBenchmarkProgress& progress = {});

/// Every *available* registered backend, in registry order.
///
/// Acquires each one, which has a consequence worth knowing: an acquired
/// backend can no longer be withdrawn, so a plugin providing one will report
/// "restart needed" when disabled afterwards. That is the price of measuring
/// it -- there is no way to time a backend without instantiating it.
[[nodiscard]] std::vector<FftBenchmarkEntry>
benchmarkFftBackends(const FftBenchmarkConfig& config, const FftBenchmarkProgress& progress = {});

/// Total measurements a config implies, for a progress bar.
[[nodiscard]] std::size_t fftBenchmarkStepCount(const FftBenchmarkConfig& config,
                                                std::size_t backendCount) noexcept;

} // namespace sweeppp
