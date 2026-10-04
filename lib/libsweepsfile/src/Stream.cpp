// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include "sweeps/Stream.hpp"

#include "sweeps/Records.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace sweeps {

using detail::fail;

void encodeStreamHeader(std::vector<std::byte>& out, const StreamHeader& header) {
    writeBytes(out, header.magic.data(), header.magic.size());
    writeU32(out, header.majorVersion);
    writeU32(out, header.minorVersion);
    writeU32(out, header.incompatibleFeatures);
}

Result<StreamHeader> decodeStreamHeader(const std::byte* data, std::size_t bytes) {
    ByteReader in(data, bytes);

    StreamHeader header;
    if (auto read = in.readBytes(header.magic.data(), header.magic.size()); !read) {
        return fail<StreamHeader>(ErrorCode::ProtocolError, "truncated stream header");
    }
    if (header.magic != kMagic) {
        return fail<StreamHeader>(ErrorCode::ProtocolError, "not a .sweeps stream (bad magic)");
    }

    auto major = in.readU32();
    auto minor = in.readU32();
    auto features = in.readU32();
    if (!major || !minor || !features) {
        return fail<StreamHeader>(ErrorCode::ProtocolError, "truncated stream header");
    }
    header.majorVersion = *major;
    header.minorVersion = *minor;
    header.incompatibleFeatures = *features;

    if (header.majorVersion > kMajorVersion) {
        return fail<StreamHeader>(ErrorCode::Unsupported,
                                  "the stream is .sweeps major version {}, newer than this "
                                  "build's {}",
                                  header.majorVersion, kMajorVersion);
    }
    if (const std::uint32_t unknown = header.incompatibleFeatures & ~kKnownFeatures; unknown != 0) {
        std::array<char, 16> bits{};
        std::snprintf(bits.data(), bits.size(), "0x%08X", static_cast<unsigned>(unknown));
        return fail<StreamHeader>(ErrorCode::Unsupported,
                                  "the stream needs .sweeps features this build does not "
                                  "implement (unknown bits {})",
                                  bits.data());
    }
    return header;
}

void RecordFramer::feed(const std::byte* data, std::size_t bytes) {
    if (m_broken || bytes == 0) {
        return;
    }
    // Consumed bytes are dropped before more are appended, so the buffer holds
    // at most one partial record plus whatever arrived with it.
    if (m_read > 0) {
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(m_read));
        m_read = 0;
    }
    m_buffer.insert(m_buffer.end(), data, data + bytes);
}

Result<bool> RecordFramer::next(StreamRecord& out) {
    if (m_broken) {
        return unexpected<Error>(m_error);
    }

    const std::size_t available = m_buffer.size() - m_read;
    if (available < RecordHeader::kBytes) {
        return false;
    }

    ByteReader in(m_buffer.data() + m_read, available);
    auto header = decodeRecordHeader(in);
    if (!header) {
        return false;
    }

    if (header->payloadBytes > m_maxPayloadBytes) {
        m_broken = true;
        m_error = Error(ErrorCode::ProtocolError,
                        detail::format("a record of {} bytes exceeds the stream's limit of {}",
                                       header->payloadBytes, m_maxPayloadBytes));
        return unexpected<Error>(m_error);
    }

    const std::size_t total = RecordHeader::kBytes + header->payloadBytes;
    if (available < total) {
        return false;
    }

    const std::byte* payload = m_buffer.data() + m_read + RecordHeader::kBytes;
    if (crc32(payload, header->payloadBytes) != header->checksum) {
        m_broken = true;
        m_error = Error(ErrorCode::Corrupt,
                        detail::format("checksum mismatch in a {} record",
                                       toString(static_cast<RecordType>(header->type))));
        return unexpected<Error>(m_error);
    }

    out.header = *header;
    out.payload.assign(payload, payload + header->payloadBytes);
    m_read += total;
    if (m_read == m_buffer.size()) {
        m_buffer.clear();
        m_read = 0;
    }
    return true;
}

Status LineMirror::apply(const StreamRecord& record) {
    return apply(record.header.type, record.payload.data(), record.payload.size());
}

Status LineMirror::apply(std::uint16_t type, const std::byte* payload, std::size_t bytes) {
    ByteReader in(payload, bytes);
    switch (static_cast<RecordType>(type)) {
    case RecordType::SegmentOpen:
        return applySegmentOpen(in);

    case RecordType::Tile:
        return applyTile(in);

    case RecordType::SegmentClose: {
        // Nothing to undo: the levels stay what they were, and the next
        // SegmentOpen replaces them.
        auto close = decodeSegmentClose(in);
        if (!close) {
            return unexpected<Error>(close.error());
        }
        return ok();
    }

    default:
        return ok();
    }
}

Status LineMirror::applySegmentOpen(ByteReader& in) {
    auto segment = decodeSegmentOpen(in, true);
    if (!segment) {
        return unexpected<Error>(segment.error());
    }
    const SegmentGrid& grid = segment->grid;
    if (grid.binCount == 0 || grid.binCount > m_maxBins) {
        return fail(ErrorCode::ProtocolError, "segment {} has {} bins; at most {} are accepted",
                    segment->id, grid.binCount, m_maxBins);
    }
    if (!std::isfinite(grid.startHz) || !std::isfinite(grid.binWidthHz) || grid.binWidthHz <= 0.0) {
        return fail(ErrorCode::ProtocolError, "segment {} has no usable grid", segment->id);
    }

    m_levels.assign(grid.binCount, static_cast<float>(kUnmeasuredDb));
    m_segment = std::move(*segment);
    m_open = true;
    m_line = 0;
    m_tilesApplied = 0;
    return ok();
}

Status LineMirror::applyTile(ByteReader& in) {
    auto tile = decodeTile(in);
    if (!tile) {
        return unexpected<Error>(tile.error());
    }
    const TileHeader& header = tile->header;
    if (!m_open || header.segmentId != m_segment.id) {
        return ok();
    }
    if (header.lod != 0 || header.lines != 1) {
        return fail(ErrorCode::ProtocolError, "a tile of {} lines at level {} on a live stream",
                    header.lines, header.lod);
    }

    // In u64: a hostile freqBlock times the tile width overflows a u32.
    const std::uint64_t binCount = m_segment.grid.binCount;
    const std::uint64_t first = std::uint64_t{header.freqBlock} * kTileBins;
    if (first >= binCount || header.bins != std::min<std::uint64_t>(kTileBins, binCount - first)) {
        return fail(ErrorCode::ProtocolError, "tile {} of {} bins does not fit a {}-bin grid",
                    header.freqBlock, header.bins, binCount);
    }
    if (!std::isfinite(header.originDb)) {
        return fail(ErrorCode::ProtocolError, "tile {} has no usable origin", header.freqBlock);
    }

    float* levels = m_levels.data() + first;
    const double origin = static_cast<double>(header.originDb);
    for (std::size_t i = 0; i < header.bins; ++i) {
        const std::uint8_t value = tile->data[i];
        levels[i] = isMeasured(value) ? static_cast<float>(dequantiseDb(value, origin))
                                      : static_cast<float>(kUnmeasuredDb);
    }
    m_line = header.timeBlock;
    ++m_tilesApplied;
    return ok();
}

void LineMirror::reset() noexcept {
    m_open = false;
    m_segment = SegmentInfo{};
    m_levels.clear();
    m_line = 0;
    m_tilesApplied = 0;
}

} // namespace sweeps
