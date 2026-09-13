// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The version helpers, without a radio.
//
// These were unreachable while they lived inside the driver, and one of them
// deserves better than being first exercised on hardware: `plausible()` decides
// whether a table read out of libbladeRF's private data still looks like a
// table. The case it guards against is a layout change upstream, which produces
// not a crash but a confident wrong answer -- "libbladeRF supports firmware up
// to v2371483.0.0" -- and there is no radio in the world that provokes it.
#include "BladeRfVersions.hpp"

#include <array>
#include <cstdint>
#include <doctest/doctest.h>
#include <string_view>

using namespace sweeppp;
using namespace sweeppp::blade;

namespace {

/// One version, spelled out.
///
/// Not `constexpr`: `bladerf_version` carries a `describe` pointer libbladeRF
/// fills in, and a literal of it is not a constant expression. The `struct`
/// tag is required rather than tidy -- libbladeRF has a *function* called
/// `bladerf_version` too, which hides the type.
struct bladerf_version version(std::uint16_t major, std::uint16_t minor, std::uint16_t patch) {
    struct bladerf_version made{};
    made.major = major;
    made.minor = minor;
    made.patch = patch;
    made.describe = nullptr;
    return made;
}

} // namespace

TEST_CASE("an FPGA size names the board variant") {
    CHECK(std::string_view(fpgaVariant(BLADERF_FPGA_40KLE)) == "x40");
    CHECK(std::string_view(fpgaVariant(BLADERF_FPGA_115KLE)) == "x115");
    CHECK(std::string_view(fpgaVariant(BLADERF_FPGA_A4)) == "xA4");
#if SWEEPPP_BLADERF_HAS_FPGA_A5
    CHECK(std::string_view(fpgaVariant(BLADERF_FPGA_A5)) == "xA5");
#endif
    CHECK(std::string_view(fpgaVariant(BLADERF_FPGA_A9)) == "xA9");

    // Unknown is a real answer from a board whose flash has not been read, and
    // it must not become a made-up variant.
    CHECK(fpgaVariant(BLADERF_FPGA_UNKNOWN) == nullptr);
}

TEST_CASE("a model name reads the way an owner would say it") {
    CHECK(modelName("bladerf2", BLADERF_FPGA_A9) == "bladeRF 2.0 micro xA9");
    CHECK(modelName("bladerf1", BLADERF_FPGA_115KLE) == "bladeRF x115");

    // No variant to add, so the name stops rather than gaining a stray space.
    CHECK(modelName("bladerf1", BLADERF_FPGA_UNKNOWN) == "bladeRF");
    CHECK(modelName(nullptr, BLADERF_FPGA_UNKNOWN) == "bladeRF");
}

TEST_CASE("versions render and order by component") {
    CHECK(versionText(version(2, 4, 0)) == "2.4.0");
    CHECK(versionText(version(0, 15, 3)) == "0.15.3");

    CHECK(olderThan(version(2, 3, 9), version(2, 4, 0)));
    CHECK(olderThan(version(1, 99, 99), version(2, 0, 0)));
    CHECK_FALSE(olderThan(version(2, 4, 0), version(2, 4, 0)));

    // Component by component, not lexicographically: "2.10.0" is newer than
    // "2.9.0" even though the string compares the other way.
    CHECK(olderThan(version(2, 9, 0), version(2, 10, 0)));
}

TEST_CASE("a version report says how current the driver believes it is") {
    SUBCASE("nothing known, so nothing claimed") {
        // What an unresolved weak symbol produces. Saying nothing is the point:
        // a missing table must not read as "you are up to date".
        const VersionReport report = describeVersion(version(2, 4, 0), std::nullopt);
        CHECK(report.version == "2.4.0");
        CHECK(report.knownLatest.empty());
        CHECK_FALSE(report.aheadOfDriver);
    }

    SUBCASE("behind what the driver knows of") {
        const VersionReport report = describeVersion(version(2, 2, 0), version(2, 4, 0));
        CHECK(report.version == "2.2.0");
        CHECK(report.knownLatest == "2.4.0");
        CHECK_FALSE(report.aheadOfDriver);
    }

    SUBCASE("level with it") {
        const VersionReport report = describeVersion(version(2, 4, 0), version(2, 4, 0));
        CHECK_FALSE(report.aheadOfDriver);
    }

    SUBCASE("ahead of it") {
        // Not a fault in itself -- but from here libbladeRF is guessing about a
        // device newer than itself, and the panel says so.
        const VersionReport report = describeVersion(version(2, 5, 0), version(2, 4, 0));
        CHECK(report.aheadOfDriver);
    }
}

#ifdef SWEEPPP_BLADERF_COMPAT_TABLES

TEST_CASE("a table that does not read like a table is refused") {
    const std::array kEntries{
        BladeCompatEntry{.version = version(2, 3, 0), .requiredCounterpart = version(0, 14, 0)},
        BladeCompatEntry{.version = version(2, 4, 0), .requiredCounterpart = version(0, 15, 0)},
    };

    SUBCASE("a real one is accepted") {
        const BladeCompatTable table{.entries = kEntries.data(), .length = 2};
        CHECK(plausible(&table));
    }

    SUBCASE("an unresolved weak symbol has no address at all") {
        CHECK_FALSE(plausible(nullptr));
    }

    SUBCASE("no entries") {
        const BladeCompatTable table{.entries = nullptr, .length = 2};
        CHECK_FALSE(plausible(&table));
    }

    SUBCASE("no length") {
        const BladeCompatTable table{.entries = kEntries.data(), .length = 0};
        CHECK_FALSE(plausible(&table));
    }

    SUBCASE("a length nothing could have") {
        // The shape a layout change upstream produces: the struct is read at
        // the wrong offsets, so the length is whatever happened to be there.
        const BladeCompatTable table{.entries = kEntries.data(), .length = 4'000'000};
        CHECK_FALSE(plausible(&table));
    }

    SUBCASE("a version nothing could have") {
        const std::array kNonsense{BladeCompatEntry{.version = version(31337, 0, 0),
                                                    .requiredCounterpart = version(0, 0, 0)}};
        const BladeCompatTable table{.entries = kNonsense.data(), .length = 1};
        CHECK_FALSE(plausible(&table));
    }
}

TEST_CASE("the newest entry is found by scanning, not by position") {
    // The tables happen to be sorted newest-first, and nothing in libbladeRF
    // promises they will stay that way -- so this is deliberately out of order
    // in both directions.
    const std::array kEntries{
        BladeCompatEntry{.version = version(2, 2, 0), .requiredCounterpart = version(0, 13, 0)},
        BladeCompatEntry{.version = version(2, 4, 0), .requiredCounterpart = version(0, 15, 0)},
        BladeCompatEntry{.version = version(2, 3, 0), .requiredCounterpart = version(0, 14, 0)},
    };
    const BladeCompatTable table{.entries = kEntries.data(), .length = 3};

    const auto newest = newestKnown(&table);
    REQUIRE(newest.has_value());
    CHECK(versionText(*newest) == "2.4.0");

    // An implausible table yields nothing rather than a wrong maximum.
    CHECK_FALSE(newestKnown(nullptr).has_value());
}

#endif
