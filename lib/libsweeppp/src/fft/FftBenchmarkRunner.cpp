// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/fft/FftBenchmarkRunner.hpp"

#include "sweeppp/core/Clock.hpp"
#include "sweeppp/fft/FftBackendManager.hpp"

#include <format>
#include <utility>

namespace sweeppp {

FftBenchmarkRunner::~FftBenchmarkRunner() {
    // The worker touches this object's members, so it must not outlive it.
    // Cancelling first bounds the wait to one measurement rather than to the
    // whole remaining run -- which matters on application exit, where the
    // alternative is the window staying up for a second after the user closed
    // it.
    m_cancel.store(true);
    join();
}

void FftBenchmarkRunner::join() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void FftBenchmarkRunner::start(FftBenchmarkConfig config) {
    if (m_running.load()) {
        return;
    }

    // A previous run's thread has finished but has not been joined yet:
    // `running` goes false inside the worker, just before it returns.
    join();

    {
        const std::lock_guard lock(m_mutex);
        m_results.clear();
        m_currentStep.clear();
    }

    // The step count the progress bar divides by. Counted from the same things
    // the worker iterates -- available backends, sizes and thread counts -- so
    // a backend that turns out to be unacquirable simply makes the bar finish
    // early rather than making it wrong.
    std::size_t backends = 0;
    for (const FftBackendInfo& info : FftBackendManager::instance().enumerate()) {
        backends += info.available ? 1 : 0;
    }

    m_cancel.store(false);
    m_complete.store(false);
    m_stepsDone.store(0);
    m_stepsTotal.store(fftBenchmarkStepCount(config, backends));
    m_startedNs.store(monotonicNs());
    m_finishedNs.store(0);
    m_running.store(true);

    m_thread = std::thread([this, config = std::move(config)] {
        // The callback fires *before* each measurement, so when it runs for
        // sample n the bar should read n completed -- store first, then count.
        // Incrementing first would show the run finished while the last and
        // longest transform was still going.
        std::size_t started = 0;
        auto progress = [this, &started](std::string_view backend, std::size_t size,
                                         std::size_t threads) {
            if (m_cancel.load()) {
                return false;
            }
            m_stepsDone.store(started);
            ++started;
            {
                const std::lock_guard lock(m_mutex);
                m_currentStep = threads == 1 ? std::format("{} at {} points", backend, size)
                                             : std::format("{} at {} points, {} threads", backend,
                                                           size, threads);
            }
            return true;
        };

        std::vector<FftBenchmarkEntry> results = benchmarkFftBackends(config, progress);
        std::size_t done = 0;
        for (const FftBenchmarkEntry& entry : results) {
            done += entry.samples.size();
        }

        {
            const std::lock_guard lock(m_mutex);
            m_results = std::move(results);
            m_currentStep.clear();
        }

        m_stepsDone.store(done);
        m_finishedNs.store(monotonicNs());
        m_complete.store(!m_cancel.load());
        m_running.store(false);
    });
}

void FftBenchmarkRunner::cancel() {
    m_cancel.store(true);
    join();
}

std::vector<FftBenchmarkEntry> FftBenchmarkRunner::results() const {
    const std::lock_guard lock(m_mutex);
    return m_results;
}

std::string FftBenchmarkRunner::currentStep() const {
    const std::lock_guard lock(m_mutex);
    return m_currentStep;
}

double FftBenchmarkRunner::elapsedSeconds() const noexcept {
    const std::uint64_t started = m_startedNs.load();
    if (started == 0) {
        return 0.0;
    }
    const std::uint64_t finished = m_finishedNs.load();
    return nsToSeconds((finished != 0 ? finished : monotonicNs()) - started);
}

} // namespace sweeppp
