// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include "sweeps/SessionReader.hpp"

#include "sweeps/Clock.hpp"
#include "sweeps/Config.hpp"
#include "sweeps/Metadata.hpp"
#include "sweeps/Records.hpp"
#include "sweeps/Text.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <utility>

namespace fs = std::filesystem;

namespace sweeps {

using detail::fail;

namespace {

constexpr const char* kCategory = "session";

template <typename... Args>
void logInfo(const Log& log, std::string_view fmt, Args&&... args) {
    if (log.enabled()) {
        log.emit(LogLevel::Info, kCategory, detail::format(fmt, std::forward<Args>(args)...));
    }
}

template <typename... Args>
void logWarn(const Log& log, std::string_view fmt, Args&&... args) {
    if (log.enabled()) {
        log.emit(LogLevel::Warn, kCategory, detail::format(fmt, std::forward<Args>(args)...));
    }
}

} // namespace

std::string SessionSummary::versionString() const {
    return detail::format("{}.{}", majorVersion, minorVersion);
}

Result<std::unique_ptr<SessionReader>> SessionReader::open(const fs::path& path, Log log) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return fail<std::unique_ptr<SessionReader>>(ErrorCode::NotFound, "no such session: {}",
                                                    path.string());
    }

    const auto size = static_cast<std::size_t>(fs::file_size(path, ec));
    if (ec || size < FileHeader::kBytes) {
        return fail<std::unique_ptr<SessionReader>>(
            ErrorCode::Corrupt, "{} is too small to be a session ({} bytes)", path.string(), size);
    }

    // mmap rather than read: scrollback into a multi-hour session must not load
    // the whole file, and a tile query should touch only the pages it needs.
    auto mapped = MappedFile::open(path);
    if (!mapped) {
        return unexpected<Error>(mapped.error());
    }

    auto reader = std::unique_ptr<SessionReader>(new SessionReader);
    reader->m_path = path;
    reader->m_log = std::move(log);
    reader->m_file = std::move(*mapped);

    if (auto parsed = reader->parse(); !parsed) {
        return unexpected<Error>(parsed.error());
    }

    reader->m_summary.fileBytes = reader->size();
    return Result<std::unique_ptr<SessionReader>>(std::move(reader));
}

SessionReader::~SessionReader() = default;

const SegmentInfo* SessionReader::segment(std::uint32_t id) const noexcept {
    for (const SegmentInfo& candidate : m_segments) {
        if (candidate.id == id) {
            return &candidate;
        }
    }
    return nullptr;
}

SegmentInfo* SessionReader::mutableSegment(std::uint32_t id) noexcept {
    for (SegmentInfo& candidate : m_segments) {
        if (candidate.id == id) {
            return &candidate;
        }
    }
    return nullptr;
}

Status SessionReader::checkVersion(const FileHeader& header) {
    // The decision procedure from §11.2 of the specification, in order. It
    // replaces a single `version > kFormatVersion` gate, which was
    // all-or-nothing: any additive change had to bump the version, and bumping
    // it made every existing reader refuse the file.

    // 1. Magic is checked by the caller, which has the bytes.

    // 2. A newer major version means the file's structure is not this one.
    if (header.majorVersion > kMajorVersion) {
        return fail(ErrorCode::Unsupported,
                    "{} was written by a newer major version of the .sweeps format "
                    "({} > {})",
                    m_path.string(), header.majorVersion, kMajorVersion);
    }

    // 3. A feature bit we do not know means we would misread the file rather
    //    than merely read less of it. Naming the bits is what lets someone work
    //    out what they are missing.
    if (const std::uint32_t unknown = header.incompatibleFeatures & ~kKnownFeatures; unknown != 0) {
        std::array<char, 16> bits{};
        std::snprintf(bits.data(), bits.size(), "0x%08X", static_cast<unsigned>(unknown));
        return fail(ErrorCode::Unsupported,
                    "{} requires .sweeps features this build does not implement "
                    "(unknown bits {})",
                    m_path.string(), bits.data());
    }

    // 4. A newer minor version is additive by definition. Read it, and say so.
    if (header.minorVersion > kMinorVersion) {
        m_summary.newerMinorVersion = true;
        logWarn(m_log,
                "{}: written by .sweeps {}.{}, newer than this reader's {}.{}; "
                "additive content will be skipped",
                m_path.filename().string(), header.majorVersion, header.minorVersion, kMajorVersion,
                kMinorVersion);
    }

    // 5. Otherwise, a full read.
    return ok();
}

Status SessionReader::parse() {
    ByteReader header(data(), size());

    std::array<char, 4> magic{};
    if (auto read = header.readBytes(magic.data(), magic.size()); !read) {
        return read;
    }
    if (magic != kMagic) {
        return fail(ErrorCode::Corrupt, "{} is not a Sweep++ session (bad magic)", m_path.string());
    }

    auto majorVersion = header.readU32();
    auto indexOffset = header.readU64();
    auto createdWallNs = header.readU64();
    auto minorVersion = header.readU32();
    auto incompatibleFeatures = header.readU32();
    if (!majorVersion || !indexOffset || !createdWallNs || !minorVersion || !incompatibleFeatures) {
        return fail(ErrorCode::Corrupt, "truncated session header");
    }

    FileHeader decoded;
    decoded.majorVersion = *majorVersion;
    decoded.indexOffset = *indexOffset;
    decoded.createdWallNs = *createdWallNs;
    decoded.minorVersion = *minorVersion;
    decoded.incompatibleFeatures = *incompatibleFeatures;

    m_summary.majorVersion = decoded.majorVersion;
    m_summary.minorVersion = decoded.minorVersion;
    m_summary.incompatibleFeatures = decoded.incompatibleFeatures;
    m_summary.createdWallNs = decoded.createdWallNs;

    if (auto supported = checkVersion(decoded); !supported) {
        return supported;
    }

    // Always scan for the manifest, segments and events -- they are small, and
    // the scan is what tells us whether the tail is intact.
    const bool haveIndex = decoded.indexOffset > 0 && decoded.indexOffset < size();
    if (auto scanned = scanRecords(!haveIndex); !scanned) {
        return scanned;
    }

    if (haveIndex) {
        if (auto loaded = loadIndexAt(decoded.indexOffset); !loaded) {
            // A damaged index is not a damaged file. Tiles are self-describing,
            // so falling back to the scan recovers everything that was written.
            logWarn(m_log, "{}: index unusable ({}); recovering by scan", m_path.string(),
                    loaded.error().message());
            m_index.clear();
            if (auto rescanned = scanRecords(true); !rescanned) {
                return rescanned;
            }
            m_summary.recoveredByScan = true;
        }
    } else {
        // No index means the session was not closed cleanly -- power loss, a
        // kill, a crash. This is a designed path, not an error.
        m_summary.recoveredByScan = true;
        logInfo(m_log, "{}: no index (session ended abruptly); recovered {} tiles by scan",
                m_path.filename().string(), m_index.size());
    }

    m_summary.totalTiles = m_index.size();
    for (const SegmentInfo& segment : m_segments) {
        m_summary.totalLines += segment.lineCount;
    }

    return ok();
}

Status SessionReader::scanRecords(bool rebuildIndex) {
    std::size_t offset = FileHeader::kBytes;
    m_segments.clear();
    m_events.clear();
    m_eventPayloads.clear();
    m_pluginRecords.clear();
    m_pluginPayloads.clear();

    m_summary.firstLineNs = std::numeric_limits<std::uint64_t>::max();
    m_summary.lastLineNs = 0;
    m_summary.lowestHz = std::numeric_limits<double>::max();
    m_summary.highestHz = std::numeric_limits<double>::lowest();

    while (offset + RecordHeader::kBytes <= size()) {
        ByteReader reader(data(), size());
        reader.seek(offset);

        auto type = reader.readU16();
        auto flags = reader.readU16();
        auto payloadBytes = reader.readU32();
        auto checksum = reader.readU32();
        if (!type || !flags || !payloadBytes || !checksum) {
            break;
        }

        const std::size_t payloadOffset = offset + RecordHeader::kBytes;
        if (payloadOffset + *payloadBytes > size()) {
            // A record whose payload runs past the end of the file: this is
            // where a power-loss truncation lands. Everything before it is
            // intact, which is the whole point of self-describing records.
            m_summary.truncatedBytes = size() - offset;
            logWarn(m_log, "{}: truncated {} bytes from the end; {} recovered",
                    m_path.filename().string(), m_summary.truncatedBytes, m_index.size());
            break;
        }

        // The checksum is what distinguishes a genuinely complete record from
        // a partially written one whose length field happens to be plausible.
        const std::uint32_t actual = crc32(data() + payloadOffset, *payloadBytes);
        if (actual != *checksum) {
            m_summary.truncatedBytes = size() - offset;
            logWarn(m_log, "{}: checksum mismatch at offset {}; stopping scan",
                    m_path.filename().string(), offset);
            break;
        }

        ByteReader payload(data() + payloadOffset, *payloadBytes);

        switch (static_cast<RecordType>(*type)) {
        case RecordType::Manifest: {
            if (auto decoded = Metadata::decode(payload)) {
                m_manifest = std::move(*decoded);
                m_summary.name = m_manifest.getString("name");
                m_summary.appVersion = m_manifest.getString("app_version");
            } else {
                // Said out loud rather than swallowed. The tiles in this file are
                // still readable -- the manifest is metadata, never structure --
                // so the honest outcome is a session with no name and a reason
                // for it, not a refusal and not a silent blank.
                logWarn(m_log,
                        "{}: the manifest record is unreadable ({}); this session's "
                        "metadata will be missing",
                        m_path.filename().string(), decoded.error().message());
            }
            break;
        }

        case RecordType::SegmentOpen: {
            // An unreadable config keeps the segment: its tiles are still
            // readable at the grid it declares.
            auto decoded = decodeSegmentOpen(payload, false);
            if (!decoded) {
                break;
            }
            SegmentInfo segment = std::move(*decoded);

            m_summary.lowestHz = std::min(m_summary.lowestHz, segment.grid.startHz);
            m_summary.highestHz = std::max(m_summary.highestHz, segment.grid.stopHz());
            m_segments.push_back(std::move(segment));
            break;
        }

        case RecordType::SegmentClose: {
            if (auto close = decodeSegmentClose(payload)) {
                // By id, not by position. An extracted file contains segments
                // whose ids do not start at zero, and indexing by position
                // there attributes one segment's extent to another.
                if (SegmentInfo* segment = mutableSegment(close->id)) {
                    segment->endMonotonicNs = close->endMonotonicNs;
                    segment->lineCount = close->lineCount;
                }
            }
            break;
        }

        case RecordType::Event: {
            auto event = decodeEvent(payload);
            if (!event) {
                logWarn(m_log, "{}: skipping an unreadable event at offset {} ({})",
                        m_path.filename().string(), offset, event.error().message());
                break;
            }
            m_events.push_back(std::move(*event));
            // The bytes as well as the decoding. An extraction copies the
            // payload rather than re-encoding it, which is what makes an event
            // kind this build does not know survive the copy intact.
            m_eventPayloads.push_back(RawPayload{payloadOffset, *payloadBytes});
            break;
        }

        case RecordType::Tile: {
            if (auto tile = decodeTileHeader(payload)) {
                const TileKey key =
                    TileKey::of(tile->segmentId, tile->lod, tile->timeBlock, tile->freqBlock);
                if (rebuildIndex) {
                    IndexEntry entry;
                    entry.key = key;
                    entry.offset = offset;
                    entry.length = static_cast<std::uint32_t>(RecordHeader::kBytes + *payloadBytes);
                    entry.firstLineNs = tile->firstLineNs;
                    entry.lastLineNs = tile->lastLineNs;
                    m_index[key] = entry;
                }
                if (tile->lod == 0) {
                    m_summary.firstLineNs = std::min(m_summary.firstLineNs, tile->firstLineNs);
                    m_summary.lastLineNs = std::max(m_summary.lastLineNs, tile->lastLineNs);
                    if (SegmentInfo* segment = mutableSegment(tile->segmentId);
                        segment != nullptr && segment->endMonotonicNs == 0) {
                        // A segment never formally closed (abrupt end): infer
                        // its line count from the tiles that survived.
                        segment->lineCount =
                            std::max(segment->lineCount,
                                     (static_cast<std::uint64_t>(tile->timeBlock) * kTileLines) +
                                         tile->lines);
                    }
                }
            }
            break;
        }

        case RecordType::PluginData: {
            auto decoded = decodePluginData(payload);
            if (!decoded) {
                logWarn(m_log, "{}: skipping a plugin record at offset {} ({})",
                        m_path.filename().string(), offset, decoded.error().message());
                break;
            }

            PluginRecord record;
            record.pluginId = std::move(decoded->pluginId);
            record.recordName = std::move(decoded->recordName);
            record.schemaVersion = decoded->schemaVersion;
            record.monotonicNs = decoded->monotonicNs;
            record.body = decoded->body;
            record.bodyBytes = decoded->bodyBytes;

            m_pluginRecords.push_back(std::move(record));
            m_pluginPayloads.push_back(RawPayload{payloadOffset, *payloadBytes});
            break;
        }

        case RecordType::Index:
        case RecordType::EndOfStream:
        case RecordType::Telemetry:
            break;

        default:
            // An unrecognised record type is skipped, not treated as damage.
            // Every record carries its length precisely so that a reader can
            // walk past content a newer minor version introduced.
            break;
        }

        offset = payloadOffset + *payloadBytes;
    }

    if (m_summary.firstLineNs == std::numeric_limits<std::uint64_t>::max()) {
        m_summary.firstLineNs = 0;
    }
    if (m_segments.empty()) {
        m_summary.lowestHz = 0.0;
        m_summary.highestHz = 0.0;
    }

    return ok();
}

Status SessionReader::loadIndexAt(std::uint64_t offset) {
    ByteReader reader(data(), size());
    reader.seek(static_cast<std::size_t>(offset));

    auto type = reader.readU16();
    auto flags = reader.readU16();
    auto payloadBytes = reader.readU32();
    auto checksum = reader.readU32();
    if (!type || !flags || !payloadBytes || !checksum) {
        return fail(ErrorCode::Corrupt, "unreadable index header");
    }
    if (static_cast<RecordType>(*type) != RecordType::Index) {
        return fail(ErrorCode::Corrupt, "record at the index offset is a {}, not an index",
                    toString(static_cast<RecordType>(*type)));
    }

    const std::size_t payloadOffset = static_cast<std::size_t>(offset) + RecordHeader::kBytes;
    if (payloadOffset + *payloadBytes > size()) {
        return fail(ErrorCode::Corrupt, "index payload runs past the end of the file");
    }
    if (crc32(data() + payloadOffset, *payloadBytes) != *checksum) {
        return fail(ErrorCode::Corrupt, "index checksum mismatch");
    }

    ByteReader payload(data() + payloadOffset, *payloadBytes);

    auto segmentCount = payload.readU32();
    if (!segmentCount) {
        return fail(ErrorCode::Corrupt, "unreadable index segment count");
    }
    for (std::uint32_t i = 0; i < *segmentCount; ++i) {
        auto id = payload.readU32();
        auto lines = payload.readU64();
        auto startNs = payload.readU64();
        auto endNs = payload.readU64();
        if (!id || !lines || !startNs || !endNs) {
            return fail(ErrorCode::Corrupt, "unreadable index segment entry");
        }
        // An extract's index describes every segment of its *source*, including
        // ones it did not copy. Entries with no matching SegmentOpen are
        // tolerated rather than treated as corruption.
        if (SegmentInfo* segment = mutableSegment(*id)) {
            segment->lineCount = *lines;
            segment->endMonotonicNs = *endNs;
        }
    }

    auto tileCount = payload.readU32();
    if (!tileCount) {
        return fail(ErrorCode::Corrupt, "unreadable index tile count");
    }

    m_index.clear();
    for (std::uint32_t i = 0; i < *tileCount; ++i) {
        auto segmentId = payload.readU32();
        auto lod = payload.readU32();
        auto timeBlock = payload.readU32();
        auto freqBlock = payload.readU32();
        auto tileOffset = payload.readU64();
        auto length = payload.readU32();
        auto firstNs = payload.readU64();
        auto lastNs = payload.readU64();

        if (!segmentId || !lod || !timeBlock || !freqBlock || !tileOffset || !length || !firstNs ||
            !lastNs) {
            return fail(ErrorCode::Corrupt, "unreadable index tile entry {}", i);
        }
        if (*tileOffset + *length > size()) {
            return fail(ErrorCode::Corrupt, "index entry {} points past the end of the file", i);
        }

        const TileKey key = TileKey::of(*segmentId, *lod, *timeBlock, *freqBlock);
        IndexEntry entry;
        entry.key = key;
        entry.offset = *tileOffset;
        entry.length = *length;
        entry.firstLineNs = *firstNs;
        entry.lastLineNs = *lastNs;
        m_index[key] = entry;
    }

    return ok();
}

Result<HistoryTile> SessionReader::readTileAt(const IndexEntry& entry) const {
    // Verify the record before trusting a byte of it. When tiles are reached
    // through the index the scan's checks were never applied to them, so
    // without this a corrupt tile would be handed back as data and drawn as if
    // it were a measurement.
    if (entry.offset + RecordHeader::kBytes > size()) {
        return fail<HistoryTile>(ErrorCode::Corrupt, "tile offset {} is past the end of the file",
                                 entry.offset);
    }

    ByteReader recordHeader(data(), size());
    recordHeader.seek(static_cast<std::size_t>(entry.offset));
    auto type = recordHeader.readU16();
    auto flags = recordHeader.readU16();
    auto payloadBytes = recordHeader.readU32();
    auto checksum = recordHeader.readU32();
    if (!type || !flags || !payloadBytes || !checksum) {
        return fail<HistoryTile>(ErrorCode::Corrupt, "unreadable tile record header");
    }
    if (static_cast<RecordType>(*type) != RecordType::Tile) {
        return fail<HistoryTile>(ErrorCode::Corrupt, "record at {} is a {}, not a tile",
                                 entry.offset, toString(static_cast<RecordType>(*type)));
    }

    const std::size_t payloadOffset = static_cast<std::size_t>(entry.offset) + RecordHeader::kBytes;
    if (payloadOffset + *payloadBytes > size()) {
        return fail<HistoryTile>(ErrorCode::Corrupt, "tile payload runs past the end of the file");
    }
    if (crc32(data() + payloadOffset, *payloadBytes) != *checksum) {
        return fail<HistoryTile>(ErrorCode::Corrupt, "tile checksum mismatch at offset {}",
                                 entry.offset);
    }

    ByteReader reader(data() + payloadOffset, *payloadBytes);
    auto decoded = decodeTile(reader);
    if (!decoded) {
        return fail<HistoryTile>(ErrorCode::Corrupt, "tile at {}: {}", entry.offset,
                                 decoded.error().message());
    }

    HistoryTile tile;
    tile.segmentId = decoded->header.segmentId;
    tile.lod = decoded->header.lod;
    tile.timeBlock = decoded->header.timeBlock;
    tile.freqBlock = decoded->header.freqBlock;
    tile.lines = decoded->header.lines;
    tile.bins = decoded->header.bins;
    tile.originDb = decoded->header.originDb;
    tile.firstLineNs = decoded->header.firstLineNs;
    tile.lastLineNs = decoded->header.lastLineNs;
    tile.data = std::move(decoded->data);

    // The tile's frequency extent comes from its segment's grid, looked up by
    // id. Using the segment's position here is how an extract that excludes
    // segment 0 ends up drawing every tile at another segment's frequencies.
    if (const SegmentInfo* owner = segment(tile.segmentId)) {
        const SegmentGrid& grid = owner->grid;
        tile.binWidthHz = grid.binWidthHz;
        tile.startHz =
            grid.startHz + (grid.binWidthHz * static_cast<double>(tile.freqBlock * kTileBins));
    }

    return tile;
}

std::uint32_t SessionReader::chooseLod(std::uint64_t fromNs, std::uint64_t toNs,
                                       std::uint32_t maxLines, std::uint32_t segmentId) const {
    const SegmentInfo* info = segment(segmentId);
    if (maxLines == 0 || info == nullptr) {
        return 0;
    }

    const std::uint64_t segmentSpanNs =
        (info->endMonotonicNs > info->startMonotonicNs ? info->endMonotonicNs
                                                       : m_summary.lastLineNs) -
        info->startMonotonicNs;
    if (segmentSpanNs == 0 || info->lineCount == 0) {
        return 0;
    }

    const double lineIntervalNs =
        static_cast<double>(segmentSpanNs) / static_cast<double>(info->lineCount);
    const auto rangeNs = static_cast<double>(toNs > fromNs ? toNs - fromNs : segmentSpanNs);
    const double linesAtNative = rangeNs / std::max(lineIntervalNs, 1.0);

    // Coarsest level that still fills the request. Without this, drawing three
    // hours of waterfall would mean reading three hours of lines.
    std::uint32_t ideal = kLodLevels - 1;
    for (std::uint32_t lod = 0; lod < kLodLevels; ++lod) {
        if (linesAtNative / static_cast<double>(lodDecimation(lod)) <=
            static_cast<double>(maxLines)) {
            ideal = lod;
            break;
        }
    }

    // A level only exists if tiles were actually written for it. Short
    // sessions never fill a coarse tile, and a session truncated by power loss
    // loses the coarse levels first because they are flushed last. Returning a
    // level with no tiles would answer a perfectly good query with nothing.
    if (hasTilesAtLod(segmentId, ideal)) {
        return ideal;
    }
    for (std::uint32_t lod = ideal; lod-- > 0;) {
        if (hasTilesAtLod(segmentId, lod)) {
            return lod;
        }
    }
    for (std::uint32_t lod = ideal + 1; lod < kLodLevels; ++lod) {
        if (hasTilesAtLod(segmentId, lod)) {
            return lod;
        }
    }
    return 0;
}

bool SessionReader::hasTilesAtLod(std::uint32_t segmentId, std::uint32_t lod) const {
    const TileKey lower = TileKey::of(segmentId, lod, 0, 0);
    const auto it = m_index.lower_bound(lower);
    return it != m_index.end() && it->first.segmentId == segmentId && it->first.lod == lod;
}

Result<std::vector<HistoryTile>> SessionReader::query(const HistoryQuery& request) const {
    std::vector<HistoryTile> tiles;

    for (const SegmentInfo& segmentInfo : m_segments) {
        if (request.segmentId && *request.segmentId != segmentInfo.id) {
            continue;
        }

        // Frequency overlap. A query spanning a mid-session change returns
        // segment-tagged tiles from both sides -- a renderer already does
        // per-pixel decimation, so resampling them onto one display grid is
        // the path it takes anyway.
        if (segmentInfo.grid.stopHz() < request.fromHz || segmentInfo.grid.startHz > request.toHz) {
            continue;
        }

        const std::uint32_t lod =
            request.lod ? *request.lod
                        : chooseLod(request.fromNs, request.toNs, request.maxLines, segmentInfo.id);

        for (const auto& indexed : m_index) {
            const TileKey& key = indexed.first;
            const IndexEntry& entry = indexed.second;
            if (key.segmentId != segmentInfo.id || key.lod != lod) {
                continue;
            }
            if (entry.lastLineNs < request.fromNs || entry.firstLineNs > request.toNs) {
                continue;
            }

            const double tileStartHz =
                segmentInfo.grid.startHz +
                segmentInfo.grid.binWidthHz * static_cast<double>(key.freqBlock * kTileBins);
            const double tileStopHz =
                tileStartHz + segmentInfo.grid.binWidthHz * static_cast<double>(kTileBins);
            if (tileStopHz < request.fromHz || tileStartHz > request.toHz) {
                continue;
            }

            auto tile = readTileAt(entry);
            if (!tile) {
                logWarn(m_log, "skipping unreadable tile: {}", tile.error().message());
                continue;
            }
            tiles.push_back(std::move(*tile));
        }
    }

    // Time order, then frequency: the natural order for a renderer walking a
    // waterfall top to bottom.
    std::sort(tiles.begin(), tiles.end(), [](const HistoryTile& a, const HistoryTile& b) {
        if (a.segmentId != b.segmentId) {
            return a.segmentId < b.segmentId;
        }
        if (a.firstLineNs != b.firstLineNs) {
            return a.firstLineNs < b.firstLineNs;
        }
        return a.startHz < b.startHz;
    });

    return tiles;
}

std::vector<SessionEvent> SessionReader::eventsInRange(std::uint64_t fromNs,
                                                       std::uint64_t toNs) const {
    std::vector<SessionEvent> result;
    for (const SessionEvent& event : m_events) {
        if (event.monotonicNs >= fromNs && event.monotonicNs <= toNs) {
            result.push_back(event);
        }
    }
    return result;
}

Result<std::vector<float>> SessionReader::spectrumAt(std::uint64_t monotonicNs,
                                                     std::uint32_t segmentId) const {
    const SegmentInfo* info = segment(segmentId);
    if (info == nullptr) {
        return fail<std::vector<float>>(ErrorCode::NotFound, "no segment {}", segmentId);
    }

    std::vector<float> spectrum(info->grid.binCount, -std::numeric_limits<float>::infinity());
    bool found = false;

    // Native LOD only: clicking a waterfall line should show that instant, not
    // a max-hold of the surrounding window.
    for (const auto& indexed : m_index) {
        const TileKey& key = indexed.first;
        const IndexEntry& entry = indexed.second;
        if (key.segmentId != segmentId || key.lod != 0) {
            continue;
        }
        if (monotonicNs < entry.firstLineNs || monotonicNs > entry.lastLineNs) {
            continue;
        }

        auto tile = readTileAt(entry);
        if (!tile || tile->lines == 0) {
            continue;
        }

        // Nearest line by interpolation across the tile's own time extent.
        const std::uint64_t span =
            tile->lastLineNs > tile->firstLineNs ? tile->lastLineNs - tile->firstLineNs : 1;
        const double position =
            static_cast<double>(monotonicNs - tile->firstLineNs) / static_cast<double>(span);
        const auto line =
            std::min(static_cast<std::uint32_t>(position * static_cast<double>(tile->lines)),
                     tile->lines - 1);

        const std::uint32_t firstBin = key.freqBlock * kTileBins;
        for (std::uint32_t i = 0; i < tile->bins && firstBin + i < spectrum.size(); ++i) {
            spectrum[firstBin + i] = static_cast<float>(tile->dbAt(line, i));
        }
        found = true;
    }

    if (!found) {
        return fail<std::vector<float>>(ErrorCode::NotFound, "no data at {} in segment {}",
                                        monotonicNs, segmentId);
    }
    return spectrum;
}

Result<std::uint64_t> SessionReader::verify() const {
    // Walk every record and check every payload against its checksum. The
    // format has always made this possible -- each record carries its own
    // length and CRC -- and nothing exposed it until now.
    std::uint64_t records = 0;
    std::size_t offset = FileHeader::kBytes;

    while (offset + RecordHeader::kBytes <= size()) {
        ByteReader reader(data(), size());
        reader.seek(offset);

        auto type = reader.readU16();
        auto flags = reader.readU16();
        auto payloadBytes = reader.readU32();
        auto checksum = reader.readU32();
        if (!type || !flags || !payloadBytes || !checksum) {
            return fail<std::uint64_t>(ErrorCode::Corrupt, "unreadable record header at offset {}",
                                       offset);
        }

        const std::size_t payloadOffset = offset + RecordHeader::kBytes;
        if (payloadOffset + *payloadBytes > size()) {
            return fail<std::uint64_t>(ErrorCode::Corrupt,
                                       "record {} at offset {} runs {} bytes past the end of "
                                       "the file",
                                       records, offset, payloadOffset + *payloadBytes - size());
        }
        if (crc32(data() + payloadOffset, *payloadBytes) != *checksum) {
            return fail<std::uint64_t>(ErrorCode::Corrupt,
                                       "record {} ({}) at offset {} fails its checksum", records,
                                       toString(static_cast<RecordType>(*type)), offset);
        }

        ++records;
        offset = payloadOffset + *payloadBytes;
    }

    if (offset != size()) {
        return fail<std::uint64_t>(ErrorCode::Corrupt,
                                   "{} trailing bytes after the last complete record",
                                   size() - offset);
    }

    return records;
}

Status SessionReader::extract(const fs::path& destination, const HistoryQuery& range,
                              const ExtractOptions& options) const {
    // Tile copy, not re-encode. The extracted tiles are bit-identical to the
    // source, which is exactly what blocking tiles in frequency as well as
    // time buys.
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out) {
        return fail(ErrorCode::IoError, "could not open {} for writing", destination.string());
    }

    const std::uint64_t createdWallNs =
        options.createdWallNs != 0 ? options.createdWallNs : wallClockNs();
    const std::string appVersion = options.applicationVersion.empty()
                                       ? std::string(libraryVersion())
                                       : options.applicationVersion;

    std::uint64_t written = 0;
    const auto emit = [&](RecordType type, const std::vector<std::byte>& payload) {
        std::vector<std::byte> header;
        writeU16(header, static_cast<std::uint16_t>(type));
        writeU16(header, 0);
        writeU32(header, static_cast<std::uint32_t>(payload.size()));
        writeU32(header, crc32(payload.data(), payload.size()));
        out.write(reinterpret_cast<const char*>(header.data()),
                  static_cast<std::streamsize>(header.size()));
        out.write(reinterpret_cast<const char*>(payload.data()),
                  static_cast<std::streamsize>(payload.size()));
        written += header.size() + payload.size();
    };

    std::vector<std::byte> fileHeader;
    writeBytes(fileHeader, kMagic.data(), kMagic.size());
    writeU32(fileHeader, kMajorVersion);
    writeU64(fileHeader, 0);
    writeU64(fileHeader, createdWallNs);
    writeU32(fileHeader, kMinorVersion);
    writeU32(fileHeader, 0); // incompatibleFeatures: none are defined at 1.0
    fileHeader.resize(FileHeader::kBytes, std::byte{0});
    out.write(reinterpret_cast<const char*>(fileHeader.data()),
              static_cast<std::streamsize>(fileHeader.size()));
    written = fileHeader.size();

    Metadata manifest;
    manifest.setString("name", detail::format("{}-extract", m_summary.name));
    manifest.setString("created", formatWallClockIso8601(createdWallNs));
    manifest.setString("app_version", appVersion);
    manifest.setInt("format_version", static_cast<std::int64_t>(kMajorVersion));
    manifest.setString("extracted_from", m_path.filename().string());
    manifest.setInt("extract_from_ns", static_cast<std::int64_t>(range.fromNs));
    manifest.setInt("extract_to_ns", static_cast<std::int64_t>(range.toNs));
    manifest.setFloat("extract_from_hz", range.fromHz);
    manifest.setFloat("extract_to_hz", range.toHz);
    manifest.setInt("tile_bins", static_cast<std::int64_t>(kTileBins));
    manifest.setInt("tile_lines", static_cast<std::int64_t>(kTileLines));
    manifest.setFloat("db_per_step", kDbPerStep);

    std::vector<std::byte> manifestPayload;
    manifest.encode(manifestPayload);
    emit(RecordType::Manifest, manifestPayload);

    std::vector<IndexEntry> newIndex;
    std::map<std::uint32_t, std::uint64_t> linesPerSegment;
    std::size_t tilesSkipped = 0;

    for (const SegmentInfo& segmentInfo : m_segments) {
        if (segmentInfo.grid.stopHz() < range.fromHz || segmentInfo.grid.startHz > range.toHz) {
            continue;
        }
        if (segmentInfo.endMonotonicNs != 0 && segmentInfo.endMonotonicNs < range.fromNs) {
            continue;
        }
        if (segmentInfo.startMonotonicNs > range.toNs) {
            continue;
        }

        // The original id is preserved rather than renumbered. Renumbering
        // would break every tile record's segmentId, which is copied verbatim.
        // The config is re-encoded in full so the extract stands alone.
        std::vector<std::byte> segmentPayload;
        encodeSegmentOpen(segmentPayload, segmentInfo);

        emit(RecordType::SegmentOpen, segmentPayload);

        std::uint64_t linesCopied = 0;

        for (const auto& indexed : m_index) {
            const TileKey& key = indexed.first;
            const IndexEntry& entry = indexed.second;
            if (key.segmentId != segmentInfo.id) {
                continue;
            }
            if (entry.lastLineNs < range.fromNs || entry.firstLineNs > range.toNs) {
                ++tilesSkipped;
                continue;
            }

            const double tileStartHz =
                segmentInfo.grid.startHz +
                segmentInfo.grid.binWidthHz * static_cast<double>(key.freqBlock * kTileBins);
            const double tileStopHz =
                tileStartHz + segmentInfo.grid.binWidthHz * static_cast<double>(kTileBins);
            if (tileStopHz < range.fromHz || tileStartHz > range.toHz) {
                ++tilesSkipped;
                continue;
            }

            // The payload is copied byte-for-byte out of the source mapping.
            // No decode, no re-quantise: the output tiles are identical to the
            // input ones.
            const std::size_t payloadOffset =
                static_cast<std::size_t>(entry.offset) + RecordHeader::kBytes;
            const std::size_t payloadBytes = entry.length - RecordHeader::kBytes;
            if (payloadOffset + payloadBytes > size()) {
                continue;
            }

            std::vector<std::byte> payload(data() + payloadOffset,
                                           data() + payloadOffset + payloadBytes);

            const std::uint64_t offset = written;
            emit(RecordType::Tile, payload);

            IndexEntry copied;
            copied.key = key;
            copied.offset = offset;
            copied.length = entry.length;
            copied.firstLineNs = entry.firstLineNs;
            copied.lastLineNs = entry.lastLineNs;
            newIndex.push_back(copied);

            if (key.lod == 0) {
                // The tile header's own line count, not a whole block: a
                // partial tile at the end of a segment must not inflate the
                // extract's reported length.
                ByteReader tileHeader(data() + payloadOffset, payloadBytes);
                tileHeader.seek(16); // past segmentId, lod, timeBlock, freqBlock
                if (auto lines = tileHeader.readU32()) {
                    linesCopied += *lines;
                }
            }
        }

        // Frequency blocks multiply the per-LOD-0 count, so divide back out to
        // get lines rather than tile-rows.
        const std::uint32_t freqBlocks =
            std::max<std::uint32_t>(1, (segmentInfo.grid.binCount + kTileBins - 1) / kTileBins);
        linesCopied /= freqBlocks;
        linesPerSegment[segmentInfo.id] = linesCopied;

        std::vector<std::byte> closePayload;
        writeU32(closePayload, segmentInfo.id);
        writeU64(closePayload, std::min(range.toNs, m_summary.lastLineNs));
        writeU64(closePayload, linesCopied);
        emit(RecordType::SegmentClose, closePayload);
    }

    // Event payloads are copied byte for byte out of the source mapping, the way
    // tiles are. Re-encoding them field by field would drop whatever this build
    // does not understand -- an event kind added by a newer minor version, a
    // field appended to a body it does know -- and it is also how the decoded
    // `segmentId` is preserved rather than restamped, for free.
    for (std::size_t i = 0; i < m_events.size() && i < m_eventPayloads.size(); ++i) {
        const SessionEvent& event = m_events[i];
        if (event.monotonicNs < range.fromNs || event.monotonicNs > range.toNs) {
            continue;
        }
        const RawPayload& raw = m_eventPayloads[i];
        if (raw.offset + raw.bytes > size()) {
            continue;
        }
        emit(RecordType::Event,
             std::vector<std::byte>(data() + raw.offset, data() + raw.offset + raw.bytes));
    }

    for (std::size_t i = 0; i < m_pluginRecords.size() && i < m_pluginPayloads.size(); ++i) {
        const PluginRecord& record = m_pluginRecords[i];
        // A record with no timestamp is not tied to a moment, so no time range
        // can exclude it: dropping it would lose a plugin's session-wide state
        // every time anything was cut out of the file.
        const bool inRange = record.monotonicNs == 0 || (record.monotonicNs >= range.fromNs &&
                                                         record.monotonicNs <= range.toNs);
        if (!inRange) {
            continue;
        }
        const RawPayload& raw = m_pluginPayloads[i];
        if (raw.offset + raw.bytes > size()) {
            continue;
        }
        emit(RecordType::PluginData,
             std::vector<std::byte>(data() + raw.offset, data() + raw.offset + raw.bytes));
    }

    std::vector<std::byte> indexPayload;
    writeU32(indexPayload, static_cast<std::uint32_t>(m_segments.size()));
    for (const SegmentInfo& segmentInfo : m_segments) {
        // The extract's own line count, not the source's -- otherwise a
        // 30-second cut out of a three-hour session would claim to be three
        // hours long.
        const auto copied = linesPerSegment.find(segmentInfo.id);
        writeU32(indexPayload, segmentInfo.id);
        writeU64(indexPayload, copied != linesPerSegment.end() ? copied->second : 0);
        writeU64(indexPayload, segmentInfo.startMonotonicNs);
        writeU64(indexPayload, segmentInfo.endMonotonicNs);
    }
    writeU32(indexPayload, static_cast<std::uint32_t>(newIndex.size()));
    for (const IndexEntry& entry : newIndex) {
        writeU32(indexPayload, entry.key.segmentId);
        writeU32(indexPayload, entry.key.lod);
        writeU32(indexPayload, entry.key.timeBlock);
        writeU32(indexPayload, entry.key.freqBlock);
        writeU64(indexPayload, entry.offset);
        writeU32(indexPayload, entry.length);
        writeU64(indexPayload, entry.firstLineNs);
        writeU64(indexPayload, entry.lastLineNs);
    }

    const std::uint64_t indexOffset = written;
    emit(RecordType::Index, indexPayload);
    emit(RecordType::EndOfStream, {});

    out.flush();
    out.seekp(8);
    std::vector<std::byte> offsetBytes;
    writeU64(offsetBytes, indexOffset);
    out.write(reinterpret_cast<const char*>(offsetBytes.data()),
              static_cast<std::streamsize>(offsetBytes.size()));
    out.flush();

    if (!out) {
        return fail(ErrorCode::IoError, "write failed while extracting to {}",
                    destination.string());
    }

    // Extraction granularity is the tile, so a range that clips a tile keeps
    // the whole tile. Saying so keeps the operator from reading the output as
    // an exact cut -- and never silently truncating is the same principle.
    logInfo(m_log, "extracted {} tiles to {} ({} outside the range were skipped)", newIndex.size(),
            destination.filename().string(), tilesSkipped);
    return ok();
}

} // namespace sweeps
