// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "GoldenSession.hpp"

#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/history/SessionReader.hpp>
#include <vector>

using namespace sweeppp;

namespace {

std::vector<std::uint8_t> readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

std::string describeFirstDifference(const std::vector<std::uint8_t>& expected,
                                    const std::vector<std::uint8_t>& actual) {
    const std::size_t common = std::min(expected.size(), actual.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (expected[i] != actual[i]) {
            return std::format("first difference at offset {} (0x{:X}): golden 0x{:02X}, "
                               "written 0x{:02X}; sizes {} vs {}",
                               i, i, expected[i], actual[i], expected.size(), actual.size());
        }
    }
    if (expected.size() != actual.size()) {
        return std::format("identical for {} bytes, then lengths diverge: golden {}, written {}",
                           common, expected.size(), actual.size());
    }
    return "identical";
}

class ScopedTempFile {
public:
    explicit ScopedTempFile(std::string_view name)
        : m_path(std::filesystem::temp_directory_path() /
                 std::format("sweeppp-golden-{}-{}", monotonicNs(), name)) {}

    ~ScopedTempFile() {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    ScopedTempFile(const ScopedTempFile&) = delete;
    ScopedTempFile& operator=(const ScopedTempFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

} // namespace

TEST_CASE("the recorder writes the same bytes as the library's synchronous writer") {
    // The writer split's correctness condition, stated as a test.
    //
    // libsweepsfile's own suite drives the *synchronous* writer through an
    // equivalent fixture and compares against this same committed file. Both
    // passing means the recorder's queue, its frame-to-view translation and its
    // deferral of segment-boundary events reproduce exactly what the single
    // asynchronous writer produced before it was split -- rather than something
    // that merely looks equivalent.
    //
    // This is also the backward-compatibility claim: the committed file was
    // written by the pre-extraction code, and both paths still reproduce it.
    const ScopedTempFile written("recorder.sweeps");
    REQUIRE(golden::writeGoldenSession(written.path()).has_value());

    const std::filesystem::path reference = golden::goldenPath();
    REQUIRE_MESSAGE(std::filesystem::exists(reference),
                    "golden file missing: " << reference.string());

    const std::vector<std::uint8_t> expected = readAll(reference);
    const std::vector<std::uint8_t> actual = readAll(written.path());

    REQUIRE_FALSE(expected.empty());
    CHECK_MESSAGE(expected == actual, describeFirstDifference(expected, actual));
}

TEST_CASE("the recorder writes deterministically across runs") {
    // Byte-equality against a committed file only proves anything if the path
    // under test is deterministic. A clock read or a dropped frame leaking into
    // the output would show up here rather than as a golden failure that "goes
    // away on a rerun".
    const ScopedTempFile first("first.sweeps");
    const ScopedTempFile second("second.sweeps");

    REQUIRE(golden::writeGoldenSession(first.path()).has_value());
    REQUIRE(golden::writeGoldenSession(second.path()).has_value());

    const std::vector<std::uint8_t> a = readAll(first.path());
    const std::vector<std::uint8_t> b = readAll(second.path());
    REQUIRE_FALSE(a.empty());
    CHECK_MESSAGE(a == b, describeFirstDifference(a, b));
}

TEST_CASE("the reference session opens through Sweep++'s own reader") {
    // The reader half, from this side of the library boundary: the same file the
    // recorder is checked against must also read back as a complete v1 session.
    auto reader = session::SessionReader::open(golden::goldenPath());
    REQUIRE(reader.has_value());

    const session::SessionSummary& summary = (*reader)->summary();
    CHECK(summary.majorVersion == sweeps::kMajorVersion);
    CHECK(summary.minorVersion == sweeps::kMinorVersion);
    CHECK(summary.incompatibleFeatures == 0);
    CHECK_FALSE(summary.newerMinorVersion);
    CHECK_FALSE(summary.recoveredByScan);
    CHECK(summary.name == golden::kSessionName);
    CHECK(summary.totalLines == golden::kWideLines + golden::kNarrowLines);
    CHECK((*reader)->verify().has_value());
}
