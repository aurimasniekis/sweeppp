// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/pipeline/FrameBus.hpp"

#include "sweeppp/core/Clock.hpp"
#include "sweeppp/core/Log.hpp"

#include <algorithm>
#include <cmath>

namespace sweeppp {

// AcquisitionConfig::gridDiffers used to be defined here, a long way from its
// declaration. It moved to libsweepsfile with the structure itself: the rule
// for when a new segment is required is a property of the session format.

FrameBus::SubscriptionId FrameBus::subscribe(IFrameConsumer* consumer) {
    if (consumer == nullptr) {
        return 0;
    }

    const std::unique_lock lock(m_mutex);
    const SubscriptionId id = ++m_nextId;
    m_entries.push_back(
        Entry{.id = id, .consumer = consumer, .stats = std::make_shared<ConsumerStats>()});
    return id;
}

void FrameBus::unsubscribe(SubscriptionId id) {
    const std::unique_lock lock(m_mutex);
    std::erase_if(m_entries, [id](const Entry& entry) { return entry.id == id; });
}

void FrameBus::publish(const SpectrumFramePtr& frame) noexcept {
    if (!frame) {
        return;
    }

    m_framesPublished.fetch_add(1, std::memory_order_relaxed);

    // Serialises the whole fan-out: consumers are guaranteed never to be
    // re-entered concurrently, which is what lets them be plain
    // single-threaded code.
    const std::lock_guard publishLock(m_publishMutex);
    const std::shared_lock lock(m_mutex);
    for (const Entry& entry : m_entries) {
        ConsumerStats& stats = *entry.stats;

        // A gap in what this consumer saw means it dropped frames -- either it
        // declined them internally or it was added late. Counting per consumer
        // is what keeps "the recorder is behind" distinguishable from "the
        // radio is dropping samples".
        const std::uint64_t previous =
            stats.lastSequence.exchange(frame->sequence, std::memory_order_relaxed);
        if (previous != 0 && frame->sequence > previous + 1) {
            stats.framesDropped.fetch_add(frame->sequence - previous - 1,
                                          std::memory_order_relaxed);
        }

        const std::uint64_t started = monotonicNs();
        try {
            entry.consumer->onFrame(frame);
        } catch (...) {
            // A plugin consumer that throws must not take the radio down with
            // it. Counted as a drop and otherwise ignored.
            stats.framesDropped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const std::uint64_t elapsedUs = (monotonicNs() - started) / 1000;

        stats.framesDelivered.fetch_add(1, std::memory_order_relaxed);

        std::uint64_t previousMax = stats.maxCallbackUs.load(std::memory_order_relaxed);
        while (elapsedUs > previousMax && !stats.maxCallbackUs.compare_exchange_weak(
                                              previousMax, elapsedUs, std::memory_order_relaxed)) {
        }
    }
}

std::vector<ConsumerSnapshot> FrameBus::consumerStats() const {
    const std::shared_lock lock(m_mutex);

    std::vector<ConsumerSnapshot> snapshots;
    snapshots.reserve(m_entries.size());

    for (const Entry& entry : m_entries) {
        const std::uint64_t delivered =
            entry.stats->framesDelivered.load(std::memory_order_relaxed);
        const std::uint64_t dropped = entry.stats->framesDropped.load(std::memory_order_relaxed);
        const std::uint64_t total = delivered + dropped;

        snapshots.push_back(ConsumerSnapshot{
            .name = std::string(entry.consumer->consumerName()),
            .framesDelivered = delivered,
            .framesDropped = dropped,
            .maxCallbackUs = entry.stats->maxCallbackUs.load(std::memory_order_relaxed),
            .dropFraction =
                total > 0 ? static_cast<double>(dropped) / static_cast<double>(total) : 0.0});
    }

    return snapshots;
}

std::size_t FrameBus::consumerCount() const {
    const std::shared_lock lock(m_mutex);
    return m_entries.size();
}

void FrameBus::resetStats() {
    const std::shared_lock lock(m_mutex);
    for (const Entry& entry : m_entries) {
        entry.stats->reset();
    }
    m_framesPublished.store(0, std::memory_order_relaxed);
}

// ------------------------------------------------------- FrameHistoryRing

FrameHistoryRing::FrameHistoryRing(std::size_t capacity)
    : m_frames(std::max<std::size_t>(capacity, 1)), m_capacity(std::max<std::size_t>(capacity, 1)) {
}

void FrameHistoryRing::onFrame(const SpectrumFramePtr& frame) noexcept {
    const std::lock_guard lock(m_mutex);
    // Overwrites the oldest. Bounded memory is the point -- an unbounded
    // history here would be the one consumer able to exhaust the machine.
    m_frames[m_next] = frame;
    m_next = (m_next + 1) % m_capacity;
    if (m_count < m_capacity) {
        ++m_count;
    }
}

std::vector<SpectrumFramePtr> FrameHistoryRing::recent(std::size_t count) const {
    const std::lock_guard lock(m_mutex);

    std::vector<SpectrumFramePtr> result;
    const std::size_t take = std::min(count, m_count);
    result.reserve(take);

    for (std::size_t i = 0; i < take; ++i) {
        const std::size_t index = (m_next + m_capacity - 1 - i) % m_capacity;
        result.push_back(m_frames[index]);
    }
    return result;
}

std::vector<SpectrumFramePtr> FrameHistoryRing::range(std::uint64_t fromNs,
                                                      std::uint64_t toNs) const {
    const std::lock_guard lock(m_mutex);

    std::vector<SpectrumFramePtr> result;
    const std::size_t start = m_count < m_capacity ? 0 : m_next;

    for (std::size_t i = 0; i < m_count; ++i) {
        const SpectrumFramePtr& frame = m_frames[(start + i) % m_capacity];
        if (frame && frame->hostTimeNs >= fromNs && frame->hostTimeNs <= toNs) {
            result.push_back(frame);
        }
    }
    return result;
}

SpectrumFramePtr FrameHistoryRing::latest() const {
    const std::lock_guard lock(m_mutex);
    if (m_count == 0) {
        return nullptr;
    }
    return m_frames[(m_next + m_capacity - 1) % m_capacity];
}

std::size_t FrameHistoryRing::size() const {
    const std::lock_guard lock(m_mutex);
    return m_count;
}

void FrameHistoryRing::clear() {
    const std::lock_guard lock(m_mutex);
    std::ranges::fill(m_frames, nullptr);
    m_next = 0;
    m_count = 0;
}

} // namespace sweeppp
