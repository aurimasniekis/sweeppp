// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/SpscRing.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace sweeppp {

/// Bounded lock-free queue with multiple producers and multiple consumers.
///
/// The acquisition path is single-producer, but the *consumer* side is the FFT
/// worker pool -- N threads all pulling from one queue. That makes an SPSC
/// ring the wrong structure: two workers popping concurrently would corrupt
/// the tail index and hand the same slot to both.
///
/// This is Vyukov's bounded queue: each cell carries its own sequence number,
/// which encodes whose turn it is. A producer may write cell `i` only when its
/// sequence equals the enqueue position; a consumer may read it only when the
/// sequence is one past. That per-cell handshake is what removes the need for
/// any separate "is this slot still in use" bookkeeping -- the race that a
/// side table of in-flight items would reintroduce.
///
/// Both push and pop are lock-free and wait-free in the uncontended case, and
/// neither ever blocks. A full queue means push returns false and the caller
/// drops and counts, exactly as the USB callback contract requires.
template <typename T, std::size_t Capacity>
class MpmcQueue {
    static_assert(Capacity >= 2, "MpmcQueue needs at least two slots");
    static_assert(std::has_single_bit(Capacity), "MpmcQueue capacity must be a power of two");

public:
    static constexpr std::size_t kCapacity = Capacity;
    static constexpr std::size_t kMask = Capacity - 1;

    MpmcQueue() {
        for (std::size_t i = 0; i < Capacity; ++i) {
            m_cells[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    MpmcQueue(const MpmcQueue&) = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;

    /// Returns false when full. Never blocks, never allocates.
    [[nodiscard]] bool push(T&& value) noexcept {
        Cell* cell = nullptr;
        std::size_t position = m_enqueue.load(std::memory_order_relaxed);

        while (true) {
            cell = &m_cells[position & kMask];
            const std::size_t sequence = cell->sequence.load(std::memory_order_acquire);
            const auto difference =
                static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(position);

            if (difference == 0) {
                // The cell is ours if we win the position.
                if (m_enqueue.compare_exchange_weak(position, position + 1,
                                                    std::memory_order_relaxed)) {
                    break;
                }
            } else if (difference < 0) {
                // The cell still holds an item a consumer has not taken: full.
                return false;
            } else {
                position = m_enqueue.load(std::memory_order_relaxed);
            }
        }

        cell->data = std::move(value);
        // Release publishes the write above to whichever consumer reads it.
        cell->sequence.store(position + 1, std::memory_order_release);
        return true;
    }

    /// Returns false when empty.
    [[nodiscard]] bool pop(T& out) noexcept {
        Cell* cell = nullptr;
        std::size_t position = m_dequeue.load(std::memory_order_relaxed);

        while (true) {
            cell = &m_cells[position & kMask];
            const std::size_t sequence = cell->sequence.load(std::memory_order_acquire);
            const auto difference =
                static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(position + 1);

            if (difference == 0) {
                if (m_dequeue.compare_exchange_weak(position, position + 1,
                                                    std::memory_order_relaxed)) {
                    break;
                }
            } else if (difference < 0) {
                return false; // empty
            } else {
                position = m_dequeue.load(std::memory_order_relaxed);
            }
        }

        out = std::move(cell->data);
        cell->data = T{};
        // Hands the cell back to a producer one lap later.
        cell->sequence.store(position + Capacity, std::memory_order_release);
        return true;
    }

    /// Approximate occupancy. Feeds a fill gauge redrawn a few times a second,
    /// so an instant of staleness does not matter.
    [[nodiscard]] std::size_t sizeApprox() const noexcept {
        const std::size_t enqueue = m_enqueue.load(std::memory_order_acquire);
        const std::size_t dequeue = m_dequeue.load(std::memory_order_acquire);
        return enqueue >= dequeue ? enqueue - dequeue : 0;
    }

    [[nodiscard]] float fillFraction() const noexcept {
        return static_cast<float>(sizeApprox()) / static_cast<float>(Capacity);
    }

    [[nodiscard]] bool empty() const noexcept { return sizeApprox() == 0; }

    /// Drains everything. Only safe once producers and consumers have stopped.
    void clear() noexcept {
        T discarded{};
        while (pop(discarded)) {
        }
    }

private:
    struct Cell {
        std::atomic<std::size_t> sequence{0};
        T data{};
    };

    // The two positions live on separate cache lines: producers touch one and
    // consumers the other, and sharing a line would put them in a false-
    // sharing fight that costs more than the queue operation itself.
    alignas(kCacheLineSize) std::array<Cell, Capacity> m_cells{};
    alignas(kCacheLineSize) std::atomic<std::size_t> m_enqueue{0};
    alignas(kCacheLineSize) std::atomic<std::size_t> m_dequeue{0};
};

} // namespace sweeppp
