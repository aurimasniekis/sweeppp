// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#pragma once

#include "sweeps/AcquisitionConfig.hpp"
#include "sweeps/Metadata.hpp"
#include "sweeps/Result.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

/// The `.sweeps` session container, version 1.
///
/// This header is the normative C++ statement of the byte format specified in
/// `sweeps-format-v1.md`, at the root of this library. Where the two disagree,
/// the specification is a bug report against this file.
namespace sweeps {

/// File magic. Four bytes, checked before anything else is trusted.
inline constexpr std::array<char, 4> kMagic{'S', 'W', 'P', 'P'};

// ---------------------------------------------------------------------------
// Versioning. See §11 of the specification -- the rules there are what make
// this format extensible, and they are not obvious from the constants alone.
// ---------------------------------------------------------------------------

/// Bumped only when a reader that does not know the value cannot read the file
/// at all: a field changing meaning, order or width; a record type removed;
/// tile geometry or quantisation altered.
inline constexpr std::uint32_t kMajorVersion = 1;

/// Bumped for additive change: a new record type, a new manifest key, a field
/// appended to an existing record payload. **A reader MUST NOT refuse a file
/// whose minor version it does not know** -- it reads what it understands and
/// skips the rest.
inline constexpr std::uint32_t kMinorVersion = 0;

/// Bitmask of features a reader must understand to read the file *correctly*,
/// as opposed to merely completely. Set for a structurally additive change a
/// naive reader would misread rather than skip -- a compressed tile payload,
/// say. Version 1.0 defines none.
inline constexpr std::uint32_t kKnownFeatures = 0;

/// Quantisation of stored levels.
///
/// uint8 at 0.5 dB per step spans 128 dB, which is far beyond the ~50-70 dB of
/// usable dynamic range these radios have -- so the quantisation is never the
/// limiting factor. The decisive property is that this is *byte-identical to
/// the waterfall texture format*: uploading a tile to the GPU and exporting it
/// are both a memcpy, with no conversion path to get wrong.
inline constexpr double kDbPerStep = 0.5;
inline constexpr double kQuantSpanDb = 255.0 * kDbPerStep; // 127.5 dB

/// The byte reserved for "nothing was ever measured here".
///
/// Coverage is not a level, and the two must not be spelled the same way. A
/// sweep of two disjoint spans leaves the bins between them untouched; storing
/// those at the bottom of the scale makes them indistinguishable from spectrum
/// that was looked at and found quiet. The gap then paints as the colormap's
/// darkest colour rather than as background, and any reducer that averages or
/// interpolates across it fabricates a signal bridging the two sides.
///
/// Measured levels therefore start at 1. What that costs is the bottom half-dB
/// step of a 127.5 dB window against radios with 50-70 dB of usable range.
inline constexpr std::uint8_t kUnmeasuredByte = 0;

[[nodiscard]] constexpr bool isMeasured(std::uint8_t value) noexcept {
    return value != kUnmeasuredByte;
}

/// The dB value an unmeasured bin carries outside the quantised form.
///
/// Impossible as a reading rather than merely low, so "never looked" survives
/// every stage that handles levels as floats.
inline constexpr double kUnmeasuredDb = -200.0;

/// Whether a level is a reading at all.
///
/// A floor rather than an equality: a resample or an average that touches one
/// unmeasured bin lands near the sentinel instead of on it. Written as a
/// negated `>` so that NaN and -inf answer no.
[[nodiscard]] constexpr bool isMeasuredDb(double db) noexcept {
    return db > kUnmeasuredDb + 10.0;
}

/// dB -> stored byte, relative to a per-tile origin. Measured levels occupy
/// 1..255; anything that is not a reading becomes `kUnmeasuredByte`.
[[nodiscard]] constexpr std::uint8_t quantiseDb(double db, double originDb) noexcept {
    if (!isMeasuredDb(db)) {
        return kUnmeasuredByte;
    }
    const double steps = (db - originDb) / kDbPerStep;
    if (steps <= 1.0) {
        return 1;
    }
    if (steps >= 255.0) {
        return 255;
    }
    // +0.5 rounds to nearest; truncation would bias every stored level low by
    // up to a quarter of a dB, which accumulates visibly in a max-hold trace.
    return static_cast<std::uint8_t>(steps + 0.5);
}

/// Stored byte -> dB.
[[nodiscard]] constexpr double dequantiseDb(std::uint8_t value, double originDb) noexcept {
    return isMeasured(value) ? originDb + static_cast<double>(value) * kDbPerStep : kUnmeasuredDb;
}

/// Record kinds in the chunk stream.
///
/// The same stream serves the file and the network: streaming live means
/// emitting these records over TCP instead of to disk. One codec, so the
/// remote reader *is* the replay reader.
enum class RecordType : std::uint16_t {
    Manifest = 1, ///< Typed session metadata.
    SegmentOpen = 2,
    SegmentClose = 3,
    Event = 4,
    Tile = 5,
    Index = 6, ///< Written at close; a truncated file is recovered by scan.
    EndOfStream = 7,
    Telemetry = 8, ///< Remote only: the far end's drop/throttle counters.
    /// A producer's own record, opaque to the container. One type serves every
    /// producer: `pluginId` already names an arbitrary one, so a second
    /// "custom data" type would be a second mechanism for one job.
    PluginData = 9,
};

/// Record types and event kinds from here up are private and experimental use,
/// and MUST NOT appear in an interchanged file.
inline constexpr std::uint16_t kPrivateUseFirst = 0xFF00;

inline constexpr std::size_t kMaxPluginIdBytes = 128;

[[nodiscard]] std::string_view toString(RecordType type) noexcept;

/// Every record carries its own length and type, which is what makes a
/// truncated file recoverable: a reader can walk records until one does not
/// fit, and everything before that point is intact.
struct RecordHeader {
    std::uint16_t type = 0;
    /// Reserved. Writers MUST write zero; readers MUST ignore unknown bits.
    /// Anything a reader could not safely ignore is signalled by a header
    /// feature bit instead -- skipping an unknown *record* flag would mean
    /// misreading a record the reader believed it understood.
    std::uint16_t flags = 0;
    std::uint32_t payloadBytes = 0;
    /// CRC32 of the payload. A power-loss truncation can leave a partially
    /// written record whose length field is plausible; the checksum is what
    /// distinguishes that from a good one.
    std::uint32_t checksum = 0;

    static constexpr std::size_t kBytes = 12;
};

/// File header, fixed size, at offset 0.
///
/// The last two fields occupy bytes 24-31, which every reader before the
/// versioning scheme was written down ignored entirely and every writer filled
/// with zeros. Naming them is therefore a pure reinterpretation: every
/// `.sweeps` file ever written already reads as 1.0 with no feature bits, and
/// nothing needs rewriting.
struct FileHeader {
    std::array<char, 4> magic = kMagic;
    std::uint32_t majorVersion = kMajorVersion;
    /// Offset of the index record, or 0 when the file was not closed cleanly.
    /// Zero is not an error -- it means "recover by scanning", which is the
    /// designed path rather than a fallback.
    std::uint64_t indexOffset = 0;
    std::uint64_t createdWallNs = 0;
    std::uint32_t minorVersion = kMinorVersion;
    std::uint32_t incompatibleFeatures = 0;

    static constexpr std::size_t kBytes = 32;
};

/// A frequency grid. One per segment.
///
/// Segments are what make a mid-session parameter change harmless: each owns
/// its own grid, so tiles written before the change stay readable at their
/// original resolution and extent forever. A single global grid would make
/// every old tile uninterpretable the moment the operator changed span.
struct SegmentGrid {
    double startHz = 0.0;
    double binWidthHz = 0.0;
    std::uint32_t binCount = 0;

    [[nodiscard]] double stopHz() const noexcept {
        return startHz + binWidthHz * static_cast<double>(binCount);
    }
};

/// One acquisition configuration's worth of session.
struct SegmentInfo {
    /// The segment's identity, **not its position**. An extracted file
    /// legitimately contains segments whose ids neither start at zero nor run
    /// contiguously, so readers must look segments up by id.
    std::uint32_t id = 0;
    SegmentGrid grid;

    std::uint64_t startWallNs = 0;
    std::uint64_t startMonotonicNs = 0;
    std::uint64_t endMonotonicNs = 0; ///< 0 while still open.

    /// The full acquisition config in force, so old tiles remain interpretable
    /// without reference to anything outside the file.
    AcquisitionConfig config;

    /// Why this segment was opened. "session start", "sample rate changed",
    /// "fft size changed" -- shown in the History window's timeline.
    std::string reason;

    /// Lines written so far, which is the segment's time extent in tile rows.
    std::uint64_t lineCount = 0;
};

/// Tiles are blocked in frequency as well as time.
///
/// This is what makes "extract the last 30 minutes of that band" a tile copy
/// plus a new index -- no re-encoding, no re-quantisation, bit-identical
/// output. Full-width lines would force reading the entire span to extract one
/// narrow band, and would be the wrong shape for the remote and web tile
/// endpoints too.
inline constexpr std::uint32_t kTileBins = 1024;
inline constexpr std::uint32_t kTileLines = 256;

/// Time-decimation levels: native, /8 and /64.
///
/// Without a pyramid, drawing three hours of waterfall means reading three
/// hours of lines. The overhead is 1/8 + 1/64 = ~14%.
inline constexpr std::uint32_t kLodLevels = 3;
[[nodiscard]] constexpr std::uint32_t lodDecimation(std::uint32_t level) noexcept {
    return 1U << (3U * level); // 1, 8, 64
}

/// Identifies one tile.
struct TileKey {
    std::uint32_t segmentId = 0;
    std::uint32_t lod = 0;
    std::uint32_t timeBlock = 0;
    std::uint32_t freqBlock = 0;

    /// C++17 has no defaulted `<=>`, so the ordering is spelled out.
    ///
    /// **It is load-bearing, not a convenience.** A writer accumulates pending
    /// tiles in a `std::map` keyed on this, and flushes them by iterating it --
    /// so this comparison determines the order tiles appear in the file. The
    /// member order below reproduces what a defaulted comparison gave, and
    /// changing it changes output bytes.
    [[nodiscard]] bool operator<(const TileKey& other) const noexcept {
        return std::tie(segmentId, lod, timeBlock, freqBlock) <
               std::tie(other.segmentId, other.lod, other.timeBlock, other.freqBlock);
    }

    [[nodiscard]] bool operator==(const TileKey& other) const noexcept {
        return std::tie(segmentId, lod, timeBlock, freqBlock) ==
               std::tie(other.segmentId, other.lod, other.timeBlock, other.freqBlock);
    }

    [[nodiscard]] bool operator!=(const TileKey& other) const noexcept { return !(*this == other); }

    /// Named constructor, because C++17 has no designated initialisers and
    /// `TileKey{segment, lod, time, freq}` at a call site says nothing about
    /// which of four same-typed numbers is which.
    [[nodiscard]] static TileKey of(std::uint32_t segmentId, std::uint32_t lod,
                                    std::uint32_t timeBlock, std::uint32_t freqBlock) noexcept {
        TileKey key;
        key.segmentId = segmentId;
        key.lod = lod;
        key.timeBlock = timeBlock;
        key.freqBlock = freqBlock;
        return key;
    }
};

/// A tile's header, written before its pixel data.
struct TileHeader {
    std::uint32_t segmentId = 0;
    std::uint32_t lod = 0;
    std::uint32_t timeBlock = 0;
    std::uint32_t freqBlock = 0;

    std::uint32_t lines = 0;
    std::uint32_t bins = 0;

    /// dB value that stored byte 0 represents. Per tile rather than per file,
    /// so a quiet band and a loud one each get the full 255-step range.
    float originDb = 0.0F;

    /// Monotonic timestamps of this tile's first and last line, so a time
    /// query does not have to reconstruct them from line arithmetic.
    std::uint64_t firstLineNs = 0;
    std::uint64_t lastLineNs = 0;

    /// 6 x u32 + f32 + 2 x u64. The serialised size, which is all this is used
    /// for -- it sizes a `reserve()`. It read 48 for years, which cost a
    /// reallocation per tile and nothing else, because no reader or writer ever
    /// derived an offset from it.
    static constexpr std::size_t kBytes = 44;
};

/// Where a tile lives in the file.
struct IndexEntry {
    TileKey key;
    std::uint64_t offset = 0;
    std::uint32_t length = 0;
    std::uint64_t firstLineNs = 0;
    std::uint64_t lastLineNs = 0;
};

// ---------------------------------------------------------------------------
// Events.
//
// The event id determines the payload, exactly as `RecordType` does for records.
// A single flat set of fields shared by every kind would be a convention rather
// than a structure, and a lossy one: each kind carries a different number of
// values, so the fields that did not fit would simply not be recorded.
// ---------------------------------------------------------------------------

struct RetuneData {
    double centerHz = 0.0;
    std::uint32_t stepIndex = 0;
};

struct ParameterChangedData {
    std::string key;
    std::string value;
    /// True when the change redefines the frequency grid and therefore closes
    /// the current segment.
    bool gridAffecting = false;
    /// Gain, reference level: the grid is intact but the noise floor moved, so
    /// later analysis of these tiles must know it happened.
    bool calibrationAffecting = false;
};

struct SweepPassData {
    std::uint64_t passId = 0;
    double startHz = 0.0;
    double stopHz = 0.0;
    double durationSeconds = 0.0;
};

struct MarkerData {
    std::string label;
    double frequencyHz = 0.0;
    double levelDbm = 0.0;
};

struct AnnotationData {
    std::string text;
    double startHz = 0.0;
    double stopHz = 0.0;
};

struct SegmentBoundaryData {
    std::string reason;
};

struct ThrottleChangedData {
    std::string reason;
    double processedFraction = 0.0;
};

struct DeviceErrorData {
    std::string deviceId;
    std::string message;
};

/// A plugin's own event. `pluginId` is reverse-DNS -- `org.sweeppp.bandplan` --
/// which is what keeps two plugins from colliding without a registry.
struct PluginEventData {
    std::string pluginId;
    std::string eventName;
    Metadata fields;
};

/// The body of an event whose kind this build does not know, kept byte for byte.
///
/// Also what `Kind::Alert` decodes to: the kind is defined and reserved, and
/// nothing writes it, so there is no body to name.
struct UnknownEventData {
    std::vector<std::byte> body;
};

using EventBody = std::variant<RetuneData, ParameterChangedData, SweepPassData, MarkerData,
                               AnnotationData, SegmentBoundaryData, ThrottleChangedData,
                               DeviceErrorData, PluginEventData, UnknownEventData>;

/// Event stream entry.
///
/// The events are what make replay *faithful*. Frames alone reproduce the
/// waterfall; the events reproduce everything around it, so on playback the
/// RBW / FFT size / gain / span readouts change at the same moments they did
/// live.
struct SessionEvent {
    enum class Kind : std::uint16_t {
        Retune = 1,
        ParameterChanged = 2,
        SweepPass = 3,
        Marker = 4,
        Annotation = 5,
        /// **Reserved.** The body is undefined and writers MUST NOT emit it.
        /// Naming a kind that nothing writes is more honest than inventing a
        /// payload for a feature that does not exist yet.
        Alert = 6,
        SegmentBoundary = 7,
        ThrottleChanged = 8,
        DeviceError = 9,
        Plugin = 10,
    };

    /// Raw rather than the enum, so an id this build does not know survives a
    /// read instead of being discarded or forced into a neighbouring meaning.
    std::uint16_t kind = 0;
    std::uint64_t monotonicNs = 0;
    std::uint64_t wallNs = 0;
    std::uint32_t segmentId = 0;

    EventBody body;

    /// The usual construction: a known kind and the body that belongs to it.
    /// `segmentId` is left at zero because a live writer stamps it anyway.
    template <typename Body>
    [[nodiscard]] static SessionEvent of(Kind kind, std::uint64_t monotonicNs, std::uint64_t wallNs,
                                         Body body) {
        SessionEvent event;
        event.kind = static_cast<std::uint16_t>(kind);
        event.monotonicNs = monotonicNs;
        event.wallNs = wallNs;
        event.body = std::move(body);
        return event;
    }

    [[nodiscard]] Kind kindEnum() const noexcept { return static_cast<Kind>(kind); }

    /// The body when it is of this type, else nullptr.
    template <typename Body>
    [[nodiscard]] const Body* as() const noexcept {
        return std::get_if<Body>(&body);
    }
};

[[nodiscard]] std::string_view toString(SessionEvent::Kind kind) noexcept;

/// The text spelling of a raw kind id -- "retune", "plugin", "unknown".
[[nodiscard]] std::string_view eventKindName(std::uint16_t kind) noexcept;

[[nodiscard]] Result<SessionEvent::Kind> eventKindFromString(std::string_view name);

/// An event's body as a metadata object.
///
/// Bodies are heterogeneous, so one renderer per kind would be nine of them.
/// Going through `Metadata` instead means the JSON writer that already exists
/// for the manifest renders every event too, including a kind this build has
/// never seen -- whose bytes come out as hex rather than as nothing.
///
/// In the library rather than in whichever caller wanted it first, because
/// there are now three: `sweeps info`, `sweeps events` and the C ABI's
/// `sweeps_event_body_json`. The key spellings are what those emit, so a second
/// copy would be a second answer to "what is this event called in JSON".
[[nodiscard]] Metadata eventBodyMetadata(const SessionEvent& event);

/// The Event record payload: the common prefix, then the body the kind names.
void encodeEvent(std::vector<std::byte>& out, const SessionEvent& event);
[[nodiscard]] Result<SessionEvent> decodeEvent(ByteReader& in);

/// CRC-32/ISO-HDLC, for record payload integrity.
[[nodiscard]] std::uint32_t crc32(const void* data, std::size_t bytes,
                                  std::uint32_t seed = 0) noexcept;

// ---------------------------------------------------------------------------
// Little-endian scalar encoding.
//
// Written explicitly rather than by memcpy of a struct: the format must be
// identical on every platform, and a struct layout is a compiler's business,
// not a file format's.
// ---------------------------------------------------------------------------

void writeU8(std::vector<std::byte>& out, std::uint8_t value);
void writeU16(std::vector<std::byte>& out, std::uint16_t value);
void writeU32(std::vector<std::byte>& out, std::uint32_t value);
void writeU64(std::vector<std::byte>& out, std::uint64_t value);
void writeF32(std::vector<std::byte>& out, float value);
void writeF64(std::vector<std::byte>& out, double value);
void writeString(std::vector<std::byte>& out, std::string_view value);
void writeBytes(std::vector<std::byte>& out, const void* data, std::size_t bytes);

/// Cursor over a byte span. Every read is bounds-checked, because these bytes
/// may come from a truncated file or a hostile network peer.
class ByteReader {
public:
    ByteReader(const std::byte* data, std::size_t size) noexcept : m_data(data), m_size(size) {}

    [[nodiscard]] Result<std::uint8_t> readU8();
    [[nodiscard]] Result<std::uint16_t> readU16();
    [[nodiscard]] Result<std::uint32_t> readU32();
    [[nodiscard]] Result<std::uint64_t> readU64();
    [[nodiscard]] Result<float> readF32();
    [[nodiscard]] Result<double> readF64();
    [[nodiscard]] Result<std::string> readString();
    [[nodiscard]] Status readBytes(void* destination, std::size_t bytes);

    [[nodiscard]] std::size_t offset() const noexcept { return m_offset; }
    [[nodiscard]] std::size_t remaining() const noexcept { return m_size - m_offset; }
    [[nodiscard]] bool exhausted() const noexcept { return m_offset >= m_size; }
    void seek(std::size_t offset) noexcept { m_offset = std::min(offset, m_size); }

private:
    const std::byte* m_data = nullptr;
    std::size_t m_size = 0;
    std::size_t m_offset = 0;
};

} // namespace sweeps
