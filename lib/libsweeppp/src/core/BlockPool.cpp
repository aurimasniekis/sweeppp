// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/core/BlockPool.hpp"

#include <cstdlib>
#include <new>

namespace sweeppp {
namespace {

constexpr std::size_t roundUpTo(std::size_t value, std::size_t multiple) noexcept {
    return ((value + multiple - 1) / multiple) * multiple;
}

} // namespace

// --------------------------------------------------------------- BlockRef

BlockRef::~BlockRef() {
    reset();
}

BlockRef::BlockRef(const BlockRef& other) : m_pool(other.m_pool), m_index(other.m_index) {
    if (valid()) {
        m_pool->retain(m_index);
    }
}

BlockRef& BlockRef::operator=(const BlockRef& other) {
    if (this != &other) {
        BlockPool* pool = other.m_pool;
        const std::uint32_t index = other.m_index;
        if (pool != nullptr && index != kInvalidIndex) {
            pool->retain(index);
        }
        reset();
        m_pool = pool;
        m_index = index;
    }
    return *this;
}

void BlockRef::reset() noexcept {
    if (valid()) {
        m_pool->release(m_index);
    }
    m_pool = nullptr;
    m_index = kInvalidIndex;
}

std::byte* BlockRef::data() const noexcept {
    return valid() ? m_pool->blockData(m_index) : nullptr;
}

std::size_t BlockRef::capacityBytes() const noexcept {
    return valid() ? m_pool->blockBytes() : 0;
}

std::span<std::byte> BlockRef::bytes() const noexcept {
    if (!valid()) {
        return {};
    }
    return {m_pool->blockData(m_index), m_pool->blockBytes()};
}

// -------------------------------------------------------------- BlockPool

Result<std::unique_ptr<BlockPool>> BlockPool::create(std::size_t blockBytes,
                                                     std::uint32_t blockCount) {
    if (blockBytes == 0 || blockCount == 0) {
        return fail<std::unique_ptr<BlockPool>>(
            ErrorCode::InvalidArgument, "block pool needs a non-zero size and count (got {}x{})",
            blockCount, blockBytes);
    }
    // kInvalidIndex is the free-list terminator, so it cannot also be a block.
    if (blockCount >= BlockRef::kInvalidIndex) {
        return fail<std::unique_ptr<BlockPool>>(ErrorCode::InvalidArgument,
                                                "block count {} exceeds the addressable maximum",
                                                blockCount);
    }

    const std::size_t aligned = roundUpTo(blockBytes, kCacheLineSize);
    const std::size_t total = aligned * static_cast<std::size_t>(blockCount);

    // Every block starts on a cache line so that two workers processing
    // adjacent blocks never share one.
    auto* storage = static_cast<std::byte*>(
        ::operator new(total, std::align_val_t{kCacheLineSize}, std::nothrow));
    if (storage == nullptr) {
        return fail<std::unique_ptr<BlockPool>>(
            ErrorCode::OutOfMemory, "could not allocate {} blocks of {} bytes ({:.1f} MiB)",
            blockCount, aligned, static_cast<double>(total) / (1024.0 * 1024.0));
    }

    return std::unique_ptr<BlockPool>(new BlockPool(storage, aligned, blockCount));
}

BlockPool::BlockPool(std::byte* storage, std::size_t blockBytes, std::uint32_t blockCount)
    : m_storage(storage), m_blockBytes(blockBytes), m_blockCount(blockCount),
      m_refCounts(blockCount), m_next(blockCount) {
    // Thread the free list in reverse so acquire() hands out block 0 first --
    // it makes a trace of block indices readable during debugging.
    std::uint32_t head = BlockRef::kInvalidIndex;
    for (std::uint32_t i = blockCount; i > 0; --i) {
        const std::uint32_t index = i - 1;
        m_refCounts[index].store(0, std::memory_order_relaxed);
        m_next[index].store(head, std::memory_order_relaxed);
        head = index;
    }
    m_freeHead.store(pack(head, 0), std::memory_order_release);
}

BlockPool::~BlockPool() {
    ::operator delete(m_storage, std::align_val_t{kCacheLineSize});
}

BlockRef BlockPool::acquire() noexcept {
    std::uint64_t head = m_freeHead.load(std::memory_order_acquire);

    while (true) {
        const std::uint32_t index = unpackIndex(head);
        if (index == BlockRef::kInvalidIndex) {
            m_exhausted.fetch_add(1, std::memory_order_relaxed);
            return {};
        }

        const std::uint32_t next = m_next[index].load(std::memory_order_relaxed);
        const std::uint64_t desired = pack(next, unpackTag(head) + 1);

        if (m_freeHead.compare_exchange_weak(head, desired, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            m_refCounts[index].store(1, std::memory_order_relaxed);
            m_inUse.fetch_add(1, std::memory_order_relaxed);
            return {this, index};
        }
        // head was refreshed by the failed exchange; retry.
    }
}

void BlockPool::retain(std::uint32_t index) noexcept {
    m_refCounts[index].fetch_add(1, std::memory_order_relaxed);
}

void BlockPool::release(std::uint32_t index) noexcept {
    // release/acquire pairing: the last releaser must see every write made by
    // every other holder before the block is handed out again.
    if (m_refCounts[index].fetch_sub(1, std::memory_order_release) != 1) {
        return;
    }
    std::atomic_thread_fence(std::memory_order_acquire);

    m_inUse.fetch_sub(1, std::memory_order_relaxed);

    std::uint64_t head = m_freeHead.load(std::memory_order_relaxed);
    while (true) {
        m_next[index].store(unpackIndex(head), std::memory_order_relaxed);
        const std::uint64_t desired = pack(index, unpackTag(head) + 1);
        if (m_freeHead.compare_exchange_weak(head, desired, std::memory_order_release,
                                             std::memory_order_relaxed)) {
            return;
        }
    }
}

} // namespace sweeppp
