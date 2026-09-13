// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/ui/ColorMap.hpp"

#include "sweeppp/core/Toml.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace sweeppp::ui {
namespace {

/// Parses two hex digits.
Result<int> hexByte(std::string_view text) {
    int value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return fail<int>(ErrorCode::ParseError, "'{}' is not a hex byte", text);
    }
    return value;
}

Color lerp(const Color& from, const Color& to, float t) noexcept {
    return {.r = from.r + (to.r - from.r) * t,
            .g = from.g + (to.g - from.g) * t,
            .b = from.b + (to.b - from.b) * t,
            .a = from.a + (to.a - from.a) * t};
}

ColorMap make(std::string name, std::initializer_list<std::pair<float, const char*>> stops) {
    std::vector<ColorStop> parsed;
    parsed.reserve(stops.size());
    for (const auto& [position, hex] : stops) {
        if (auto color = Color::fromHex(hex)) {
            parsed.push_back(ColorStop{.position = position, .color = *color});
        }
    }
    return ColorMap{std::move(name), std::move(parsed)};
}

} // namespace

Result<Color> Color::fromHex(std::string_view hex) {
    std::string_view text = hex;
    if (text.starts_with('#')) {
        text.remove_prefix(1);
    }

    if (text.size() != 6 && text.size() != 8) {
        return fail<Color>(ErrorCode::ParseError,
                           "'{}' is not a colour; expected #RRGGBB or #RRGGBBAA", hex);
    }

    auto r = hexByte(text.substr(0, 2));
    auto g = hexByte(text.substr(2, 2));
    auto b = hexByte(text.substr(4, 2));
    if (!r || !g || !b) {
        return fail<Color>(ErrorCode::ParseError, "'{}' is not a colour", hex);
    }

    int alpha = 255;
    if (text.size() == 8) {
        auto a = hexByte(text.substr(6, 2));
        if (!a) {
            return fail<Color>(ErrorCode::ParseError, "'{}' has a bad alpha", hex);
        }
        alpha = *a;
    }

    return Color{.r = static_cast<float>(*r) / 255.0F,
                 .g = static_cast<float>(*g) / 255.0F,
                 .b = static_cast<float>(*b) / 255.0F,
                 .a = static_cast<float>(alpha) / 255.0F};
}

std::string Color::toHex(bool includeAlpha) const {
    const auto toByte = [](float value) {
        return static_cast<int>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    };

    if (includeAlpha) {
        return std::format("#{:02X}{:02X}{:02X}{:02X}", toByte(r), toByte(g), toByte(b), toByte(a));
    }
    return std::format("#{:02X}{:02X}{:02X}", toByte(r), toByte(g), toByte(b));
}

std::uint32_t Color::packed() const noexcept {
    const auto toByte = [](float value) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    };
    // 0xAABBGGRR -- ImGui's ImU32 layout, so this can be handed straight to a
    // draw list with no conversion.
    return toByte(r) | (toByte(g) << 8U) | (toByte(b) << 16U) | (toByte(a) << 24U);
}

ColorMap::ColorMap(std::string name, std::vector<ColorStop> stops) : m_name(std::move(name)) {
    setStops(std::move(stops));
}

void ColorMap::setStops(std::vector<ColorStop> stops) {
    m_stops = std::move(stops);
    // Sorted, so an editor may drag one stop past another without corrupting
    // the gradient.
    std::ranges::sort(
        m_stops, [](const ColorStop& a, const ColorStop& b) { return a.position < b.position; });
    bake();
}

Color ColorMap::sample(float position) const noexcept {
    if (m_stops.empty()) {
        return {};
    }

    const float clamped = std::clamp(position, 0.0F, 1.0F);

    if (clamped <= m_stops.front().position) {
        return m_stops.front().color;
    }
    if (clamped >= m_stops.back().position) {
        return m_stops.back().color;
    }

    for (std::size_t i = 1; i < m_stops.size(); ++i) {
        if (clamped <= m_stops[i].position) {
            const ColorStop& from = m_stops[i - 1];
            const ColorStop& to = m_stops[i];
            const float span = to.position - from.position;
            const float t = span > 0.0F ? (clamped - from.position) / span : 0.0F;
            return lerp(from.color, to.color, t);
        }
    }

    return m_stops.back().color;
}

Color ColorMap::sampleDb(float db, float minDb, float maxDb) const noexcept {
    const float span = maxDb - minDb;
    return sample(span > 0.0F ? (db - minDb) / span : 0.0F);
}

void ColorMap::bake() {
    for (std::size_t i = 0; i < kLutSize; ++i) {
        const float position = static_cast<float>(i) / static_cast<float>(kLutSize - 1);
        m_lut[i] = sample(position).packed();
    }
}

Result<ColorMap> ColorMap::loadFromToml(const std::filesystem::path& path) {
    auto table = toml_util::load(path);
    if (!table) {
        return std::unexpected(table.error());
    }

    std::string name = toml_util::getString(*table, "colormap.name", path.stem().string());

    const ::toml::array* stopArray = toml_util::at(*table, "colormap.stops").as_array();
    if (stopArray == nullptr) {
        return fail<ColorMap>(ErrorCode::ParseError, "{}: no colormap.stops array", path.string());
    }

    std::vector<ColorStop> stops;
    for (const ::toml::node& node : *stopArray) {
        const ::toml::table* entry = node.as_table();
        if (entry == nullptr) {
            continue;
        }

        const auto position = (*entry)["pos"].value<double>();
        const auto hex = (*entry)["color"].value<std::string>();
        if (!position || !hex) {
            continue;
        }

        auto color = Color::fromHex(*hex);
        if (!color) {
            return std::unexpected(color.error().withContext(path.string()));
        }
        stops.push_back(ColorStop{.position = static_cast<float>(*position), .color = *color});
    }

    if (stops.size() < 2) {
        return fail<ColorMap>(ErrorCode::ParseError, "{}: a colormap needs at least two stops",
                              path.string());
    }

    return ColorMap{std::move(name), std::move(stops)};
}

Status ColorMap::saveToToml(const std::filesystem::path& path) const {
    ::toml::table root;
    ::toml::table& colormap = toml_util::ensureTable(root, "colormap");
    colormap.insert_or_assign("name", m_name);

    ::toml::array stops;
    for (const ColorStop& stop : m_stops) {
        ::toml::table entry;
        entry.insert_or_assign("pos", static_cast<double>(stop.position));
        entry.insert_or_assign("color", stop.color.toHex(stop.color.a < 1.0F));
        stops.push_back(std::move(entry));
    }
    colormap.insert_or_assign("stops", std::move(stops));

    return toml_util::save(path, root, "Sweep++ colormap");
}

std::vector<ColorMap> builtinColorMaps() {
    // Neutral, descriptive names by rule -- no third-party product name appears
    // in any shipped string or identifier.
    return {
        // The classic blue-cyan-green-yellow-red ramp. High perceived contrast
        // and the most familiar look for a spectrum display, at the cost of
        // being perceptually non-uniform.
        make("spectral", {{0.00F, "#000080"},
                          {0.35F, "#00FFFF"},
                          {0.55F, "#00FF00"},
                          {0.75F, "#FFFF00"},
                          {1.00F, "#800000"}}),

        // The traditional receiver waterfall ramp, and the reason a waterfall
        // usually reads as "black with signals in it".
        //
        // What distinguishes it from `spectral` is where it spends its range:
        // the bottom quarter stays within a hair of black, so an unoccupied
        // band shows as empty rather than as a wash of colour. Only above that
        // does it climb -- blue, white, yellow, orange -- and the very top
        // *darkens* into deep red, which keeps the strongest carriers legible
        // as shapes instead of saturating into one flat blob.
        //
        // A perceptually uniform map is the honest choice for reading a level
        // off the display, but this one is far better at the thing a waterfall
        // is actually for: noticing that something is there at all.
        make("midnight", {{0.000F, "#000020"},
                          {0.083F, "#000030"},
                          {0.167F, "#000050"},
                          {0.250F, "#000091"},
                          {0.333F, "#1E90FF"},
                          {0.417F, "#FFFFFF"},
                          {0.500F, "#FFFF00"},
                          {0.583F, "#FE6D16"},
                          {0.667F, "#FF0000"},
                          {0.750F, "#C60000"},
                          {0.833F, "#9F0000"},
                          {0.917F, "#750000"},
                          {1.000F, "#4A0000"}}),

        // Similar reach, but smoother and without the harsh cyan/green banding.
        make("rainbow", {{0.00F, "#30123B"},
                         {0.25F, "#4489F0"},
                         {0.50F, "#3FE3B4"},
                         {0.70F, "#C4E64B"},
                         {0.85F, "#FB8022"},
                         {1.00F, "#7A0403"}}),

        // Perceptually uniform: equal steps in value look like equal steps in
        // brightness, so structure is not invented by the palette. The right
        // default for measurement.
        make("meadow", {{0.00F, "#440154"},
                        {0.25F, "#3B528B"},
                        {0.50F, "#21918C"},
                        {0.75F, "#5EC962"},
                        {1.00F, "#FDE725"}}),

        make("ember", {{0.00F, "#000004"},
                       {0.25F, "#420A68"},
                       {0.50F, "#932667"},
                       {0.75F, "#DD513A"},
                       {0.90F, "#FCA50A"},
                       {1.00F, "#FCFFA4"}}),

        make("magma", {{0.00F, "#000004"},
                       {0.25F, "#3B0F70"},
                       {0.50F, "#8C2981"},
                       {0.75F, "#DE4968"},
                       {0.90F, "#FE9F6D"},
                       {1.00F, "#FCFDBF"}}),

        make("plasma", {{0.00F, "#0D0887"},
                        {0.25F, "#6A00A8"},
                        {0.50F, "#B12A90"},
                        {0.75F, "#E16462"},
                        {0.90F, "#FCA636"},
                        {1.00F, "#F0F921"}}),

        // Monochrome: the honest choice when a screenshot must survive being
        // printed, and the easiest for judging relative level.
        make("grayscale", {{0.00F, "#000000"}, {1.00F, "#FFFFFF"}}),

        make("ice",
             {{0.00F, "#000814"}, {0.35F, "#003566"}, {0.70F, "#00A6FB"}, {1.00F, "#CAF0F8"}}),

        make("fire", {{0.00F, "#000000"},
                      {0.30F, "#7F0000"},
                      {0.60F, "#FF4500"},
                      {0.85F, "#FFD700"},
                      {1.00F, "#FFFFFF"}}),

        // Green-on-black, the traditional analyser look.
        make("classic",
             {{0.00F, "#000000"}, {0.40F, "#003B00"}, {0.70F, "#00C000"}, {1.00F, "#CCFFCC"}}),

        make("aurora", {{0.00F, "#01050A"},
                        {0.30F, "#12403A"},
                        {0.55F, "#1FA37A"},
                        {0.80F, "#7BE495"},
                        {1.00F, "#EAFFD0"}}),
    };
}

const ColorMap& builtinColorMap(std::string_view name) {
    static const std::vector<ColorMap> maps = builtinColorMaps();

    for (const ColorMap& map : maps) {
        if (map.name() == name) {
            return map;
        }
    }
    return maps.front();
}

} // namespace sweeppp::ui
