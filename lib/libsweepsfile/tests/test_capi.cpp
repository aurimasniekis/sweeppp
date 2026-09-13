// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

/// The C ABI's edges, asserted from C++.
///
/// `test_capi.c` is the conformance half -- it reads the golden file through
/// the C header and must agree with `test_golden.cpp` about what is in it. This
/// file is the other half: the failure paths, and the claim that the two APIs
/// describe one file rather than two. Both are far easier to write here, where
/// there is a test framework and the C++ API to compare against.

#include "GoldenSession.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <sweeps/SessionReader.hpp>
#include <sweeps/sweeps.h>
#include <vector>

using namespace sweeps::test;

namespace {

/// RAII around the C reader, so a failing CHECK cannot leak the handle.
class CReader {
public:
    explicit CReader(const std::string& path) {
        m_status = sweeps_reader_open(path.c_str(), &m_handle);
    }

    ~CReader() { sweeps_reader_close(m_handle); }

    CReader(const CReader&) = delete;
    CReader& operator=(const CReader&) = delete;

    [[nodiscard]] sweeps_status_t status() const noexcept { return m_status; }
    [[nodiscard]] sweeps_reader_t* get() const noexcept { return m_handle; }

private:
    sweeps_reader_t* m_handle = nullptr;
    sweeps_status_t m_status = SWEEPS_ERR_UNKNOWN;
};

std::string toString(sweeps_str_t str) {
    return std::string(str.data, str.len);
}

} // namespace

TEST_CASE("the header is usable from C++ and its entry points are noexcept") {
    // SWEEPS_NOEXCEPT has to expand to `noexcept` here, because since C++17 the
    // exception specification is part of a function's type: if it did not, the
    // definitions in CApi.cpp would not match these declarations, and this
    // translation unit is the only thing that would notice.
    static_assert(noexcept(sweeps_abi_version()), "entry points must be noexcept in C++");
    static_assert(noexcept(sweeps_last_error()), "entry points must be noexcept in C++");

    CHECK(sweeps_abi_version() == SWEEPS_ABI_VERSION);
}

TEST_CASE("every status has its own name") {
    // Not decoration. The C status values are assigned by hand rather than
    // derived from ErrorCode's ordinal, precisely so inserting an enumerator in
    // the middle of that enum cannot renumber this ABI -- which means nothing
    // but a test keeps the two lists in step.
    const sweeps_status_t all[] = {
        SWEEPS_OK,
        SWEEPS_ERR_UNKNOWN,
        SWEEPS_ERR_INVALID_ARGUMENT,
        SWEEPS_ERR_NOT_FOUND,
        SWEEPS_ERR_UNSUPPORTED,
        SWEEPS_ERR_UNAVAILABLE,
        SWEEPS_ERR_IO,
        SWEEPS_ERR_PARSE,
        SWEEPS_ERR_OUT_OF_RANGE,
        SWEEPS_ERR_OUT_OF_MEMORY,
        SWEEPS_ERR_TIMED_OUT,
        SWEEPS_ERR_CANCELLED,
        SWEEPS_ERR_PERMISSION_DENIED,
        SWEEPS_ERR_ALREADY_EXISTS,
        SWEEPS_ERR_DEVICE,
        SWEEPS_ERR_PROTOCOL,
        SWEEPS_ERR_CORRUPT,
        SWEEPS_ERR_WRONG_TYPE,
    };

    std::set<std::string> names;
    for (const sweeps_status_t status : all) {
        const char* name = sweeps_status_name(status);
        REQUIRE(name != nullptr);
        CHECK(names.insert(name).second);
    }
    CHECK(names.size() == sizeof(all) / sizeof(all[0]));

    // A value from a newer library still answers rather than reading past the
    // switch.
    CHECK(sweeps_status_name(static_cast<sweeps_status_t>(-424242)) != nullptr);
}

TEST_CASE("a failure leaves a message, and a success does not erase it") {
    sweeps_reader_t* reader = nullptr;
    const sweeps_status_t status =
        sweeps_reader_open("/definitely/not/here/session.sweeps", &reader);

    CHECK(status != SWEEPS_OK);
    CHECK(reader == nullptr);

    const sweeps_str_t message = sweeps_last_error();
    REQUIRE(message.data != nullptr);
    CHECK(message.len > 0);
    CHECK(message.data[message.len] == '\0');
    // The status says how to branch; this says which file. A code alone would
    // have thrown away the half an operator needs.
    CHECK(toString(message).find("session.sweeps") != std::string::npos);

    // Deliberately not cleared on success: the status is the discriminant, and
    // clearing would put a thread-local write on every hot-path accessor.
    const std::string before = toString(sweeps_last_error());
    CHECK(sweeps_abi_version() == SWEEPS_ABI_VERSION);
    CHECK(toString(sweeps_last_error()) == before);
}

TEST_CASE("a null or stale handle is diagnosed rather than dereferenced") {
    sweeps_summary_t summary{};
    summary.struct_size = sizeof(summary);

    CHECK(sweeps_reader_summary(nullptr, &summary) == SWEEPS_ERR_INVALID_ARGUMENT);
    CHECK(sweeps_reader_verify(nullptr, nullptr) == SWEEPS_ERR_INVALID_ARGUMENT);
    CHECK(sweeps_reader_manifest(nullptr) == nullptr);
    CHECK(sweeps_reader_event_at(nullptr, 0) == nullptr);
    CHECK(sweeps_reader_plugin_at(nullptr, 0) == nullptr);
    CHECK(sweeps_tiles_count(nullptr) == 0);
    CHECK(sweeps_tiles_at(nullptr, 0) == nullptr);
    CHECK(sweeps_tile_data(nullptr, nullptr) == nullptr);
    CHECK(sweeps_writer_bytes_written(nullptr) == 0);
    CHECK(sweeps_writer_close(nullptr) == SWEEPS_ERR_INVALID_ARGUMENT);
    CHECK(sweeps_metadata_count(nullptr) == 0);
    CHECK(sweeps_metadata_type_of(nullptr, "k") == SWEEPS_VALUE_ABSENT);

    // Every release path is null-safe, so a binding's cleanup does not have to
    // guard.
    sweeps_reader_close(nullptr);
    sweeps_writer_destroy(nullptr);
    sweeps_tiles_free(nullptr);
    sweeps_metadata_destroy(nullptr);
}

TEST_CASE("struct_size governs both directions") {
    const CReader reader(goldenPath().string());
    REQUIRE(reader.status() == SWEEPS_OK);

    SUBCASE("a zeroed struct is refused rather than answered") {
        // The reason the field exists. `sweeps_query_t q = {0}` is the C
        // reflex, and a zeroed query means "up to nanosecond zero" -- which
        // would return nothing, successfully, forever.
        sweeps_query_t query{};
        sweeps_tiles_t* tiles = nullptr;
        CHECK(sweeps_reader_query(reader.get(), &query, &tiles) == SWEEPS_ERR_INVALID_ARGUMENT);
        CHECK(tiles == nullptr);

        sweeps_summary_t summary{};
        CHECK(sweeps_reader_summary(reader.get(), &summary) == SWEEPS_ERR_INVALID_ARGUMENT);
    }

    SUBCASE("an older caller gets the prefix it declared, and is told how much") {
        // The forward-compatibility case: a caller compiled against a header
        // with fewer fields than this library has.
        constexpr std::size_t kShort = offsetof(sweeps_summary_t, total_lines);

        std::vector<unsigned char> raw(sizeof(sweeps_summary_t), 0xCD);
        auto* summary = reinterpret_cast<sweeps_summary_t*>(raw.data());
        summary->struct_size = static_cast<std::uint32_t>(kShort);

        REQUIRE(sweeps_reader_summary(reader.get(), summary) == SWEEPS_OK);
        CHECK(summary->struct_size == kShort);
        CHECK(summary->major_version == sweeps::kMajorVersion);
        CHECK(summary->created_wall_ns == kCreatedWallNs);

        for (std::size_t i = kShort; i < raw.size(); ++i) {
            CHECK(raw[i] == 0xCD);
        }
    }

    SUBCASE("a newer caller's input is refused rather than truncated") {
        // The other direction, and the one that must fail loudly: the caller
        // set fields this library would drop on the floor.
        sweeps_query_t query;
        sweeps_query_init(&query);
        query.struct_size = sizeof(query) + 4;

        sweeps_tiles_t* tiles = nullptr;
        CHECK(sweeps_reader_query(reader.get(), &query, &tiles) == SWEEPS_ERR_INVALID_ARGUMENT);
        CHECK(tiles == nullptr);
        CHECK(toString(sweeps_last_error()).find("newer") != std::string::npos);
    }
}

TEST_CASE("a query built by hand and one built by _init are not the same query") {
    // The trap `sweeps_query_init` exists for, made visible. Both queries below
    // are well-formed; only one of them asks for the session.
    const CReader reader(goldenPath().string());
    REQUIRE(reader.status() == SWEEPS_OK);

    sweeps_query_t zeroed{};
    zeroed.struct_size = sizeof(zeroed);

    sweeps_tiles_t* none = nullptr;
    REQUIRE(sweeps_reader_query(reader.get(), &zeroed, &none) == SWEEPS_OK);
    REQUIRE(none != nullptr);
    CHECK(sweeps_tiles_count(none) == 0);
    sweeps_tiles_free(none);

    sweeps_query_t initialised;
    sweeps_query_init(&initialised);

    sweeps_tiles_t* all = nullptr;
    REQUIRE(sweeps_reader_query(reader.get(), &initialised, &all) == SWEEPS_OK);
    REQUIRE(all != nullptr);
    CHECK(sweeps_tiles_count(all) > 0);
    sweeps_tiles_free(all);
}

TEST_CASE("a typed accessor answers for its own body only") {
    const CReader reader(goldenPath().string());
    REQUIRE(reader.status() == SWEEPS_OK);

    const std::size_t count = sweeps_reader_event_count(reader.get());
    REQUIRE(count > 0);

    bool checkedOne = false;
    for (std::size_t i = 0; i < count; ++i) {
        const sweeps_event_t* event = sweeps_reader_event_at(reader.get(), i);
        REQUIRE(event != nullptr);
        if (sweeps_event_kind(event) != SWEEPS_EVENT_MARKER) {
            continue;
        }

        sweeps_str_t label{};
        double hz = 0.0;
        double dbm = 0.0;
        CHECK(sweeps_event_marker(event, &label, &hz, &dbm) == SWEEPS_OK);
        CHECK(toString(label) == "m1");

        // Every other kind's accessor refuses. No coercion, and in particular no
        // plausible-looking reading of a neighbouring field.
        CHECK(sweeps_event_retune(event, &hz, nullptr) == SWEEPS_ERR_WRONG_TYPE);
        CHECK(sweeps_event_annotation(event, &label, &hz, &dbm) == SWEEPS_ERR_WRONG_TYPE);
        CHECK(sweeps_event_sweep_pass(event, nullptr, nullptr, nullptr, nullptr) ==
              SWEEPS_ERR_WRONG_TYPE);
        CHECK(sweeps_event_segment_boundary(event, &label) == SWEEPS_ERR_WRONG_TYPE);
        CHECK(sweeps_event_device_error(event, &label, &label) == SWEEPS_ERR_WRONG_TYPE);
        CHECK(sweeps_event_plugin(event, &label, &label, nullptr) == SWEEPS_ERR_WRONG_TYPE);
        // A known kind is not "unknown", even though both are readable.
        CHECK(sweeps_event_unknown_body(event, nullptr) == SWEEPS_ERR_WRONG_TYPE);

        // The label is still the one from the successful call: a refused
        // accessor writes nothing.
        CHECK(toString(label) == "m1");
        checkedOne = true;
        break;
    }
    CHECK(checkedOne);
}

TEST_CASE("absent and empty stay distinct in metadata") {
    // The distinction `Metadata::find` draws, carried across the boundary. A
    // value-returning accessor cannot express it -- it has to return the
    // fallback either way -- which is why the pointer-shaped ones return a
    // status instead.
    sweeps_metadata_t* built = sweeps_metadata_create();
    REQUIRE(built != nullptr);

    REQUIRE(sweeps_metadata_set_bytes(built, "empty", nullptr, 0) == SWEEPS_OK);
    REQUIRE(sweeps_metadata_set_string(built, "blank", "") == SWEEPS_OK);

    sweeps_bytes_t bytes{};
    CHECK(sweeps_metadata_get_bytes(built, "empty", &bytes) == SWEEPS_OK);
    CHECK(bytes.len == 0);
    CHECK(sweeps_metadata_get_bytes(built, "missing", &bytes) == SWEEPS_ERR_NOT_FOUND);
    CHECK(sweeps_metadata_get_bytes(built, "blank", &bytes) == SWEEPS_ERR_WRONG_TYPE);

    CHECK(sweeps_metadata_type_of(built, "blank") == SWEEPS_VALUE_STRING);
    CHECK(sweeps_metadata_type_of(built, "missing") == SWEEPS_VALUE_ABSENT);
    CHECK(sweeps_metadata_get_string(built, "blank", "fallback").len == 0);
    CHECK(toString(sweeps_metadata_get_string(built, "missing", "fallback")) == "fallback");

    sweeps_metadata_destroy(built);
}

TEST_CASE("the C and C++ APIs describe one file, not two") {
    // The claim the whole exercise makes. Both read the same frozen bytes, so
    // any disagreement is a defect in the shim rather than a difference of
    // opinion about the format.
    auto cpp = sweeps::SessionReader::open(goldenPath());
    REQUIRE(cpp.has_value());

    const CReader c(goldenPath().string());
    REQUIRE(c.status() == SWEEPS_OK);

    const sweeps::SessionSummary& expected = (*cpp)->summary();

    sweeps_summary_t summary{};
    summary.struct_size = sizeof(summary);
    REQUIRE(sweeps_reader_summary(c.get(), &summary) == SWEEPS_OK);

    CHECK(toString(sweeps_reader_name(c.get())) == expected.name);
    CHECK(toString(sweeps_reader_app_version(c.get())) == expected.appVersion);
    CHECK(summary.created_wall_ns == expected.createdWallNs);
    CHECK(summary.total_lines == expected.totalLines);
    CHECK(summary.total_tiles == expected.totalTiles);
    CHECK(summary.file_bytes == expected.fileBytes);
    CHECK(summary.lowest_hz == doctest::Approx(expected.lowestHz));
    CHECK(summary.highest_hz == doctest::Approx(expected.highestHz));

    REQUIRE(sweeps_reader_segment_count(c.get()) == (*cpp)->segments().size());
    for (std::size_t i = 0; i < (*cpp)->segments().size(); ++i) {
        const sweeps::SegmentInfo& want = (*cpp)->segments()[i];

        sweeps_segment_t got{};
        got.struct_size = sizeof(got);
        REQUIRE(sweeps_reader_segment_at(c.get(), i, &got) == SWEEPS_OK);

        CHECK(got.id == want.id);
        CHECK(got.bin_count == want.grid.binCount);
        CHECK(got.start_hz == doctest::Approx(want.grid.startHz));
        CHECK(got.bin_width_hz == doctest::Approx(want.grid.binWidthHz));
        CHECK(got.line_count == want.lineCount);
        CHECK(got.fft_size == want.config.fftSize);
        CHECK(got.window == static_cast<std::uint32_t>(want.config.window));
        CHECK(got.rbw_hz == doctest::Approx(want.config.rbwHz));
        CHECK(toString(sweeps_reader_segment_reason(c.get(), i)) == want.reason);
        CHECK(toString(sweeps_reader_segment_device_id(c.get(), i)) == want.config.deviceId);
        CHECK(got.gain_count == want.config.gains.size());
    }

    CHECK(sweeps_reader_event_count(c.get()) == (*cpp)->events().size());
    CHECK(sweeps_metadata_count(sweeps_reader_manifest(c.get())) == (*cpp)->manifest().size());

    SUBCASE("and the tile bytes are the same bytes") {
        sweeps_query_t query;
        sweeps_query_init(&query);

        sweeps_tiles_t* tiles = nullptr;
        REQUIRE(sweeps_reader_query(c.get(), &query, &tiles) == SWEEPS_OK);
        REQUIRE(tiles != nullptr);

        const sweeps::HistoryQuery cppQuery;
        auto expectedTiles = (*cpp)->query(cppQuery);
        REQUIRE(expectedTiles.has_value());

        REQUIRE(sweeps_tiles_count(tiles) == expectedTiles->size());
        for (std::size_t i = 0; i < expectedTiles->size(); ++i) {
            const sweeps::HistoryTile& want = (*expectedTiles)[i];
            const sweeps_tile_t* tile = sweeps_tiles_at(tiles, i);
            REQUIRE(tile != nullptr);

            sweeps_tile_info_t info{};
            info.struct_size = sizeof(info);
            REQUIRE(sweeps_tile_info(tile, &info) == SWEEPS_OK);
            CHECK(info.segment_id == want.segmentId);
            CHECK(info.lod == want.lod);
            CHECK(info.lines == want.lines);
            CHECK(info.bins == want.bins);
            CHECK(info.origin_db == want.originDb);

            std::size_t bytes = 0;
            const std::uint8_t* data = sweeps_tile_data(tile, &bytes);
            REQUIRE(data != nullptr);
            REQUIRE(bytes == want.data.size());
            CHECK(std::memcmp(data, want.data.data(), bytes) == 0);
        }

        sweeps_tiles_free(tiles);
    }
}

TEST_CASE("a borrowed view outlives the call that produced it") {
    // The lifetime rule the header states, exercised rather than asserted:
    // every view points into storage owned by the handle, so it stays good
    // until the handle is closed -- not merely until the next call.
    const CReader reader(goldenPath().string());
    REQUIRE(reader.status() == SWEEPS_OK);

    const sweeps_str_t name = sweeps_reader_name(reader.get());
    const sweeps_str_t reason = sweeps_reader_segment_reason(reader.get(), 0);

    // Plenty of unrelated work in between, including allocation.
    std::uint64_t records = 0;
    CHECK(sweeps_reader_verify(reader.get(), &records) == SWEEPS_OK);
    for (int i = 0; i < 64; ++i) {
        sweeps_query_t query;
        sweeps_query_init(&query);
        sweeps_tiles_t* tiles = nullptr;
        REQUIRE(sweeps_reader_query(reader.get(), &query, &tiles) == SWEEPS_OK);
        sweeps_tiles_free(tiles);
    }

    CHECK(toString(name) == kSessionName);
    CHECK(toString(reason) == "session start");
}
