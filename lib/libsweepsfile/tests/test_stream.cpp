// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <sweeps/FileFormat.hpp>
#include <sweeps/Records.hpp>
#include <sweeps/Stream.hpp>
#include <vector>

using namespace sweeps;

namespace {

/// Three records of different sizes back to back, as a stream would carry them.
std::vector<std::byte> threeRecords() {
    std::vector<std::byte> stream;
    for (std::size_t size : {0U, 5U, 3000U}) {
        std::vector<std::byte> payload(size);
        for (std::size_t i = 0; i < size; ++i) {
            payload[i] = static_cast<std::byte>(i * 7);
        }
        appendRecord(stream, static_cast<std::uint16_t>(RecordType::PluginData), payload.data(),
                     payload.size());
    }
    return stream;
}

std::vector<StreamRecord> drain(RecordFramer& framer) {
    std::vector<StreamRecord> records;
    for (;;) {
        StreamRecord record;
        auto got = framer.next(record);
        REQUIRE(got.has_value());
        if (!*got) {
            return records;
        }
        records.push_back(std::move(record));
    }
}

} // namespace

TEST_CASE("a stream header round-trips and is exactly sixteen bytes") {
    std::vector<std::byte> out;
    encodeStreamHeader(out, StreamHeader{});
    REQUIRE(out.size() == StreamHeader::kBytes);

    auto decoded = decodeStreamHeader(out.data(), out.size());
    REQUIRE(decoded.has_value());
    CHECK(decoded->majorVersion == kMajorVersion);
    CHECK(decoded->minorVersion == kMinorVersion);
    CHECK(decoded->incompatibleFeatures == 0);
}

TEST_CASE("a stream header follows the file's version rules") {
    SUBCASE("something that is not a stream") {
        const std::string http = "GET / HTTP/1.1\r\n";
        auto decoded =
            decodeStreamHeader(reinterpret_cast<const std::byte*>(http.data()), http.size());
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code() == ErrorCode::ProtocolError);
    }

    SUBCASE("a newer major version") {
        StreamHeader header;
        header.majorVersion = kMajorVersion + 1;
        std::vector<std::byte> out;
        encodeStreamHeader(out, header);
        auto decoded = decodeStreamHeader(out.data(), out.size());
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code() == ErrorCode::Unsupported);
    }

    SUBCASE("a feature bit this build does not know") {
        StreamHeader header;
        header.incompatibleFeatures = 0x4;
        std::vector<std::byte> out;
        encodeStreamHeader(out, header);
        CHECK_FALSE(decodeStreamHeader(out.data(), out.size()).has_value());
    }

    SUBCASE("a newer minor version is read") {
        StreamHeader header;
        header.minorVersion = kMinorVersion + 3;
        std::vector<std::byte> out;
        encodeStreamHeader(out, header);
        CHECK(decodeStreamHeader(out.data(), out.size()).has_value());
    }

    SUBCASE("too short") {
        std::vector<std::byte> out;
        encodeStreamHeader(out, StreamHeader{});
        CHECK_FALSE(decodeStreamHeader(out.data(), 9).has_value());
    }
}

TEST_CASE("the framer returns the same records however the bytes arrive") {
    const std::vector<std::byte> stream = threeRecords();

    SUBCASE("all at once") {
        RecordFramer framer;
        framer.feed(stream.data(), stream.size());
        const std::vector<StreamRecord> records = drain(framer);
        REQUIRE(records.size() == 3);
        CHECK(records[0].payload.empty());
        CHECK(records[1].payload.size() == 5);
        CHECK(records[2].payload.size() == 3000);
        CHECK(records[2].payload[100] == static_cast<std::byte>(700 % 256));
        CHECK(framer.buffered() == 0);
    }

    SUBCASE("one byte at a time") {
        RecordFramer framer;
        std::vector<StreamRecord> records;
        for (const std::byte byte : stream) {
            framer.feed(&byte, 1);
            for (StreamRecord& record : drain(framer)) {
                records.push_back(std::move(record));
            }
        }
        REQUIRE(records.size() == 3);
        CHECK(records[2].payload.size() == 3000);
    }

    SUBCASE("split at every offset") {
        for (std::size_t split = 0; split <= stream.size(); ++split) {
            CAPTURE(split);
            RecordFramer framer;
            framer.feed(stream.data(), split);
            std::vector<StreamRecord> records = drain(framer);
            framer.feed(stream.data() + split, stream.size() - split);
            for (StreamRecord& record : drain(framer)) {
                records.push_back(std::move(record));
            }
            REQUIRE(records.size() == 3);
            CHECK(records[1].payload.size() == 5);
        }
    }
}

TEST_CASE("a record over the limit breaks the stream before its payload is buffered") {
    std::vector<std::byte> stream;
    RecordHeader header;
    header.type = static_cast<std::uint16_t>(RecordType::Tile);
    header.payloadBytes = 0xFFFFFFF0U;
    encodeRecordHeader(stream, header);

    RecordFramer framer(4096);
    framer.feed(stream.data(), stream.size());
    StreamRecord record;
    auto got = framer.next(record);
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error().code() == ErrorCode::ProtocolError);

    // Broken for good: nothing after a bad length can be trusted.
    const std::vector<std::byte> more = threeRecords();
    framer.feed(more.data(), more.size());
    CHECK_FALSE(framer.next(record).has_value());
}

TEST_CASE("a checksum mismatch breaks the stream") {
    std::vector<std::byte> stream = threeRecords();
    // Flip a byte inside the third record's payload.
    stream[stream.size() - 10] ^= std::byte{0x01};

    RecordFramer framer;
    framer.feed(stream.data(), stream.size());
    StreamRecord record;
    REQUIRE(framer.next(record).value());
    REQUIRE(framer.next(record).value());
    auto broken = framer.next(record);
    REQUIRE_FALSE(broken.has_value());
    CHECK(broken.error().code() == ErrorCode::Corrupt);
}

TEST_CASE("records of a type this build does not know still frame") {
    std::vector<std::byte> stream;
    const std::vector<std::byte> payload{std::byte{9}};
    appendRecord(stream, 0x0042, payload.data(), payload.size());

    RecordFramer framer;
    framer.feed(stream.data(), stream.size());
    StreamRecord record;
    REQUIRE(framer.next(record).value());
    CHECK(record.header.type == 0x0042);
}

// ---------------------------------------------------------------- LineMirror

namespace {

// The golden stream: what `tests/test_capi.c` and the Python suite read, so
// its numbers are restated there and must change in all three together.
//
//   segment 7   100 MHz + 1500 x 1 kHz   line 0: both blocks; line 1: block 1
//   (an unknown record type 0x0042 between the two lines)
//   segment 8   433.05 MHz + 300 x 2.5 kHz   line 0
//
// each line followed by a PluginData commit, then EndOfStream.
constexpr std::uint64_t kStreamWallNs = 1'770'000'000'000'000'000ULL;
constexpr std::uint64_t kStreamStartNs = 1'000'000'000ULL;
constexpr std::uint64_t kStreamLineNs = 50'000'000ULL;

std::filesystem::path goldenStreamPath() {
    return std::filesystem::path(SWEEPSFILE_TEST_DATA_DIR) / "v1-golden.sweepstream";
}

AcquisitionConfig streamConfig(double startHz, double binWidthHz, std::uint32_t bins,
                               std::uint32_t fftSize) {
    AcquisitionConfig config;
    config.centerHz = startHz + binWidthHz * static_cast<double>(bins) * 0.5;
    config.spanHz = binWidthHz * static_cast<double>(bins);
    config.sampleRate = 2e6;
    config.fftSize = fftSize;
    config.window = WindowType::Hann;
    config.windowEnbw = 1.5;
    config.overlap = 0.5;
    config.rbwHz = 2e6 * 1.5 / static_cast<double>(fftSize);
    config.referenceLevelDbm = -30.0;
    config.dbfsToDbmOffset = -12.5;
    config.deviceId = "golden-stream";
    config.deviceLabel = "Golden stream device";
    config.gains.emplace_back("lna", 24.0);
    return config;
}

/// Segment 7's line 0: a ramp, one unmeasured bin, one peak.
std::vector<float> firstLevels() {
    std::vector<float> levels(1500);
    for (std::size_t bin = 0; bin < levels.size(); ++bin) {
        levels[bin] = -100.0F + static_cast<float>(bin % 40) * 0.5F;
    }
    levels[10] = static_cast<float>(kUnmeasuredDb);
    levels[700] = -20.0F;
    return levels;
}

void appendSegmentOpen(std::vector<std::byte>& stream, std::uint32_t id, double startHz,
                       double binWidthHz, std::uint32_t bins, std::uint32_t fftSize,
                       std::uint64_t ns, const char* reason) {
    SegmentInfo segment;
    segment.id = id;
    segment.grid = SegmentGrid{startHz, binWidthHz, bins};
    segment.startWallNs = kStreamWallNs + (ns - kStreamStartNs);
    segment.startMonotonicNs = ns;
    segment.config = streamConfig(startHz, binWidthHz, bins, fftSize);
    segment.reason = reason;

    std::vector<std::byte> payload;
    encodeSegmentOpen(payload, segment);
    appendRecord(stream, static_cast<std::uint16_t>(RecordType::SegmentOpen), payload.data(),
                 payload.size());
}

void appendTile(std::vector<std::byte>& stream, std::uint32_t segmentId, std::uint32_t line,
                std::uint32_t block, const std::vector<float>& levels, std::uint64_t ns,
                std::uint32_t lod = 0, std::uint32_t lines = 1) {
    const std::size_t first = std::size_t{block} * kTileBins;
    const std::size_t count = std::min<std::size_t>(kTileBins, levels.size() - first);
    TileHeader header;
    header.segmentId = segmentId;
    header.lod = lod;
    header.timeBlock = line;
    header.freqBlock = block;
    header.lines = lines;
    header.bins = static_cast<std::uint32_t>(count);
    header.originDb = chooseTileOrigin(levels.data() + first, count);
    header.firstLineNs = ns;
    header.lastLineNs = ns;

    std::vector<std::uint8_t> quantised(count * lines);
    for (std::uint32_t row = 0; row < lines; ++row) {
        quantiseTile(levels.data() + first, count, header.originDb, quantised.data() + row * count);
    }
    std::vector<std::byte> payload;
    encodeTile(payload, header, quantised.data(), quantised.size());
    appendRecord(stream, static_cast<std::uint16_t>(RecordType::Tile), payload.data(),
                 payload.size());
}

void appendCommit(std::vector<std::byte>& stream, std::uint64_t ns) {
    std::vector<std::byte> payload;
    REQUIRE(
        encodePluginData(payload, "org.example.stream", "commit", 1, ns, nullptr, 0).has_value());
    appendRecord(stream, static_cast<std::uint16_t>(RecordType::PluginData), payload.data(),
                 payload.size());
}

std::vector<std::byte> goldenStream() {
    std::vector<std::byte> stream;
    encodeStreamHeader(stream, StreamHeader{});

    std::uint64_t ns = kStreamStartNs;
    appendSegmentOpen(stream, 7, 100e6, 1000.0, 1500, 2048, ns, "stream start");

    std::vector<float> levels = firstLevels();
    appendTile(stream, 7, 0, 0, levels, ns);
    appendTile(stream, 7, 0, 1, levels, ns);
    appendCommit(stream, ns);

    const std::vector<std::byte> unknown{std::byte{0x5A}};
    appendRecord(stream, 0x0042, unknown.data(), unknown.size());

    ns += kStreamLineNs;
    levels[1200] = -15.0F;
    appendTile(stream, 7, 1, 1, levels, ns);
    appendCommit(stream, ns);

    std::vector<std::byte> payload;
    encodeSegmentClose(payload, SegmentClose{7, ns, 2});
    appendRecord(stream, static_cast<std::uint16_t>(RecordType::SegmentClose), payload.data(),
                 payload.size());

    ns += kStreamLineNs;
    appendSegmentOpen(stream, 8, 433.05e6, 2500.0, 300, 512, ns, "acquisition changed");
    std::vector<float> narrow(300);
    for (std::size_t bin = 0; bin < narrow.size(); ++bin) {
        narrow[bin] = -90.0F + static_cast<float>(bin % 10);
    }
    appendTile(stream, 8, 0, 0, narrow, ns);
    appendCommit(stream, ns);

    appendRecord(stream, static_cast<std::uint16_t>(RecordType::EndOfStream), nullptr, 0);
    return stream;
}

/// The records after the stream header.
std::vector<StreamRecord> recordsOf(const std::vector<std::byte>& stream) {
    RecordFramer framer;
    framer.feed(stream.data() + StreamHeader::kBytes, stream.size() - StreamHeader::kBytes);
    return drain(framer);
}

bool near(float actual, float expected) {
    return std::abs(actual - expected) <= 0.25F + 1e-4F;
}

} // namespace

TEST_CASE("the stream encoders reproduce the golden stream byte for byte") {
    // The stream's tripwire, as test_golden.cpp is the file's. Regenerate with
    // SWEEPSFILE_UPDATE_GOLDEN=1, only when the format is meant to move.
    const std::vector<std::byte> written = goldenStream();
    const std::filesystem::path reference = goldenStreamPath();

    if (const char* update = std::getenv("SWEEPSFILE_UPDATE_GOLDEN");
        update != nullptr && std::string(update) == "1") {
        std::ofstream out(reference, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(written.data()),
                  static_cast<std::streamsize>(written.size()));
        MESSAGE("golden stream regenerated at " << reference.string());
    }

    std::ifstream in(reference, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "golden stream missing: " << reference.string()
                                                         << " (regenerate with "
                                                            "SWEEPSFILE_UPDATE_GOLDEN=1)");
    const std::vector<char> committed((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
    REQUIRE(committed.size() == written.size());
    CHECK(std::memcmp(committed.data(), written.data(), written.size()) == 0);
}

TEST_CASE("a line mirror rebuilds the line from a SegmentOpen and its tiles") {
    const std::vector<StreamRecord> records = recordsOf(goldenStream());
    REQUIRE(records.size() == 12);

    LineMirror mirror;
    CHECK(mirror.segment() == nullptr);
    CHECK(mirror.levels().empty());

    REQUIRE(mirror.apply(records[0]).has_value());
    REQUIRE(mirror.segment() != nullptr);
    CHECK(mirror.segment()->id == 7);
    CHECK(mirror.segment()->grid.binCount == 1500);
    CHECK(mirror.segment()->config.fftSize == 2048);
    CHECK(mirror.segment()->reason == "stream start");
    REQUIRE(mirror.levels().size() == 1500);
    CHECK(mirror.levels()[0] == static_cast<float>(kUnmeasuredDb));

    REQUIRE(mirror.apply(records[1]).has_value());
    REQUIRE(mirror.apply(records[2]).has_value());
    CHECK(mirror.tilesApplied() == 2);
    CHECK(mirror.line() == 0);

    const std::vector<float> expected = firstLevels();
    for (std::size_t bin = 0; bin < expected.size(); ++bin) {
        CAPTURE(bin);
        if (bin == 10) {
            CHECK(mirror.levels()[bin] == static_cast<float>(kUnmeasuredDb));
        } else {
            CHECK(near(mirror.levels()[bin], expected[bin]));
        }
    }

    // The commit and the unknown record are not the mirror's.
    REQUIRE(mirror.apply(records[3]).has_value());
    REQUIRE(mirror.apply(records[4]).has_value());
    CHECK(mirror.tilesApplied() == 2);

    // Line 1 resends block 1 alone; block 0 keeps what it had.
    REQUIRE(mirror.apply(records[5]).has_value());
    CHECK(mirror.line() == 1);
    CHECK(mirror.tilesApplied() == 3);
    CHECK(near(mirror.levels()[1200], -15.0F));
    CHECK(near(mirror.levels()[700], -20.0F));

    // A SegmentClose leaves the line as it was.
    REQUIRE(mirror.apply(records[7]).has_value());
    CHECK(mirror.segment()->id == 7);
    CHECK(near(mirror.levels()[1200], -15.0F));
}

TEST_CASE("a line mirror refuses a tile that does not fit, and keeps its line") {
    const std::vector<StreamRecord> records = recordsOf(goldenStream());
    LineMirror mirror;
    REQUIRE(mirror.apply(records[0]).has_value());
    REQUIRE(mirror.apply(records[1]).has_value());
    const std::vector<float> before = mirror.levels();

    const auto applyOne = [&](const std::vector<std::byte>& record) {
        const std::vector<StreamRecord> one = [&] {
            RecordFramer framer;
            framer.feed(record.data(), record.size());
            return drain(framer);
        }();
        REQUIRE(one.size() == 1);
        return mirror.apply(one[0]);
    };
    const auto refused = [&](const std::vector<std::byte>& record) {
        auto status = applyOne(record);
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code() == ErrorCode::ProtocolError);
    };

    std::vector<float> levels(3000, -50.0F);
    std::vector<std::byte> record;

    SUBCASE("a block past the grid") {
        appendTile(record, 7, 0, 2, levels, kStreamStartNs);
        refused(record);
    }
    SUBCASE("a short block where a full one belongs") {
        levels.resize(1400);
        appendTile(record, 7, 0, 1, levels, kStreamStartNs);
        refused(record);
    }
    SUBCASE("a pyramid level") {
        appendTile(record, 7, 0, 0, levels, kStreamStartNs, 1);
        refused(record);
    }
    SUBCASE("more than one line") {
        appendTile(record, 7, 0, 0, levels, kStreamStartNs, 0, 2);
        refused(record);
    }
    SUBCASE("no usable origin") {
        TileHeader header;
        header.segmentId = 7;
        header.lines = 1;
        header.bins = 1024;
        header.originDb = std::numeric_limits<float>::quiet_NaN();
        const std::vector<std::uint8_t> data(1024, 1);
        std::vector<std::byte> payload;
        encodeTile(payload, header, data.data(), data.size());
        appendRecord(record, static_cast<std::uint16_t>(RecordType::Tile), payload.data(),
                     payload.size());
        refused(record);
    }
    SUBCASE("a tile for another segment is ignored") {
        appendTile(record, 99, 0, 0, levels, kStreamStartNs);
        CHECK(applyOne(record).has_value());
    }

    CHECK(mirror.levels() == before);
    CHECK(mirror.tilesApplied() == 1);
}

TEST_CASE("a second segment replaces the first") {
    const std::vector<StreamRecord> records = recordsOf(goldenStream());
    LineMirror mirror;
    for (std::size_t i = 0; i < 9; ++i) {
        REQUIRE(mirror.apply(records[i]).has_value());
    }

    REQUIRE(mirror.segment() != nullptr);
    CHECK(mirror.segment()->id == 8);
    CHECK(mirror.segment()->grid.startHz == 433.05e6);
    CHECK(mirror.segment()->reason == "acquisition changed");
    REQUIRE(mirror.levels().size() == 300);
    CHECK(mirror.tilesApplied() == 0);
    for (const float level : mirror.levels()) {
        CHECK(level == static_cast<float>(kUnmeasuredDb));
    }

    // A late tile of the superseded segment changes nothing.
    REQUIRE(mirror.apply(records[5]).has_value());
    CHECK(mirror.tilesApplied() == 0);

    REQUIRE(mirror.apply(records[9]).has_value());
    CHECK(near(mirror.levels()[0], -90.0F));
    CHECK(near(mirror.levels()[299], -81.0F));

    mirror.reset();
    CHECK(mirror.segment() == nullptr);
    CHECK(mirror.levels().empty());
}

TEST_CASE("a line mirror refuses a grid over its limit") {
    const std::vector<StreamRecord> records = recordsOf(goldenStream());
    LineMirror mirror(1000);
    auto status = mirror.apply(records[0]);
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::ProtocolError);
    CHECK(mirror.segment() == nullptr);

    // With no segment open, tiles have nothing to land on.
    CHECK(mirror.apply(records[1]).has_value());
    CHECK(mirror.levels().empty());
}
