// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/pipeline/SpectrumFrame.hpp"
#include "sweeppp/remote/Messages.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <sweeps/FileFormat.hpp>
#include <sweeps/Stream.hpp>
#include <vector>

namespace sweeppp::remote {

/// The bins that changed across one or more frames: a range, nothing, or
/// unknown -- which is everything.
struct ChangedBins {
    bool known = true;
    std::size_t first = 0;
    std::size_t end = 0;

    [[nodiscard]] static ChangedBins unknown() noexcept { return {.known = false}; }

    /// What `frame` says changed since the frame before it.
    [[nodiscard]] static ChangedBins of(const SpectrumFrame& frame) noexcept;

    [[nodiscard]] bool empty() const noexcept { return known && first >= end; }

    void add(const ChangedBins& other) noexcept;
};

/// `frame` reduced to at most `maxBins`, each new bin the strongest of the
/// `ceil(bins / maxBins)` it covers -- so a narrow carrier survives the
/// reduction at its true level -- and unmeasured where none of them was.
/// `changed` is narrowed to the reduced grid along with it.
[[nodiscard]] SpectrumFrame reduceFrame(const SpectrumFrame& frame, std::size_t maxBins,
                                        ChangedBins& changed);

/// `reduceFrame` for a stream of frames, keeping the reduced grid between
/// them: only the groups `changed` touches are recomputed, so a sweep's
/// partial frames cost their own width rather than the whole grid's.
class FrameReducer {
public:
    [[nodiscard]] SpectrumFrame reduce(const SpectrumFrame& frame, std::size_t maxBins,
                                       ChangedBins& changed);

private:
    std::vector<float> m_levels;
    std::size_t m_sourceBins = 0;
    double m_startHz = 0.0;
    double m_binWidthHz = 0.0;
    std::size_t m_group = 0;
};

/// Turns the frames a server publishes into the records that rebuild them on
/// the client: a SegmentOpen when the grid or the acquisition config changes,
/// a tile for each 1024-bin block whose quantised levels changed, and a
/// commit. A sweep's partial frames therefore cost only the part of the span
/// measured since the last one.
class FrameEncoder {
public:
    struct Stats {
        std::uint64_t frames = 0;
        std::uint64_t segments = 0;
        std::uint64_t tilesSent = 0;
        /// Blocks whose levels moved, but not by enough to change a byte.
        std::uint64_t tilesUnchanged = 0;
    };

    /// Appends the records that take a mirror from the last frame encoded to
    /// `frame`. `changed` is every bin that moved since that frame; blocks
    /// outside it are not even compared. A frame with no bins appends
    /// nothing.
    void encode(const SpectrumFrame& frame, std::vector<std::byte>& out,
                ChangedBins changed = ChangedBins::unknown());

    /// Appends a SegmentClose for the open segment, if there is one.
    void close(std::uint64_t monotonicNs, std::vector<std::byte>& out);

    /// Forgets what was sent, for a new receiver: the next frame opens a
    /// segment and sends every block.
    void reset() noexcept;

    [[nodiscard]] const Stats& stats() const noexcept { return m_stats; }

private:
    void openSegment(const SpectrumFrame& frame, std::vector<std::byte>& out);

    bool m_open = false;
    std::uint32_t m_nextSegmentId = 0;
    sweeps::SegmentInfo m_segment;
    std::uint32_t m_line = 0;

    /// What the receiver holds, block by block: the levels last looked at,
    /// and the origin and bytes last sent.
    std::vector<float> m_levels;
    std::vector<std::uint8_t> m_bytes;
    std::vector<float> m_origins;
    std::vector<bool> m_sent;

    std::vector<std::uint8_t> m_quantised;
    std::vector<std::byte> m_payload;
    Stats m_stats;
};

/// The client's copy of the server's current line, rebuilt from the records
/// `FrameEncoder` sends: a `sweeps::LineMirror`, and the commit that turns its
/// line into a frame.
class FrameMirror {
public:
    FrameMirror() noexcept;

    /// Applies a SegmentOpen, SegmentClose or Tile; any other record type is
    /// not this class's and is ignored. ProtocolError for a segment or tile
    /// that does not fit: a grid wider than `kMaxGridBins`, a tile outside
    /// the grid, a level of detail or line count no stream sends. A tile for a
    /// segment other than the open one is ignored (Appendix C.5).
    Status apply(const sweeps::StreamRecord& record);

    /// The frame `commit` completes, its times still the server's. An error
    /// when it names a segment that is not open.
    [[nodiscard]] Result<std::shared_ptr<SpectrumFrame>> commit(const FrameCommit& commit);

    /// The open segment, or null before the first.
    [[nodiscard]] const sweeps::SegmentInfo* segment() const noexcept { return m_line.segment(); }

    void reset() noexcept { m_line.reset(); }

private:
    sweeps::LineMirror m_line;
};

} // namespace sweeppp::remote
