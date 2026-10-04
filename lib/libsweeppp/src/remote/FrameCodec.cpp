// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/remote/FrameCodec.hpp"

#include "sweeppp/remote/Protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sweeps/Records.hpp>

namespace sweeppp::remote {
namespace {

using sweeps::kTileBins;
using sweeps::RecordType;

constexpr auto recordType(RecordType type) noexcept {
    return static_cast<std::uint16_t>(type);
}

/// Every field, exactly: a receiver's frames carry the segment's config, so
/// any change at all -- a gain, a reference level -- is a new segment.
bool sameConfig(const AcquisitionConfig& a, const AcquisitionConfig& b) noexcept {
    return a.centerHz == b.centerHz && a.spanHz == b.spanHz && a.sampleRate == b.sampleRate &&
           a.fftSize == b.fftSize && a.window == b.window && a.windowBeta == b.windowBeta &&
           a.windowEnbw == b.windowEnbw && a.overlap == b.overlap && a.rbwHz == b.rbwHz &&
           a.gains == b.gains && a.referenceLevelDbm == b.referenceLevelDbm &&
           a.dbfsToDbmOffset == b.dbfsToDbmOffset && a.deviceId == b.deviceId &&
           a.deviceLabel == b.deviceLabel;
}

std::size_t blockCount(std::size_t bins) noexcept {
    return (bins + kTileBins - 1) / kTileBins;
}

} // namespace

// ----------------------------------------------------------------- ChangedBins

ChangedBins ChangedBins::of(const SpectrumFrame& frame) noexcept {
    if (!frame.dirtyKnown) {
        return unknown();
    }
    return ChangedBins{.first = frame.dirtyFirstBin, .end = frame.dirtyEndBin};
}

void ChangedBins::add(const ChangedBins& other) noexcept {
    if (!known || !other.known) {
        *this = unknown();
        return;
    }
    if (other.empty()) {
        return;
    }
    if (empty()) {
        *this = other;
        return;
    }
    first = std::min(first, other.first);
    end = std::max(end, other.end);
}

namespace {

std::size_t groupFor(std::size_t bins, std::size_t maxBins) noexcept {
    return maxBins == 0 ? 1 : std::max<std::size_t>(1, (bins + maxBins - 1) / maxBins);
}

/// The strongest measured level of each group in [firstGroup, endGroup).
void reduceGroups(const std::vector<float>& source, std::size_t group, std::size_t firstGroup,
                  std::size_t endGroup, std::vector<float>& out) {
    const std::size_t bins = source.size();
    for (std::size_t index = firstGroup; index < endGroup; ++index) {
        const std::size_t first = index * group;
        const std::size_t last = std::min(first + group, bins);
        float strongest = kUnmeasuredDbfs;
        for (std::size_t bin = first; bin < last; ++bin) {
            const float level = source[bin];
            if (measured(level) && (!measured(strongest) || level > strongest)) {
                strongest = level;
            }
        }
        out[index] = strongest;
    }
}

/// Everything but the levels.
SpectrumFrame reducedShell(const SpectrumFrame& frame, std::size_t group) {
    SpectrumFrame reduced;
    reduced.sequence = frame.sequence;
    reduced.hostTimeNs = frame.hostTimeNs;
    reduced.wallTimeNs = frame.wallTimeNs;
    reduced.deviceTimeNs = frame.deviceTimeNs;
    reduced.sweepPass = frame.sweepPass;
    reduced.sweepStep = frame.sweepStep;
    reduced.passComplete = frame.passComplete;
    reduced.startHz = frame.startHz;
    reduced.binWidthHz = frame.binWidthHz * static_cast<double>(group);
    reduced.config = frame.config;
    reduced.averageCount = frame.averageCount;
    reduced.clippedFraction = frame.clippedFraction;
    return reduced;
}

void narrow(ChangedBins& changed, std::size_t group) noexcept {
    if (changed.known && !changed.empty()) {
        changed.first /= group;
        changed.end = (changed.end + group - 1) / group;
    }
}

} // namespace

SpectrumFrame reduceFrame(const SpectrumFrame& frame, std::size_t maxBins, ChangedBins& changed) {
    FrameReducer reducer;
    return reducer.reduce(frame, maxBins, changed);
}

SpectrumFrame FrameReducer::reduce(const SpectrumFrame& frame, std::size_t maxBins,
                                   ChangedBins& changed) {
    const std::size_t bins = frame.binCount();
    const std::size_t group = groupFor(bins, maxBins);
    const std::size_t groups = (bins + group - 1) / group;

    // A different grid, or nothing known about what moved: all of it again.
    const bool sameGrid = bins == m_sourceBins && group == m_group && frame.startHz == m_startHz &&
                          frame.binWidthHz == m_binWidthHz;
    if (!sameGrid || !changed.known) {
        m_levels.assign(groups, kUnmeasuredDbfs);
        reduceGroups(frame.binsDbfs, group, 0, groups, m_levels);
        m_sourceBins = bins;
        m_group = group;
        m_startHz = frame.startHz;
        m_binWidthHz = frame.binWidthHz;
    } else if (!changed.empty()) {
        const std::size_t first = std::min(changed.first / group, groups);
        const std::size_t end = std::min((changed.end + group - 1) / group, groups);
        reduceGroups(frame.binsDbfs, group, first, end, m_levels);
    }

    SpectrumFrame reduced = reducedShell(frame, group);
    reduced.binsDbfs = m_levels;
    narrow(changed, group);
    return reduced;
}

// ---------------------------------------------------------------- FrameEncoder

void FrameEncoder::openSegment(const SpectrumFrame& frame, std::vector<std::byte>& out) {
    if (m_open) {
        close(frame.hostTimeNs, out);
    }

    m_segment = sweeps::SegmentInfo{};
    m_segment.id = m_nextSegmentId++;
    m_segment.grid = sweeps::SegmentGrid{.startHz = frame.startHz,
                                         .binWidthHz = frame.binWidthHz,
                                         .binCount = static_cast<std::uint32_t>(frame.binCount())};
    m_segment.startWallNs = frame.wallTimeNs;
    m_segment.startMonotonicNs = frame.hostTimeNs;
    m_segment.config = frame.config;
    m_segment.reason = m_stats.segments == 0 ? "stream start" : "acquisition changed";

    m_payload.clear();
    sweeps::encodeSegmentOpen(m_payload, m_segment);
    sweeps::appendRecord(out, recordType(RecordType::SegmentOpen), m_payload.data(),
                         m_payload.size());

    const std::size_t bins = frame.binCount();
    m_levels.assign(bins, kUnmeasuredDbfs);
    m_bytes.assign(bins, sweeps::kUnmeasuredByte);
    m_origins.assign(blockCount(bins), 0.0F);
    m_sent.assign(blockCount(bins), false);
    m_line = 0;
    m_open = true;
    ++m_stats.segments;
}

void FrameEncoder::encode(const SpectrumFrame& frame, std::vector<std::byte>& out,
                          ChangedBins changed) {
    const std::size_t bins = frame.binCount();
    if (bins == 0) {
        return;
    }
    if (!m_open || m_segment.grid.binCount != bins || m_segment.grid.startHz != frame.startHz ||
        m_segment.grid.binWidthHz != frame.binWidthHz ||
        !sameConfig(m_segment.config, frame.config)) {
        openSegment(frame, out);
    }

    m_quantised.resize(kTileBins);
    for (std::size_t block = 0; block < m_sent.size(); ++block) {
        const std::size_t first = block * kTileBins;
        const std::size_t count = std::min<std::size_t>(kTileBins, bins - first);
        const float* levels = frame.binsDbfs.data() + first;

        // Outside what changed, what was sent still stands; only a block
        // never sent has to be looked at.
        if (m_sent[block] && changed.known &&
            (first + count <= changed.first || first >= changed.end)) {
            continue;
        }
        if (m_sent[block] &&
            std::memcmp(levels, m_levels.data() + first, count * sizeof(float)) == 0) {
            continue;
        }
        std::memcpy(m_levels.data() + first, levels, count * sizeof(float));

        const float origin = sweeps::chooseTileOrigin(levels, count);
        sweeps::quantiseTile(levels, count, origin, m_quantised.data());
        if (m_sent[block] && origin == m_origins[block] &&
            std::memcmp(m_quantised.data(), m_bytes.data() + first, count) == 0) {
            ++m_stats.tilesUnchanged;
            continue;
        }

        const sweeps::TileHeader header{.segmentId = m_segment.id,
                                        .lod = 0,
                                        .timeBlock = m_line,
                                        .freqBlock = static_cast<std::uint32_t>(block),
                                        .lines = 1,
                                        .bins = static_cast<std::uint32_t>(count),
                                        .originDb = origin,
                                        .firstLineNs = frame.hostTimeNs,
                                        .lastLineNs = frame.hostTimeNs};
        m_payload.clear();
        sweeps::encodeTile(m_payload, header, m_quantised.data(), count);
        sweeps::appendRecord(out, recordType(RecordType::Tile), m_payload.data(), m_payload.size());

        std::memcpy(m_bytes.data() + first, m_quantised.data(), count);
        m_origins[block] = origin;
        m_sent[block] = true;
        ++m_stats.tilesSent;
    }

    const FrameCommit commit{.segmentId = m_segment.id,
                             .line = m_line,
                             .sequence = frame.sequence,
                             .hostTimeNs = frame.hostTimeNs,
                             .wallTimeNs = frame.wallTimeNs,
                             .deviceTimeNs = frame.deviceTimeNs,
                             .sweepPass = frame.sweepPass,
                             .sweepStep = frame.sweepStep,
                             .passComplete = frame.passComplete,
                             .averageCount = frame.averageCount,
                             .clippedFraction = frame.clippedFraction};
    appendMessage(out, msg::kFrame, commit.toMetadata(), frame.hostTimeNs);

    ++m_line;
    ++m_stats.frames;
}

void FrameEncoder::close(std::uint64_t monotonicNs, std::vector<std::byte>& out) {
    if (!m_open) {
        return;
    }
    m_payload.clear();
    sweeps::encodeSegmentClose(m_payload, sweeps::SegmentClose{.id = m_segment.id,
                                                               .endMonotonicNs = monotonicNs,
                                                               .lineCount = m_line});
    sweeps::appendRecord(out, recordType(RecordType::SegmentClose), m_payload.data(),
                         m_payload.size());
    m_open = false;
}

void FrameEncoder::reset() noexcept {
    m_open = false;
    m_line = 0;
}

// ----------------------------------------------------------------- FrameMirror

Status FrameMirror::apply(const sweeps::StreamRecord& record) {
    sweeps::ByteReader in(record.payload.data(), record.payload.size());
    switch (static_cast<RecordType>(record.header.type)) {
    case RecordType::SegmentOpen:
        return applySegmentOpen(in);

    case RecordType::Tile:
        return applyTile(in);

    case RecordType::SegmentClose: {
        // Nothing to undo: the levels stay what they were, and the next
        // SegmentOpen replaces them.
        auto close = adopt(sweeps::decodeSegmentClose(in));
        if (!close) {
            return std::unexpected(std::move(close).error());
        }
        return ok();
    }

    default:
        return ok();
    }
}

Status FrameMirror::applySegmentOpen(sweeps::ByteReader& in) {
    auto segment = adopt(sweeps::decodeSegmentOpen(in, true));
    if (!segment) {
        return std::unexpected(std::move(segment).error());
    }
    const sweeps::SegmentGrid& grid = segment->grid;
    if (grid.binCount == 0 || grid.binCount > kMaxGridBins) {
        return fail(ErrorCode::ProtocolError, "segment {} has {} bins; at most {} are accepted",
                    segment->id, grid.binCount, kMaxGridBins);
    }
    if (!std::isfinite(grid.startHz) || !std::isfinite(grid.binWidthHz) || grid.binWidthHz <= 0.0) {
        return fail(ErrorCode::ProtocolError, "segment {} has no usable grid", segment->id);
    }

    m_levels.assign(grid.binCount, kUnmeasuredDbfs);
    m_segment = std::move(*segment);
    return ok();
}

Status FrameMirror::applyTile(sweeps::ByteReader& in) {
    auto tile = adopt(sweeps::decodeTile(in));
    if (!tile) {
        return std::unexpected(std::move(tile).error());
    }
    const sweeps::TileHeader& header = tile->header;
    if (!m_segment || header.segmentId != m_segment->id) {
        return ok();
    }
    if (header.lod != 0 || header.lines != 1) {
        return fail(ErrorCode::ProtocolError, "a tile of {} lines at level {} on a live stream",
                    header.lines, header.lod);
    }

    // In u64: a hostile freqBlock times the tile width overflows a u32.
    const std::uint64_t binCount = m_segment->grid.binCount;
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
        levels[i] = sweeps::isMeasured(value)
                        ? static_cast<float>(sweeps::dequantiseDb(value, origin))
                        : kUnmeasuredDbfs;
    }
    return ok();
}

Result<std::shared_ptr<SpectrumFrame>> FrameMirror::commit(const FrameCommit& commit) {
    if (!m_segment || commit.segmentId != m_segment->id) {
        return fail<std::shared_ptr<SpectrumFrame>>(ErrorCode::ProtocolError,
                                                    "a frame for segment {}, which is not open",
                                                    commit.segmentId);
    }

    auto frame = std::make_shared<SpectrumFrame>();
    frame->sequence = commit.sequence;
    frame->hostTimeNs = commit.hostTimeNs;
    frame->wallTimeNs = commit.wallTimeNs;
    frame->deviceTimeNs = commit.deviceTimeNs;
    frame->sweepPass = commit.sweepPass;
    frame->sweepStep = commit.sweepStep;
    frame->passComplete = commit.passComplete;
    frame->startHz = m_segment->grid.startHz;
    frame->binWidthHz = m_segment->grid.binWidthHz;
    frame->binsDbfs = m_levels;
    frame->config = m_segment->config;
    frame->averageCount = commit.averageCount;
    frame->clippedFraction = commit.clippedFraction;
    return frame;
}

void FrameMirror::reset() noexcept {
    m_segment.reset();
    m_levels.clear();
}

} // namespace sweeppp::remote
