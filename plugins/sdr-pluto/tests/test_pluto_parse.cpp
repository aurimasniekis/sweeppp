// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The strings a Pluto and libiio describe themselves with. The samples are
// copied from real radios and from libiio's own source.
#include "PlutoParse.hpp"

#include <doctest/doctest.h>

using namespace sweeppp::pluto;

TEST_CASE("the serial comes out of a USB scan description") {
    CHECK(serialFromDescription("0456:b673 (Analog Devices Inc. PlutoSDR (ADALM-PLUTO)), "
                                "serial=1044730a199700191f001800f3d5c9e6ce") ==
          "1044730a199700191f001800f3d5c9e6ce");
    CHECK(serialFromDescription("0456:b673 (Analog Devices Inc. PlutoSDR (ADALM-PLUTO)), "
                                "serial=") == "");
    CHECK(serialFromDescription("0456:b673 (Analog Devices Inc. PlutoSDR (ADALM-PLUTO))") == "");
}

TEST_CASE("a URI is told apart from a serial") {
    CHECK(isUri("ip:192.168.2.1"));
    CHECK(isUri("ip:pluto.local"));
    CHECK(isUri("usb:1.5.5"));
    CHECK_FALSE(isUri("1044730a199700191f001800f3d5c9e6ce"));
    CHECK_FALSE(isUri(""));

    CHECK(isNetworkUri("ip:192.168.2.1"));
    CHECK_FALSE(isNetworkUri("usb:1.5.5"));
}

TEST_CASE("an IIO range reads as min, step and max") {
    const auto lo = parseRange("[70000000 1 6000000000]");
    REQUIRE(lo.has_value());
    CHECK(lo->min == 70e6);
    CHECK(lo->step == 1.0);
    CHECK(lo->max == 6e9);

    const auto gain = parseRange("[-3 1 71]");
    REQUIRE(gain.has_value());
    CHECK(gain->min == -3.0);
    CHECK(gain->max == 71.0);

    // A trailing newline is how sysfs hands every attribute over.
    CHECK(parseRange("[2083333 1 61440000]\n").has_value());
}

TEST_CASE("a value list or a malformed range is not a range") {
    CHECK_FALSE(parseRange("manual fast_attack slow_attack hybrid").has_value());
    CHECK_FALSE(parseRange("[1 2]").has_value());
    CHECK_FALSE(parseRange("[1 2 3 4]").has_value());
    CHECK_FALSE(parseRange("[10 1 5]").has_value());
    CHECK_FALSE(parseRange("[a 1 5]").has_value());
    CHECK_FALSE(parseRange("").has_value());
}

TEST_CASE("the gain modes split on whitespace") {
    const auto modes = splitWords("manual fast_attack slow_attack hybrid\n");
    REQUIRE(modes.size() == 4);
    CHECK(modes[0] == "manual");
    CHECK(modes[3] == "hybrid");
    CHECK(splitWords("").empty());
    CHECK(splitWords("  ").empty());
}

TEST_CASE("the hardware model gives the revision and the RF chip") {
    const HardwareModel revC = parseHardwareModel("Analog Devices PlutoSDR Rev.C (Z7010-AD9363A)");
    CHECK(revC.revision == "Rev.C");
    CHECK(revC.chip == "AD9363A");

    const HardwareModel bare = parseHardwareModel("PlutoSDR");
    CHECK(bare.revision.empty());
    CHECK(bare.chip.empty());
}
