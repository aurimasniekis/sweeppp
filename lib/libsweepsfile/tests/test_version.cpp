// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include "GoldenSession.hpp"
#include "SessionFixture.hpp"

#include <algorithm>
#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <sweeps/Clock.hpp>
#include <sweeps/FileFormat.hpp>
#include <sweeps/SessionReader.hpp>
#include <sweeps/SessionWriter.hpp>
#include <system_error>
#include <utility>
#include <vector>

using namespace sweeps;
using namespace sweeps::test;

namespace {

/// An event kind no build of the library knows, for the cases about carrying
/// one through unchanged.
///
/// At namespace scope rather than inside the case that uses it: the predicates
/// below read it from captureless lambdas, which is well-formed but which MSVC
/// rejects with C3493, and capturing it instead earns an unused-capture warning
/// from Clang. Neither compiler has anything to say about this.
constexpr std::uint16_t kFutureKind = 4242;

/// A copy of the golden file with a few bytes changed.
///
/// Building the compatibility cases by patching a real file rather than
/// hand-authoring one per case is the point: everything except the field under
/// test is genuine, so a case cannot pass because the fixture happened to be
/// malformed in a compensating way.
class PatchedGolden {
public:
    explicit PatchedGolden(const std::string& name)
        : m_path(std::filesystem::temp_directory_path() /
                 ("sweepsfile-version-" + std::to_string(monotonicNs()) + "-" + name)) {
        std::ifstream in(goldenPath(), std::ios::binary);
        m_bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    ~PatchedGolden() {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    PatchedGolden(const PatchedGolden&) = delete;
    PatchedGolden& operator=(const PatchedGolden&) = delete;

    [[nodiscard]] bool loaded() const { return m_bytes.size() > FileHeader::kBytes; }

    void putU32(std::size_t offset, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            m_bytes[offset + static_cast<std::size_t>(i)] =
                static_cast<char>((value >> (8 * i)) & 0xFFU);
        }
    }

    /// Appends bytes at the end of the file, before nothing -- used for a
    /// record the reader has never seen.
    void append(const std::vector<std::uint8_t>& extra) {
        for (const std::uint8_t byte : extra) {
            m_bytes.push_back(static_cast<char>(byte));
        }
    }

    [[nodiscard]] const std::filesystem::path& write() {
        std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
        out.write(m_bytes.data(), static_cast<std::streamsize>(m_bytes.size()));
        out.close();
        return m_path;
    }

private:
    std::filesystem::path m_path;
    std::vector<char> m_bytes;
};

constexpr std::size_t kMajorOffset = 4;
constexpr std::size_t kMinorOffset = 24;
constexpr std::size_t kFeaturesOffset = 28;

} // namespace

TEST_CASE("the golden file reads as version 1.0 with no feature bits") {
    // The claim the whole re-specification rests on: bytes 24-31 of every file
    // ever written are zero, so naming them changed nothing.
    auto reader = SessionReader::open(goldenPath());
    REQUIRE(reader.has_value());

    const SessionSummary& summary = (*reader)->summary();
    CHECK(summary.majorVersion == 1);
    CHECK(summary.minorVersion == 0);
    CHECK(summary.incompatibleFeatures == 0);
    CHECK_FALSE(summary.newerMinorVersion);
    CHECK(summary.versionString() == "1.0");
}

TEST_CASE("a newer major version is refused, naming both versions") {
    PatchedGolden patched("major.sweeps");
    REQUIRE(patched.loaded());
    patched.putU32(kMajorOffset, kMajorVersion + 1);

    auto reader = SessionReader::open(patched.write());
    REQUIRE_FALSE(reader.has_value());
    CHECK(reader.error().code() == ErrorCode::Unsupported);

    const std::string message = reader.error().message();
    CHECK(message.find(std::to_string(kMajorVersion + 1)) != std::string::npos);
    CHECK(message.find(std::to_string(kMajorVersion)) != std::string::npos);
}

TEST_CASE("an unknown incompatible feature bit is refused, naming the bit") {
    // The case the bitmask exists for: a file that is structurally readable but
    // would be *misread* by a reader unaware of the feature.
    PatchedGolden patched("feature.sweeps");
    REQUIRE(patched.loaded());
    patched.putU32(kFeaturesOffset, 0x0000'0004U);

    auto reader = SessionReader::open(patched.write());
    REQUIRE_FALSE(reader.has_value());
    CHECK(reader.error().code() == ErrorCode::Unsupported);
    CHECK(reader.error().message().find("0x00000004") != std::string::npos);
}

TEST_CASE("a newer minor version opens and reads normally") {
    // The property the whole scheme exists for. A reader built today must read
    // a 1.7 file, skipping only what 1.7 added -- not refuse it.
    PatchedGolden patched("minor.sweeps");
    REQUIRE(patched.loaded());
    patched.putU32(kMinorOffset, kMinorVersion + 7);

    auto reader = SessionReader::open(patched.write());
    REQUIRE(reader.has_value());

    const SessionSummary& summary = (*reader)->summary();
    CHECK(summary.majorVersion == kMajorVersion);
    CHECK(summary.minorVersion == kMinorVersion + 7);
    // Surfaced, so a caller can say so -- but not fatal.
    CHECK(summary.newerMinorVersion);

    // And everything actually read.
    CHECK((*reader)->segments().size() == 2);
    CHECK(summary.totalLines == kWideLines + kNarrowLines);
    CHECK(summary.name == kSessionName);

    HistoryQuery request;
    request.maxLines = 100'000;
    auto tiles = (*reader)->query(request);
    REQUIRE(tiles.has_value());
    CHECK_FALSE(tiles->empty());
}

TEST_CASE("a known feature bit combined with a newer minor version still opens") {
    // Version 1.0 defines no feature bits, so this asserts the *shape* of the
    // check rather than a specific bit: only bits outside KNOWN_FEATURES may
    // cause a refusal.
    PatchedGolden patched("known-feature.sweeps");
    REQUIRE(patched.loaded());
    patched.putU32(kFeaturesOffset, kKnownFeatures);
    patched.putU32(kMinorOffset, kMinorVersion + 1);

    auto reader = SessionReader::open(patched.write());
    REQUIRE(reader.has_value());
    CHECK((*reader)->summary().incompatibleFeatures == kKnownFeatures);
}

TEST_CASE("an unknown record type mid-stream is skipped, leaving its neighbours intact") {
    // The first of the two extensibility guarantees: every record carries its
    // length, so a reader can walk past content a newer minor version added.
    // Without this, adding a record type would be a breaking change.
    PatchedGolden patched("unknown-record.sweeps");
    REQUIRE(patched.loaded());

    // Type 4242, a 5-byte payload, appended after the end-of-stream record. The
    // index still points at the real index, so the scan is what walks past it.
    const std::vector<std::uint8_t> payload{0xDE, 0xAD, 0xBE, 0xEF, 0x42};
    const std::uint32_t checksum = crc32(payload.data(), payload.size());

    std::vector<std::uint8_t> record{
        0x92, 0x10,             // type = 4242
        0x00, 0x00,             // flags
        0x05, 0x00, 0x00, 0x00, // payloadBytes = 5
    };
    for (int i = 0; i < 4; ++i) {
        record.push_back(static_cast<std::uint8_t>((checksum >> (8 * i)) & 0xFFU));
    }
    record.insert(record.end(), payload.begin(), payload.end());
    patched.append(record);

    // Zeroing indexOffset forces the reader down the scan path, which is where
    // an unknown record type actually has to be walked over.
    patched.putU32(8, 0);
    patched.putU32(12, 0);

    auto reader = SessionReader::open(patched.write());
    REQUIRE(reader.has_value());

    const SessionSummary& summary = (*reader)->summary();
    // Everything before and after the unknown record survived the walk.
    CHECK((*reader)->segments().size() == 2);
    CHECK(summary.totalTiles > 0);
    CHECK(summary.truncatedBytes == 0);
    CHECK((*reader)->events().size() >= 2);
}

TEST_CASE("an unknown event kind survives a read and an extraction byte for byte") {
    // The record-level guarantee, extended to event ids. A kind this build does
    // not know must round-trip rather than be discarded -- which is precisely
    // why extract() copies event payloads instead of re-encoding them field by
    // field, and why the in-memory event keeps its raw u16 kind.
    const ScopedTempDir temp;
    const std::filesystem::path source = temp.file("unknown-kind.sweeps");
    const std::filesystem::path extracted = temp.file("unknown-kind-cut.sweeps");

    const std::vector<std::byte> futureBody{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                            std::byte{0xEF}};

    constexpr std::uint64_t kFirstNs = 1'000'000'000;
    {
        auto writer = SessionWriter::create(source);
        REQUIRE(writer.has_value());

        for (std::uint32_t i = 0; i < 200; ++i) {
            const TestFrame frame =
                makeFrame(kFirstNs + i * 20'000'000, 90e6, 9765.625, 2048, -90.0F);
            REQUIRE((*writer)->writeFrame(frame.view()).has_value());
        }

        SessionEvent event;
        event.kind = kFutureKind;
        event.monotonicNs = kFirstNs + 1;
        event.wallNs = kFirstNs + 1;
        UnknownEventData body;
        body.body = futureBody;
        event.body = std::move(body);
        REQUIRE((*writer)->recordEvent(event).has_value());

        REQUIRE((*writer)->close().has_value());
    }

    auto reader = SessionReader::open(source);
    REQUIRE(reader.has_value());

    const auto found =
        std::find_if((*reader)->events().begin(), (*reader)->events().end(),
                     [](const SessionEvent& event) { return event.kind == kFutureKind; });
    REQUIRE(found != (*reader)->events().end());
    // Surfaced as a number rather than forced into a neighbouring meaning.
    CHECK(eventKindName(found->kind) == "unknown");
    REQUIRE(found->as<UnknownEventData>() != nullptr);
    CHECK(found->as<UnknownEventData>()->body == futureBody);

    HistoryQuery range;
    range.fromNs = 0;
    range.toNs = std::numeric_limits<std::uint64_t>::max();
    REQUIRE((*reader)->extract(extracted, range).has_value());

    auto cut = SessionReader::open(extracted);
    REQUIRE(cut.has_value());

    const auto copied =
        std::find_if((*cut)->events().begin(), (*cut)->events().end(),
                     [](const SessionEvent& event) { return event.kind == kFutureKind; });
    REQUIRE(copied != (*cut)->events().end());

    // Byte for byte, which is the property the verbatim copy exists for: the
    // extract carries content the build that wrote it could not interpret.
    std::vector<std::byte> before;
    std::vector<std::byte> after;
    encodeEvent(before, *found);
    encodeEvent(after, *copied);
    CHECK(before == after);
    CHECK(copied->as<UnknownEventData>() != nullptr);
    CHECK(copied->as<UnknownEventData>()->body == futureBody);
    CHECK(copied->monotonicNs == found->monotonicNs);
}

TEST_CASE("a record payload with unknown trailing bytes decodes and ignores the tail") {
    // The second extensibility guarantee: a decoder reads the fields it knows
    // and ignores any tail, which is what makes appending a field to
    // SegmentOpen invisible to an older reader.
    //
    // Exercised directly against the decoder, because synthesising a whole
    // SegmentOpen record with a longer payload would test the writer instead.
    std::vector<std::byte> payload;
    writeF64(payload, 1.5);
    writeU32(payload, 42);
    writeString(payload, "known");
    // What a future minor version appended, and this reader knows nothing of.
    writeF64(payload, 99.5);
    writeString(payload, "an added field");

    ByteReader reader(payload.data(), payload.size());
    CHECK(reader.readF64().value() == doctest::Approx(1.5));
    CHECK(reader.readU32().value() == 42);
    CHECK(reader.readString().value() == "known");

    // The decoder stops here and never checks for exhaustion -- which is
    // precisely the contract.
    CHECK_FALSE(reader.exhausted());
    CHECK(reader.remaining() > 0);
}

TEST_CASE("a truncated header is refused rather than read as zeros") {
    // The version fields sit at bytes 24-31, so a file shorter than the header
    // must not be read as though those bytes were present and zero.
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("sweepsfile-short-" + std::to_string(monotonicNs()) + ".sweeps");
    {
        std::ofstream out(path, std::ios::binary);
        const char header[] = {'S', 'W', 'P', 'P', 1, 0, 0, 0};
        out.write(header, sizeof(header));
    }

    auto reader = SessionReader::open(path);
    REQUIRE_FALSE(reader.has_value());
    CHECK(reader.error().code() == ErrorCode::Corrupt);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
