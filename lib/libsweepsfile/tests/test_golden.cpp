// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include "GoldenSession.hpp"

#include <cstdlib>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sweeps/Clock.hpp>
#include <sweeps/Metadata.hpp>
#include <sweeps/SessionReader.hpp>
#include <sweeps/Text.hpp>
#include <system_error>
#include <vector>

using namespace sweeps;
using namespace sweeps::test;

namespace {

std::vector<std::uint8_t> readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

/// Reports the first differing byte rather than "the vectors are not equal".
/// A golden failure is a format regression, and the offset is what says which.
std::string describeFirstDifference(const std::vector<std::uint8_t>& expected,
                                    const std::vector<std::uint8_t>& actual) {
    const std::size_t common = std::min(expected.size(), actual.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (expected[i] != actual[i]) {
            return detail::format("first difference at offset {}: golden {}, written {}; "
                                  "sizes {} vs {}",
                                  i, static_cast<unsigned>(expected[i]),
                                  static_cast<unsigned>(actual[i]), expected.size(), actual.size());
        }
    }
    if (expected.size() != actual.size()) {
        return detail::format("identical for {} bytes, then lengths diverge: golden {}, "
                              "written {}",
                              common, expected.size(), actual.size());
    }
    return "identical";
}

class ScopedTempFile {
public:
    explicit ScopedTempFile(const std::string& name)
        : m_path(std::filesystem::temp_directory_path() /
                 ("sweepsfile-golden-" + std::to_string(monotonicNs()) + "-" + name)) {}

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

TEST_CASE("the writer reproduces the golden file byte for byte") {
    // The tripwire for the format. The manifest, the header, tile ordering and
    // quantisation all feed file bytes, and this is what says whether any of
    // them moved -- which a version number cannot say, because the failure mode
    // is a change nobody thought to bump a version for.
    //
    // Regenerate deliberately with SWEEPSFILE_UPDATE_GOLDEN=1, and only when the
    // format is *meant* to move. Regenerating to make a red test green is the
    // one habit that turns this into decoration.
    const ScopedTempFile written("written.sweeps");
    REQUIRE(writeGoldenSession(written.path()).has_value());

    const std::filesystem::path reference = goldenPath();

    if (const char* update = std::getenv("SWEEPSFILE_UPDATE_GOLDEN");
        update != nullptr && std::string(update) == "1") {
        std::filesystem::create_directories(reference.parent_path());
        std::filesystem::copy_file(written.path(), reference,
                                   std::filesystem::copy_options::overwrite_existing);
        MESSAGE("golden regenerated at " << reference.string());
    }

    REQUIRE_MESSAGE(std::filesystem::exists(reference),
                    "golden file missing: " << reference.string()
                                            << " (regenerate with SWEEPSFILE_UPDATE_GOLDEN=1)");

    const std::vector<std::uint8_t> expected = readAll(reference);
    const std::vector<std::uint8_t> actual = readAll(written.path());

    REQUIRE_FALSE(expected.empty());
    CHECK_MESSAGE(expected == actual, describeFirstDifference(expected, actual));
}

TEST_CASE("the golden file writes deterministically across runs") {
    // Byte-equality against a committed file only proves anything if the writer
    // is deterministic in the first place. A clock or filesystem read that
    // leaked into the output would show up here rather than as a golden failure
    // that "goes away on a rerun".
    const ScopedTempFile first("first.sweeps");
    const ScopedTempFile second("second.sweeps");

    REQUIRE(writeGoldenSession(first.path()).has_value());
    REQUIRE(writeGoldenSession(second.path()).has_value());

    const std::vector<std::uint8_t> a = readAll(first.path());
    const std::vector<std::uint8_t> b = readAll(second.path());
    REQUIRE_FALSE(a.empty());
    CHECK_MESSAGE(a == b, describeFirstDifference(a, b));
}

TEST_CASE("the golden file opens and reads as a v1 session") {
    // Byte-identity would be satisfied by two identically broken files. This is
    // the half that says the bytes mean what they should.
    auto reader = SessionReader::open(goldenPath());
    REQUIRE(reader.has_value());

    const SessionSummary& summary = (*reader)->summary();
    CHECK(summary.name == kSessionName);
    CHECK(summary.appVersion == kApplicationVersion);
    CHECK(summary.createdWallNs == kCreatedWallNs);
    CHECK_FALSE(summary.recoveredByScan);
    CHECK(summary.truncatedBytes == 0);

    REQUIRE((*reader)->segments().size() == 2);
    const std::vector<SegmentInfo>& segments = (*reader)->segments();

    CHECK(segments[0].id == 0);
    CHECK(segments[0].grid.binCount == kWideBins);
    CHECK(segments[0].config.fftSize == 4096);
    CHECK(segments[0].reason == "session start");
    CHECK(segments[0].startWallNs == kCreatedWallNs);

    CHECK(segments[1].id == 1);
    CHECK(segments[1].grid.binCount == kNarrowBins);
    CHECK(segments[1].config.fftSize == 8192);
    CHECK(segments[1].config.deviceLabel == "Golden reference device");
    REQUIRE(segments[1].config.gains.size() == 2);
    CHECK(segments[1].config.gains[0].first == "lna");

    // Two frequency blocks in segment 0, and the pyramid reaching LOD 2 in
    // segment 1: the two structural properties the fixture exists to cover.
    CHECK((*reader)->hasTilesAtLod(0, 0));
    CHECK((*reader)->hasTilesAtLod(1, 2));

    CHECK(summary.totalLines == kWideLines + kNarrowLines);
    CHECK((*reader)->events().size() >= 2);

    // Every record intact, which is a stronger statement than "it parsed".
    auto records = (*reader)->verify();
    REQUIRE(records.has_value());
    CHECK(*records > 0);
}

TEST_CASE("the golden file's manifest is the documented profile") {
    auto reader = SessionReader::open(goldenPath());
    REQUIRE(reader.has_value());

    const Metadata& manifest = (*reader)->manifest();

    // Typed, key by key: Appendix B's profile, as a reader sees it.
    CHECK(manifest.size() == 10);
    CHECK(manifest.getString("name") == kSessionName);
    CHECK(manifest.getString("app_version") == kApplicationVersion);
    CHECK(manifest.getString("created") == "2026-02-02T02:40:00Z");
    CHECK(manifest.getString("notes") == kNotes);
    CHECK(manifest.getInt("format_version") == kMajorVersion);
    CHECK(manifest.getInt("bins_per_line") == kWideBins);
    CHECK(manifest.getInt("tile_bins") == kTileBins);
    CHECK(manifest.getInt("tile_lines") == kTileLines);
    CHECK(manifest.getInt("lod_levels") == kLodLevels);
    CHECK(manifest.getFloat("db_per_step") == doctest::Approx(kDbPerStep));

    // Types are part of the contract, not an accident of how a value was set:
    // `created` is a string so that `strings` on the file still shows a date,
    // and `db_per_step` is a float so that a half never arrives as a zero.
    REQUIRE(manifest.find("created") != nullptr);
    CHECK(manifest.find("created")->type() == Value::Type::String);
    REQUIRE(manifest.find("db_per_step") != nullptr);
    CHECK(manifest.find("db_per_step")->type() == Value::Type::Float);
    REQUIRE(manifest.find("tile_bins") != nullptr);
    CHECK(manifest.find("tile_bins")->type() == Value::Type::Int);
}
