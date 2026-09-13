// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/ui/ColorMap.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace sweeppp::ui {

/// Colours of the application chrome.
struct ChromeTheme {
    bool dark = true;

    Color windowBackground;
    Color panelBackground;
    Color headerBackground;
    Color text;
    Color textDim;
    Color accent;
    Color accentHover;
    Color border;
    Color separator;
    /// Buttons are *raised*: lighter than the panel behind them.
    Color buttonBackground;
    Color buttonHover;
    Color buttonActive;

    /// Data-entry controls are *inset*: darker than the panel behind them.
    ///
    /// Depth is carried by fill rather than by an outline. Bordering every
    /// framed widget would separate them too, but ImGui applies FrameBorderSize
    /// to buttons and headers as well, and the result is a box around
    /// everything -- visual noise that makes the panel harder to scan, not
    /// easier. A raised/inset pair reads instantly and still survives glare,
    /// because the fill difference is large.
    Color inputBackground;
    Color inputHover;
    Color inputActive;

    /// Start green / stop red. Named by role, not colour, so a theme may pick
    /// different hues without the code reading as a lie.
    Color start;
    Color stop;
    Color record;

    /// Status badge colours for the drop/throttle indicator.
    Color ok;
    Color warning;
    Color danger;

    float rounding = 4.0F;
    float borderSize = 1.0F;
};

/// Colours of the spectrum plot.
struct SpectrumTheme {
    Color background;
    Color grid;
    Color axisText;

    /// One per trace. Independently toggled and coloured.
    Color traceLive;
    Color traceMaxHold;
    Color traceMinHold;
    Color traceAverage;

    Color cursor;
    Color marker;
    Color markerText;
    Color selection;

    /// Contribution spans are drawn under the trace; the alpha keeps them from
    /// competing with the signal.
    float contributionAlpha = 0.18F;

    /// Colormap used for the gradient fill under the live trace. Separate from
    /// the waterfall's on purpose -- they often want to match, but need not.
    std::string fillColorMap = "spectral";
    float fillAlpha = 0.55F;

    float traceThickness = 1.4F;
};

struct WaterfallTheme {
    std::string colorMap = "spectral";
};

/// A complete look: chrome, spectrum and colormaps.
///
/// Themes are TOML files in `resources/themes/` (built in) and the user config
/// directory (user-authored), switchable live with no restart, and recorded in
/// the profile.
class Theme {
public:
    Theme();

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    void setName(std::string name) { m_name = std::move(name); }

    [[nodiscard]] const ChromeTheme& chrome() const noexcept { return m_chrome; }
    [[nodiscard]] const SpectrumTheme& spectrum() const noexcept { return m_spectrum; }
    [[nodiscard]] const WaterfallTheme& waterfall() const noexcept { return m_waterfall; }

    [[nodiscard]] ChromeTheme& chrome() noexcept { return m_chrome; }
    [[nodiscard]] SpectrumTheme& spectrum() noexcept { return m_spectrum; }
    [[nodiscard]] WaterfallTheme& waterfall() noexcept { return m_waterfall; }

    /// Colormaps this theme defines, in addition to the built-ins.
    [[nodiscard]] const std::vector<ColorMap>& colorMaps() const noexcept { return m_colorMaps; }
    void addColorMap(ColorMap map);

    /// Resolves a colormap by name: theme-local first, then built-in.
    [[nodiscard]] const ColorMap& resolveColorMap(std::string_view name) const;

    [[nodiscard]] const ColorMap& waterfallColorMap() const {
        return resolveColorMap(m_waterfall.colorMap);
    }
    [[nodiscard]] const ColorMap& spectrumFillColorMap() const {
        return resolveColorMap(m_spectrum.fillColorMap);
    }

    [[nodiscard]] static Result<Theme> loadFromToml(const std::filesystem::path& path);
    [[nodiscard]] Status saveToToml(const std::filesystem::path& path) const;

    /// The shipped themes.
    [[nodiscard]] static Theme builtinDark();
    /// Maximum surface separation, for direct sunlight or a poor external
    /// panel -- where the standard dark theme's surfaces merge into one sheet.
    [[nodiscard]] static Theme builtinHighContrast();
    [[nodiscard]] static Theme builtinLight();
    [[nodiscard]] static std::vector<Theme> builtinThemes();

private:
    std::string m_name = "Dark";
    ChromeTheme m_chrome;
    SpectrumTheme m_spectrum;
    WaterfallTheme m_waterfall;
    std::vector<ColorMap> m_colorMaps;
};

/// Loads every theme found on the search path, user overrides masking
/// built-ins by name.
[[nodiscard]] std::vector<Theme> discoverThemes();

/// Loads every colormap found on the search path, plus the built-ins.
[[nodiscard]] std::vector<ColorMap> discoverColorMaps();

} // namespace sweeppp::ui
