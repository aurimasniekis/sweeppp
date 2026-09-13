// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <sweeppp/fft/FftBenchmark.hpp>
#include <thread>
#include <vector>

namespace sweeppp::ui {

/// The FFT benchmark, off the UI thread.
///
/// It has to be off it. Most of the run is FFTW's measuring planner -- half a
/// second at 65536, and the transforms themselves are milliseconds -- so a
/// comparison takes about a second, and longer if the active FFT size is
/// large. On the render thread that is a frozen window, which reads as a hang
/// rather than as work.
///
/// The table appears when the run finishes; what moves during it is the
/// progress line and bar. Publishing sample by sample would mean holding the
/// lock on the render thread's path for a table that is complete a second
/// later anyway.
class FftBenchmarkRunner {
public:
    FftBenchmarkRunner() = default;
    ~FftBenchmarkRunner();

    FftBenchmarkRunner(const FftBenchmarkRunner&) = delete;
    FftBenchmarkRunner& operator=(const FftBenchmarkRunner&) = delete;

    /// Starts a run, replacing any previous results. Ignored while one is
    /// already going.
    void start(FftBenchmarkConfig config);

    /// Asks the worker to stop at the next measurement boundary. Returns once
    /// it has: the wait is bounded by a single transform, which is
    /// microseconds, plus at worst one plan.
    void cancel();

    [[nodiscard]] bool running() const noexcept { return m_running.load(); }

    /// True once a run has finished without being cancelled.
    [[nodiscard]] bool complete() const noexcept { return m_complete.load(); }

    /// A copy, because the worker appends to the original while the UI draws.
    [[nodiscard]] std::vector<FftBenchmarkEntry> results() const;

    /// What is being measured right now, for the progress line.
    [[nodiscard]] std::string currentStep() const;

    /// Measurements finished out of the number planned, for a progress bar.
    [[nodiscard]] std::size_t stepsDone() const noexcept { return m_stepsDone.load(); }
    [[nodiscard]] std::size_t stepsTotal() const noexcept { return m_stepsTotal.load(); }

    [[nodiscard]] double elapsedSeconds() const noexcept;

private:
    void join();

    mutable std::mutex m_mutex;
    std::vector<FftBenchmarkEntry> m_results;
    std::string m_currentStep;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_complete{false};
    std::atomic<bool> m_cancel{false};
    std::atomic<std::size_t> m_stepsDone{0};
    std::atomic<std::size_t> m_stepsTotal{0};
    std::atomic<std::uint64_t> m_startedNs{0};
    std::atomic<std::uint64_t> m_finishedNs{0};

    std::thread m_thread;
};

} // namespace sweeppp::ui
