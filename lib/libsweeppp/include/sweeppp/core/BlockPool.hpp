// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/core/SpscRing.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace sweeppp {

class BlockPool;

/// Reference-counted handle to one pooled block.
///
/// Move-only, and releasing is the destructor's job -- a block leaked on an
/// error path would shrink the pool permanently and show up much later as
/// unexplained drops.
class BlockRef {
public:
    BlockRef() = default;
    ~BlockRef();

    BlockRef(const BlockRef& other);
    BlockRef& operator=(const BlockRef& other);

    BlockRef(BlockRef&& other) noexcept : m_pool(other.m_pool), m_index(other.m_index) {
        other.m_pool = nullptr;
        other.m_index = kInvalidIndex;
    }

    BlockRef& operator=(BlockRef&& other) noexcept {
        if (this != &other) {
            reset();
            m_pool = other.m_pool;
            m_index = other.m_index;
            other.m_pool = nullptr;
            other.m_index = kInvalidIndex;
        }
        return *this;
    }

    static constexpr std::uint32_t kInvalidIndex = 0xFFFF'FFFFU;

    [[nodiscard]] bool valid() const noexcept {
        return m_pool != nullptr && m_index != kInvalidIndex;
    }
    explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] std::uint32_t index() const noexcept { return m_index; }

    /// Full extent of the block. The number of *useful* bytes is carried
    /// alongside by whatever fills it (IqBlock::frames), not by the pool.
    [[nodiscard]] std::span<std::byte> bytes() const noexcept;
    [[nodiscard]] std::byte* data() const noexcept;
    [[nodiscard]] std::size_t capacityBytes() const noexcept;

    void reset() noexcept;

private:
    friend class BlockPool;

    BlockRef(BlockPool* pool, std::uint32_t index) noexcept : m_pool(pool), m_index(index) {}

    BlockPool* m_pool = nullptr;
    std::uint32_t m_index = kInvalidIndex;
};

/// Fixed-size pool of cache-aligned sample blocks.
///
/// At 100 MS/s the acquisition path cannot allocate: a single malloc on the
/// USB transfer thread is enough to overrun the device. Every block is
/// allocated once at start-up and recycled through a lock-free free list, so
/// steady-state acquisition performs no allocation at all.
///
/// The free list is a Treiber stack over block indices with a 32-bit tag
/// packed alongside the head index. The tag is what makes it ABA-safe: without
/// it, a block popped and pushed back between one thread's load and its
/// compare-exchange would let that CAS succeed against a stale `next`.
class BlockPool {
public:
    /// `blockBytes` is rounded up to a cache line. Fails rather than throwing
    /// so that a too-large request surfaces as an operator-visible message.
    [[nodiscard]] static Result<std::unique_ptr<BlockPool>> create(std::size_t blockBytes,
                                                                   std::uint32_t blockCount);

    ~BlockPool();

    BlockPool(const BlockPool&) = delete;
    BlockPool& operator=(const BlockPool&) = delete;

    /// Takes a block, or returns an invalid handle when the pool is exhausted.
    /// Never blocks, never allocates -- callable from a USB callback.
    [[nodiscard]] BlockRef acquire() noexcept;

    [[nodiscard]] std::size_t blockBytes() const noexcept { return m_blockBytes; }
    [[nodiscard]] std::uint32_t blockCount() const noexcept { return m_blockCount; }

    /// Blocks currently checked out. Feeds the Performance panel; exact only
    /// in the sense that it is a sum of atomics read at slightly different
    /// times.
    [[nodiscard]] std::uint32_t inUse() const noexcept {
        return m_inUse.load(std::memory_order_relaxed);
    }

    [[nodiscard]] float utilisation() const noexcept {
        return static_cast<float>(inUse()) / static_cast<float>(m_blockCount);
    }

    /// Number of acquire() calls that found the pool empty. A non-zero value
    /// here means the consumer side is not keeping up, which is a different
    /// diagnosis from a full ring.
    [[nodiscard]] std::uint64_t exhaustedCount() const noexcept {
        return m_exhausted.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t totalBytes() const noexcept {
        return static_cast<std::size_t>(m_blockCount) * m_blockBytes;
    }

private:
    friend class BlockRef;

    BlockPool(std::byte* storage, std::size_t blockBytes, std::uint32_t blockCount);

    void retain(std::uint32_t index) noexcept;
    void release(std::uint32_t index) noexcept;
    [[nodiscard]] std::byte* blockData(std::uint32_t index) const noexcept {
        return m_storage + (static_cast<std::size_t>(index) * m_blockBytes);
    }

    // Head of the free list: index in the low 32 bits, ABA tag in the high 32.
    static constexpr std::uint64_t pack(std::uint32_t index, std::uint32_t tag) noexcept {
        return (static_cast<std::uint64_t>(tag) << 32U) | index;
    }
    static constexpr std::uint32_t unpackIndex(std::uint64_t value) noexcept {
        return static_cast<std::uint32_t>(value & 0xFFFF'FFFFU);
    }
    static constexpr std::uint32_t unpackTag(std::uint64_t value) noexcept {
        return static_cast<std::uint32_t>(value >> 32U);
    }

    std::byte* m_storage = nullptr;
    std::size_t m_blockBytes = 0;
    std::uint32_t m_blockCount = 0;

    alignas(kCacheLineSize) std::atomic<std::uint64_t> m_freeHead{pack(BlockRef::kInvalidIndex, 0)};

    // Parallel arrays rather than a struct per block: the refcounts are
    // touched by the workers and the next-pointers by whoever is pushing to
    // the free list, and keeping them apart avoids dragging one into the
    // other's cache line.
    std::vector<std::atomic<std::uint32_t>> m_refCounts;
    std::vector<std::atomic<std::uint32_t>> m_next;

    alignas(kCacheLineSize) std::atomic<std::uint32_t> m_inUse{0};
    std::atomic<std::uint64_t> m_exhausted{0};
};

} // namespace sweeppp
