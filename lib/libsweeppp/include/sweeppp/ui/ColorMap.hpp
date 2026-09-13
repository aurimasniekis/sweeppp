// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sweeppp::ui {

/// RGBA, 0-1.
struct Color {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 1.0F;

    [[nodiscard]] static Result<Color> fromHex(std::string_view hex);
    [[nodiscard]] std::string toHex(bool includeAlpha = false) const;

    /// Packed 0xAABBGGRR, the layout ImGui's ImU32 expects.
    [[nodiscard]] std::uint32_t packed() const noexcept;

    /// Perceived brightness, 0 to 1.
    ///
    /// The green-weighted formula, not the mean of the channels: it decides
    /// whether text on top of this colour should be black or white, and by the
    /// mean a saturated yellow and a saturated blue come out the same when one
    /// of them is nearly white to the eye and the other nearly black.
    [[nodiscard]] float luminance() const noexcept {
        return (0.2126F * r) + (0.7152F * g) + (0.0722F * b);
    }

    [[nodiscard]] Color withAlpha(float alpha) const noexcept {
        return {.r = r, .g = g, .b = b, .a = alpha};
    }
};

/// One stop in a gradient.
struct ColorStop {
    float position = 0.0F; ///< 0-1
    Color color;
};

/// A named gradient, baked into a 256-entry lookup table.
///
/// The LUT is the shared currency of the whole visual layer: the waterfall
/// fragment shader samples it as a 1D texture, the spectrum's gradient fill
/// indexes it per vertex, and the right-edge gradient bar draws it directly.
/// Because editing stops re-bakes this one table, editing, previewing and
/// selecting a built-in colormap are all the same code path -- which is what
/// makes the gradient editor nearly free.
class ColorMap {
public:
    static constexpr std::size_t kLutSize = 256;

    ColorMap() = default;
    ColorMap(std::string name, std::vector<ColorStop> stops);

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    void setName(std::string name) { m_name = std::move(name); }

    [[nodiscard]] const std::vector<ColorStop>& stops() const noexcept { return m_stops; }

    /// Replaces the stops and re-bakes. Stops are sorted by position, so an
    /// editor may drag one past another without corrupting the gradient.
    void setStops(std::vector<ColorStop> stops);

    /// Baked table, RGBA8, `kLutSize` entries. Uploaded verbatim as a 1D
    /// texture.
    [[nodiscard]] const std::array<std::uint32_t, kLutSize>& lut() const noexcept { return m_lut; }

    /// Colour at a normalised position, interpolated from the stops.
    [[nodiscard]] Color sample(float position) const noexcept;

    /// Colour for a dB value against the current gradient range.
    [[nodiscard]] Color sampleDb(float db, float minDb, float maxDb) const noexcept;

    [[nodiscard]] bool empty() const noexcept { return m_stops.empty(); }

    [[nodiscard]] static Result<ColorMap> loadFromToml(const std::filesystem::path& path);
    [[nodiscard]] Status saveToToml(const std::filesystem::path& path) const;

private:
    void bake();

    std::string m_name;
    std::vector<ColorStop> m_stops;
    std::array<std::uint32_t, kLutSize> m_lut{};
};

/// Built-in colormaps.
///
/// Names are neutral and descriptive by rule: no third-party product name
/// appears in shipped code, resources, identifiers or UI strings.
[[nodiscard]] std::vector<ColorMap> builtinColorMaps();

/// Finds a built-in by name, or the first one when the name is unknown.
[[nodiscard]] const ColorMap& builtinColorMap(std::string_view name);

} // namespace sweeppp::ui
