// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#pragma once

#include "sweeps/FileFormat.hpp"
#include "sweeps/Log.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sweeps {

/// One spectrum, as the writer needs to see it.
///
/// A view rather than a frame type of its own: the library has no opinion about
/// how an application represents a spectrum, owns no buffer, and copies nothing
/// it does not store. Whatever produced the bins keeps owning them for the
/// duration of the call.
struct FrameView {
    const float* bins = nullptr;
    std::size_t count = 0;

    /// Absolute frequency of the first bin, and the spacing between bins. A
    /// partial sweep frame covers only its step's slice, so these are not
    /// derivable from the configuration alone.
    double startHz = 0.0;
    double binWidthHz = 0.0;

    std::uint64_t monotonicNs = 0;
    std::uint64_t wallNs = 0;

    /// Never null in a valid frame. A change here is what opens a new segment.
    const AcquisitionConfig* config = nullptr;
};

struct WriterConfig {
    /// Bins written per line. Frames are decimated onto this grid before
    /// storage.
    ///
    /// Display resolution (~2k bins) is ~40 KB/s, about 150 MB/hour. Full
    /// sweep resolution is ~1.7 MB/s, about 6 GB/hour. Both are viable on
    /// disk; only the first is viable in RAM, which is why a store built on
    /// this is tiered rather than one or the other.
    std::uint32_t binsPerLine = 2048;

    /// Stop writing once the file reaches this size. 0 means unbounded.
    std::uint64_t maxBytes = 0;
    /// Stop after this much wall time. 0 means unbounded.
    double maxSeconds = 0.0;
    /// Stop if the filesystem drops below this much free space. 0 disables.
    std::uint64_t minFreeBytes = 512ULL * 1024 * 1024;

    std::string sessionName;
    std::string notes;

    /// Recorded in the manifest as `app_version`. Empty means this library's
    /// own version.
    ///
    /// Injectable because it lands in file bytes: pinning it, together with
    /// createdWallNs, is what makes a byte-for-byte golden file possible, and a
    /// golden file is what proves a change did not move the format.
    std::string applicationVersion;

    /// The file header's creation timestamp, and the manifest's `created`.
    /// 0 means "read the wall clock now".
    std::uint64_t createdWallNs = 0;

    /// Where diagnostics go. Discarded by default.
    Log log;
};

/// What one frame did to the file.
struct FrameOutcome {
    /// True when this frame's configuration or grid differed from the open
    /// segment's, so a new one was opened.
    bool segmentOpened = false;
    std::uint32_t segmentId = 0;
    /// Why, in the words written into the file. Empty unless a segment opened.
    std::string reason;

    /// True once a retention cap has stopped the writer. Further frames are
    /// accepted and ignored.
    bool retentionStopped = false;
};

/// Writes a `.sweeps` file.
///
/// **Synchronous.** Every call writes on the calling thread, and the writer owns
/// no thread, queue or lock. That is deliberate: a recorder that must not stall
/// acquisition needs a queue, but the shape of that queue is an application's
/// decision -- and building it in would put threading in the one library that
/// has no business containing any.
///
/// The same container serves a live session, a saved recording and an alert
/// capture. They differ in extent and retention, never in code path.
class SessionWriter {
public:
    [[nodiscard]] static Result<std::unique_ptr<SessionWriter>>
    create(const std::filesystem::path& path, WriterConfig config = {});

    ~SessionWriter();

    SessionWriter(const SessionWriter&) = delete;
    SessionWriter& operator=(const SessionWriter&) = delete;

    /// Stores one spectrum, opening a new segment if the grid changed.
    [[nodiscard]] Result<FrameOutcome> writeFrame(const FrameView& frame);

    /// Appends an event to the stream, immediately.
    ///
    /// The record's `segmentId` is the segment open at this moment, not
    /// whatever the caller put in the event. A live event is stamped where it
    /// actually landed; only an extraction preserves a decoded value.
    [[nodiscard]] Status recordEvent(const SessionEvent& event);

    /// Appends a producer's own record. `body` is opaque to the container.
    ///
    /// `monotonicNs` of 0 means the record is not tied to a moment, which is
    /// what keeps it in an extraction whatever range was asked for.
    [[nodiscard]] Status writePluginData(std::string_view pluginId, std::string_view recordName,
                                         std::uint32_t schemaVersion, std::uint64_t monotonicNs,
                                         const void* body, std::size_t bodyBytes);

    [[nodiscard]] Status writePluginEvent(std::string_view pluginId, std::string_view eventName,
                                          std::uint64_t monotonicNs, std::uint64_t wallNs,
                                          const Metadata& fields);

    /// Finishes the file: flushes tiles, writes the index, patches the header.
    /// Idempotent, and called by the destructor -- but a session ended by power
    /// loss is still readable, by design.
    [[nodiscard]] Status close();

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }
    [[nodiscard]] std::uint64_t bytesWritten() const noexcept { return m_bytesWritten; }
    [[nodiscard]] std::uint64_t linesWritten() const noexcept { return m_linesWritten; }
    [[nodiscard]] std::uint32_t segmentCount() const noexcept {
        return static_cast<std::uint32_t>(m_segments.size());
    }

    /// Timestamp of the most recent frame written. Defines a segment's end.
    [[nodiscard]] std::uint64_t lastFrameNs() const noexcept { return m_lastFrameNs; }

    /// True once a retention cap stopped the writer.
    [[nodiscard]] bool retentionReached() const noexcept { return m_retentionReached; }
    [[nodiscard]] const std::string& retentionReason() const noexcept { return m_retentionReason; }

private:
    SessionWriter(std::filesystem::path path, std::unique_ptr<char[]> streamBuffer,
                  std::ofstream stream, WriterConfig config);

    /// One tile being accumulated in memory before it is written.
    ///
    /// Held as floats and quantised at flush, not as it arrives. The origin is
    /// per tile, and it can only be chosen well once the tile's actual range is
    /// known -- deriving it from the first line means a burst arriving later
    /// saturates at the top of the scale and is recorded 40 dB low. The cost is
    /// ~1 MiB per tile in flight, which is nothing next to getting the levels
    /// wrong.
    struct PendingTile {
        TileHeader header;
        std::vector<float> values;
        std::uint32_t linesFilled = 0;
    };

    /// Accumulator for one LOD level's in-progress line.
    struct LodAccumulator {
        std::vector<float> maxHold;
        std::uint32_t sourceLines = 0;
        std::uint64_t firstNs = 0;
        std::uint64_t lastNs = 0;
        std::uint64_t lineIndex = 0;
    };

    struct SegmentState {
        SegmentInfo info;
        std::vector<LodAccumulator> lods;
        std::map<TileKey, PendingTile> pending;
    };

    [[nodiscard]] Status writeRecord(RecordType type, const std::vector<std::byte>& payload);
    [[nodiscard]] Status writeManifest();

    [[nodiscard]] Status openSegment(const AcquisitionConfig& config, const SegmentGrid& grid,
                                     std::uint64_t monotonicNs, std::uint64_t wallNs,
                                     std::string reason);
    [[nodiscard]] Status closeSegment(std::uint64_t monotonicNs);

    void appendLine(SegmentState& segment, std::uint32_t lod, const std::vector<float>& bins,
                    std::uint64_t monotonicNs);
    [[nodiscard]] Status flushTile(const TileKey& key, PendingTile& tile);
    [[nodiscard]] Status flushSegmentTiles(SegmentState& segment);
    [[nodiscard]] Status writeIndex();

    [[nodiscard]] bool retentionExceeded();

    /// Resamples a frame's bins onto the segment grid, taking the max within
    /// each destination bin.
    ///
    /// Max rather than mean, for the same reason the LOD pyramid uses max: a
    /// narrow signal falling between stored bins must survive. Averaging would
    /// dilute it into the floor -- and finding narrow signals is the point.
    void resampleToGrid(const FrameView& frame, const SegmentGrid& grid,
                        std::vector<float>& out) const;

    std::filesystem::path m_path;

    /// The stream's put area, sized far above the default 4 KiB.
    ///
    /// Declared before m_stream so it is destroyed after it: the filebuf holds
    /// a bare pointer to this for its whole life. Records are small and
    /// frequent -- a retune event per sweep step -- and a large buffer turns
    /// that stream of tiny writes into occasional bulk ones, which is easier on
    /// both the syscall path and the drive.
    std::unique_ptr<char[]> m_streamBuffer;
    std::ofstream m_stream;
    WriterConfig m_config;

    std::vector<SegmentState> m_segments;
    std::vector<IndexEntry> m_index;

    std::uint64_t m_bytesWritten = 0;
    std::uint64_t m_linesWritten = 0;
    std::uint64_t m_startWallNs = 0;
    std::uint64_t m_startMonotonicNs = 0;
    /// Timestamp of the most recent frame written, which is what defines a
    /// segment's end -- see writeFrame.
    std::uint64_t m_lastFrameNs = 0;

    bool m_closed = false;
    bool m_headerWritten = false;
    bool m_retentionReached = false;
    std::string m_retentionReason;

    std::vector<float> m_resampleScratch;
};

} // namespace sweeps
