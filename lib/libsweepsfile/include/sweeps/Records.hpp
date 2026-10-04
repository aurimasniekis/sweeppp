// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#pragma once

#include "sweeps/AcquisitionConfig.hpp"
#include "sweeps/FileFormat.hpp"
#include "sweeps/Result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/// Record payloads, one encoder and one decoder each.
///
/// The same records are written to a file and sent down a live stream, so
/// these are shared by the writer, the reader and anything that streams: one
/// definition of each payload's byte order rather than one per caller.
namespace sweeps {

// ---------------------------------------------------------------------------
// Record framing (§3.2)
// ---------------------------------------------------------------------------

/// The 12-byte header for `payload`, its checksum computed.
[[nodiscard]] RecordHeader makeRecordHeader(std::uint16_t type, const std::byte* payload,
                                            std::size_t bytes) noexcept;

void encodeRecordHeader(std::vector<std::byte>& out, const RecordHeader& header);
[[nodiscard]] Result<RecordHeader> decodeRecordHeader(ByteReader& in);

/// A whole record -- header, then payload -- appended to `out`.
void appendRecord(std::vector<std::byte>& out, std::uint16_t type, const std::byte* payload,
                  std::size_t bytes);

// ---------------------------------------------------------------------------
// SegmentOpen and SegmentClose (§4.3, §4.4)
// ---------------------------------------------------------------------------

void encodeAcquisitionConfig(std::vector<std::byte>& out, const AcquisitionConfig& config);

/// Tolerates a tail after the gains, which is where a newer minor version
/// appends (§11.5).
[[nodiscard]] Result<AcquisitionConfig> decodeAcquisitionConfig(ByteReader& in);

/// Identity, grid, start times and reason, then the acquisition config.
/// `endMonotonicNs` and `lineCount` belong to SegmentClose and are not written.
void encodeSegmentOpen(std::vector<std::byte>& out, const SegmentInfo& segment);

/// `requireConfig` false keeps a segment whose config is unreadable, with a
/// default one in its place: a file's tiles stay readable even then. A live
/// stream asks for the config too, since nothing can be drawn without it.
[[nodiscard]] Result<SegmentInfo> decodeSegmentOpen(ByteReader& in, bool requireConfig = true);

struct SegmentClose {
    std::uint32_t id = 0;
    std::uint64_t endMonotonicNs = 0;
    std::uint64_t lineCount = 0;
};

void encodeSegmentClose(std::vector<std::byte>& out, const SegmentClose& close);
[[nodiscard]] Result<SegmentClose> decodeSegmentClose(ByteReader& in);

// ---------------------------------------------------------------------------
// Tile (§4.6, §7)
// ---------------------------------------------------------------------------

/// The reference writer's origin rule (§7.4), over the measured values only.
///
/// Both branches are kept exactly as the writer has always computed them:
/// they differ in the last bit of the stored f32, and changing either changes
/// output bytes.
[[nodiscard]] float chooseTileOrigin(const float* values, std::size_t count) noexcept;

/// Quantises `count` levels against `originDb` (§7.3). `out` holds `count`.
void quantiseTile(const float* values, std::size_t count, float originDb,
                  std::uint8_t* out) noexcept;

/// `bytes` must be `header.lines * header.bins`.
void encodeTile(std::vector<std::byte>& out, const TileHeader& header, const std::uint8_t* data,
                std::size_t bytes);

/// The 44-byte header alone, for a caller that does not want the data copied.
[[nodiscard]] Result<TileHeader> decodeTileHeader(ByteReader& in);

struct TileRecord {
    TileHeader header;
    std::vector<std::uint8_t> data;
};

/// Header and data. `lines * bins` is checked against what remains before
/// anything is allocated (§12.2).
[[nodiscard]] Result<TileRecord> decodeTile(ByteReader& in);

// ---------------------------------------------------------------------------
// PluginData (§4.10)
// ---------------------------------------------------------------------------

/// Refuses a pluginId that is empty or longer than `kMaxPluginIdBytes`, and a
/// body too large for its u32 length.
[[nodiscard]] Status encodePluginData(std::vector<std::byte>& out, std::string_view pluginId,
                                      std::string_view recordName, std::uint32_t schemaVersion,
                                      std::uint64_t monotonicNs, const void* body,
                                      std::size_t bodyBytes);

/// A decoded PluginData record. `body` points into the buffer the reader was
/// over and is valid only for as long as that is.
struct PluginDataView {
    std::string pluginId;
    std::string recordName;
    std::uint32_t schemaVersion = 0;
    std::uint64_t monotonicNs = 0;
    const std::byte* body = nullptr;
    std::uint32_t bodyBytes = 0;
};

/// Checks `bodyBytes` against what remains before pointing at it.
[[nodiscard]] Result<PluginDataView> decodePluginData(ByteReader& in);

} // namespace sweeps
