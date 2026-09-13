// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <type_traits>

namespace sweeppp {

/// Hardware destructive interference size. libc++ does not define
/// std::hardware_destructive_interference_size, and on Apple silicon the value
/// that matters is 128 (the L2 prefetch pair), not 64.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t kCacheLineSize = 128;
#else
inline constexpr std::size_t kCacheLineSize = 64;
#endif

/// Wait-free single-producer / single-consumer ring buffer.
///
/// This sits between the USB transfer thread and the FFT workers, so the
/// producer side must never block, allocate or take a lock -- a stalled USB
/// callback causes device-side overruns, which is exactly the failure the
/// telemetry is meant to be reporting rather than causing.
///
/// The head and tail indices live on separate cache lines. Sharing one line
/// would put the producer and consumer into a false-sharing ping-pong that
/// costs more than the queue operation itself.
///
/// `T` must be trivially copyable: the intended payload is a block *index*,
/// not a block.
template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "SpscRing payloads must be trivially copyable");
    static_assert(Capacity >= 2, "SpscRing needs at least two slots");
    static_assert(std::has_single_bit(Capacity), "SpscRing capacity must be a power of two");

public:
    static constexpr std::size_t kCapacity = Capacity;
    static constexpr std::size_t kMask = Capacity - 1;

    SpscRing() = default;

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    /// Producer side. Returns false when full -- the caller drops the item and
    /// bumps a counter rather than waiting.
    [[nodiscard]] bool push(const T& value) noexcept {
        const std::size_t head = m_head.load(std::memory_order_relaxed);
        const std::size_t next = head + 1;

        // Re-read the consumer's index only when our cached copy says full.
        // In the common case this avoids touching the consumer's cache line at
        // all, which is the whole point of caching it.
        if (next - m_cachedTail > Capacity) {
            m_cachedTail = m_tail.load(std::memory_order_acquire);
            if (next - m_cachedTail > Capacity) {
                return false;
            }
        }

        m_slots[head & kMask] = value;
        m_head.store(next, std::memory_order_release);
        return true;
    }

    /// Consumer side. Returns false when empty.
    [[nodiscard]] bool pop(T& out) noexcept {
        const std::size_t tail = m_tail.load(std::memory_order_relaxed);

        if (tail == m_cachedHead) {
            m_cachedHead = m_head.load(std::memory_order_acquire);
            if (tail == m_cachedHead) {
                return false;
            }
        }

        out = m_slots[tail & kMask];
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Approximate occupancy. Safe to call from any thread, exact from neither
    /// -- it feeds a fill gauge that is redrawn a few times a second.
    [[nodiscard]] std::size_t sizeApprox() const noexcept {
        const std::size_t head = m_head.load(std::memory_order_acquire);
        const std::size_t tail = m_tail.load(std::memory_order_acquire);
        return head >= tail ? head - tail : 0;
    }

    [[nodiscard]] float fillFraction() const noexcept {
        return static_cast<float>(sizeApprox()) / static_cast<float>(Capacity);
    }

    [[nodiscard]] bool empty() const noexcept { return sizeApprox() == 0; }

    /// Discards everything queued. Consumer-side only, and only while the
    /// producer is stopped.
    void clear() noexcept {
        m_tail.store(m_head.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    // Monotonic counters rather than wrapped indices: the difference between
    // them is the occupancy directly, with no ambiguity between full and empty
    // and no wasted slot.
    alignas(kCacheLineSize) std::atomic<std::size_t> m_head{0};
    std::size_t m_cachedTail = 0; // producer's cached view of m_tail

    alignas(kCacheLineSize) std::atomic<std::size_t> m_tail{0};
    std::size_t m_cachedHead = 0; // consumer's cached view of m_head

    alignas(kCacheLineSize) std::array<T, Capacity> m_slots{};
};

} // namespace sweeppp
