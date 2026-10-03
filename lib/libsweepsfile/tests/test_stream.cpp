// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
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
