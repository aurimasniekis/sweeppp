// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#pragma once

#include "sweeps/FileFormat.hpp"
#include "sweeps/Log.hpp"
#include "sweeps/MappedFile.hpp"
#include "sweeps/Metadata.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sweeps {

/// A time+frequency range to fetch.
///
/// Shaped as tiles rather than "give me frames" on purpose: a history viewer,
/// the extraction path, plugins and a web tile endpoint are then all the same
/// call.
struct HistoryQuery {
    /// Monotonic nanoseconds. 0/max means "whatever the session covers".
    std::uint64_t fromNs = 0;
    std::uint64_t toNs = std::numeric_limits<std::uint64_t>::max();

    double fromHz = 0.0;
    double toHz = std::numeric_limits<double>::max();

    /// Caps on what comes back. The reader picks the LOD level that satisfies
    /// maxLines, which is what keeps "draw three hours" from meaning "read
    /// three hours of lines".
    std::uint32_t maxLines = 2048;
    std::uint32_t maxBins = 4096;

    /// Restrict to one segment, **by id**. Unset means every segment
    /// overlapping the range, which is what a query spanning a mid-session
    /// parameter change returns.
    std::optional<std::uint32_t> segmentId;

    /// Serve exactly this pyramid level, ignoring `maxLines`.
    ///
    /// Unset is the normal case: the reader picks the coarsest level that fits
    /// the line budget. A caller that knows which level it wants -- an export,
    /// a conformance test -- should say so rather than reverse-engineer a
    /// `maxLines` that happens to select it, which is a heuristic it would then
    /// be coupled to. A level with no tiles returns nothing rather than
    /// silently falling back.
    std::optional<std::uint32_t> lod;
};

/// One returned tile, with its true extents.
///
/// The extents are returned rather than assumed because the reader may have
/// served a coarser LOD than asked for, and because a query spanning a
/// parameter change returns tiles from segments with different grids.
struct HistoryTile {
    std::uint32_t segmentId = 0;
    std::uint32_t lod = 0;

    /// The tile's position in the segment's grid. Carried rather than left to
    /// be reconstructed from `startHz` and `firstLineNs`, because reassembling
    /// full waterfall rows out of several frequency blocks needs both, and
    /// deriving them from floating-point frequencies is a rounding bug waiting
    /// to happen.
    std::uint32_t timeBlock = 0;
    std::uint32_t freqBlock = 0;

    std::uint32_t lines = 0;
    std::uint32_t bins = 0;

    std::uint64_t firstLineNs = 0;
    std::uint64_t lastLineNs = 0;

    double startHz = 0.0;
    double binWidthHz = 0.0;

    float originDb = 0.0F;

    /// Row-major, `lines` rows of `bins` bytes. Byte-identical to a waterfall
    /// texture, so upload is a memcpy.
    std::vector<std::uint8_t> data;

    [[nodiscard]] double stopHz() const noexcept {
        return startHz + binWidthHz * static_cast<double>(bins);
    }

    [[nodiscard]] std::uint8_t at(std::uint32_t line, std::uint32_t bin) const noexcept {
        return data[static_cast<std::size_t>(line) * bins + bin];
    }

    [[nodiscard]] double dbAt(std::uint32_t line, std::uint32_t bin) const noexcept {
        return dequantiseDb(at(line, bin), static_cast<double>(originDb));
    }
};

/// One `PluginData` record.
///
/// `body` is a view into the mapping rather than a copy, valid for the life of
/// the reader: the checksum was verified during the scan, so handing back a
/// pointer costs nothing and materialises no blob a caller never asked for.
struct PluginRecord {
    std::string pluginId;
    std::string recordName;
    std::uint32_t schemaVersion = 0;
    /// 0 when the record is not tied to a moment.
    std::uint64_t monotonicNs = 0;

    const std::byte* body = nullptr;
    std::size_t bodyBytes = 0;
};

/// How a session file was opened.
struct SessionSummary {
    std::string name;
    std::string appVersion;

    /// The container version triple from the file header. Reported as three
    /// values rather than one because that is what the file carries -- a
    /// readout that showed only the major would hide the very thing the minor
    /// version exists to communicate.
    std::uint32_t majorVersion = 0;
    std::uint32_t minorVersion = 0;
    std::uint32_t incompatibleFeatures = 0;

    std::uint64_t createdWallNs = 0;

    std::uint64_t totalLines = 0;
    std::uint64_t totalTiles = 0;
    std::uint64_t fileBytes = 0;

    std::uint64_t firstLineNs = 0;
    std::uint64_t lastLineNs = 0;

    double lowestHz = 0.0;
    double highestHz = 0.0;

    /// True when the index was missing or unusable and the file was recovered
    /// by scanning. Surfaced rather than hidden -- an operator should know a
    /// session ended abruptly.
    bool recoveredByScan = false;
    std::uint64_t truncatedBytes = 0;

    /// Set when the file's minor version is newer than this reader knows. Not
    /// an error: the file was read, and additive content was skipped.
    bool newerMinorVersion = false;

    [[nodiscard]] double durationSeconds() const noexcept {
        return lastLineNs > firstLineNs ? static_cast<double>(lastLineNs - firstLineNs) * 1e-9
                                        : 0.0;
    }

    /// "1.0" -- for a readout that wants one string.
    [[nodiscard]] std::string versionString() const;
};

/// What an extraction records about itself.
struct ExtractOptions {
    /// Written to the manifest's `app_version`. Empty means this library's own
    /// version, which is the honest answer when nothing else is supplied.
    std::string applicationVersion;

    /// Manifest `created` and the file header's stamp. 0 reads the wall clock.
    std::uint64_t createdWallNs = 0;
};

/// Reads a `.sweeps` file.
///
/// The file is mmap'd, so scrollback into a multi-hour session never loads the
/// whole thing -- a tile query touches only the pages it needs.
class SessionReader {
public:
    /// `log` is where diagnostics go: a damaged index, a truncated tail, a tile
    /// that failed its checksum. Default-constructed, they are discarded. The
    /// library holds no global logger, so this is the only way to hear them.
    [[nodiscard]] static Result<std::unique_ptr<SessionReader>>
    open(const std::filesystem::path& path, Log log = {});

    ~SessionReader();

    SessionReader(const SessionReader&) = delete;
    SessionReader& operator=(const SessionReader&) = delete;

    [[nodiscard]] const SessionSummary& summary() const noexcept { return m_summary; }
    [[nodiscard]] const std::vector<SegmentInfo>& segments() const noexcept { return m_segments; }
    [[nodiscard]] const std::vector<SessionEvent>& events() const noexcept { return m_events; }

    [[nodiscard]] const std::vector<PluginRecord>& pluginRecords() const noexcept {
        return m_pluginRecords;
    }

    /// The session manifest, typed. Empty when the file carries no manifest
    /// record or when the one it carries could not be decoded -- the reader logs
    /// which of the two it was.
    [[nodiscard]] const Metadata& manifest() const noexcept { return m_manifest; }

    /// The segment with this id, or nullptr.
    ///
    /// By id, never by position: an extracted file contains segments whose ids
    /// neither start at zero nor run contiguously.
    [[nodiscard]] const SegmentInfo* segment(std::uint32_t id) const noexcept;

    /// Fetches tiles covering the query, choosing the LOD that satisfies
    /// `maxLines`.
    [[nodiscard]] Result<std::vector<HistoryTile>> query(const HistoryQuery& query) const;

    /// Every event in a time range.
    [[nodiscard]] std::vector<SessionEvent> eventsInRange(std::uint64_t fromNs,
                                                          std::uint64_t toNs) const;

    /// Reconstructs the spectrum at one instant, so clicking a waterfall line
    /// shows that moment in a linked plot.
    [[nodiscard]] Result<std::vector<float>> spectrumAt(std::uint64_t monotonicNs,
                                                        std::uint32_t segmentId) const;

    /// Copies a time+frequency range into a new standalone `.sweeps`.
    ///
    /// A tile copy plus a new index and manifest -- no re-encoding and no
    /// re-quantisation, so the extracted tiles are bit-identical to the
    /// source. That is what the 2D tiling exists for.
    [[nodiscard]] Status extract(const std::filesystem::path& destination,
                                 const HistoryQuery& range,
                                 const ExtractOptions& options = {}) const;

    /// Walks every record and verifies every checksum.
    ///
    /// The format has always supported this and nothing exposed it. Reports the
    /// number of records checked, or the first failure.
    [[nodiscard]] Result<std::uint64_t> verify() const;

    /// LOD level whose line spacing best fits `maxLines` over the range.
    ///
    /// Never returns a level with no tiles: short sessions never fill a coarse
    /// tile, and a truncated one loses the coarse levels first.
    [[nodiscard]] std::uint32_t chooseLod(std::uint64_t fromNs, std::uint64_t toNs,
                                          std::uint32_t maxLines, std::uint32_t segmentId) const;

    /// Whether any tile exists for this segment at this level.
    [[nodiscard]] bool hasTilesAtLod(std::uint32_t segmentId, std::uint32_t lod) const;

private:
    SessionReader() = default;

    /// Where a record's payload sits in the mapping.
    ///
    /// Kept alongside the decoded form for records an extraction copies rather
    /// than re-encodes. The scan verified the checksum before either was
    /// recorded, so these bounds describe bytes that were intact when read.
    struct RawPayload {
        std::size_t offset = 0;
        std::size_t bytes = 0;
    };

    [[nodiscard]] Status parse();
    [[nodiscard]] Status checkVersion(const FileHeader& header);
    [[nodiscard]] Status loadIndexAt(std::uint64_t offset);
    [[nodiscard]] Status scanRecords(bool rebuildIndex);
    [[nodiscard]] Result<HistoryTile> readTileAt(const IndexEntry& entry) const;

    [[nodiscard]] SegmentInfo* mutableSegment(std::uint32_t id) noexcept;

    [[nodiscard]] const std::byte* data() const noexcept { return m_file.data(); }
    [[nodiscard]] std::size_t size() const noexcept { return m_file.size(); }

    MappedFile m_file;
    Log m_log;

    std::filesystem::path m_path;
    SessionSummary m_summary;
    Metadata m_manifest;

    std::vector<SegmentInfo> m_segments;
    std::vector<SessionEvent> m_events;
    /// Parallel to m_events, entry for entry.
    std::vector<RawPayload> m_eventPayloads;
    std::vector<PluginRecord> m_pluginRecords;
    /// Parallel to m_pluginRecords, entry for entry.
    std::vector<RawPayload> m_pluginPayloads;
    std::map<TileKey, IndexEntry> m_index;
};

} // namespace sweeps
