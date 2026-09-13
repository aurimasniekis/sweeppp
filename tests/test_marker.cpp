// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/ui/Marker.hpp>
#include <sweeppp/ui/MarkerPreset.hpp>

using namespace sweeppp::ui;

TEST_CASE("adding a marker names it and selects it") {
    MarkerSet markers;

    const Marker& first = markers.add(100e6);
    CHECK(first.id == 1);
    CHECK(markers.activeId == 1);

    const Marker& second = markers.add(200e6);
    CHECK(second.id == 2);
    CHECK(markers.activeId == 2);
    CHECK(markers.items.size() == 2);

    REQUIRE(markers.active() != nullptr);
    CHECK(markers.active()->frequencyHz == doctest::Approx(200e6));
}

TEST_CASE("a name in use is never handed to a second marker") {
    // The id is what goes into a MarkerEvent's label, into the .sweeps event
    // stream and into the channels plugin's map, so two markers sharing one
    // would make every reference to it ambiguous. Deleting from the middle
    // leaves the counter where it is for exactly that reason.
    MarkerSet markers;
    markers.add(100e6);
    markers.add(200e6);

    REQUIRE(markers.remove(1));
    markers.add(300e6);

    CHECK(markers.items.size() == 2);
    CHECK(markers.items[0].id == 2);
    CHECK(markers.items[1].id == 3);
}

TEST_CASE("the counter follows the set rather than the session") {
    // Numbering that only ever climbed would have an operator who cleared the
    // list placing M7 next, with nothing called M1 to M6 anywhere.
    MarkerSet markers;
    markers.add(100e6);
    markers.add(200e6);
    markers.add(300e6);

    // Deleting the highest frees its name again.
    REQUIRE(markers.remove(3));
    CHECK(markers.nextId == 3);

    markers.clear();
    CHECK(markers.items.empty());
    CHECK(markers.activeId == 0);
    CHECK(markers.add(400e6).id == 1);

    // And so does deleting the last one left.
    REQUIRE(markers.remove(1));
    CHECK(markers.add(500e6).id == 1);
}

TEST_CASE("deleting the selected marker moves the selection to a neighbour") {
    MarkerSet markers;
    markers.add(100e6); // M1
    markers.add(200e6); // M2
    markers.add(300e6); // M3

    // The one that slid into its place.
    markers.activeId = 2;
    REQUIRE(markers.remove(2));
    CHECK(markers.activeId == 3);

    // Deleting from the end falls back to the last remaining marker.
    markers.activeId = 3;
    REQUIRE(markers.remove(3));
    CHECK(markers.activeId == 1);

    // And the last one out leaves nothing selected.
    REQUIRE(markers.remove(1));
    CHECK(markers.activeId == 0);
    CHECK(markers.active() == nullptr);
}

TEST_CASE("deleting a marker that is not selected leaves the selection alone") {
    MarkerSet markers;
    markers.add(100e6);
    markers.add(200e6);
    markers.activeId = 2;

    REQUIRE(markers.remove(1));
    CHECK(markers.activeId == 2);

    CHECK_FALSE(markers.remove(99));
}

TEST_CASE("the nearest marker respects the tolerance and ignores hidden ones") {
    MarkerSet markers;
    markers.add(100e6);
    markers.add(101e6);

    const Marker* hit = markers.nearest(100.2e6, 0.5e6);
    REQUIRE(hit != nullptr);
    CHECK(hit->id == 1);

    // Further away than the tolerance is a miss, not the closest of a bad lot:
    // a click on empty spectrum has to be able to mean "select nothing".
    CHECK(markers.nearest(105e6, 0.5e6) == nullptr);

    // Hidden means not on the plot, so there is nothing there to click.
    markers.items[0].visible = false;
    const Marker* second = markers.nearest(100.2e6, 2e6);
    REQUIRE(second != nullptr);
    CHECK(second->id == 2);
}

// ---------------------------------------------------------------- presets

namespace {

/// A temporary file path that cleans itself up.
class ScopedPresetFile {
public:
    ScopedPresetFile()
        : m_path(std::filesystem::temp_directory_path() /
                 std::format("sweeppp-marker-presets-{}.toml", sweeppp::monotonicNs())) {}
    ~ScopedPresetFile() {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }
    ScopedPresetFile(const ScopedPresetFile&) = delete;
    ScopedPresetFile& operator=(const ScopedPresetFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

[[nodiscard]] const MarkerPreset* find(const MarkerPresetStore& store, std::string_view name) {
    const auto match = std::ranges::find(store.presets(), name, &MarkerPreset::name);
    return match == store.presets().end() ? nullptr : &*match;
}

} // namespace

TEST_CASE("a missing marker preset file is a first run, not a failure") {
    const ScopedPresetFile file;
    const MarkerPresetStore store = MarkerPresetStore::load(file.path());

    // Nothing ships: which frequencies are worth marking is the operator's own.
    CHECK(store.presets().empty());
}

TEST_CASE("marker presets round-trip through the file") {
    const ScopedPresetFile file;

    MarkerSet markers;
    markers.add(100.5e6);
    markers.add(433.92e6).peakLocked = true;
    markers.add(868e6).visible = false;

    MarkerPresetStore store = MarkerPresetStore::load(file.path());
    store.add(presetFromMarkers(markers, "Bench"));
    store.add(presetFromMarkers(markers, "Later"));
    store.setFavourite("Later", true);
    REQUIRE(store.save(file.path()).has_value());

    const MarkerPresetStore reloaded = MarkerPresetStore::load(file.path());

    const MarkerPreset* saved = find(reloaded, "Bench");
    REQUIRE(saved != nullptr);
    REQUIRE(saved->markers.size() == 3);
    CHECK(saved->markers[0].frequencyHz == doctest::Approx(100.5e6));
    CHECK(saved->markers[1].peakLocked);
    CHECK_FALSE(saved->markers[2].visible);

    // Favourites sort ahead of everything else.
    REQUIRE(reloaded.presets().size() == 2);
    CHECK(reloaded.presets().front().name == "Later");
}

TEST_CASE("saving under a name already used updates it rather than duplicating") {
    MarkerSet first;
    first.add(100e6);

    MarkerSet second;
    second.add(200e6);
    second.add(300e6);

    MarkerPresetStore store;
    store.add(presetFromMarkers(first, "Mine"));
    store.setFavourite("Mine", true);
    store.add(presetFromMarkers(second, "Mine"));

    CHECK(store.presets().size() == 1);

    const MarkerPreset* updated = find(store, "Mine");
    REQUIRE(updated != nullptr);
    REQUIRE(updated->markers.size() == 2);
    CHECK(updated->markers.front().frequencyHz == doctest::Approx(200e6));
    // Re-saving a set is not a reason to drop it from the shortlist.
    CHECK(updated->favourite);
}

TEST_CASE("applying a preset replaces the set and numbers it from M1") {
    MarkerSet source;
    source.add(100e6);
    source.add(200e6).peakLocked = true;
    const MarkerPreset preset = presetFromMarkers(source, "Two");

    MarkerSet target;
    target.add(1e6);
    target.add(2e6);
    target.add(3e6);
    REQUIRE(target.remove(1));

    applyPreset(preset, target);

    REQUIRE(target.items.size() == 2);
    CHECK(target.items[0].id == 1);
    CHECK(target.items[0].frequencyHz == doctest::Approx(100e6));
    CHECK(target.items[1].id == 2);
    CHECK(target.items[1].peakLocked);
    CHECK(target.activeId == 2);
}

TEST_CASE("placing a preset alongside what is up skips frequencies already marked") {
    MarkerSet source;
    source.add(100e6);
    source.add(200e6);
    const MarkerPreset preset = presetFromMarkers(source, "Two");

    MarkerSet target;
    target.add(100e6);

    addPresetToMarkers(preset, target);
    REQUIRE(target.items.size() == 2);
    CHECK(target.items[1].frequencyHz == doctest::Approx(200e6));

    // Applying it a second time is a no-op, not a second copy of the set.
    addPresetToMarkers(preset, target);
    CHECK(target.items.size() == 2);
}

TEST_CASE("a marker preset describes what it holds") {
    MarkerSet one;
    one.add(100.5e6);
    CHECK(presetFromMarkers(one, "x").describe() == "100.5 MHz");

    MarkerSet several;
    several.add(2.4e9);
    several.add(88e6);
    several.add(433e6);
    CHECK(presetFromMarkers(several, "x").describe() == "3 markers, 88 MHz - 2.4 GHz");

    CHECK(MarkerPreset{}.describe() == "empty");
}
