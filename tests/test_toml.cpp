// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/core/Paths.hpp>
#include <sweeppp/core/Toml.hpp>

using namespace sweeppp;
namespace tu = sweeppp::toml_util;

namespace {

/// Temporary directory that removes itself, so a failing test cannot leave
/// files behind that make the next run pass for the wrong reason.
class ScopedTempDir {
public:
    ScopedTempDir()
        : m_path(std::filesystem::temp_directory_path() /
                 std::format("sweeppp-test-{}", monotonicNs())) {
        std::filesystem::create_directories(m_path);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

} // namespace

TEST_CASE("frequency parsing accepts the spellings people actually write") {
    struct Case {
        const char* text;
        double hz;
    };

    const Case cases[] = {
        {"100", 100.0},        {"100e6", 100e6}, {"2.4G", 2.4e9},    {"2.4 GHz", 2.4e9},
        {"868.3MHz", 868.3e6}, {"433m", 433e6},  {"1_000_000", 1e6}, {"12.5k", 12.5e3},
        {"1T", 1e12},          {"-5M", -5e6},
    };

    for (const Case& testCase : cases) {
        CAPTURE(testCase.text);
        const auto parsed = tu::parseFrequency(testCase.text);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == doctest::Approx(testCase.hz));
    }

    // Trailing junk is an error, not a silent truncation.
    CHECK_FALSE(tu::parseFrequency("2.4GG").has_value());
    CHECK_FALSE(tu::parseFrequency("").has_value());
    CHECK_FALSE(tu::parseFrequency("wideband").has_value());
}

TEST_CASE("duration parsing handles units") {
    CHECK(tu::parseDuration("250ms").value() == doctest::Approx(0.25));
    CHECK(tu::parseDuration("1.5s").value() == doctest::Approx(1.5));
    CHECK(tu::parseDuration("2 min").value() == doctest::Approx(120.0));
    CHECK(tu::parseDuration("1h").value() == doctest::Approx(3600.0));
    CHECK(tu::parseDuration("500us").value() == doctest::Approx(0.0005));
    CHECK(tu::parseDuration("30").value() == doctest::Approx(30.0));
    CHECK_FALSE(tu::parseDuration("soon").has_value());
}

TEST_CASE("frequency formatting picks a readable unit") {
    CHECK(tu::formatFrequencyShort(2.4e9) == "2.4 GHz");
    CHECK(tu::formatFrequencyShort(868.3e6) == "868.3 MHz");
    CHECK(tu::formatFrequencyShort(12.5e3) == "12.5 kHz");
    CHECK(tu::formatFrequency(2.4e9, 3) == "2.400 GHz");
}

TEST_CASE("byte formatting uses binary for sizes and decimal for rates") {
    CHECK(tu::formatBytes(1024) == "1.0 KiB");
    CHECK(tu::formatBytes(1536 * 1024) == "1.5 MiB");
    CHECK(tu::formatByteRate(1.7e6) == "1.70 MB/s");
    CHECK(tu::formatByteRate(40e3) == "40.0 kB/s");
}

TEST_CASE("dotted-path lookup reaches nested tables") {
    const auto table = tu::parse(R"(
[theme]
name = "Sweep Dark"

[theme.spectrum]
trace_live = "#4ADE80"
grid_alpha = 0.35
points = 2048
enabled = true
)",
                                 "inline");
    REQUIRE(table.has_value());

    CHECK(tu::getString(*table, "theme.name", "") == "Sweep Dark");
    CHECK(tu::getString(*table, "theme.spectrum.trace_live", "") == "#4ADE80");
    CHECK(tu::getDouble(*table, "theme.spectrum.grid_alpha", 0.0) == doctest::Approx(0.35));
    CHECK(tu::getInt(*table, "theme.spectrum.points", 0) == 2048);
    CHECK(tu::getBool(*table, "theme.spectrum.enabled", false));

    // A missing path yields the fallback rather than throwing or asserting.
    CHECK(tu::getString(*table, "theme.spectrum.absent", "default") == "default");
    CHECK(tu::getInt(*table, "no.such.path", -1) == -1);

    // require* names the key it could not read.
    const auto missing = tu::requireString(*table, "theme.spectrum.absent");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().message().find("theme.spectrum.absent") != std::string::npos);
}

TEST_CASE("parse errors report line and column") {
    const auto bad = tu::parse("key = = 5\n", "broken.toml");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code() == ErrorCode::ParseError);
    CHECK(bad.error().message().find("broken.toml") != std::string::npos);
}

TEST_CASE("every SdrValue-shaped scalar round-trips through a saved file") {
    const ScopedTempDir temp;
    const std::filesystem::path file = temp.path() / "profile.toml";

    // The four SdrValue alternatives map onto TOML's native scalar types with
    // no encoding tricks, which is the property this test pins down. If any of
    // them needed a string wrapper, profiles would stop being hand-editable.
    toml::table root;
    toml::table& device = tu::ensureTable(root, "device.parameters");
    device.insert_or_assign("bias_tee", true);
    device.insert_or_assign("lna_gain", std::int64_t{24});
    device.insert_or_assign("center_hz", 2.4e9);
    device.insert_or_assign("antenna", "RX1");

    toml::table& theme = tu::ensureTable(root, "theme.spectrum");
    theme.insert_or_assign("trace_live", "#4ADE80");
    theme.insert_or_assign("fill_colormap", "jet");

    toml::table& waterfall = tu::ensureTable(root, "theme.waterfall");
    waterfall.insert_or_assign("colormap", "jet");

    REQUIRE(tu::save(file, root, "Sweep++ test profile").has_value());
    REQUIRE(std::filesystem::exists(file));

    const auto reloaded = tu::load(file);
    REQUIRE(reloaded.has_value());

    CHECK(tu::getBool(*reloaded, "device.parameters.bias_tee", false) == true);
    CHECK(tu::getInt(*reloaded, "device.parameters.lna_gain", 0) == 24);
    CHECK(tu::getDouble(*reloaded, "device.parameters.center_hz", 0.0) == doctest::Approx(2.4e9));
    CHECK(tu::getString(*reloaded, "device.parameters.antenna", "") == "RX1");

    CHECK(tu::getString(*reloaded, "theme.spectrum.trace_live", "") == "#4ADE80");
    CHECK(tu::getString(*reloaded, "theme.spectrum.fill_colormap", "") == "jet");
    CHECK(tu::getString(*reloaded, "theme.waterfall.colormap", "") == "jet");
}

TEST_CASE("saving is atomic and leaves no temporary behind") {
    const ScopedTempDir temp;
    const std::filesystem::path file = temp.path() / "nested" / "dir" / "settings.toml";

    toml::table root;
    root.insert_or_assign("value", std::int64_t{1});
    REQUIRE(tu::save(file, root).has_value());

    // Parent directories are created, and the .tmp used for the atomic rename
    // must not survive.
    CHECK(std::filesystem::exists(file));
    CHECK_FALSE(std::filesystem::exists(file.string() + ".tmp"));

    // Overwriting keeps the file valid at every instant.
    root.insert_or_assign("value", std::int64_t{2});
    REQUIRE(tu::save(file, root).has_value());
    CHECK(tu::load(file).value()["value"].value_or(0) == 2);
}

TEST_CASE("a header comment survives serialisation without breaking the parse") {
    toml::table root;
    root.insert_or_assign("name", "example");

    const std::string text = tu::toString(root, "Generated by Sweep++\nDo not edit while running");
    CHECK(text.starts_with("# Generated by Sweep++\n# Do not edit while running\n"));

    const auto reparsed = tu::parse(text, "generated");
    REQUIRE(reparsed.has_value());
    CHECK(tu::getString(*reparsed, "name", "") == "example");
}

TEST_CASE("arrays read back as vectors") {
    const auto table = tu::parse(R"(
stops = [0.0, 0.35, 0.55, 1.0]
names = ["jet", "turbo", "viridis"]
)",
                                 "inline");
    REQUIRE(table.has_value());

    const std::vector<double> stops = tu::getDoubleArray(*table, "stops");
    REQUIRE(stops.size() == 4);
    CHECK(stops[1] == doctest::Approx(0.35));

    const std::vector<std::string> names = tu::getStringArray(*table, "names");
    REQUIRE(names.size() == 3);
    CHECK(names[2] == "viridis");

    CHECK(tu::getDoubleArray(*table, "absent").empty());
}

TEST_CASE("a frequency key accepts both a number and a suffixed string") {
    const auto table = tu::parse(R"(
numeric = 2.4e9
suffixed = "2.4 GHz"
)",
                                 "inline");
    REQUIRE(table.has_value());

    const auto numeric = tu::frequencyFrom(tu::at(*table, "numeric"), "numeric");
    const auto suffixed = tu::frequencyFrom(tu::at(*table, "suffixed"), "suffixed");
    REQUIRE(numeric.has_value());
    REQUIRE(suffixed.has_value());
    CHECK(*numeric == doctest::Approx(*suffixed));

    // A bad value names its key so the user can find it in the file.
    const auto bad = tu::frequencyFrom(tu::at(*table, "absent"), "sweep.segments[0].start");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().message().find("sweep.segments[0].start") != std::string::npos);
}

TEST_CASE("loading a missing file reports NotFound") {
    const auto missing = tu::load("/nonexistent/path/to/settings.toml");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code() == ErrorCode::NotFound);
}

TEST_CASE("resource search path puts user overrides ahead of built-ins") {
    const ScopedTempDir temp;
    Paths::setConfigDirOverride(temp.path() / "config");

    // instance() caches, so this only takes effect in a process that has not
    // resolved paths yet. The ordering property is what matters here.
    const Paths& paths = Paths::instance();
    const std::vector<std::filesystem::path> searchPath = paths.searchPath("themes");
    REQUIRE(searchPath.size() == 2);
    CHECK(searchPath[0] == paths.configDir() / "themes");
    CHECK(searchPath[1] == paths.resourcesDir() / "themes");
}
