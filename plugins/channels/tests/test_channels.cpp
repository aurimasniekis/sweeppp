// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The channel model: the file format, the tree the ids describe, and the one
// filter both queries share.
//
// The format is what an operator hand-edits and what the shipped files are
// written in, so it is the plugin's contract rather than an implementation
// detail. The filter is here for a sharper reason: unticking a group has to
// remove its entries from the plot *and* from the marker's chip, and that is
// only true by construction while both queries take the same mask.
#include "ChannelSet.hpp"

#include <algorithm>
#include <array>
#include <doctest/doctest.h>
#include <fstream>
#include <set>
#include <string>
#include <sweeppp/core/Paths.hpp>
#include <vector>

using namespace sweeppp;
using namespace channels;

namespace {

std::filesystem::path shippedDir() {
    return Paths::instance().resourcesDir() / "channels";
}

/// A temporary file that removes itself, so a failing CHECK does not leave one
/// behind to confuse the next run.
class ScopedFile {
public:
    ScopedFile(std::string name, std::string_view content)
        : m_path(std::filesystem::temp_directory_path() / std::move(name)) {
        std::ofstream out(m_path);
        out << content;
    }

    ~ScopedFile() {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    ScopedFile(const ScopedFile&) = delete;
    ScopedFile& operator=(const ScopedFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

const ChannelEntry* entryNamed(const ChannelSet& set, std::string_view name) {
    const auto match = std::ranges::find_if(
        set.entries(), [name](const ChannelEntry& entry) { return entry.name == name; });
    return match != set.entries().end() ? &*match : nullptr;
}

} // namespace

TEST_CASE("every shipped file parses, and the tree it describes holds together") {
    std::vector<std::string> problems;
    const std::array<std::filesystem::path, 1> directories{shippedDir()};
    const std::vector<ChannelSet> sets = ChannelSet::discover(directories, &problems);

    REQUIRE_FALSE(sets.empty());
    CAPTURE(problems.size());
    for (const std::string& problem : problems) {
        CAPTURE(problem);
        CHECK(false);
    }

    std::set<std::string> ids;
    for (const ChannelSet& set : sets) {
        CAPTURE(set.id());
        CHECK(ids.insert(set.id()).second);
        CHECK_FALSE(set.name().empty());
        CHECK_FALSE(set.groups().empty());
        CHECK_FALSE(set.entries().empty());

        std::set<std::string> groupIds;
        for (const ChannelGroup& group : set.groups()) {
            CAPTURE(group.id);
            CHECK(groupIds.insert(group.id).second);
            CHECK_FALSE(group.name.empty());

            // A group's parent is declared before it, which is what lets both
            // resolvers be a single forward pass.
            if (group.parent != kNoParent) {
                CHECK(group.parent < static_cast<std::size_t>(&group - set.groups().data()));
            }
        }

        // Sorted by start, which is what makes a range query a scan.
        for (std::size_t i = 1; i < set.entries().size(); ++i) {
            CAPTURE(i);
            CHECK(set.entries()[i].startHz >= set.entries()[i - 1].startHz);
        }

        for (const ChannelEntry& entry : set.entries()) {
            CAPTURE(entry.name);
            CHECK_FALSE(entry.name.empty());
            CHECK(entry.startHz > 0.0);
            CHECK(entry.stopHz >= entry.startHz);
            CHECK(entry.group < set.groups().size());
        }
    }

    // The families the application is verified against.
    const auto named = [&sets](std::string_view id) {
        return std::ranges::any_of(sets, [id](const ChannelSet& set) { return set.id() == id; });
    };
    CHECK(named("fpv"));
    CHECK(named("wifi"));
    CHECK(named("cellular"));
    CHECK(named("beacons"));
    CHECK(named("lora-ism-rc"));
}

TEST_CASE("the shipped FPV raceband is what a VTX would tune") {
    const auto set = ChannelSet::load(shippedDir() / "fpv.toml");
    REQUIRE(set.has_value());

    const std::size_t raceband = set->findGroup("analog/58/r");
    REQUIRE(raceband != kNoParent);

    // Every 5.8 GHz band is eight channels, and the file's 12 MHz reaches them
    // through the group without any of them naming a width.
    const auto inGroup = std::ranges::count_if(
        set->entries(), [raceband](const ChannelEntry& entry) { return entry.group == raceband; });
    CHECK(inGroup == 8);

    const ChannelEntry* r1 = entryNamed(*set, "R1 5658");
    REQUIRE(r1 != nullptr);
    CHECK(r1->kind == ChannelKind::Channel);
    CHECK(r1->startHz == doctest::Approx(5652e6));
    CHECK(r1->stopHz == doctest::Approx(5664e6));

    // The parents an id path implies, without a `parent` key anywhere.
    const ChannelGroup& group = set->groups()[raceband];
    REQUIRE(group.parent != kNoParent);
    CHECK(set->groups()[group.parent].id == "analog/58");
    const std::size_t grandparent = set->groups()[group.parent].parent;
    REQUIRE(grandparent != kNoParent);
    CHECK(set->groups()[grandparent].id == "analog");
    CHECK(set->groups()[grandparent].parent == kNoParent);
}

TEST_CASE("the shipped beacons are spots, and on by default") {
    const auto set = ChannelSet::load(shippedDir() / "beacons.toml");
    REQUIRE(set.has_value());

    const ChannelEntry* adsb = entryNamed(*set, "ADS-B 1090ES");
    REQUIRE(adsb != nullptr);
    CHECK(adsb->kind == ChannelKind::Spot);
    CHECK(adsb->stopHz == adsb->startHz);
    CHECK(adsb->startHz == doctest::Approx(1090e6));

    // Closed rather than half-open: a spot has no width, so a half-open range
    // would contain nothing at all.
    CHECK(adsb->contains(1090e6));
    CHECK_FALSE(adsb->contains(1090e6 + 1.0));

    const std::vector<std::uint8_t> defaults = set->resolveDefaults();
    REQUIRE(defaults.size() == set->groups().size());
    CHECK(std::ranges::all_of(defaults, [](std::uint8_t on) { return on != 0; }));
}

TEST_CASE("a fresh install paints about twenty things, not six hundred") {
    std::vector<std::string> problems;
    const std::array<std::filesystem::path, 1> directories{shippedDir()};
    const std::vector<ChannelSet> sets = ChannelSet::discover(directories, &problems);
    REQUIRE_FALSE(sets.empty());

    std::size_t defaultEntries = 0;
    for (const ChannelSet& set : sets) {
        const std::vector<std::uint8_t> defaults = set.resolveDefaults();
        for (const ChannelEntry& entry : set.entries()) {
            if (defaults[entry.group] != 0) {
                ++defaultEntries;
            }
        }

        // Every file's roots are off unless the file says otherwise, which is
        // the whole reason the key exists: a first run that painted six
        // hundred spans would be worse than one that painted none.
        if (set.id() == "fpv" || set.id() == "cellular" || set.id() == "lora-ism-rc") {
            CAPTURE(set.id());
            CHECK(std::ranges::none_of(defaults, [](std::uint8_t on) { return on != 0; }));
        }
    }

    const auto wifi =
        std::ranges::find_if(sets, [](const ChannelSet& set) { return set.id() == "wifi"; });
    REQUIRE(wifi != sets.end());
    const std::vector<std::uint8_t> wifiDefaults = wifi->resolveDefaults();

    const std::size_t band24 = wifi->findGroup("24");
    REQUIRE(band24 != kNoParent);
    CHECK(wifiDefaults[band24] != 0);

    const std::size_t band5 = wifi->findGroup("5");
    REQUIRE(band5 != kNoParent);
    CHECK(wifiDefaults[band5] == 0);

    CHECK(defaultEntries > 10);
    CHECK(defaultEntries < 60);
}

TEST_CASE("a channel is a centre and a width, or a start and a stop") {
    const ScopedFile file("sweeppp-channels-spans.toml", R"(
[channels]
id = "spans"
name = "Spans"
width = "1 MHz"

[[group]]
id = "g"
name = "Group"

  [[group.channel]]
  name = "from the file's width"
  center = "100 MHz"

  [[group.channel]]
  name = "its own width"
  center = "200 MHz"
  width = "4 MHz"

  [[group.channel]]
  name = "start and stop"
  start = "300 MHz"
  stop = "310 MHz"

  [[group.channel]]
  name = "a suffixless number"
  center = 400000000
  width = 2000000

[[group]]
id = "narrow"
name = "No width anywhere"
width = "0"

  [[group.channel]]
  name = "a beacon"
  center = "500 MHz"

  [[group.channel]]
  name = "a beacon called a band"
  center = "600 MHz"
  type = "band"
)");

    std::vector<std::string> problems;
    const auto set = ChannelSet::load(file.path(), &problems);
    REQUIRE(set.has_value());
    CHECK(problems.empty());
    REQUIRE(set->entries().size() == 6);

    const ChannelEntry* inherited = entryNamed(*set, "from the file's width");
    REQUIRE(inherited != nullptr);
    CHECK(inherited->startHz == doctest::Approx(99.5e6));
    CHECK(inherited->stopHz == doctest::Approx(100.5e6));

    const ChannelEntry* own = entryNamed(*set, "its own width");
    REQUIRE(own != nullptr);
    CHECK(own->widthHz() == doctest::Approx(4e6));

    const ChannelEntry* edges = entryNamed(*set, "start and stop");
    REQUIRE(edges != nullptr);
    CHECK(edges->startHz == doctest::Approx(300e6));
    CHECK(edges->stopHz == doctest::Approx(310e6));
    CHECK(edges->kind == ChannelKind::Channel);

    // "400 MHz" and 400000000 are the same entry written two ways, which is
    // what going through the project's one frequency parser buys.
    const ChannelEntry* bare = entryNamed(*set, "a suffixless number");
    REQUIRE(bare != nullptr);
    CHECK(bare->startHz == doctest::Approx(399e6));
    CHECK(bare->stopHz == doctest::Approx(401e6));

    // No width anywhere is a spot, which is what `stop_hz == start_hz` means
    // in the ABI -- unless the file says otherwise, and then it says otherwise.
    const ChannelEntry* beacon = entryNamed(*set, "a beacon");
    REQUIRE(beacon != nullptr);
    CHECK(beacon->kind == ChannelKind::Spot);
    CHECK(beacon->stopHz == beacon->startHz);

    const ChannelEntry* declared = entryNamed(*set, "a beacon called a band");
    REQUIRE(declared != nullptr);
    CHECK(declared->kind == ChannelKind::Band);
}

TEST_CASE("colour comes from the nearest ancestor that names one") {
    const ScopedFile file("sweeppp-channels-colors.toml", R"(
[channels]
id = "colors"
name = "Colours"
width = "1 MHz"

[channels.colors]
aaa_first = "#102030"
named = "#FF0000"

[[group]]
id = "root"
name = "Root"

[[group]]
id = "root/mid"
name = "Middle"
color = "named"

[[group]]
id = "root/mid/leaf"
name = "Leaf"

[[group]]
id = "root/mid/literal"
name = "Literal"
color = "#00FF00"

  [[group.channel]]
  name = "somewhere"
  center = "100 MHz"
)");

    std::vector<std::string> problems;
    const auto set = ChannelSet::load(file.path(), &problems);
    REQUIRE(set.has_value());
    CHECK(problems.empty());

    const auto colorOf = [&set](std::string_view id) {
        const std::size_t index = set->findGroup(id);
        REQUIRE(index != kNoParent);
        return set->groups()[index].color;
    };

    // A root that names nothing takes the file's first colour rather than
    // grey, so a file that bothers to define a palette is used by all of it.
    CHECK(colorOf("root").r == doctest::Approx(16.0 / 255.0));

    // The key indirection: "named" is looked up in [channels.colors], so
    // recolouring a whole family is one edit.
    CHECK(colorOf("root/mid").r == doctest::Approx(1.0F));
    CHECK(colorOf("root/mid").g == doctest::Approx(0.0F));

    // Inherited down the chain from the nearest ancestor that set one.
    CHECK(colorOf("root/mid/leaf").r == doctest::Approx(1.0F));
    CHECK(colorOf("root/mid/leaf").g == doctest::Approx(0.0F));

    // A literal wins over both.
    CHECK(colorOf("root/mid/literal").g == doctest::Approx(1.0F));
    CHECK(colorOf("root/mid/literal").r == doctest::Approx(0.0F));
}

TEST_CASE("a group whose parent was never declared is reported, not silently rooted") {
    const ScopedFile file("sweeppp-channels-orphan.toml", R"(
[channels]
id = "orphan"
name = "Orphan"
width = "1 MHz"

[[group]]
id = "here"
name = "Here"

  [[group.channel]]
  name = "kept"
  center = "100 MHz"

[[group]]
id = "missing/child"
name = "Child of nothing"

  [[group.channel]]
  name = "dropped with its group"
  center = "200 MHz"

[[group]]
id = "here"
name = "Declared twice"
)");

    std::vector<std::string> problems;
    const auto set = ChannelSet::load(file.path(), &problems);
    REQUIRE(set.has_value());

    // The rest of the file survives: one bad row costs its own row.
    REQUIRE(set->groups().size() == 1);
    CHECK(set->groups().front().id == "here");
    REQUIRE(set->entries().size() == 1);
    CHECK(set->entries().front().name == "kept");

    REQUIRE(problems.size() == 2);
    CHECK(problems[0].contains("missing"));
    CHECK(problems[1].contains("twice"));
}

TEST_CASE("a user file replaces a shipped one carrying the same id") {
    const std::filesystem::path userDir =
        std::filesystem::temp_directory_path() / "sweeppp-channels-user";
    std::error_code ec;
    std::filesystem::create_directories(userDir, ec);

    {
        std::ofstream out(userDir / "mine.toml");
        out << "[channels]\nid = \"beacons\"\nname = \"Beacons\"\n\n"
            << "[[group]]\nid = \"mine\"\nname = \"Mine\"\n\n"
            << "  [[group.channel]]\n  name = \"my beacon\"\n  center = \"100 MHz\"\n";
    }

    const std::array<std::filesystem::path, 2> directories{shippedDir(), userDir};
    std::vector<std::string> problems;
    const std::vector<ChannelSet> sets = ChannelSet::discover(directories, &problems);

    std::filesystem::remove_all(userDir, ec);

    CHECK(problems.empty());

    const auto beacons =
        std::ranges::find_if(sets, [](const ChannelSet& set) { return set.id() == "beacons"; });
    REQUIRE(beacons != sets.end());

    // Replaced outright rather than merged: someone correcting a table wants
    // their version, not their version plus ours.
    REQUIRE(beacons->entries().size() == 1);
    CHECK(beacons->entries().front().name == "my beacon");

    // A different id sits beside it as its own branch.
    CHECK(std::ranges::any_of(sets, [](const ChannelSet& set) { return set.id() == "wifi"; }));
}

TEST_CASE("disabling a group takes its whole subtree out of both queries") {
    const ScopedFile file("sweeppp-channels-filter.toml", R"(
[channels]
id = "filter"
name = "Filter"
width = "2 MHz"

[[group]]
id = "top"
name = "Top"

  [[group.channel]]
  name = "at the top"
  center = "100 MHz"

[[group]]
id = "top/mid"
name = "Middle"

[[group]]
id = "top/mid/leaf"
name = "Leaf"

  [[group.channel]]
  name = "deep"
  center = "100 MHz"

[[group]]
id = "other"
name = "Other"

  [[group.channel]]
  name = "elsewhere"
  center = "100 MHz"
)");

    const auto set = ChannelSet::load(file.path());
    REQUIRE(set.has_value());

    // The two resolvers as the plugin chains them: groups, then entries.
    const auto mask = [&set](const std::set<std::string, std::less<>>& disabled) {
        return set->resolveEntries(set->resolveEnabled(disabled), disabled);
    };

    // Nothing disabled: every entry answers.
    std::vector<std::uint8_t> on = mask({});
    CHECK(set->entriesIn(90e6, 110e6, on).size() == 3);
    CHECK(set->entriesAt(100e6, on).size() == 3);

    // Disabling an ancestor removes the descendant it never names, from both
    // queries -- one mask, so the plot and the chip cannot disagree.
    on = mask({"top/mid"});
    CHECK(set->entriesIn(90e6, 110e6, on).size() == 2);
    CHECK(set->entriesAt(100e6, on).size() == 2);
    CHECK(std::ranges::none_of(set->entriesAt(100e6, on),
                               [](const ChannelEntry* entry) { return entry->name == "deep"; }));

    on = mask({"top"});
    REQUIRE(set->entriesAt(100e6, on).size() == 1);
    CHECK(set->entriesAt(100e6, on).front()->name == "elsewhere");

    // One entry, by name, inside a group that is otherwise on: the finest
    // thing the operator can untick, and it leaves both queries the same way a
    // group does.
    on = mask({ChannelSet::entryKey("top/mid/leaf", "deep")});
    CHECK(set->entriesIn(90e6, 110e6, on).size() == 2);
    CHECK(std::ranges::none_of(set->entriesAt(100e6, on),
                               [](const ChannelEntry* entry) { return entry->name == "deep"; }));

    // An id naming nothing is inert rather than an error, which is what lets a
    // choice survive its data file being away for a release.
    CHECK(set->entriesAt(100e6, mask({"nothing/here"})).size() == 3);

    // A group's own flag and its children's are independent, which is what
    // makes hiding a branch non-destructive: `top/mid` off hides `deep`
    // whether or not `deep` is itself unticked, and clearing `top/mid` brings
    // back exactly what was ticked inside it.
    const std::set<std::string, std::less<>> both{"top/mid",
                                                  ChannelSet::entryKey("top", "at the top")};
    on = mask(both);
    REQUIRE(set->entriesAt(100e6, on).size() == 1);
    CHECK(set->entriesAt(100e6, on).front()->name == "elsewhere");
}

TEST_CASE("entries are selected by overlap, and a spot by the point it is on") {
    const auto set = ChannelSet::load(shippedDir() / "wifi.toml");
    REQUIRE(set.has_value());

    // Channel 6 is centred on 2437 MHz and 20 MHz wide.
    const std::vector<const ChannelEntry*> at = set->entriesAt(2437e6);
    REQUIRE_FALSE(at.empty());
    for (std::size_t i = 1; i < at.size(); ++i) {
        CAPTURE(i);
        CHECK(at[i]->widthHz() >= at[i - 1]->widthHz());
    }

    // Half-open, so abutting channels do not both claim their shared edge.
    const ChannelEntry* six = entryNamed(*set, "Wi-Fi 2.4G ch 6");
    REQUIRE(six != nullptr);
    CHECK(six->contains(six->startHz));
    CHECK_FALSE(six->contains(six->stopHz));

    // An entry wider than the view is still drawn: zooming into the middle of
    // one must not make it disappear.
    const std::vector<const ChannelEntry*> visible = set->entriesIn(2436e6, 2438e6);
    CHECK(std::ranges::any_of(visible, [six](const ChannelEntry* entry) { return entry == six; }));

    CHECK(set->entriesIn(1.0, 2.0).empty());
}

TEST_CASE("discovery never fails on a directory that is not there") {
    const std::array<std::filesystem::path, 1> directories{"/nonexistent/channels"};

    std::vector<std::string> problems;
    CHECK(ChannelSet::discover(directories, &problems).empty());

    // Missing is not a problem worth reporting; a file that will not parse is,
    // and that distinction is what keeps the log useful on a fresh install.
    CHECK(problems.empty());
}
