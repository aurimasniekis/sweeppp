// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The band plan's loader, which lives in plugins/bandplan now.
//
// It moved out of libsweeppp, and the drawing it used to feed moved back into
// the host. What did not move is what these tests check: the file format, the
// narrowest-first rule and the overlap-not-containment selection are the
// plugin's contract with the TOML an operator hand-edits, and relocating the
// code is not a licence to change any of them.
#include "BandPlan.hpp"

#include <algorithm>
#include <array>
#include <doctest/doctest.h>
#include <fstream>
#include <set>
#include <string>
#include <sweeppp/core/Paths.hpp>
#include <vector>

using namespace sweeppp;
using namespace bandplan;

namespace {

std::filesystem::path shippedPlan() {
    return Paths::instance().resourcesDir() / "bandplans" / "itu-region-1.toml";
}

} // namespace

TEST_CASE("the shipped band plan parses") {
    const auto plan = BandPlan::load(shippedPlan());
    REQUIRE(plan.has_value());

    CHECK(plan->name() == "ITU Region 1");
    CHECK(plan->bands().size() >= 30);

    // Sorted by start frequency, which the drawing relies on.
    for (std::size_t i = 1; i < plan->bands().size(); ++i) {
        CAPTURE(i);
        CHECK(plan->bands()[i].startHz >= plan->bands()[i - 1].startHz);
    }

    // Every band got a colour from its group rather than the fallback grey.
    for (const Band& band : plan->bands()) {
        CAPTURE(band.name);
        CHECK(band.stopHz > band.startHz);
        CHECK_FALSE(band.group.empty());
    }
}

TEST_CASE("the ECA table parses, and says what is at the frequencies it covers") {
    // Generated from ERC Report 25 rather than written by hand, which is
    // exactly why it is tested: nobody reads five hundred and fifty entries,
    // so the extraction has to be checked by asking it the questions an
    // operator would.
    const auto plan =
        BandPlan::load(Paths::instance().resourcesDir() / "bandplans" / "eca-europe.toml");
    REQUIRE(plan.has_value());

    CHECK(plan->name() == "ECA (Europe)");
    CHECK(plan->bands().size() > 400);

    for (std::size_t i = 1; i < plan->bands().size(); ++i) {
        CAPTURE(i);
        CHECK(plan->bands()[i].startHz >= plan->bands()[i - 1].startHz);
    }

    for (const Band& band : plan->bands()) {
        CAPTURE(band.name);
        CHECK(band.stopHz > band.startHz);
        CHECK_FALSE(band.name.empty());
        CHECK_FALSE(band.group.empty());

        // Every band's colour came from its group rather than the fallback
        // grey, which is what proves the group vocabulary and the palette
        // agree -- a typo in either would show up as a grey span and nowhere
        // else.
        CHECK_FALSE((band.color.r == 0.5F && band.color.g == 0.5F && band.color.b == 0.5F));
    }

    const auto nameAt = [&plan](double hz) {
        const std::vector<const Band*> found = plan->bandsAt(hz);
        return found.empty() ? std::string{} : found.front()->name;
    };

    // Landmarks an operator would notice being wrong.
    CHECK(nameAt(98e6).contains("FM"));
    CHECK(nameAt(145e6).contains("Amateur"));
    CHECK(nameAt(112e6).contains("ILS"));
    CHECK(nameAt(2.44e9).contains("ISM"));

    // The category crosses the ABI into the marker's hover line and heads a
    // branch of the settings tree, so it is written for a reader rather than
    // keyed like an identifier.
    const std::vector<const Band*> broadcast = plan->bandsAt(98e6);
    REQUIRE_FALSE(broadcast.empty());
    CHECK(broadcast.front()->group == "Broadcasting");
}

TEST_CASE("every band containing a frequency is returned, narrowest first") {
    const auto plan = BandPlan::load(shippedPlan());
    REQUIRE(plan.has_value());

    const std::vector<const Band*> fm = plan->bandsAt(100e6);
    REQUIRE_FALSE(fm.empty());
    CHECK(fm.front()->startHz <= 100e6);
    CHECK(fm.front()->stopHz > 100e6);

    // Allocations nest -- 2.4 GHz Wi-Fi sits inside a wider ISM allocation --
    // and both are true at once. All of them are returned now, so the nesting
    // is something the operator sees rather than something the loader hides;
    // narrowest still leads, because that is the more specific answer and the
    // host reads the first entry as the title.
    const std::vector<const Band*> nested = plan->bandsAt(2.45e9);
    for (std::size_t i = 1; i < nested.size(); ++i) {
        CAPTURE(i);
        CHECK(nested[i - 1]->widthHz() <= nested[i]->widthHz());
    }
    for (const Band& band : plan->bands()) {
        if (band.contains(2.45e9)) {
            REQUIRE_FALSE(nested.empty());
            CHECK(nested.front()->widthHz() <= band.widthHz());
        }
    }

    // Somewhere with no allocation is a normal answer, not a guess.
    CHECK(plan->bandsAt(1.0).empty());
}

TEST_CASE("the optional description and type keys are parsed") {
    const auto plan = BandPlan::load(shippedPlan());
    REQUIRE(plan.has_value());

    // Present on a handful of the shipped entries and empty on the rest,
    // which is what the host shows on hover.
    CHECK(std::ranges::any_of(plan->bands(),
                              [](const Band& band) { return !band.description.empty(); }));

    // And a hand-written plan carries the keys through, whatever the shipped
    // one happens to use today.
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "sweeppp-bandplan-type.toml";
    {
        std::ofstream out(file);
        out << "[bandplan]\nname = \"Types\"\n\n"
            << "[[band]]\nname = \"An allocation\"\nstart = \"1 MHz\"\nstop = \"2 MHz\"\n"
            << "group = \"test\"\ndescription = \"a sentence\"\n\n"
            << "[[band]]\nname = \"A channel\"\nstart = \"1.4 MHz\"\nstop = \"1.5 MHz\"\n"
            << "group = \"test\"\ntype = \"channel\"\n\n"
            << "[[band]]\nname = \"A beacon\"\nstart = \"1.45 MHz\"\ntype = \"spot\"\n";
    }

    const auto typed = BandPlan::load(file);
    std::error_code ec;
    std::filesystem::remove(file, ec);

    REQUIRE(typed.has_value());
    REQUIRE(typed->bands().size() == 3);
    CHECK(typed->bands()[0].kind == ContributionKind::Band);
    CHECK(typed->bands()[0].description == "a sentence");
    CHECK(typed->bands()[1].kind == ContributionKind::Channel);

    // A spot is one frequency and gives no stop.
    CHECK(typed->bands()[2].kind == ContributionKind::Spot);
    CHECK(typed->bands()[2].stopHz == typed->bands()[2].startHz);

    // The channel nests inside the allocation, and leads because it is
    // narrower.
    const std::vector<const Band*> at = typed->bandsAt(1.45e6);
    REQUIRE(at.size() == 3);
    CHECK(at[0]->name == "A beacon");
    CHECK(at[1]->name == "A channel");
    CHECK(at[2]->name == "An allocation");
}

TEST_CASE("bands are selected by overlap, not containment") {
    const auto plan = BandPlan::load(shippedPlan());
    REQUIRE(plan.has_value());

    // A band wider than the view still has to be drawn: zooming into the
    // middle of an allocation should not make its label disappear.
    const std::vector<const Band*> visible = plan->bandsIn(99e6, 101e6);
    REQUIRE_FALSE(visible.empty());
    CHECK(visible.front()->startHz < 99e6);

    CHECK(plan->bandsIn(1.0, 2.0).empty());
}

TEST_CASE("unticking a service takes its allocations out of both queries") {
    const auto plan = BandPlan::load(shippedPlan());
    REQUIRE(plan.has_value());
    REQUIRE_FALSE(plan->groups().empty());

    // Every band points at its own category, and the categories are sorted --
    // which is what lets the settings tree branch on an index rather than on a
    // string compare per band per frame.
    for (const Band& band : plan->bands()) {
        CAPTURE(band.name);
        REQUIRE(band.groupIndex < plan->groups().size());
        CHECK(plan->groups()[band.groupIndex] == band.group);
    }
    CHECK(std::ranges::is_sorted(plan->groups()));

    // The two resolvers as the plugin chains them: services, then bands.
    const auto mask = [&plan](const std::set<std::string, std::less<>>& disabled) {
        return plan->resolveBands(plan->resolveEnabled(disabled), disabled);
    };

    const std::vector<const Band*> all = plan->bandsAt(98e6, mask({}));
    REQUIRE_FALSE(all.empty());
    const std::string service = all.front()->group;
    const std::string name = all.front()->name;

    // A whole service off takes every allocation in it with it, from the
    // drawing query and the marker query alike -- one mask, so the plot and
    // the chip cannot disagree.
    const std::vector<std::uint8_t> serviceOff = mask({service});
    CHECK(std::ranges::none_of(plan->bandsAt(98e6, serviceOff),
                               [&service](const Band* b) { return b->group == service; }));
    CHECK(std::ranges::none_of(plan->bandsIn(87e6, 109e6, serviceOff),
                               [&service](const Band* b) { return b->group == service; }));

    // One allocation off by name, inside a service that is otherwise on.
    const std::vector<std::uint8_t> oneOff = mask({BandPlan::bandKey(service, name)});
    CHECK(std::ranges::none_of(plan->bandsAt(98e6, oneOff),
                               [&name](const Band* b) { return b->name == name; }));
    CHECK(std::ranges::any_of(plan->bandsIn(1e3, 100e9, oneOff),
                              [&service](const Band* b) { return b->group == service; }));

    // A key naming nothing is inert rather than an error, which is what lets a
    // choice survive its plan file being away for a release.
    CHECK(plan->bandsAt(98e6, mask({"nothing-here"})).size() == all.size());
}

TEST_CASE("discovery never fails, and finds the built-ins") {
    // Directories are passed in rather than taken from Paths, which is the
    // whole reason this survived the move into a plugin: a plugin's copy of
    // `Paths::instance()` is not the host's, so the host tells it where to
    // look and the loader has no singleton in it at all.
    const std::array<std::filesystem::path, 1> directories{Paths::instance().resourcesDir() /
                                                           "bandplans"};

    std::vector<std::string> problems;
    const std::vector<BandPlan> plans = BandPlan::discover(directories, &problems);

    REQUIRE_FALSE(plans.empty());
    CHECK(problems.empty());

    const auto match = std::ranges::find_if(
        plans, [](const BandPlan& plan) { return plan.name() == "ITU Region 1"; });
    CHECK(match != plans.end());
}

TEST_CASE("a directory that does not exist is not an error") {
    const std::array<std::filesystem::path, 1> directories{"/nonexistent/bandplans"};

    std::vector<std::string> problems;
    CHECK(BandPlan::discover(directories, &problems).empty());

    // Missing is not a problem worth reporting; a file that will not parse is,
    // and that distinction is what keeps the log useful on a fresh install.
    CHECK(problems.empty());
}
