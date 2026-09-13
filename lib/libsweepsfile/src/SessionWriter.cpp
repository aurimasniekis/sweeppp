// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include "sweeps/SessionWriter.hpp"

#include "sweeps/Clock.hpp"
#include "sweeps/Config.hpp"
#include "sweeps/Metadata.hpp"
#include "sweeps/Text.hpp"
#include "sweeps/WindowType.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
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

template <typename... Args>
void logError(const Log& log, std::string_view fmt, Args&&... args) {
    if (log.enabled()) {
        log.emit(LogLevel::Error, kCategory, detail::format(fmt, std::forward<Args>(args)...));
    }
}

/// Encodes an acquisition config into a record payload.
void encodeConfig(std::vector<std::byte>& out, const AcquisitionConfig& config) {
    writeF64(out, config.centerHz);
    writeF64(out, config.spanHz);
    writeF64(out, config.sampleRate);
    writeU32(out, config.fftSize);
    writeU32(out, static_cast<std::uint32_t>(config.window));
    writeF64(out, config.windowBeta);
    writeF64(out, config.windowEnbw);
    writeF64(out, config.overlap);
    writeF64(out, config.rbwHz);
    writeF64(out, config.referenceLevelDbm);
    writeF64(out, config.dbfsToDbmOffset);
    writeString(out, config.deviceId);
    writeString(out, config.deviceLabel);

    writeU32(out, static_cast<std::uint32_t>(config.gains.size()));
    for (const auto& gain : config.gains) {
        writeString(out, gain.first);
        writeF64(out, gain.second);
    }
}

} // namespace

Result<std::unique_ptr<SessionWriter>> SessionWriter::create(const fs::path& path,
                                                             WriterConfig config) {
    if (config.binsPerLine == 0) {
        return fail<std::unique_ptr<SessionWriter>>(ErrorCode::InvalidArgument,
                                                    "binsPerLine must be non-zero");
    }

    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
    }

    // Buffer attached before the file is opened -- pubsetbuf is only honoured
    // on a closed filebuf, so doing this after open() silently keeps the
    // default 4 KiB.
    constexpr std::size_t kStreamBufferBytes = 1024 * 1024;
    auto streamBuffer = std::unique_ptr<char[]>(new char[kStreamBufferBytes]);

    std::ofstream stream;
    stream.rdbuf()->pubsetbuf(streamBuffer.get(), kStreamBufferBytes);
    stream.open(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return fail<std::unique_ptr<SessionWriter>>(ErrorCode::IoError,
                                                    "could not open {} for writing", path.string());
    }

    auto writer = std::unique_ptr<SessionWriter>(
        new SessionWriter(path, std::move(streamBuffer), std::move(stream), std::move(config)));

    if (auto written = writer->writeManifest(); !written) {
        return unexpected<Error>(written.error());
    }

    return Result<std::unique_ptr<SessionWriter>>(std::move(writer));
}

SessionWriter::SessionWriter(fs::path path, std::unique_ptr<char[]> streamBuffer,
                             std::ofstream stream, WriterConfig config)
    : m_path(std::move(path)), m_streamBuffer(std::move(streamBuffer)), m_stream(std::move(stream)),
      m_config(std::move(config)) {
    m_startWallNs = m_config.createdWallNs != 0 ? m_config.createdWallNs : wallClockNs();
    m_startMonotonicNs = monotonicNs();
}

SessionWriter::~SessionWriter() {
    if (!m_closed) {
        (void)close();
    }
}

Status SessionWriter::writeRecord(RecordType type, const std::vector<std::byte>& payload) {
    RecordHeader header;
    header.type = static_cast<std::uint16_t>(type);
    header.flags = 0;
    header.payloadBytes = static_cast<std::uint32_t>(payload.size());
    header.checksum = crc32(payload.data(), payload.size());

    std::vector<std::byte> encoded;
    encoded.reserve(RecordHeader::kBytes);
    writeU16(encoded, header.type);
    writeU16(encoded, header.flags);
    writeU32(encoded, header.payloadBytes);
    writeU32(encoded, header.checksum);

    m_stream.write(reinterpret_cast<const char*>(encoded.data()),
                   static_cast<std::streamsize>(encoded.size()));
    m_stream.write(reinterpret_cast<const char*>(payload.data()),
                   static_cast<std::streamsize>(payload.size()));

    if (!m_stream) {
        return fail(ErrorCode::IoError, "write failed at {} bytes", m_bytesWritten);
    }

    m_bytesWritten += encoded.size() + payload.size();
    return ok();
}

Status SessionWriter::writeManifest() {
    // Fixed-size file header first. indexOffset stays 0 until close() patches
    // it -- a file that never gets patched is not broken, it is simply one
    // that must be recovered by scan, which is a designed path.
    std::vector<std::byte> header;
    writeBytes(header, kMagic.data(), kMagic.size());
    writeU32(header, kMajorVersion);
    writeU64(header, 0);
    writeU64(header, m_startWallNs);
    // Bytes 24-31, written from named constants rather than left to resize()'s
    // zero fill. Both happen to be zero at 1.0, so the bytes are the same either
    // way -- but a writer that states them is one that keeps stating them
    // correctly once they are not.
    writeU32(header, kMinorVersion);
    writeU32(header, 0); // incompatibleFeatures: none are defined at 1.0
    header.resize(FileHeader::kBytes, std::byte{0});

    m_stream.write(reinterpret_cast<const char*>(header.data()),
                   static_cast<std::streamsize>(header.size()));
    if (!m_stream) {
        return fail(ErrorCode::IoError, "could not write the session header");
    }
    m_bytesWritten = header.size();
    m_headerWritten = true;

    // Typed metadata, encoded directly as the record's payload. `created` stays
    // an RFC 3339 string rather than an epoch integer because a human running
    // `strings` on a session should still be able to see when it was recorded.
    Metadata manifest;
    manifest.setString("name",
                       m_config.sessionName.empty()
                           ? detail::format("session-{}", formatWallClockCompact(m_startWallNs))
                           : m_config.sessionName);
    manifest.setString("created", formatWallClockIso8601(m_startWallNs));
    manifest.setString("app_version", m_config.applicationVersion.empty()
                                          ? std::string(libraryVersion())
                                          : m_config.applicationVersion);
    manifest.setInt("format_version", static_cast<std::int64_t>(kMajorVersion));
    manifest.setInt("bins_per_line", static_cast<std::int64_t>(m_config.binsPerLine));
    manifest.setInt("tile_bins", static_cast<std::int64_t>(kTileBins));
    manifest.setInt("tile_lines", static_cast<std::int64_t>(kTileLines));
    manifest.setInt("lod_levels", static_cast<std::int64_t>(kLodLevels));
    manifest.setFloat("db_per_step", kDbPerStep);
    if (!m_config.notes.empty()) {
        manifest.setString("notes", m_config.notes);
    }

    std::vector<std::byte> payload;
    manifest.encode(payload);
    return writeRecord(RecordType::Manifest, payload);
}

Status SessionWriter::openSegment(const AcquisitionConfig& config, const SegmentGrid& grid,
                                  std::uint64_t monotonicNs, std::uint64_t wallNs,
                                  std::string reason) {
    SegmentState segment;
    segment.info.id = static_cast<std::uint32_t>(m_segments.size());
    segment.info.grid = grid;
    // The opening frame's wall time, not "now". A segment's extent is defined
    // by its data -- the same rule closeSegment already follows -- and reading
    // the clock here would make the recorded start depend on writer scheduling.
    segment.info.startWallNs = wallNs != 0 ? wallNs : m_startWallNs;
    segment.info.startMonotonicNs = monotonicNs;
    segment.info.config = config;
    segment.info.reason = std::move(reason);

    // One accumulator per LOD. Level 0 passes lines through; higher levels
    // hold a max across their decimation window.
    segment.lods.resize(kLodLevels);
    for (LodAccumulator& lod : segment.lods) {
        lod.maxHold.assign(grid.binCount, -std::numeric_limits<float>::infinity());
        lod.sourceLines = 0;
        lod.lineIndex = 0;
    }

    std::vector<std::byte> payload;
    writeU32(payload, segment.info.id);
    writeF64(payload, grid.startHz);
    writeF64(payload, grid.binWidthHz);
    writeU32(payload, grid.binCount);
    writeU64(payload, segment.info.startWallNs);
    writeU64(payload, segment.info.startMonotonicNs);
    writeString(payload, segment.info.reason);
    encodeConfig(payload, config);

    if (auto written = writeRecord(RecordType::SegmentOpen, payload); !written) {
        return written;
    }

    logInfo(m_config.log, "segment {} opened: {} bins of {} from {} ({})", segment.info.id,
            grid.binCount, formatFrequencyShort(grid.binWidthHz),
            formatFrequencyShort(grid.startHz), segment.info.reason);

    m_segments.push_back(std::move(segment));
    return ok();
}

Status SessionWriter::closeSegment(std::uint64_t monotonicNs) {
    if (m_segments.empty()) {
        return ok();
    }

    SegmentState& segment = m_segments.back();
    if (segment.info.endMonotonicNs != 0) {
        return ok();
    }

    if (auto flushed = flushSegmentTiles(segment); !flushed) {
        return flushed;
    }
    segment.info.endMonotonicNs = monotonicNs;

    std::vector<std::byte> payload;
    writeU32(payload, segment.info.id);
    writeU64(payload, monotonicNs);
    writeU64(payload, segment.info.lineCount);
    return writeRecord(RecordType::SegmentClose, payload);
}

void SessionWriter::resampleToGrid(const FrameView& frame, const SegmentGrid& grid,
                                   std::vector<float>& out) const {
    out.assign(grid.binCount, -std::numeric_limits<float>::infinity());
    if (frame.count == 0 || frame.bins == nullptr || grid.binWidthHz <= 0.0) {
        return;
    }

    // Max within each destination bin, not mean. A narrow carrier falling
    // between stored bins must survive the resample; averaging would dilute it
    // into the noise floor, and finding narrow signals is the entire point.
    for (std::size_t i = 0; i < frame.count; ++i) {
        const double hz = frame.startHz + frame.binWidthHz * (static_cast<double>(i) + 0.5);
        const double position = (hz - grid.startHz) / grid.binWidthHz;
        if (position < 0.0) {
            continue;
        }
        const auto destination = static_cast<std::size_t>(position);
        if (destination >= out.size()) {
            continue;
        }
        out[destination] = std::max(out[destination], frame.bins[i]);
    }

    // A destination bin no source bin reached keeps the -inf it started with
    // and is stored as unmeasured. Carrying the previous bin's value into it
    // instead -- which this used to do -- writes a flat line across the gap
    // between two spans and bakes it into the file, where no reader can tell
    // it from measurement.
}

Result<FrameOutcome> SessionWriter::writeFrame(const FrameView& frame) {
    FrameOutcome outcome;

    if (m_closed || frame.config == nullptr) {
        return outcome;
    }
    if (m_retentionReached || retentionExceeded()) {
        outcome.retentionStopped = true;
        return outcome;
    }

    // The stored grid: display resolution, capped at the frame's own bin count
    // so a narrow span is never upsampled into false detail.
    const double frameStopHz = frame.startHz + frame.binWidthHz * static_cast<double>(frame.count);

    SegmentGrid grid;
    grid.startHz = frame.startHz;
    grid.binCount =
        std::min<std::uint32_t>(m_config.binsPerLine, static_cast<std::uint32_t>(frame.count));
    // Spelled as (stop - start) / binCount rather than the algebraically equal
    // (binWidth * count) / binCount. The two differ in the last bits once
    // startHz is large, and this value determines every stored bin's frequency.
    grid.binWidthHz = grid.binCount > 0
                          ? (frameStopHz - frame.startHz) / static_cast<double>(grid.binCount)
                          : 0.0;

    // A parameter change closes the current segment and opens a new one. It
    // never mutates or reinterprets what is already written, so tiles recorded
    // under any past configuration stay readable at their original resolution
    // and extent forever.
    const bool needNewSegment =
        m_segments.empty() || m_segments.back().info.config.gridDiffers(*frame.config) ||
        std::abs(m_segments.back().info.grid.startHz - grid.startHz) > 1e-3 ||
        m_segments.back().info.grid.binCount != grid.binCount;

    if (needNewSegment) {
        std::string reason = "session start";
        if (!m_segments.empty()) {
            const AcquisitionConfig& previous = m_segments.back().info.config;
            if (previous.fftSize != frame.config->fftSize) {
                reason =
                    detail::format("FFT size {} -> {}", previous.fftSize, frame.config->fftSize);
            } else if (std::abs(previous.sampleRate - frame.config->sampleRate) > 1.0) {
                reason = detail::format("sample rate {} -> {}",
                                        formatFrequencyShort(previous.sampleRate),
                                        formatFrequencyShort(frame.config->sampleRate));
            } else if (previous.window != frame.config->window) {
                reason = detail::format("window {} -> {}", toString(previous.window),
                                        toString(frame.config->window));
            } else {
                reason = detail::format("frequency range -> {} .. {}",
                                        formatFrequencyShort(grid.startHz),
                                        formatFrequencyShort(grid.stopHz()));
            }

            if (auto closed = closeSegment(frame.monotonicNs); !closed) {
                logError(m_config.log, "{}", closed.error().describe());
                return unexpected<Error>(closed.error());
            }
        }

        if (auto opened = openSegment(*frame.config, grid, frame.monotonicNs, frame.wallNs, reason);
            !opened) {
            logError(m_config.log, "{}", opened.error().describe());
            return unexpected<Error>(opened.error());
        }

        outcome.segmentOpened = true;
        outcome.segmentId = m_segments.back().info.id;
        outcome.reason = m_segments.back().info.reason;
    }

    SegmentState& segment = m_segments.back();
    resampleToGrid(frame, segment.info.grid, m_resampleScratch);

    appendLine(segment, 0, m_resampleScratch, frame.monotonicNs);

    ++segment.info.lineCount;
    ++m_linesWritten;

    // A segment's extent is defined by its data, not by when the writer
    // happened to close. Using the wall clock here would make a segment whose
    // last frame arrived ten seconds ago claim to span those ten seconds, and
    // every LOD and seek calculation derived from the span would be wrong.
    m_lastFrameNs = frame.monotonicNs;

    return outcome;
}

void SessionWriter::appendLine(SegmentState& segment, std::uint32_t lod,
                               const std::vector<float>& bins, std::uint64_t monotonicNs) {
    if (lod >= segment.lods.size()) {
        return;
    }

    LodAccumulator& accumulator = segment.lods[lod];
    const std::uint32_t binCount = segment.info.grid.binCount;

    // Every level stores this line; higher levels also feed the next one up.
    const std::uint64_t lineIndex = accumulator.lineIndex++;
    const std::uint32_t timeBlock = static_cast<std::uint32_t>(lineIndex / kTileLines);
    const std::uint32_t lineInTile = static_cast<std::uint32_t>(lineIndex % kTileLines);

    for (std::uint32_t freqBlock = 0; freqBlock * kTileBins < binCount; ++freqBlock) {
        const std::uint32_t firstBin = freqBlock * kTileBins;
        const std::uint32_t tileBins = std::min(kTileBins, binCount - firstBin);

        const TileKey key = TileKey::of(segment.info.id, lod, timeBlock, freqBlock);

        auto emplaced = segment.pending.try_emplace(key);
        PendingTile& tile = emplaced.first->second;

        if (emplaced.second) {
            tile.header.segmentId = segment.info.id;
            tile.header.lod = lod;
            tile.header.timeBlock = timeBlock;
            tile.header.freqBlock = freqBlock;
            tile.header.lines = 0;
            tile.header.bins = tileBins;
            tile.header.originDb = 0.0F;
            tile.header.firstLineNs = monotonicNs;
            tile.header.lastLineNs = monotonicNs;
            tile.values.assign(static_cast<std::size_t>(kTileLines) * tileBins,
                               -std::numeric_limits<float>::infinity());
        }

        float* row = tile.values.data() + static_cast<std::size_t>(lineInTile) * tileBins;
        std::copy_n(bins.data() + firstBin, tileBins, row);

        tile.header.lastLineNs = monotonicNs;
        tile.linesFilled = lineInTile + 1;
        tile.header.lines = tile.linesFilled;

        if (tile.linesFilled >= kTileLines) {
            if (auto flushed = flushTile(key, tile); !flushed) {
                logError(m_config.log, "{}", flushed.error().describe());
            }
            segment.pending.erase(emplaced.first);
        }
    }

    // Feed the next LOD level.
    //
    // Decimate with MAX-HOLD, never mean. A 200 ms burst must still be visible
    // at the /64 level; averaging is precisely what would erase it, and finding
    // transients is the whole reason scrollback exists.
    if (lod + 1 < segment.lods.size()) {
        LodAccumulator& next = segment.lods[lod + 1];
        if (next.sourceLines == 0) {
            next.maxHold.assign(binCount, -std::numeric_limits<float>::infinity());
            next.firstNs = monotonicNs;
        }
        for (std::uint32_t i = 0; i < binCount; ++i) {
            next.maxHold[i] = std::max(next.maxHold[i], bins[i]);
        }
        next.lastNs = monotonicNs;
        ++next.sourceLines;

        // Each level is 8x coarser than the one below.
        constexpr std::uint32_t kDecimation = 8;
        if (next.sourceLines >= kDecimation) {
            appendLine(segment, lod + 1, next.maxHold, next.lastNs);
            next.sourceLines = 0;
        }
    }
}

Status SessionWriter::flushTile(const TileKey& key, PendingTile& tile) {
    const std::size_t used = static_cast<std::size_t>(tile.header.lines) * tile.header.bins;

    // Choose the origin now, from the tile's true range. 255 steps of 0.5 dB
    // span 127.5 dB; anchoring 8 dB above the peak leaves headroom without
    // wasting the scale, and puts the floor of a typical 70 dB-range tile
    // comfortably inside the window.
    // Over the measurements only. A bin the sweep never reached carries the
    // unmeasured sentinel, and letting that set the floor would choose the
    // origin from a level nothing recorded.
    float peak = -std::numeric_limits<float>::infinity();
    float floor = std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < used; ++i) {
        const float value = tile.values[i];
        if (std::isfinite(value) && isMeasuredDb(static_cast<double>(value))) {
            peak = std::max(peak, value);
            floor = std::min(floor, value);
        }
    }
    if (!std::isfinite(peak)) {
        peak = 0.0F;
        floor = 0.0F;
    }

    // Prefer covering the whole range; fall back to anchoring on the peak when
    // the range exceeds what 255 steps can express, since losing the top of
    // the scale matters far more than losing the bottom of the noise floor.
    //
    // The two branches are NOT the same expression written twice: the first
    // computes in double and the second in float, and they can differ in the
    // last bit of a value that is stored as F32. Collapsing them changes output
    // bytes, so they stay as they are.
    const auto span = static_cast<double>(peak - floor);
    tile.header.originDb = span <= kQuantSpanDb - 8.0
                               ? static_cast<float>(static_cast<double>(peak) + 8.0 - kQuantSpanDb)
                               : peak - static_cast<float>(kQuantSpanDb) + 8.0F;

    // Both kinds of non-measurement land on the same byte: a bin no step
    // reached carries the sentinel, and one no source bin resampled onto is
    // still at -inf.
    std::vector<std::uint8_t> quantised(used);
    const auto originDb = static_cast<double>(tile.header.originDb);
    for (std::size_t i = 0; i < used; ++i) {
        quantised[i] = quantiseDb(static_cast<double>(tile.values[i]), originDb);
    }

    std::vector<std::byte> payload;
    payload.reserve(TileHeader::kBytes + quantised.size());

    writeU32(payload, tile.header.segmentId);
    writeU32(payload, tile.header.lod);
    writeU32(payload, tile.header.timeBlock);
    writeU32(payload, tile.header.freqBlock);
    writeU32(payload, tile.header.lines);
    writeU32(payload, tile.header.bins);
    writeF32(payload, tile.header.originDb);
    writeU64(payload, tile.header.firstLineNs);
    writeU64(payload, tile.header.lastLineNs);

    // Only the filled rows: a partial tile at the end of a segment must not
    // pad the session with a block of fabricated silence.
    writeBytes(payload, quantised.data(), quantised.size());

    const std::uint64_t offset = m_bytesWritten;
    if (auto written = writeRecord(RecordType::Tile, payload); !written) {
        return written;
    }

    IndexEntry entry;
    entry.key = key;
    entry.offset = offset;
    entry.length = static_cast<std::uint32_t>(RecordHeader::kBytes + payload.size());
    entry.firstLineNs = tile.header.firstLineNs;
    entry.lastLineNs = tile.header.lastLineNs;
    m_index.push_back(entry);
    return ok();
}

Status SessionWriter::flushSegmentTiles(SegmentState& segment) {
    // Flush the deepest LOD first so a partially accumulated coarse line still
    // makes it into the file rather than being lost at close.
    for (std::uint32_t lod = 0; lod + 1 < segment.lods.size(); ++lod) {
        LodAccumulator& next = segment.lods[lod + 1];
        if (next.sourceLines > 0) {
            appendLine(segment, lod + 1, next.maxHold, next.lastNs);
            next.sourceLines = 0;
        }
    }

    // Iterating the map is what puts tiles in the file in ascending TileKey
    // order. TileKey::operator< therefore determines output byte order.
    for (auto& pending : segment.pending) {
        if (pending.second.linesFilled > 0) {
            if (auto flushed = flushTile(pending.first, pending.second); !flushed) {
                return flushed;
            }
        }
    }
    segment.pending.clear();
    return ok();
}

Status SessionWriter::writeIndex() {
    std::vector<std::byte> payload;
    writeU32(payload, static_cast<std::uint32_t>(m_segments.size()));
    for (const SegmentState& segment : m_segments) {
        writeU32(payload, segment.info.id);
        writeU64(payload, segment.info.lineCount);
        writeU64(payload, segment.info.startMonotonicNs);
        writeU64(payload, segment.info.endMonotonicNs);
    }

    writeU32(payload, static_cast<std::uint32_t>(m_index.size()));
    for (const IndexEntry& entry : m_index) {
        writeU32(payload, entry.key.segmentId);
        writeU32(payload, entry.key.lod);
        writeU32(payload, entry.key.timeBlock);
        writeU32(payload, entry.key.freqBlock);
        writeU64(payload, entry.offset);
        writeU32(payload, entry.length);
        writeU64(payload, entry.firstLineNs);
        writeU64(payload, entry.lastLineNs);
    }

    const std::uint64_t indexOffset = m_bytesWritten;
    if (auto written = writeRecord(RecordType::Index, payload); !written) {
        return written;
    }

    if (auto written = writeRecord(RecordType::EndOfStream, {}); !written) {
        return written;
    }

    // Patch the header's index offset last. Until this instant the file reads
    // as "not cleanly closed", which is exactly right if we die before it.
    m_stream.flush();
    m_stream.seekp(8);
    std::vector<std::byte> offsetBytes;
    writeU64(offsetBytes, indexOffset);
    m_stream.write(reinterpret_cast<const char*>(offsetBytes.data()),
                   static_cast<std::streamsize>(offsetBytes.size()));
    m_stream.flush();

    return m_stream ? ok() : fail(ErrorCode::IoError, "could not patch the session header");
}

Status SessionWriter::close() {
    if (m_closed) {
        return ok();
    }

    // The last frame's timestamp, not "now" -- see writeFrame.
    if (auto closed = closeSegment(m_lastFrameNs != 0 ? m_lastFrameNs : monotonicNs()); !closed) {
        m_closed = true;
        return closed;
    }

    const Status written = writeIndex();
    m_stream.close();
    m_closed = true;

    logInfo(m_config.log, "closed {}: {} lines, {} segments, {} tiles, {}",
            m_path.filename().string(), m_linesWritten, m_segments.size(), m_index.size(),
            formatBytes(m_bytesWritten));

    return written;
}

Status SessionWriter::recordEvent(const SessionEvent& event) {
    SessionEvent stamped = event;
    // The segment open right now, not the one the caller named. A live event is
    // stamped where it actually landed; only an extraction preserves a decoded
    // value.
    stamped.segmentId = m_segments.empty() ? 0 : m_segments.back().info.id;

    std::vector<std::byte> payload;
    encodeEvent(payload, stamped);
    return writeRecord(RecordType::Event, payload);
}

Status SessionWriter::writePluginData(std::string_view pluginId, std::string_view recordName,
                                      std::uint32_t schemaVersion, std::uint64_t monotonicNs,
                                      const void* body, std::size_t bodyBytes) {
    if (pluginId.empty() || pluginId.size() > kMaxPluginIdBytes) {
        return fail(ErrorCode::InvalidArgument,
                    "a pluginId must be 1..{} bytes of reverse-DNS, not {}", kMaxPluginIdBytes,
                    pluginId.size());
    }
    if (bodyBytes > std::numeric_limits<std::uint32_t>::max()) {
        return fail(ErrorCode::InvalidArgument, "a plugin record body of {} bytes is too large",
                    bodyBytes);
    }

    std::vector<std::byte> payload;
    writeString(payload, pluginId);
    writeString(payload, recordName);
    writeU32(payload, schemaVersion);
    writeU64(payload, monotonicNs);
    // Explicitly length-prefixed rather than "the rest of the payload", so that
    // appending a field here stays invisible to an older reader -- the same
    // guarantee §11.5 gives every other record.
    writeU32(payload, static_cast<std::uint32_t>(bodyBytes));
    if (bodyBytes > 0 && body != nullptr) {
        writeBytes(payload, body, bodyBytes);
    }

    return writeRecord(RecordType::PluginData, payload);
}

Status SessionWriter::writePluginEvent(std::string_view pluginId, std::string_view eventName,
                                       std::uint64_t monotonicNs, std::uint64_t wallNs,
                                       const Metadata& fields) {
    if (pluginId.empty() || pluginId.size() > kMaxPluginIdBytes) {
        return fail(ErrorCode::InvalidArgument,
                    "a pluginId must be 1..{} bytes of reverse-DNS, not {}", kMaxPluginIdBytes,
                    pluginId.size());
    }

    PluginEventData body;
    body.pluginId = std::string(pluginId);
    body.eventName = std::string(eventName);
    body.fields = fields;

    return recordEvent(
        SessionEvent::of(SessionEvent::Kind::Plugin, monotonicNs, wallNs, std::move(body)));
}

bool SessionWriter::retentionExceeded() {
    if (m_retentionReached) {
        return true;
    }

    // Tiles buffered in memory count against the cap. They will be flushed at
    // close whatever happens, so ignoring them would let a file finish well
    // past its stated limit -- with 2048-bin lines that overshoot is over half
    // a megabyte, which makes a small cap meaningless.
    std::uint64_t pendingBytes = 0;
    for (const SegmentState& segment : m_segments) {
        for (const auto& pending : segment.pending) {
            pendingBytes += static_cast<std::uint64_t>(pending.second.header.lines) *
                            pending.second.header.bins;
        }
    }

    if (m_config.maxBytes > 0 && m_bytesWritten + pendingBytes >= m_config.maxBytes) {
        m_retentionReason =
            detail::format("size cap of {} reached", formatBytes(m_config.maxBytes));
    } else if (m_config.maxSeconds > 0.0 &&
               nsToSeconds(monotonicNs() - m_startMonotonicNs) >= m_config.maxSeconds) {
        m_retentionReason =
            detail::format("time cap of {} reached", formatDuration(m_config.maxSeconds));
    } else if (m_config.minFreeBytes > 0) {
        // Checked rarely: statvfs on every frame would be a syscall per frame
        // for a condition that changes on the scale of minutes.
        if ((m_linesWritten % 512) == 0) {
            std::error_code ec;
            const fs::space_info space = fs::space(
                m_path.parent_path().empty() ? fs::current_path() : m_path.parent_path(), ec);
            if (!ec && space.available < m_config.minFreeBytes) {
                m_retentionReason =
                    detail::format("free space below {}", formatBytes(m_config.minFreeBytes));
            }
        }
    }

    if (!m_retentionReason.empty()) {
        m_retentionReached = true;
        logWarn(m_config.log, "stopped writing {}: {}", m_path.filename().string(),
                m_retentionReason);
        return true;
    }
    return false;
}

} // namespace sweeps
