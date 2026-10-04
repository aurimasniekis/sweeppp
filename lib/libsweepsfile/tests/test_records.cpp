// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <limits>
#include <string>
#include <sweeps/FileFormat.hpp>
#include <sweeps/Records.hpp>
#include <vector>

using namespace sweeps;

namespace {

AcquisitionConfig sampleConfig() {
    AcquisitionConfig config;
    config.centerHz = 2.45e9;
    config.spanHz = 100e6;
    config.sampleRate = 61.44e6;
    config.fftSize = 16384;
    config.window = WindowType::Kaiser;
    config.windowBeta = 8.6;
    config.windowEnbw = 1.73;
    config.overlap = 0.5;
    config.rbwHz = 6489.2;
    config.referenceLevelDbm = -10.0;
    config.dbfsToDbmOffset = 3.5;
    config.deviceId = "bladerf-0";
    config.deviceLabel = "bladeRF 2.0 micro";
    config.gains = {{"lna", 6.0}, {"vga", 12.5}};
    return config;
}

} // namespace

TEST_CASE("a record header carries the payload's checksum and round-trips") {
    const std::vector<std::byte> payload{std::byte{1}, std::byte{2}, std::byte{3}};
    std::vector<std::byte> out;
    appendRecord(out, static_cast<std::uint16_t>(RecordType::Tile), payload.data(), payload.size());
    REQUIRE(out.size() == RecordHeader::kBytes + payload.size());

    ByteReader in(out.data(), out.size());
    auto header = decodeRecordHeader(in);
    REQUIRE(header.has_value());
    CHECK(header->type == static_cast<std::uint16_t>(RecordType::Tile));
    CHECK(header->flags == 0);
    CHECK(header->payloadBytes == payload.size());
    CHECK(header->checksum == crc32(payload.data(), payload.size()));

    ByteReader shortReader(out.data(), 5);
    CHECK_FALSE(decodeRecordHeader(shortReader).has_value());
}

TEST_CASE("a segment opening round-trips with its full configuration") {
    SegmentInfo segment;
    segment.id = 7;
    segment.grid.startHz = 70e6;
    segment.grid.binWidthHz = 3750.0;
    segment.grid.binCount = 1581334;
    segment.startWallNs = 1'700'000'000'000'000'000ULL;
    segment.startMonotonicNs = 123'456'789;
    segment.reason = "FFT size 4096 -> 16384";
    segment.config = sampleConfig();

    std::vector<std::byte> out;
    encodeSegmentOpen(out, segment);

    ByteReader in(out.data(), out.size());
    auto decoded = decodeSegmentOpen(in);
    REQUIRE(decoded.has_value());
    CHECK(decoded->id == 7);
    CHECK(decoded->grid.startHz == segment.grid.startHz);
    CHECK(decoded->grid.binWidthHz == segment.grid.binWidthHz);
    CHECK(decoded->grid.binCount == segment.grid.binCount);
    CHECK(decoded->startWallNs == segment.startWallNs);
    CHECK(decoded->startMonotonicNs == segment.startMonotonicNs);
    CHECK(decoded->reason == segment.reason);
    CHECK(decoded->config.sampleRate == segment.config.sampleRate);
    CHECK(decoded->config.window == WindowType::Kaiser);
    CHECK(decoded->config.deviceLabel == "bladeRF 2.0 micro");
    REQUIRE(decoded->config.gains.size() == 2);
    CHECK(decoded->config.gains[1].second == 12.5);
    CHECK(in.exhausted());
}

TEST_CASE("a segment whose configuration is cut short is kept only when asked") {
    SegmentInfo segment;
    segment.id = 1;
    segment.grid.binCount = 10;
    segment.config = sampleConfig();

    std::vector<std::byte> out;
    encodeSegmentOpen(out, segment);
    out.resize(out.size() - 20);

    ByteReader strict(out.data(), out.size());
    CHECK_FALSE(decodeSegmentOpen(strict).has_value());

    ByteReader lenient(out.data(), out.size());
    auto kept = decodeSegmentOpen(lenient, false);
    REQUIRE(kept.has_value());
    CHECK(kept->grid.binCount == 10);
    CHECK(kept->config.deviceId.empty());
}

TEST_CASE("a configuration claiming more gains than it holds is refused") {
    std::vector<std::byte> out;
    encodeAcquisitionConfig(out, sampleConfig());
    // The gain count sits after the device label; overwrite it with a huge one.
    AcquisitionConfig noGains = sampleConfig();
    noGains.gains.clear();
    std::vector<std::byte> prefix;
    encodeAcquisitionConfig(prefix, noGains);
    const std::size_t countOffset = prefix.size() - 4;
    out[countOffset + 0] = std::byte{0xFF};
    out[countOffset + 1] = std::byte{0xFF};
    out[countOffset + 2] = std::byte{0xFF};
    out[countOffset + 3] = std::byte{0x7F};

    ByteReader in(out.data(), out.size());
    CHECK_FALSE(decodeAcquisitionConfig(in).has_value());
}

TEST_CASE("a segment close round-trips") {
    SegmentClose close;
    close.id = 3;
    close.endMonotonicNs = 99;
    close.lineCount = 12345;

    std::vector<std::byte> out;
    encodeSegmentClose(out, close);
    ByteReader in(out.data(), out.size());
    auto decoded = decodeSegmentClose(in);
    REQUIRE(decoded.has_value());
    CHECK(decoded->id == 3);
    CHECK(decoded->endMonotonicNs == 99);
    CHECK(decoded->lineCount == 12345);
}

TEST_CASE("a tile round-trips, and its quantised levels come back within half a step") {
    std::vector<float> levels(1024);
    for (std::size_t i = 0; i < levels.size(); ++i) {
        levels[i] = -100.0F + static_cast<float>(i % 80);
    }
    levels[10] = static_cast<float>(kUnmeasuredDb);
    levels[11] = -std::numeric_limits<float>::infinity();

    TileHeader header;
    header.segmentId = 2;
    header.lod = 0;
    header.timeBlock = 41;
    header.freqBlock = 5;
    header.lines = 1;
    header.bins = 1024;
    header.originDb = chooseTileOrigin(levels.data(), levels.size());
    header.firstLineNs = 1000;
    header.lastLineNs = 1000;

    std::vector<std::uint8_t> bytes(levels.size());
    quantiseTile(levels.data(), levels.size(), header.originDb, bytes.data());

    std::vector<std::byte> out;
    encodeTile(out, header, bytes.data(), bytes.size());
    CHECK(out.size() == TileHeader::kBytes + bytes.size());

    ByteReader in(out.data(), out.size());
    auto tile = decodeTile(in);
    REQUIRE(tile.has_value());
    CHECK(tile->header.timeBlock == 41);
    CHECK(tile->header.freqBlock == 5);
    CHECK(tile->header.originDb == header.originDb);
    REQUIRE(tile->data.size() == levels.size());

    // Unmeasured stays unmeasured, by both of its spellings.
    CHECK(tile->data[10] == kUnmeasuredByte);
    CHECK(tile->data[11] == kUnmeasuredByte);
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (i == 10 || i == 11) {
            continue;
        }
        const double back = dequantiseDb(tile->data[i], static_cast<double>(header.originDb));
        CHECK(std::abs(back - static_cast<double>(levels[i])) <= (kDbPerStep / 2.0) + 1e-6);
    }
}

TEST_CASE("a tile claiming more data than it carries is refused before allocating") {
    TileHeader header;
    header.lines = 0xFFFFFFFFU;
    header.bins = 0xFFFFFFFFU;
    std::vector<std::byte> out;
    encodeTile(out, header, nullptr, 0);

    ByteReader in(out.data(), out.size());
    auto tile = decodeTile(in);
    REQUIRE_FALSE(tile.has_value());
    CHECK(tile.error().code() == ErrorCode::Corrupt);
}

TEST_CASE("the tile origin follows the measured peak and ignores unmeasured bins") {
    const std::vector<float> levels{static_cast<float>(kUnmeasuredDb), -40.0F, -90.0F};
    const float origin = chooseTileOrigin(levels.data(), levels.size());
    CHECK(origin == doctest::Approx(-40.0 + 8.0 - kQuantSpanDb));

    const std::vector<float> empty{static_cast<float>(kUnmeasuredDb)};
    CHECK(chooseTileOrigin(empty.data(), empty.size()) == doctest::Approx(8.0 - kQuantSpanDb));
}

TEST_CASE("a plugin record round-trips and points at its body") {
    const std::string body = "opaque body";
    std::vector<std::byte> out;
    REQUIRE(encodePluginData(out, "org.sweeppp.remote", "state", 1, 42, body.data(), body.size())
                .has_value());

    ByteReader in(out.data(), out.size());
    auto view = decodePluginData(in);
    REQUIRE(view.has_value());
    CHECK(view->pluginId == "org.sweeppp.remote");
    CHECK(view->recordName == "state");
    CHECK(view->schemaVersion == 1);
    CHECK(view->monotonicNs == 42);
    REQUIRE(view->bodyBytes == body.size());
    CHECK(std::string(reinterpret_cast<const char*>(view->body), view->bodyBytes) == body);
    CHECK(in.exhausted());
}

TEST_CASE("a plugin record's identity and body length are validated") {
    std::vector<std::byte> out;
    CHECK_FALSE(encodePluginData(out, "", "x", 1, 0, nullptr, 0).has_value());
    CHECK_FALSE(
        encodePluginData(out, std::string(kMaxPluginIdBytes + 1, 'a'), "x", 1, 0, nullptr, 0)
            .has_value());

    out.clear();
    REQUIRE(encodePluginData(out, "org.example", "x", 1, 0, "abc", 3).has_value());
    out.resize(out.size() - 2); // body shorter than declared
    ByteReader in(out.data(), out.size());
    CHECK_FALSE(decodePluginData(in).has_value());
}

namespace {

/// The byte-at-a-time CRC the file format was first written with, as the
/// reference the faster one must match bit for bit.
std::uint32_t referenceCrc32(const std::uint8_t* data, std::size_t bytes, std::uint32_t seed) {
    std::uint32_t crc = ~seed;
    for (std::size_t i = 0; i < bytes; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0U ? (0xEDB8'8320U ^ (crc >> 1U)) : (crc >> 1U);
        }
    }
    return ~crc;
}

} // namespace

TEST_CASE("the checksum matches the byte-at-a-time CRC at every length and alignment") {
    // The standard check value for CRC-32/ISO-HDLC.
    const std::string check = "123456789";
    CHECK(crc32(check.data(), check.size()) == 0xCBF4'3926U);
    CHECK(crc32(nullptr, 0) == 0U);

    std::vector<std::uint8_t> data(4096 + 16);
    std::uint32_t state = 0x1234'5678U;
    for (std::uint8_t& byte : data) {
        state = (state * 1'664'525U) + 1'013'904'223U;
        byte = static_cast<std::uint8_t>(state >> 24U);
    }

    for (std::size_t offset = 0; offset < 8; ++offset) {
        for (std::size_t length :
             {0UL, 1UL, 7UL, 8UL, 9UL, 15UL, 16UL, 17UL, 63UL, 64UL, 1000UL, 4096UL}) {
            const std::uint8_t* start = data.data() + offset;
            CHECK(crc32(start, length) == referenceCrc32(start, length, 0));
        }
    }

    // Chained through the seed, as a file writer folding in pieces would.
    const std::uint32_t whole = crc32(data.data(), 1000);
    const std::uint32_t first = crc32(data.data(), 333);
    CHECK(crc32(data.data() + 333, 667, first) == whole);
}
