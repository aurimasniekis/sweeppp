// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/profile/Profile.hpp>

using namespace sweeppp;

namespace {

class ScopedProfileFile {
public:
    ScopedProfileFile()
        : m_path(std::filesystem::temp_directory_path() /
                 std::format("sweeppp-profile-{}.toml", monotonicNs())) {}
    ~ScopedProfileFile() {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }
    ScopedProfileFile(const ScopedProfileFile&) = delete;
    ScopedProfileFile& operator=(const ScopedProfileFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

} // namespace

TEST_CASE("a profile round-trips every part of a configuration") {
    // Restoring half a setup is worse than restoring none of it: an RBW
    // without the sample rate that produced it is not a resolution. So this
    // checks one field from each section rather than trusting that a file
    // which loads at all loaded everything.
    const ScopedProfileFile file;

    Profile saved;
    saved.name = "bench";
    saved.sweeping = true;

    saved.deviceDriver = "hackrf";
    saved.deviceId = "0123456789abcdef";
    saved.deviceLabel = "HackRF One r9";
    saved.deviceParameters = {{"sample_rate", SdrValue{20e6}},
                              {"lna_gain", SdrValue{std::int64_t{24}}},
                              {"amp_enable", SdrValue{true}},
                              {"antenna", SdrValue{std::string("port a")}}};

    saved.sweepPlan.segments = {SweepSegment{.startHz = 2.4e9, .stopHz = 2.5e9},
                                SweepSegment{.startHz = 5.1e9, .stopHz = 5.9e9}};
    saved.sweepPlan.sampleRate = 20e6;
    saved.sweepPlan.rbwHz = 25e3;
    saved.sweepPlan.dcGuardFraction = 0.07;

    saved.pipeline.fftSize = 8192;
    saved.pipeline.overlap = 0.5;
    saved.pipeline.averageCount = 4;

    saved.corrections = {.dcRemoval = false, .flatten = true, .spurMask = false, .autoSpurs = true};

    saved.view.themeName = "High Contrast";
    saved.view.layout.panels.front().yMinDb = -120.0F;
    saved.view.layout.panels.front().yMaxDb = -20.0F;
    saved.view.layout.panels.front().gradientMinDb = -80.0F;
    saved.view.layout.panels.front().gradientMaxDb = -25.0F;
    saved.view.layout.panels.front().waterfallFraction = 0.62F;
    saved.view.maxHoldDecayDbPerSec = 12.5F;
    saved.view.fillStyle = 0;
    saved.view.markerReadout = 3;
    saved.view.showAntennaRanges = true;

    saved.view.markers.items = {ui::Marker{.id = 1, .frequencyHz = 433.92e6},
                                ui::Marker{.id = 2, .frequencyHz = 868.3e6, .visible = false},
                                ui::Marker{.id = 3, .frequencyHz = 2.412e9, .peakLocked = true}};
    saved.view.markers.activeId = 2;
    saved.view.markers.nextId = 4;

    REQUIRE(saved.save(file.path()).has_value());

    const auto loaded = Profile::load(file.path());
    REQUIRE(loaded.has_value());

    CHECK(loaded->name == "bench");

    CHECK(loaded->deviceDriver == "hackrf");
    CHECK(loaded->deviceId == "0123456789abcdef");
    CHECK(loaded->deviceLabel == "HackRF One r9");

    // Each variant alternative has to survive as itself. A gain that came back
    // as a double would be coerced on the way into the device and probably
    // work, right up until a parameter where it does not.
    REQUIRE(loaded->deviceParameters.size() == 4);
    const auto find = [&loaded](std::string_view key) {
        const auto match = std::ranges::find_if(
            loaded->deviceParameters, [key](const auto& entry) { return entry.first == key; });
        REQUIRE(match != loaded->deviceParameters.end());
        return match->second;
    };
    CHECK(std::holds_alternative<double>(find("sample_rate")));
    CHECK(std::holds_alternative<std::int64_t>(find("lna_gain")));
    CHECK(std::holds_alternative<bool>(find("amp_enable")));
    CHECK(std::holds_alternative<std::string>(find("antenna")));
    CHECK(asInt(find("lna_gain")) == 24);
    CHECK(asString(find("antenna")) == "port a");

    // The plan is embedded through its own serialiser, so a discontinuous one
    // has to survive intact rather than collapsing to its extent.
    REQUIRE(loaded->sweepPlan.segments.size() == 2);
    CHECK(loaded->sweepPlan.segments[1].startHz == doctest::Approx(5.1e9));
    CHECK(loaded->sweepPlan.rbwHz == doctest::Approx(25e3));
    CHECK(loaded->sweepPlan.dcGuardFraction == doctest::Approx(0.07));

    // Drawing flags travel with a profile the way every other display choice
    // does. The *assignments* it shades from do not -- loading a saved job
    // must not unscrew an antenna -- so this is the flag alone.
    CHECK(loaded->view.showAntennaRanges);

    CHECK(loaded->pipeline.fftSize == 8192);
    CHECK(loaded->pipeline.overlap == doctest::Approx(0.5));
    CHECK(loaded->pipeline.averageCount == 4);

    // Each switch is checked against a value that differs from its default,
    // so a load that silently fell back to the defaults would fail here.
    CHECK_FALSE(loaded->corrections.dcRemoval);
    CHECK(loaded->corrections.flatten);
    CHECK_FALSE(loaded->corrections.spurMask);
    CHECK(loaded->corrections.autoSpurs);

    CHECK(loaded->view.themeName == "High Contrast");
    REQUIRE(loaded->view.layout.panels.size() == 1);
    CHECK(loaded->view.layout.panels.front().yMinDb == doctest::Approx(-120.0F));
    CHECK(loaded->view.layout.panels.front().gradientMaxDb == doctest::Approx(-25.0F));
    CHECK(loaded->view.layout.panels.front().waterfallFraction == doctest::Approx(0.62F));
    CHECK(loaded->view.maxHoldDecayDbPerSec == doctest::Approx(12.5F));
    CHECK(loaded->view.fillStyle == 0);
    CHECK(loaded->view.markerReadout == 3);

    REQUIRE(loaded->view.markers.items.size() == 3);
    CHECK(loaded->view.markers.items[1].id == 2);
    CHECK(loaded->view.markers.items[1].frequencyHz == doctest::Approx(868.3e6));
    CHECK_FALSE(loaded->view.markers.items[1].visible);
    CHECK(loaded->view.markers.items[2].peakLocked);
    CHECK(loaded->view.markers.nextId == 4);

    // The markers come back; the selection does not. It is what the right
    // button moves and what backspace deletes, so restoring it would make the
    // first gesture of a new session an edit of the last one.
    CHECK(loaded->view.markers.activeId == 0);
}

TEST_CASE("a hand-edited marker list is repaired rather than rejected") {
    // Everything else in the file still has to load. A duplicate id or a
    // frequency of zero is a line someone typed, not a reason to give an
    // operator their defaults back.
    const ScopedProfileFile file;

    {
        std::ofstream out(file.path());
        out << "[display]\n"
               "active_marker = 7\n"
               "marker_readout = -3\n"
               "\n"
               "[[display.markers]]\n"
               "id = 1\n"
               "frequency = 433920000.0\n"
               "\n"
               "[[display.markers]]\n"
               "id = 1\n"
               "frequency = 868300000.0\n"
               "\n"
               "[[display.markers]]\n"
               "id = 4\n"
               "frequency = 0.0\n"
               "\n"
               "[[display.markers]]\n"
               "id = 0\n"
               "frequency = 2412000000.0\n";
    }

    const auto loaded = Profile::load(file.path());
    REQUIRE(loaded.has_value());

    // The duplicate, the zero frequency and the id of zero are all gone.
    REQUIRE(loaded->view.markers.items.size() == 1);
    CHECK(loaded->view.markers.items[0].id == 1);
    CHECK(loaded->view.markers.items[0].frequencyHz == doctest::Approx(433.92e6));

    // Past the highest id that survived, so the next marker placed cannot
    // collide with one already on the plot.
    CHECK(loaded->view.markers.nextId == 2);

    // `active_marker` is a key nothing writes any more, and reading one left in
    // a file by hand would restore a selection this build deliberately drops.
    CHECK(loaded->view.markers.activeId == 0);
    CHECK(loaded->view.markerReadout == 0);
}

TEST_CASE("a profile from elsewhere cannot put the display out of reach") {
    // Files get hand-edited, and get written by builds with different bounds.
    // A level outside what the controls can reach would leave the operator
    // with a display they cannot drag back.
    const ScopedProfileFile file;

    Profile saved;
    ui::PanelView& panel = saved.view.layout.panels.front();
    panel.yMinDb = -9000.0F;
    panel.yMaxDb = 9000.0F;
    panel.gradientMinDb = -9000.0F;
    panel.gradientMaxDb = 9000.0F;
    panel.waterfallFraction = 40.0F;
    saved.view.layout.splits.x = 3.0F;
    saved.view.layout.splits.y = -1.0F;
    saved.view.layout.splits.thirdsX = {0.95F, 0.05F};
    REQUIRE(saved.save(file.path()).has_value());

    const auto loaded = Profile::load(file.path());
    REQUIRE(loaded.has_value());

    REQUIRE(loaded->view.layout.panels.size() == 1);
    const ui::PanelView& back = loaded->view.layout.panels.front();
    CHECK(back.yMinDb >= ui::kScaleFloorDbfs);
    CHECK(back.yMaxDb <= ui::kScaleCeilingDbfs);
    CHECK(back.yMaxDb - back.yMinDb >= ui::kMinScaleSpanDb);
    CHECK(back.gradientMinDb >= ui::kScaleFloorDbfs);
    CHECK(back.gradientMaxDb <= ui::kScaleCeilingDbfs);
    CHECK(back.waterfallFraction <= 0.95F);
    CHECK(back.waterfallFraction >= 0.05F);
    CHECK(loaded->view.layout.splits.x == doctest::Approx(ui::kMaxSplit));
    CHECK(loaded->view.layout.splits.y == doctest::Approx(ui::kMinSplit));
    const std::array<float, 2>& thirds = loaded->view.layout.splits.thirdsX;
    CHECK(thirds[1] - thirds[0] >= ui::kMinThird - 1e-6F);
    CHECK(1.0F - thirds[1] >= ui::kMinThird - 1e-6F);
}

TEST_CASE("a panel layout round-trips, mode, arrangement and levels") {
    const ScopedProfileFile file;

    Profile saved;
    ui::PanelLayout& layout = saved.view.layout;
    layout.rowsForTwo = true;
    layout.splits.x = 0.3F;
    layout.splits.y = 0.7F;
    layout.splits.thirdsX = {0.25F, 0.6F};
    layout.splits.thirdsY = {0.4F, 0.8F};
    layout.overview = false;
    layout.panels.front().yMinDb = -100.0F;
    layout.panels.front().viewStartHz = 90e6;
    layout.panels.front().viewStopHz = 100e6;
    layout.panels.front().waterfallPaused = true;
    ui::PanelView* second = layout.add(layout.panels.front());
    REQUIRE(second != nullptr);
    second->gradientMaxDb = -30.0F;
    second->viewStartHz = 433e6;
    second->viewStopHz = 435e6;
    layout.focusedId = second->id;
    ui::PanelView* third = layout.add(layout.panels.front());
    REQUIRE(third != nullptr);
    third->detached = true;
    REQUIRE(saved.save(file.path()).has_value());

    const auto loaded = Profile::load(file.path());
    REQUIRE(loaded.has_value());
    const ui::PanelLayout& back = loaded->view.layout;

    CHECK(back.mode == ui::PanelMode::Mirror);
    CHECK(back.rowsForTwo);
    CHECK(back.splits.x == doctest::Approx(0.3F));
    CHECK(back.splits.y == doctest::Approx(0.7F));
    CHECK(back.splits.thirdsX[0] == doctest::Approx(0.25F));
    CHECK(back.splits.thirdsX[1] == doctest::Approx(0.6F));
    CHECK(back.splits.thirdsY[1] == doctest::Approx(0.8F));
    CHECK_FALSE(back.overview);
    REQUIRE(back.panels.size() == 3);
    CHECK(back.panels[0].yMinDb == doctest::Approx(-100.0F));
    CHECK(back.panels[1].gradientMaxDb == doctest::Approx(-30.0F));
    CHECK_FALSE(back.panels[1].detached);
    CHECK(back.panels[2].detached);
    CHECK(back.nextId == 4);

    // Two Mirror panels at different zooms are the point of the layout, so
    // their windows are kept. A pause is never restored.
    CHECK(back.panels[0].viewStartHz == doctest::Approx(90e6));
    CHECK(back.panels[1].viewStopHz == doctest::Approx(435e6));
    CHECK_FALSE(back.panels[0].waterfallPaused);

    // Focus is where the hand was, not a setting.
    CHECK(back.focusedId == back.panels[0].id);
}

TEST_CASE("a lone panel's window is not saved, and Spans keeps segments") {
    const ScopedProfileFile file;

    Profile saved;
    saved.view.layout.panels.front().viewStartHz = 90e6;
    saved.view.layout.panels.front().viewStopHz = 100e6;
    REQUIRE(saved.save(file.path()).has_value());
    auto loaded = Profile::load(file.path());
    REQUIRE(loaded.has_value());
    CHECK(loaded->view.layout.panels.front().viewStopHz == 0.0);

    saved.view.layout.mode = ui::PanelMode::Spans;
    saved.view.layout.panels.front().segment = {88e6, 108e6};
    REQUIRE(saved.save(file.path()).has_value());
    loaded = Profile::load(file.path());
    REQUIRE(loaded.has_value());
    CHECK(loaded->view.layout.mode == ui::PanelMode::Spans);
    CHECK(loaded->view.layout.panels.front().segment.startHz == doctest::Approx(88e6));
    CHECK(loaded->view.layout.panels.front().segment.stopHz == doctest::Approx(108e6));
}

TEST_CASE("a hand-edited panel list is repaired rather than rejected") {
    const ScopedProfileFile file;

    {
        std::ofstream out(file.path());
        out << "[display]\n"
               "panel_mode = \"sideways\"\n"
               "panel_layout = \"hexagon\"\n"
               "panel_split_x = 0.01\n"
               "\n";
        // Twelve panels, a duplicate id, an id of zero, and all torn off.
        for (const int id : {1, 2, 2, 0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}) {
            out << std::format("[[display.panels]]\nid = {}\ndetached = true\n"
                               "waterfall_fraction = 7.0\ny_min = 50.0\n\n",
                               id);
        }
    }

    const auto loaded = Profile::load(file.path());
    REQUIRE(loaded.has_value());
    const ui::PanelLayout& layout = loaded->view.layout;

    CHECK(layout.mode == ui::PanelMode::Mirror);
    CHECK_FALSE(layout.rowsForTwo);
    CHECK(layout.splits.x == doctest::Approx(ui::kMinSplit));

    // The repaired list, cut to the limit: ids 1 to 9 in order.
    REQUIRE(layout.panels.size() == ui::kMaxPanels);
    for (std::size_t i = 0; i < ui::kMaxPanels; ++i) {
        CHECK(layout.panels[i].id == static_cast<int>(i) + 1);
    }
    CHECK(layout.nextId == static_cast<int>(ui::kMaxPanels) + 1);

    // Every panel torn off would leave the main window empty.
    CHECK(layout.attachedCount() >= 1);
    for (const ui::PanelView& panel : layout.panels) {
        CHECK(panel.waterfallFraction <= 0.95F);
        CHECK(panel.yMaxDb - panel.yMinDb >= ui::kMinScaleSpanDb);
    }
}

TEST_CASE("a profile with no panels gets the default one") {
    const ScopedProfileFile file;
    {
        std::ofstream out(file.path());
        out << "[display]\ntheme = \"Dark\"\n";
    }
    const auto loaded = Profile::load(file.path());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->view.layout.panels.size() == 1);
    CHECK(loaded->view.layout.focusedId == loaded->view.layout.panels.front().id);
}

TEST_CASE("a missing profile is reported, not invented") {
    const auto loaded =
        Profile::load(std::filesystem::temp_directory_path() / "sweeppp-does-not-exist.toml");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code() == ErrorCode::NotFound);
}
