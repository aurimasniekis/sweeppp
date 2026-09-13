// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The device listing, without a radio.
//
// A radio is opened by its position in its library's list, so a listing read
// one entry out does not fail: it opens the neighbouring radio under the
// requested one's name. Nothing on a desk with one Fobos on it provokes that,
// which is why the format's corners are pinned here.
#include "FobosSerials.hpp"

#include <doctest/doctest.h>
#include <string>
#include <vector>

using namespace sweeppp::fobos;

TEST_CASE("a listing splits into one serial per radio") {
    const std::vector<std::string> serials = parseSerials("A1B2C3 D4E5F6 ", 2);
    REQUIRE(serials.size() == 2);
    CHECK(serials[0] == "A1B2C3");
    CHECK(serials[1] == "D4E5F6");
}

TEST_CASE("a radio with no serial string keeps its position") {
    // Opened, but its descriptor read nothing -- so the library appended only
    // the separator. Collapsing the two spaces would move the third radio to
    // the second radio's index.
    const std::vector<std::string> serials = parseSerials("A1  C3 ", 3);
    REQUIRE(serials.size() == 3);
    CHECK(serials[0] == "A1");
    CHECK(serials[1].empty());
    CHECK(serials[2] == "C3");
}

TEST_CASE("the library's placeholder for an unopenable radio reads as no serial") {
    const std::vector<std::string> serials = parseSerials("XXXXXXXXXXXX A1 ", 2);
    REQUIRE(serials.size() == 2);
    CHECK(serials[0].empty());
    CHECK(serials[1] == "A1");
}

TEST_CASE("the answer is exactly as long as the library's count") {
    SUBCASE("a listing cut short is padded") {
        const std::vector<std::string> serials = parseSerials("A1 ", 3);
        REQUIRE(serials.size() == 3);
        CHECK(serials[0] == "A1");
        CHECK(serials[2].empty());
    }

    SUBCASE("entries past the count are ignored") {
        const std::vector<std::string> serials = parseSerials("A1 B2 C3 ", 2);
        REQUIRE(serials.size() == 2);
        CHECK(serials[1] == "B2");
    }

    SUBCASE("a missing trailing space loses nothing") {
        const std::vector<std::string> serials = parseSerials("A1 B2", 2);
        REQUIRE(serials.size() == 2);
        CHECK(serials[1] == "B2");
    }

    SUBCASE("nothing listed") {
        CHECK(parseSerials("", 0).empty());
    }
}

TEST_CASE("a readable, unique serial is the id") {
    const std::vector<ListedRadio> radios = listedRadios({"A1"}, {"B2"});
    REQUIRE(radios.size() == 2);

    CHECK(radios[0].firmware == Firmware::Standard);
    CHECK(radios[0].index == 0);
    CHECK(radios[0].id == "A1");

    // Each library counts from zero, so the agile radio is index 0 of its own
    // list however many standard ones precede it.
    CHECK(radios[1].firmware == Firmware::Agile);
    CHECK(radios[1].index == 0);
    CHECK(radios[1].id == "B2");
}

TEST_CASE("a radio with no serial is named by its position") {
    const std::vector<ListedRadio> radios = listedRadios({"A1", ""}, {""});
    REQUIRE(radios.size() == 3);
    CHECK(radios[1].id == "index-1");
    CHECK(radios[1].serial.empty());
    // Prefixed, or the agile list's first radio would share the standard
    // list's first position name.
    CHECK(radios[2].id == "agile-index-0");
}

TEST_CASE("a serial two radios share is not an id for either") {
    // Across the two lists as well as within one: a serial names one radio or
    // it names none.
    const std::vector<ListedRadio> radios = listedRadios({"SAME", "A1"}, {"SAME"});
    REQUIRE(radios.size() == 3);
    CHECK(radios[0].id == "index-0");
    CHECK(radios[1].id == "A1");
    CHECK(radios[2].id == "agile-index-0");
    // The serial itself is still reported; only the id avoids it.
    CHECK(radios[0].serial == "SAME");
}
