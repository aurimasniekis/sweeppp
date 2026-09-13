// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/pipeline/SpectrumFrame.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace sweeppp {

/// A consumer of live spectrum frames.
///
/// The contract is the same one the USB callback lives under: **non-blocking
/// and drop-aware**. `onFrame` runs on the publishing thread, so a consumer
/// that blocks blocks acquisition. A consumer that cannot keep up must drop
/// and count, not wait.
///
/// This is what makes recording and alerting addable later without reworking
/// the core: they attach here as peers of the UI, and a slow recorder can
/// never stall the radio.
class IFrameConsumer {
public:
    virtual ~IFrameConsumer() = default;

    IFrameConsumer(const IFrameConsumer&) = delete;
    IFrameConsumer& operator=(const IFrameConsumer&) = delete;

    /// Called for every published frame, in ascending sequence order and never
    /// concurrently with itself.
    ///
    /// The serialisation guarantee is deliberate. Several worker threads
    /// publish, but a consumer that had to be thread-safe *and* non-blocking
    /// would be almost impossible to write correctly -- and a waterfall or a
    /// session file built from out-of-order frames would show time running
    /// backwards. FrameBus pays one mutex per frame so that every consumer can
    /// be plain single-threaded code.
    ///
    /// Must not block, allocate unboundedly, or throw.
    virtual void onFrame(const SpectrumFramePtr& frame) noexcept = 0;

    /// Name shown in the Performance panel beside this consumer's counters.
    [[nodiscard]] virtual std::string_view consumerName() const noexcept = 0;

protected:
    IFrameConsumer() = default;
};

/// Per-consumer delivery counters.
///
/// Each consumer gets its own, so "the recorder is dropping frames" and "the
/// UI is dropping frames" are distinguishable statements. A single global
/// counter would make a slow consumer look like an acquisition problem.
struct ConsumerStats {
    std::atomic<std::uint64_t> framesDelivered{0};
    std::atomic<std::uint64_t> framesDropped{0};
    std::atomic<std::uint64_t> lastSequence{0};
    /// Longest onFrame() call seen, in microseconds. A consumer that spikes
    /// here is the one to suspect when acquisition latency rises.
    std::atomic<std::uint64_t> maxCallbackUs{0};

    void reset() noexcept {
        framesDelivered.store(0, std::memory_order_relaxed);
        framesDropped.store(0, std::memory_order_relaxed);
        lastSequence.store(0, std::memory_order_relaxed);
        maxCallbackUs.store(0, std::memory_order_relaxed);
    }
};

struct ConsumerSnapshot {
    std::string name;
    std::uint64_t framesDelivered = 0;
    std::uint64_t framesDropped = 0;
    std::uint64_t maxCallbackUs = 0;
    double dropFraction = 0.0;
};

/// Fan-out point for spectrum frames.
///
/// The pipeline publishes here rather than pushing to the UI directly. UI,
/// history store, session recorder, alert evaluator, remote server and plugins
/// all attach as equals. Nothing in the pipeline knows how many consumers
/// exist or what they do.
class FrameBus {
public:
    using SubscriptionId = std::uint64_t;

    FrameBus() = default;

    FrameBus(const FrameBus&) = delete;
    FrameBus& operator=(const FrameBus&) = delete;

    /// The consumer must outlive the subscription. Returns an id for
    /// unsubscribe().
    SubscriptionId subscribe(IFrameConsumer* consumer);
    void unsubscribe(SubscriptionId id);

    /// Delivers to every consumer, in subscription order.
    ///
    /// Serialised across publishers, so consumers see frames one at a time.
    /// Called from a pipeline worker. Each consumer's callback is timed, and
    /// an exception escaping a consumer is caught and counted rather than
    /// allowed to take down acquisition -- one misbehaving plugin must not
    /// stop the radio.
    void publish(const SpectrumFramePtr& frame) noexcept;

    [[nodiscard]] std::vector<ConsumerSnapshot> consumerStats() const;

    [[nodiscard]] std::size_t consumerCount() const;

    void resetStats();

    /// Total frames published, regardless of consumer count.
    [[nodiscard]] std::uint64_t framesPublished() const noexcept {
        return m_framesPublished.load(std::memory_order_relaxed);
    }

private:
    struct Entry {
        SubscriptionId id = 0;
        IFrameConsumer* consumer = nullptr;
        std::shared_ptr<ConsumerStats> stats;
    };

    // shared_mutex rather than mutex: publish() runs at frame rate and only
    // reads the list, while subscribe/unsubscribe happen a handful of times
    // per session.
    mutable std::shared_mutex m_mutex;
    /// Serialises the fan-out itself, so consumers are never re-entered
    /// concurrently -- which is what lets a consumer be plain single-threaded
    /// code.
    ///
    /// It does **not** let subscribing overtake a slow consumer's callback,
    /// whatever the arrangement of two locks suggests: publish() holds
    /// m_mutex shared across the whole fan-out, so unsubscribe() waits for the
    /// callback in progress. That wait is the only thing making unsubscribe
    /// safe -- it is what guarantees no consumer is inside onFrame when its
    /// owner destroys it. Narrowing the shared lock to the copy of the list
    /// would "fix" the contention and reintroduce a use-after-free.
    mutable std::mutex m_publishMutex;
    std::vector<Entry> m_entries;
    SubscriptionId m_nextId = 0;
    std::atomic<std::uint64_t> m_framesPublished{0};
};

/// Consumer that keeps the most recent N frames.
///
/// The "snapshot the last few seconds" affordance in its simplest form, and
/// the fallback when the full history store is not running. Bounded by frame
/// count, and drops the oldest rather than growing.
class FrameHistoryRing final : public IFrameConsumer {
public:
    explicit FrameHistoryRing(std::size_t capacity = 256);

    void onFrame(const SpectrumFramePtr& frame) noexcept override;
    [[nodiscard]] std::string_view consumerName() const noexcept override { return "history-ring"; }

    /// Most recent first.
    [[nodiscard]] std::vector<SpectrumFramePtr> recent(std::size_t count) const;

    /// Every frame whose timestamp falls in [fromNs, toNs], oldest first.
    [[nodiscard]] std::vector<SpectrumFramePtr> range(std::uint64_t fromNs,
                                                      std::uint64_t toNs) const;

    [[nodiscard]] SpectrumFramePtr latest() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
    void clear();

private:
    mutable std::mutex m_mutex;
    std::vector<SpectrumFramePtr> m_frames;
    std::size_t m_capacity;
    std::size_t m_next = 0;
    std::size_t m_count = 0;
};

} // namespace sweeppp
