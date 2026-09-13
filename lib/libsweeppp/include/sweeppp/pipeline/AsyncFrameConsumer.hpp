// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/pipeline/FrameBus.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace sweeppp {

/// Base class for a frame consumer whose work is too slow to do inline.
///
/// `IFrameConsumer::onFrame` runs on a pipeline worker thread, so a consumer
/// that does real work there does not merely slow itself down -- it slows the
/// worker, backs up the block queue, and turns into dropped *samples*. The
/// contract is that onFrame must return promptly.
///
/// Anything genuinely slow -- writing tiles to disk, evaluating alert rules,
/// pushing over a socket -- therefore belongs behind a bounded queue on its own
/// thread, which is exactly what this provides. When the queue is full the
/// frame is dropped and counted, so a consumer that cannot keep up degrades by
/// losing its own frames rather than by degrading everyone else's.
///
/// This is the shape the session recorder, the alert evaluator and the remote
/// server all take. Providing it once here is what makes "recording can be
/// added later without reworking the core" actually true.
class AsyncFrameConsumer : public IFrameConsumer {
public:
    /// `queueDepth` is the number of frames that may be buffered before this
    /// consumer starts dropping. Deeper means more tolerance of a burst, at
    /// the cost of memory and of falling further behind real time.
    explicit AsyncFrameConsumer(std::string name, std::size_t queueDepth = 64);
    ~AsyncFrameConsumer() override;

    AsyncFrameConsumer(const AsyncFrameConsumer&) = delete;
    AsyncFrameConsumer& operator=(const AsyncFrameConsumer&) = delete;

    /// Enqueues and returns immediately. Never blocks.
    void onFrame(const SpectrumFramePtr& frame) noexcept final;

    [[nodiscard]] std::string_view consumerName() const noexcept final { return m_name; }

    /// Frames this consumer dropped because its own queue was full. Distinct
    /// from the bus's per-consumer counter, which measures delivery; this one
    /// measures the consumer's own capacity.
    [[nodiscard]] std::uint64_t droppedFrames() const noexcept {
        return m_dropped.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t processedFrames() const noexcept {
        return m_processed.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t queueDepth() const;

    /// Blocks until the queue drains. For an orderly shutdown, where losing
    /// the tail of a recording would be a real loss rather than a dropped
    /// display frame.
    void flush();

    /// Stops the worker thread. Called by the destructor; safe to call twice.
    void shutdown();

protected:
    /// Runs on this consumer's own thread, one frame at a time, in order.
    /// May take as long as it needs.
    virtual void processFrame(const SpectrumFramePtr& frame) = 0;

    /// Called on the consumer thread when it starts and stops, for setup and
    /// teardown that must not happen on the caller's thread.
    virtual void onStart() {}
    virtual void onStop() {}

    /// Starts the worker. Must be called by the derived constructor, not this
    /// one -- the thread calls processFrame(), which is pure virtual until the
    /// derived object is fully constructed.
    void startWorker();

private:
    void workerLoop(std::stop_token stop);

    std::string m_name;
    std::size_t m_queueDepth;

    mutable std::mutex m_mutex;
    std::condition_variable m_notEmpty;
    std::condition_variable m_drained;
    std::deque<SpectrumFramePtr> m_queue;
    bool m_stopping = false;

    std::atomic<std::uint64_t> m_dropped{0};
    std::atomic<std::uint64_t> m_processed{0};

    std::jthread m_thread;
};

} // namespace sweeppp
