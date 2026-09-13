// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/core/Paths.hpp>
#include <sweeppp/ui/ColorMap.hpp>
#include <sweeppp/ui/Theme.hpp>

using namespace sweeppp;
using namespace sweeppp::ui;

TEST_CASE("colours parse and round-trip through hex") {
    const auto green = Color::fromHex("#4ADE80");
    REQUIRE(green.has_value());
    CHECK(green->r == doctest::Approx(0x4A / 255.0F).epsilon(0.01));
    CHECK(green->g == doctest::Approx(0xDE / 255.0F).epsilon(0.01));
    CHECK(green->b == doctest::Approx(0x80 / 255.0F).epsilon(0.01));
    CHECK(green->a == doctest::Approx(1.0F));
    CHECK(green->toHex() == "#4ADE80");

    // Alpha is optional, and preserved when present.
    const auto translucent = Color::fromHex("#FF000080");
    REQUIRE(translucent.has_value());
    CHECK(translucent->a == doctest::Approx(0x80 / 255.0F).epsilon(0.01));
    CHECK(translucent->toHex(true) == "#FF000080");

    // The leading # is optional.
    CHECK(Color::fromHex("4ADE80").has_value());

    CHECK_FALSE(Color::fromHex("#GGGGGG").has_value());
    CHECK_FALSE(Color::fromHex("#FFF").has_value());
    CHECK_FALSE(Color::fromHex("").has_value());
}

TEST_CASE("packed colour uses ImGui's byte order") {
    // 0xAABBGGRR. Getting this wrong swaps red and blue everywhere, which is
    // obvious on screen but easy to introduce and hard to spot in review.
    const auto red = Color::fromHex("#FF0000");
    REQUIRE(red.has_value());
    CHECK(red->packed() == 0xFF0000FFU);

    const auto blue = Color::fromHex("#0000FF");
    REQUIRE(blue.has_value());
    CHECK(blue->packed() == 0xFFFF0000U);
}

TEST_CASE("a colormap interpolates between its stops") {
    const ColorMap map("test", {ColorStop{.position = 0.0F, .color = Color{0, 0, 0, 1}},
                                ColorStop{.position = 1.0F, .color = Color{1, 1, 1, 1}}});

    CHECK(map.sample(0.0F).r == doctest::Approx(0.0F));
    CHECK(map.sample(1.0F).r == doctest::Approx(1.0F));
    CHECK(map.sample(0.5F).r == doctest::Approx(0.5F).epsilon(0.01));

    // Out of range clamps rather than wrapping.
    CHECK(map.sample(-1.0F).r == doctest::Approx(0.0F));
    CHECK(map.sample(2.0F).r == doctest::Approx(1.0F));
}

TEST_CASE("stops are sorted, so an editor may drag one past another") {
    ColorMap map("test", {ColorStop{.position = 1.0F, .color = Color{1, 0, 0, 1}},
                          ColorStop{.position = 0.0F, .color = Color{0, 0, 1, 1}}});

    REQUIRE(map.stops().size() == 2);
    CHECK(map.stops()[0].position == doctest::Approx(0.0F));
    CHECK(map.stops()[1].position == doctest::Approx(1.0F));

    // Blue at the bottom, red at the top -- the order the positions say, not
    // the order they were supplied in.
    CHECK(map.sample(0.0F).b == doctest::Approx(1.0F));
    CHECK(map.sample(1.0F).r == doctest::Approx(1.0F));
}

TEST_CASE("the baked LUT matches sampling the stops") {
    // The LUT is what the waterfall shader actually reads, so it must agree
    // with what the gradient editor previews.
    const ColorMap& map = builtinColorMap("spectral");
    REQUIRE(map.lut().size() == ColorMap::kLutSize);

    for (std::size_t i = 0; i < ColorMap::kLutSize; i += 17) {
        const float position = static_cast<float>(i) / static_cast<float>(ColorMap::kLutSize - 1);
        CAPTURE(i);
        CHECK(map.lut()[i] == map.sample(position).packed());
    }
}

TEST_CASE("dB sampling maps the gradient range onto the colormap") {
    const ColorMap map("test", {ColorStop{.position = 0.0F, .color = Color{0, 0, 0, 1}},
                                ColorStop{.position = 1.0F, .color = Color{1, 1, 1, 1}}});

    CHECK(map.sampleDb(-100.0F, -100.0F, -20.0F).r == doctest::Approx(0.0F));
    CHECK(map.sampleDb(-20.0F, -100.0F, -20.0F).r == doctest::Approx(1.0F));
    CHECK(map.sampleDb(-60.0F, -100.0F, -20.0F).r == doctest::Approx(0.5F).epsilon(0.01));
}

TEST_CASE("every built-in colormap is usable") {
    const std::vector<ColorMap> maps = builtinColorMaps();
    REQUIRE(maps.size() >= 10);

    for (const ColorMap& map : maps) {
        CAPTURE(map.name());
        CHECK_FALSE(map.name().empty());
        CHECK(map.stops().size() >= 2);
        CHECK_FALSE(map.empty());

        // No third-party product name appears in any shipped identifier.
        for (const std::string_view banned : {"jet", "turbo", "viridis", "inferno"}) {
            CHECK(map.name().find(banned) == std::string::npos);
        }

        // Stops span the full range, so no part of the gradient is undefined.
        CHECK(map.stops().front().position == doctest::Approx(0.0F));
        CHECK(map.stops().back().position == doctest::Approx(1.0F));
    }

    // An unknown name falls back rather than returning a dangling reference.
    CHECK_FALSE(builtinColorMap("no-such-map").name().empty());
}

TEST_CASE("constructing a theme terminates") {
    // Guards against mutual recursion between the default constructor and
    // builtinDark(): if each constructs the other, this runs the stack out
    // rather than failing an assertion. The GUI is not built under the
    // sanitizer presets, so only a unit test catches it.
    const Theme theme;
    CHECK(theme.name() == "Dark");
    CHECK(theme.chrome().dark);

    const Theme dark = Theme::builtinDark();
    const Theme light = Theme::builtinLight();
    const Theme contrast = Theme::builtinHighContrast();
    CHECK(dark.name() == "Dark");
    CHECK(light.name() == "Light");
    CHECK(contrast.name() == "High Contrast");
    CHECK_FALSE(light.chrome().dark);

    CHECK(Theme::builtinThemes().size() == 3);
}

TEST_CASE("built-in themes define every colour they need") {
    for (const Theme& theme : Theme::builtinThemes()) {
        CAPTURE(theme.name());

        // A theme left at default-constructed Colors would be all black, which
        // renders as an unusable window rather than an obvious error.
        const ChromeTheme& chrome = theme.chrome();
        CHECK(chrome.text.packed() != chrome.windowBackground.packed());
        CHECK(chrome.accent.packed() != 0xFF000000U);
        CHECK(chrome.ok.packed() != chrome.danger.packed());

        const SpectrumTheme& spectrum = theme.spectrum();
        CHECK(spectrum.traceLive.packed() != spectrum.background.packed());

        // The four traces must be distinguishable from each other.
        const std::array<std::uint32_t, 4> traces{
            spectrum.traceLive.packed(), spectrum.traceMaxHold.packed(),
            spectrum.traceMinHold.packed(), spectrum.traceAverage.packed()};
        for (std::size_t i = 0; i < traces.size(); ++i) {
            for (std::size_t j = i + 1; j < traces.size(); ++j) {
                CHECK(traces[i] != traces[j]);
            }
        }

        // Named colormaps must actually resolve.
        CHECK_FALSE(theme.waterfallColorMap().empty());
        CHECK_FALSE(theme.spectrumFillColorMap().empty());
    }
}

TEST_CASE("a theme round-trips through TOML") {
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       std::format("sweeppp-theme-{}.toml", monotonicNs());

    Theme original = Theme::builtinDark();
    original.setName("Round Trip");
    original.spectrum().traceLive = Color::fromHex("#123456").value();
    original.spectrum().fillAlpha = 0.42F;
    original.waterfall().colorMap = "ember";
    original.addColorMap(ColorMap(
        "custom", {ColorStop{0.0F, Color{0, 0, 0, 1}}, ColorStop{1.0F, Color{1, 0, 0, 1}}}));

    REQUIRE(original.saveToToml(path).has_value());

    auto reloaded = Theme::loadFromToml(path);
    REQUIRE(reloaded.has_value());

    CHECK(reloaded->name() == "Round Trip");
    CHECK(reloaded->spectrum().traceLive.toHex() == "#123456");
    CHECK(reloaded->spectrum().fillAlpha == doctest::Approx(0.42F));
    CHECK(reloaded->waterfall().colorMap == "ember");

    // The inline colormap came back too, and shadows any built-in of the same
    // name.
    REQUIRE(reloaded->colorMaps().size() == 1);
    CHECK(reloaded->colorMaps().front().name() == "custom");
    CHECK(reloaded->resolveColorMap("custom").sample(1.0F).r == doctest::Approx(1.0F));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("a theme name slugifies to the stem of its shipped file") {
    // Saving under a shipped theme's name has to land on that file's stem, or
    // discovery applies the two in the wrong order and the shipped one wins.
    CHECK(slugify("Sweep Dark") == "sweep-dark");
    CHECK(slugify("Diamond D-190") == "diamond-d-190");
    CHECK(slugify("!!").empty());
}

TEST_CASE("a partial theme file inherits the rest of its base") {
    // This is what keeps a hand-written theme short, and what stops existing
    // theme files breaking when new keys are added.
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       std::format("sweeppp-partial-{}.toml", monotonicNs());

    {
        std::ofstream out(path);
        out << "[theme]\nname = \"Partial\"\nbase = \"dark\"\n\n"
               "[theme.spectrum]\ntrace_live = \"#ABCDEF\"\n";
    }

    auto theme = Theme::loadFromToml(path);
    REQUIRE(theme.has_value());

    CHECK(theme->name() == "Partial");
    CHECK(theme->spectrum().traceLive.toHex() == "#ABCDEF");
    // Everything unspecified came from the dark base rather than turning black.
    CHECK(theme->spectrum().traceMaxHold.toHex() ==
          Theme::builtinDark().spectrum().traceMaxHold.toHex());
    CHECK(theme->chrome().accent.toHex() == Theme::builtinDark().chrome().accent.toHex());

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("a colormap round-trips through its own TOML file") {
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       std::format("sweeppp-colormap-{}.toml", monotonicNs());

    const ColorMap original("my-gradient", {ColorStop{0.0F, Color::fromHex("#000080").value()},
                                            ColorStop{0.5F, Color::fromHex("#00FF00").value()},
                                            ColorStop{1.0F, Color::fromHex("#800000").value()}});

    REQUIRE(original.saveToToml(path).has_value());

    auto reloaded = ColorMap::loadFromToml(path);
    REQUIRE(reloaded.has_value());
    CHECK(reloaded->name() == "my-gradient");
    REQUIRE(reloaded->stops().size() == 3);
    CHECK(reloaded->stops()[1].position == doctest::Approx(0.5F));
    CHECK(reloaded->stops()[1].color.toHex() == "#00FF00");

    // The LUT the shader will read is identical to the original's.
    CHECK(reloaded->lut() == original.lut());

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("a colormap with too few stops is rejected") {
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       std::format("sweeppp-bad-colormap-{}.toml", monotonicNs());

    {
        std::ofstream out(path);
        out << "[colormap]\nname = \"broken\"\n"
               "stops = [ { pos = 0.0, color = \"#000000\" } ]\n";
    }

    const auto map = ColorMap::loadFromToml(path);
    REQUIRE_FALSE(map.has_value());
    CHECK(map.error().message().find("two stops") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
