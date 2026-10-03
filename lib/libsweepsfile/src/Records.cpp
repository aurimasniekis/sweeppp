// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include "sweeps/Records.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace sweeps {

using detail::fail;

RecordHeader makeRecordHeader(std::uint16_t type, const std::byte* payload,
                              std::size_t bytes) noexcept {
    RecordHeader header;
    header.type = type;
    header.flags = 0;
    header.payloadBytes = static_cast<std::uint32_t>(bytes);
    header.checksum = crc32(payload, bytes);
    return header;
}

void encodeRecordHeader(std::vector<std::byte>& out, const RecordHeader& header) {
    writeU16(out, header.type);
    writeU16(out, header.flags);
    writeU32(out, header.payloadBytes);
    writeU32(out, header.checksum);
}

Result<RecordHeader> decodeRecordHeader(ByteReader& in) {
    auto type = in.readU16();
    auto flags = in.readU16();
    auto payloadBytes = in.readU32();
    auto checksum = in.readU32();
    if (!type || !flags || !payloadBytes || !checksum) {
        return fail<RecordHeader>(ErrorCode::Corrupt, "truncated record header");
    }
    RecordHeader header;
    header.type = *type;
    header.flags = *flags;
    header.payloadBytes = *payloadBytes;
    header.checksum = *checksum;
    return header;
}

void appendRecord(std::vector<std::byte>& out, std::uint16_t type, const std::byte* payload,
                  std::size_t bytes) {
    encodeRecordHeader(out, makeRecordHeader(type, payload, bytes));
    if (bytes > 0) {
        writeBytes(out, payload, bytes);
    }
}

void encodeAcquisitionConfig(std::vector<std::byte>& out, const AcquisitionConfig& config) {
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

Result<AcquisitionConfig> decodeAcquisitionConfig(ByteReader& in) {
    AcquisitionConfig config;

    auto centerHz = in.readF64();
    auto spanHz = in.readF64();
    auto sampleRate = in.readF64();
    auto fftSize = in.readU32();
    auto window = in.readU32();
    auto beta = in.readF64();
    auto enbw = in.readF64();
    auto overlap = in.readF64();
    auto rbw = in.readF64();
    auto referenceLevel = in.readF64();
    auto offset = in.readF64();
    auto deviceId = in.readString();
    auto deviceLabel = in.readString();
    auto gainCount = in.readU32();

    if (!centerHz || !spanHz || !sampleRate || !fftSize || !window || !beta || !enbw || !overlap ||
        !rbw || !referenceLevel || !offset || !deviceId || !deviceLabel || !gainCount) {
        return fail<AcquisitionConfig>(ErrorCode::Corrupt, "malformed acquisition config");
    }

    config.centerHz = *centerHz;
    config.spanHz = *spanHz;
    config.sampleRate = *sampleRate;
    config.fftSize = *fftSize;
    config.window = static_cast<WindowType>(*window);
    config.windowBeta = *beta;
    config.windowEnbw = *enbw;
    config.overlap = *overlap;
    config.rbwHz = *rbw;
    config.referenceLevelDbm = *referenceLevel;
    config.dbfsToDbmOffset = *offset;
    config.deviceId = std::move(*deviceId);
    config.deviceLabel = std::move(*deviceLabel);

    // Each entry is at least a 4-byte length and an 8-byte value, so a count
    // the remaining bytes cannot hold is refused before anything is reserved.
    constexpr std::size_t kMinGainBytes = 12;
    if (*gainCount > in.remaining() / kMinGainBytes) {
        return fail<AcquisitionConfig>(ErrorCode::Corrupt, "{} gains cannot fit in {} bytes",
                                       *gainCount, in.remaining());
    }
    for (std::uint32_t i = 0; i < *gainCount; ++i) {
        auto key = in.readString();
        auto value = in.readF64();
        if (!key || !value) {
            return fail<AcquisitionConfig>(ErrorCode::Corrupt, "malformed gain entry");
        }
        config.gains.emplace_back(std::move(*key), *value);
    }

    // Deliberately no check that the payload is exhausted. A newer minor
    // version may have appended fields here, and ignoring a tail is exactly
    // what makes such a bump invisible to this reader rather than fatal.
    return config;
}

void encodeSegmentOpen(std::vector<std::byte>& out, const SegmentInfo& segment) {
    writeU32(out, segment.id);
    writeF64(out, segment.grid.startHz);
    writeF64(out, segment.grid.binWidthHz);
    writeU32(out, segment.grid.binCount);
    writeU64(out, segment.startWallNs);
    writeU64(out, segment.startMonotonicNs);
    writeString(out, segment.reason);
    encodeAcquisitionConfig(out, segment.config);
}

Result<SegmentInfo> decodeSegmentOpen(ByteReader& in, bool requireConfig) {
    auto id = in.readU32();
    auto startHz = in.readF64();
    auto binWidth = in.readF64();
    auto binCount = in.readU32();
    auto startWall = in.readU64();
    auto startMono = in.readU64();
    auto reason = in.readString();
    if (!id || !startHz || !binWidth || !binCount || !startWall || !startMono || !reason) {
        return fail<SegmentInfo>(ErrorCode::Corrupt, "malformed segment header");
    }

    SegmentInfo segment;
    segment.id = *id;
    segment.grid.startHz = *startHz;
    segment.grid.binWidthHz = *binWidth;
    segment.grid.binCount = *binCount;
    segment.startWallNs = *startWall;
    segment.startMonotonicNs = *startMono;
    segment.reason = std::move(*reason);

    auto config = decodeAcquisitionConfig(in);
    if (config) {
        segment.config = std::move(*config);
    } else if (requireConfig) {
        return unexpected<Error>(config.error());
    }
    return segment;
}

void encodeSegmentClose(std::vector<std::byte>& out, const SegmentClose& close) {
    writeU32(out, close.id);
    writeU64(out, close.endMonotonicNs);
    writeU64(out, close.lineCount);
}

Result<SegmentClose> decodeSegmentClose(ByteReader& in) {
    auto id = in.readU32();
    auto endNs = in.readU64();
    auto lines = in.readU64();
    if (!id || !endNs || !lines) {
        return fail<SegmentClose>(ErrorCode::Corrupt, "malformed segment close");
    }
    SegmentClose close;
    close.id = *id;
    close.endMonotonicNs = *endNs;
    close.lineCount = *lines;
    return close;
}

float chooseTileOrigin(const float* values, std::size_t count) noexcept {
    // Over the measurements only. A bin the sweep never reached carries the
    // unmeasured sentinel, and letting that set the floor would choose the
    // origin from a level nothing recorded.
    float peak = -std::numeric_limits<float>::infinity();
    float floor = std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < count; ++i) {
        const float value = values[i];
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
    return span <= kQuantSpanDb - 8.0
               ? static_cast<float>(static_cast<double>(peak) + 8.0 - kQuantSpanDb)
               : peak - static_cast<float>(kQuantSpanDb) + 8.0F;
}

void quantiseTile(const float* values, std::size_t count, float originDb,
                  std::uint8_t* out) noexcept {
    // Both kinds of non-measurement land on the same byte: a bin no step
    // reached carries the sentinel, and one no source bin resampled onto is
    // still at -inf.
    const auto origin = static_cast<double>(originDb);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = quantiseDb(static_cast<double>(values[i]), origin);
    }
}

void encodeTile(std::vector<std::byte>& out, const TileHeader& header, const std::uint8_t* data,
                std::size_t bytes) {
    out.reserve(out.size() + TileHeader::kBytes + bytes);
    writeU32(out, header.segmentId);
    writeU32(out, header.lod);
    writeU32(out, header.timeBlock);
    writeU32(out, header.freqBlock);
    writeU32(out, header.lines);
    writeU32(out, header.bins);
    writeF32(out, header.originDb);
    writeU64(out, header.firstLineNs);
    writeU64(out, header.lastLineNs);
    if (bytes > 0) {
        writeBytes(out, data, bytes);
    }
}

Result<TileHeader> decodeTileHeader(ByteReader& in) {
    auto segmentId = in.readU32();
    auto lod = in.readU32();
    auto timeBlock = in.readU32();
    auto freqBlock = in.readU32();
    auto lines = in.readU32();
    auto bins = in.readU32();
    auto originDb = in.readF32();
    auto firstNs = in.readU64();
    auto lastNs = in.readU64();
    if (!segmentId || !lod || !timeBlock || !freqBlock || !lines || !bins || !originDb ||
        !firstNs || !lastNs) {
        return fail<TileHeader>(ErrorCode::Corrupt, "unreadable tile header");
    }

    TileHeader header;
    header.segmentId = *segmentId;
    header.lod = *lod;
    header.timeBlock = *timeBlock;
    header.freqBlock = *freqBlock;
    header.lines = *lines;
    header.bins = *bins;
    header.originDb = *originDb;
    header.firstLineNs = *firstNs;
    header.lastLineNs = *lastNs;
    return header;
}

Result<TileRecord> decodeTile(ByteReader& in) {
    auto header = decodeTileHeader(in);
    if (!header) {
        return unexpected<Error>(header.error());
    }

    // Computed in 64 bits and checked before it becomes an allocation. Both
    // operands are attacker-controlled u32s whose product does not fit in 32.
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(header->lines) * static_cast<std::uint64_t>(header->bins);
    if (bytes > static_cast<std::uint64_t>(in.remaining())) {
        return fail<TileRecord>(ErrorCode::Corrupt,
                                "tile claims {} bytes of data but only {} remain", bytes,
                                in.remaining());
    }

    TileRecord tile;
    tile.header = *header;
    tile.data.resize(static_cast<std::size_t>(bytes));
    if (auto read = in.readBytes(tile.data.data(), tile.data.size()); !read) {
        return unexpected<Error>(read.error());
    }
    return tile;
}

Status encodePluginData(std::vector<std::byte>& out, std::string_view pluginId,
                        std::string_view recordName, std::uint32_t schemaVersion,
                        std::uint64_t monotonicNs, const void* body, std::size_t bodyBytes) {
    if (pluginId.empty() || pluginId.size() > kMaxPluginIdBytes) {
        return fail(ErrorCode::InvalidArgument,
                    "a pluginId must be 1..{} bytes of reverse-DNS, not {}", kMaxPluginIdBytes,
                    pluginId.size());
    }
    if (bodyBytes > std::numeric_limits<std::uint32_t>::max()) {
        return fail(ErrorCode::InvalidArgument, "a plugin record body of {} bytes is too large",
                    bodyBytes);
    }

    writeString(out, pluginId);
    writeString(out, recordName);
    writeU32(out, schemaVersion);
    writeU64(out, monotonicNs);
    // Explicitly length-prefixed rather than "the rest of the payload", so that
    // appending a field here stays invisible to an older reader -- the same
    // guarantee §11.5 gives every other record.
    writeU32(out, static_cast<std::uint32_t>(bodyBytes));
    if (bodyBytes > 0 && body != nullptr) {
        writeBytes(out, body, bodyBytes);
    }
    return ok();
}

Result<PluginDataView> decodePluginData(ByteReader& in) {
    auto pluginId = in.readString();
    auto recordName = in.readString();
    auto schemaVersion = in.readU32();
    auto monotonicNs = in.readU64();
    auto bodyBytes = in.readU32();
    if (!pluginId || !recordName || !schemaVersion || !monotonicNs || !bodyBytes) {
        return fail<PluginDataView>(ErrorCode::Corrupt, "unreadable plugin record");
    }
    if (*bodyBytes > in.remaining()) {
        return fail<PluginDataView>(ErrorCode::Corrupt,
                                    "plugin record claims {} bytes of body but only {} remain",
                                    *bodyBytes, in.remaining());
    }

    PluginDataView view;
    view.pluginId = std::move(*pluginId);
    view.recordName = std::move(*recordName);
    view.schemaVersion = *schemaVersion;
    view.monotonicNs = *monotonicNs;
    view.body = in.position();
    view.bodyBytes = *bodyBytes;
    in.seek(in.offset() + *bodyBytes);
    return view;
}

} // namespace sweeps
