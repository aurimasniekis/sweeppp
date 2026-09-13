// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/ui/Theme.hpp"

#include "sweeppp/core/Log.hpp"
#include "sweeppp/core/Paths.hpp"
#include "sweeppp/core/Toml.hpp"

#include <algorithm>

namespace sweeppp::ui {
namespace {

Color hex(const char* text) {
    if (auto color = Color::fromHex(text)) {
        return *color;
    }
    return Color{};
}

/// Reads a colour key, keeping the existing value when absent so a partial
/// theme file inherits the rest rather than turning black.
void readColor(const ::toml::table& table, std::string_view key, Color& out) {
    const auto text = toml_util::at(table, key).value<std::string>();
    if (!text) {
        return;
    }
    if (auto parsed = Color::fromHex(*text)) {
        out = *parsed;
    } else {
        logWarn("theme", "{} is not a colour: {}", key, *text);
    }
}

void writeColor(::toml::table& table, std::string_view key, const Color& color) {
    table.insert_or_assign(std::string(key), color.toHex(color.a < 1.0F));
}

/// The built-in palettes, as free functions.
///
/// Deliberately not expressed as `Theme` objects: the default constructor
/// needs them, so anything that constructs a Theme to produce them would
/// recurse until the stack ran out.
/// Surface ladder for the dark theme.
///
/// The steps between levels are deliberately wide. In bright ambient light --
/// a laptop outdoors, or any screen with glare -- the display's effective
/// contrast collapses at the bottom of the range, and everything below roughly
/// #202020 reads as the same black. A palette whose surfaces sit within a few
/// values of each other looks refined indoors and becomes a single flat sheet
/// outdoors, with panel edges and buttons simply invisible.
///
/// So the whole ladder is lifted off pure black and the gaps widened. The plot
/// background stays dark, because that is where a *high* contrast ratio
/// against the traces matters more than surface separation.
ChromeTheme darkChrome() {
    return ChromeTheme{
        .dark = true,
        .windowBackground = hex("#12161C"),
        .panelBackground = hex("#1B212A"),
        .headerBackground = hex("#262E3A"),
        .text = hex("#F5F7FA"),
        // Secondary text still has to be readable in sun; the old value was
        // decorative rather than legible.
        .textDim = hex("#B0BCCC"),
        .accent = hex("#4D9BFF"),
        .accentHover = hex("#7AB4FF"),
        // Borders carry the layout when fills stop being distinguishable, so
        // they are much lighter than the surfaces they separate.
        .border = hex("#55637A"),
        .separator = hex("#3A4556"),
        .buttonBackground = hex("#333D4D"),
        .buttonHover = hex("#414D61"),
        .buttonActive = hex("#4F5D75"),
        .inputBackground = hex("#0D1117"),
        .inputHover = hex("#141A22"),
        .inputActive = hex("#1A222C"),
        .start = hex("#2FBF5F"),
        .stop = hex("#E5484D"),
        .record = hex("#E5484D"),
        .ok = hex("#3DDC84"),
        .warning = hex("#FFB020"),
        .danger = hex("#FF5A5A"),
        .rounding = 4.0F,
        .borderSize = 1.0F,
    };
}

/// Maximum separation, for direct sunlight or a poor external panel.
///
/// Not merely "more contrast": the surfaces are lifted further still, borders
/// approach mid-grey, and text is pure white. It gives up some visual polish
/// in exchange for staying usable where the standard dark theme washes out.
ChromeTheme highContrastChrome() {
    return ChromeTheme{
        .dark = true,
        .windowBackground = hex("#0A0C10"),
        .panelBackground = hex("#1E2530"),
        .headerBackground = hex("#2E3846"),
        .text = hex("#FFFFFF"),
        .textDim = hex("#C8D2E0"),
        .accent = hex("#64ABFF"),
        .accentHover = hex("#8FC4FF"),
        .border = hex("#7A8AA3"),
        .separator = hex("#4A5768"),
        .buttonBackground = hex("#3E4A5C"),
        .buttonHover = hex("#4F5D72"),
        .buttonActive = hex("#61708A"),
        .inputBackground = hex("#05070A"),
        .inputHover = hex("#0E141C"),
        .inputActive = hex("#161E28"),
        .start = hex("#3BE07A"),
        .stop = hex("#FF5F5F"),
        .record = hex("#FF5F5F"),
        .ok = hex("#3BE07A"),
        .warning = hex("#FFC53D"),
        .danger = hex("#FF6B6B"),
        .rounding = 3.0F,
        .borderSize = 1.0F,
    };
}

SpectrumTheme darkSpectrum() {
    return SpectrumTheme{
        // Black, not merely dark.
        //
        // The plots are one continuous surface running the width of the
        // window, and the contrast that matters inside it is trace against
        // background rather than surface against surface. Anything lighter
        // reads as a panel sitting on the window, which is exactly what this
        // is not -- and it costs dynamic range at the bottom of the colour
        // map, where the noise floor lives.
        .background = hex("#000000"),
        // Grid lines were effectively invisible in daylight at #232833. They
        // are what let an operator read a level off the plot without a marker,
        // so they need to survive glare.
        .grid = hex("#38414F"),
        .axisText = hex("#B0BCCC"),
        // Four visually separable hues that stay distinguishable when two
        // traces overlap, which they constantly do.
        .traceLive = hex("#4ADE80"),
        .traceMaxHold = hex("#EF4444"),
        .traceMinHold = hex("#60A5FA"),
        .traceAverage = hex("#FBBF24"),
        // Neutral, not a hue of their own.
        //
        // A marker is a pointer at the measurement rather than another
        // measurement, and it is drawn over six hundred allocation chips in
        // every colour a plugin felt like. A yellow cursor vanished into the
        // average trace and a pink one into whatever band it was standing in;
        // white against the plot's black reads over all of it, and the
        // selection is told from the rest by brightness instead.
        .cursor = hex("#FFFFFF"),
        .marker = hex("#9FB0C4"),
        .markerText = hex("#F8FAFC"),
        .selection = hex("#3B82F6"),
        .contributionAlpha = 0.18F,
        .fillColorMap = "midnight",
        // A soft wash under the trace, not a second display competing with it:
        // the shape of the trace is the measurement, the fill is context.
        .fillAlpha = 0.28F,
        .traceThickness = 1.4F,
    };
}

SpectrumTheme highContrastSpectrum() {
    SpectrumTheme spectrum = darkSpectrum();
    spectrum.background = hex("#000000");
    spectrum.grid = hex("#4A5768");
    spectrum.axisText = hex("#E4EAF2");
    // Thicker, more saturated traces: a 1.4 px line disappears against glare.
    spectrum.traceLive = hex("#3BE07A");
    spectrum.traceMaxHold = hex("#FF5F5F");
    spectrum.traceMinHold = hex("#64ABFF");
    spectrum.traceAverage = hex("#FFC53D");
    spectrum.traceThickness = 1.9F;
    // A lighter fill would compete with the trace it sits under.
    spectrum.fillAlpha = 0.40F;
    return spectrum;
}

ChromeTheme lightChrome() {
    return ChromeTheme{
        .dark = false,
        .windowBackground = hex("#F5F6F8"),
        .panelBackground = hex("#FFFFFF"),
        .headerBackground = hex("#ECEFF3"),
        .text = hex("#111418"),
        .textDim = hex("#5B6472"),
        .accent = hex("#2563EB"),
        .accentHover = hex("#3B82F6"),
        .border = hex("#D3D8E0"),
        .separator = hex("#E2E6EC"),
        .buttonBackground = hex("#E8ECF1"),
        .buttonHover = hex("#DDE3EA"),
        .buttonActive = hex("#CFD7E1"),
        .inputBackground = hex("#F2F5F9"),
        .inputHover = hex("#E9EEF4"),
        .inputActive = hex("#DFE6EE"),
        .start = hex("#16A34A"),
        .stop = hex("#DC2626"),
        .record = hex("#DC2626"),
        .ok = hex("#16A34A"),
        .warning = hex("#D97706"),
        .danger = hex("#DC2626"),
        .rounding = 4.0F,
        .borderSize = 1.0F,
    };
}

SpectrumTheme lightSpectrum() {
    return SpectrumTheme{
        .background = hex("#FFFFFF"),
        .grid = hex("#DFE4EA"),
        .axisText = hex("#5B6472"),
        // Darker than the dark theme's: the same hues on white would wash out.
        .traceLive = hex("#15803D"),
        .traceMaxHold = hex("#B91C1C"),
        .traceMinHold = hex("#1D4ED8"),
        .traceAverage = hex("#B45309"),
        // The dark theme's reasoning inverted: on a light plot the readable
        // neutral is the dark one.
        .cursor = hex("#0B1220"),
        .marker = hex("#4B5563"),
        .markerText = hex("#111418"),
        .selection = hex("#2563EB"),
        .contributionAlpha = 0.14F,
        .fillColorMap = "meadow",
        .fillAlpha = 0.35F,
        .traceThickness = 1.4F,
    };
}

} // namespace

Theme::Theme() : m_chrome(darkChrome()), m_spectrum(darkSpectrum()) {
    m_name = "Dark";
    m_waterfall.colorMap = "midnight";
}

void Theme::addColorMap(ColorMap map) {
    const auto existing = std::ranges::find_if(
        m_colorMaps, [&map](const ColorMap& candidate) { return candidate.name() == map.name(); });
    if (existing != m_colorMaps.end()) {
        *existing = std::move(map);
        return;
    }
    m_colorMaps.push_back(std::move(map));
}

const ColorMap& Theme::resolveColorMap(std::string_view name) const {
    // Theme-local first: a theme that ships its own gradient must be able to
    // shadow a built-in of the same name.
    const auto local = std::ranges::find_if(
        m_colorMaps, [name](const ColorMap& candidate) { return candidate.name() == name; });
    if (local != m_colorMaps.end()) {
        return *local;
    }
    return builtinColorMap(name);
}

Theme Theme::builtinDark() {
    // The default constructor already is the dark theme.
    return Theme{};
}

Theme Theme::builtinLight() {
    Theme theme;
    theme.m_name = "Light";
    theme.m_chrome = lightChrome();
    theme.m_spectrum = lightSpectrum();
    // Perceptually uniform on a light background: equal steps in level look
    // like equal steps in brightness, so the palette does not invent structure.
    theme.m_waterfall = WaterfallTheme{.colorMap = "meadow"};
    return theme;
}

Theme Theme::builtinHighContrast() {
    Theme theme;
    theme.m_name = "High Contrast";
    theme.m_chrome = highContrastChrome();
    theme.m_spectrum = highContrastSpectrum();
    theme.m_waterfall = WaterfallTheme{.colorMap = "spectral"};
    return theme;
}

std::vector<Theme> Theme::builtinThemes() {
    return {builtinDark(), builtinHighContrast(), builtinLight()};
}

Result<Theme> Theme::loadFromToml(const std::filesystem::path& path) {
    auto table = toml_util::load(path);
    if (!table) {
        return std::unexpected(table.error());
    }

    // `base` picks which built-in to start from, so a theme file only has to
    // state what it changes. A file that had to specify every colour would be
    // unwritable by hand and would break on every new key we add.
    const std::string base = toml_util::getString(*table, "theme.base", "dark");
    Theme theme = base == "light" ? builtinLight() : builtinDark();
    theme.m_name = toml_util::getString(*table, "theme.name", path.stem().string());
    theme.m_chrome.dark = base != "light";

    if (const ::toml::table* chrome = toml_util::at(*table, "theme.chrome").as_table()) {
        readColor(*chrome, "window_background", theme.m_chrome.windowBackground);
        readColor(*chrome, "panel_background", theme.m_chrome.panelBackground);
        readColor(*chrome, "header_background", theme.m_chrome.headerBackground);
        readColor(*chrome, "text", theme.m_chrome.text);
        readColor(*chrome, "text_dim", theme.m_chrome.textDim);
        readColor(*chrome, "accent", theme.m_chrome.accent);
        readColor(*chrome, "accent_hover", theme.m_chrome.accentHover);
        readColor(*chrome, "border", theme.m_chrome.border);
        readColor(*chrome, "separator", theme.m_chrome.separator);
        readColor(*chrome, "button", theme.m_chrome.buttonBackground);
        readColor(*chrome, "button_hover", theme.m_chrome.buttonHover);
        readColor(*chrome, "button_active", theme.m_chrome.buttonActive);
        readColor(*chrome, "input", theme.m_chrome.inputBackground);
        readColor(*chrome, "input_hover", theme.m_chrome.inputHover);
        readColor(*chrome, "input_active", theme.m_chrome.inputActive);
        readColor(*chrome, "start", theme.m_chrome.start);
        readColor(*chrome, "stop", theme.m_chrome.stop);
        readColor(*chrome, "record", theme.m_chrome.record);
        readColor(*chrome, "ok", theme.m_chrome.ok);
        readColor(*chrome, "warning", theme.m_chrome.warning);
        readColor(*chrome, "danger", theme.m_chrome.danger);

        theme.m_chrome.rounding = toml_util::getFloat(*chrome, "rounding", theme.m_chrome.rounding);
        theme.m_chrome.borderSize =
            toml_util::getFloat(*chrome, "border_size", theme.m_chrome.borderSize);
    }

    if (const ::toml::table* spectrum = toml_util::at(*table, "theme.spectrum").as_table()) {
        readColor(*spectrum, "background", theme.m_spectrum.background);
        readColor(*spectrum, "grid", theme.m_spectrum.grid);
        readColor(*spectrum, "axis_text", theme.m_spectrum.axisText);
        readColor(*spectrum, "trace_live", theme.m_spectrum.traceLive);
        readColor(*spectrum, "trace_max", theme.m_spectrum.traceMaxHold);
        readColor(*spectrum, "trace_min", theme.m_spectrum.traceMinHold);
        readColor(*spectrum, "trace_avg", theme.m_spectrum.traceAverage);
        readColor(*spectrum, "cursor", theme.m_spectrum.cursor);
        readColor(*spectrum, "marker", theme.m_spectrum.marker);
        readColor(*spectrum, "marker_text", theme.m_spectrum.markerText);
        readColor(*spectrum, "selection", theme.m_spectrum.selection);

        theme.m_spectrum.contributionAlpha = toml_util::getFloat(
            *spectrum, "contribution_alpha", theme.m_spectrum.contributionAlpha);
        theme.m_spectrum.fillColorMap =
            toml_util::getString(*spectrum, "fill_colormap", theme.m_spectrum.fillColorMap);
        theme.m_spectrum.fillAlpha =
            toml_util::getFloat(*spectrum, "fill_alpha", theme.m_spectrum.fillAlpha);
        theme.m_spectrum.traceThickness =
            toml_util::getFloat(*spectrum, "trace_thickness", theme.m_spectrum.traceThickness);
    }

    if (const ::toml::table* waterfall = toml_util::at(*table, "theme.waterfall").as_table()) {
        theme.m_waterfall.colorMap =
            toml_util::getString(*waterfall, "colormap", theme.m_waterfall.colorMap);
    }

    // A theme may carry its own colormaps inline, so "here is my look" is one
    // file rather than several that must be kept together.
    if (const ::toml::array* colormaps = toml_util::at(*table, "colormap").as_array()) {
        for (const ::toml::node& node : *colormaps) {
            const ::toml::table* entry = node.as_table();
            if (entry == nullptr) {
                continue;
            }

            const std::string name = toml_util::getString(*entry, "name", "");
            const ::toml::array* stopArray = (*entry)["stops"].as_array();
            if (name.empty() || stopArray == nullptr) {
                continue;
            }

            std::vector<ColorStop> stops;
            for (const ::toml::node& stopNode : *stopArray) {
                const ::toml::table* stop = stopNode.as_table();
                if (stop == nullptr) {
                    continue;
                }
                const auto position = (*stop)["pos"].value<double>();
                const auto colorText = (*stop)["color"].value<std::string>();
                if (!position || !colorText) {
                    continue;
                }
                if (auto color = Color::fromHex(*colorText)) {
                    stops.push_back(
                        ColorStop{.position = static_cast<float>(*position), .color = *color});
                }
            }

            if (stops.size() >= 2) {
                theme.addColorMap(ColorMap{name, std::move(stops)});
            }
        }
    }

    return theme;
}

Status Theme::saveToToml(const std::filesystem::path& path) const {
    ::toml::table root;

    ::toml::table& theme = toml_util::ensureTable(root, "theme");
    theme.insert_or_assign("name", m_name);
    theme.insert_or_assign("base", std::string(m_chrome.dark ? "dark" : "light"));

    ::toml::table& chrome = toml_util::ensureTable(root, "theme.chrome");
    writeColor(chrome, "window_background", m_chrome.windowBackground);
    writeColor(chrome, "panel_background", m_chrome.panelBackground);
    writeColor(chrome, "header_background", m_chrome.headerBackground);
    writeColor(chrome, "text", m_chrome.text);
    writeColor(chrome, "text_dim", m_chrome.textDim);
    writeColor(chrome, "accent", m_chrome.accent);
    writeColor(chrome, "accent_hover", m_chrome.accentHover);
    writeColor(chrome, "border", m_chrome.border);
    writeColor(chrome, "separator", m_chrome.separator);
    writeColor(chrome, "button", m_chrome.buttonBackground);
    writeColor(chrome, "button_hover", m_chrome.buttonHover);
    writeColor(chrome, "button_active", m_chrome.buttonActive);
    writeColor(chrome, "input", m_chrome.inputBackground);
    writeColor(chrome, "input_hover", m_chrome.inputHover);
    writeColor(chrome, "input_active", m_chrome.inputActive);
    writeColor(chrome, "start", m_chrome.start);
    writeColor(chrome, "stop", m_chrome.stop);
    writeColor(chrome, "record", m_chrome.record);
    writeColor(chrome, "ok", m_chrome.ok);
    writeColor(chrome, "warning", m_chrome.warning);
    writeColor(chrome, "danger", m_chrome.danger);
    chrome.insert_or_assign("rounding", static_cast<double>(m_chrome.rounding));
    chrome.insert_or_assign("border_size", static_cast<double>(m_chrome.borderSize));

    ::toml::table& spectrum = toml_util::ensureTable(root, "theme.spectrum");
    writeColor(spectrum, "background", m_spectrum.background);
    writeColor(spectrum, "grid", m_spectrum.grid);
    writeColor(spectrum, "axis_text", m_spectrum.axisText);
    writeColor(spectrum, "trace_live", m_spectrum.traceLive);
    writeColor(spectrum, "trace_max", m_spectrum.traceMaxHold);
    writeColor(spectrum, "trace_min", m_spectrum.traceMinHold);
    writeColor(spectrum, "trace_avg", m_spectrum.traceAverage);
    writeColor(spectrum, "cursor", m_spectrum.cursor);
    writeColor(spectrum, "marker", m_spectrum.marker);
    writeColor(spectrum, "marker_text", m_spectrum.markerText);
    writeColor(spectrum, "selection", m_spectrum.selection);
    spectrum.insert_or_assign("contribution_alpha",
                              static_cast<double>(m_spectrum.contributionAlpha));
    spectrum.insert_or_assign("fill_colormap", m_spectrum.fillColorMap);
    spectrum.insert_or_assign("fill_alpha", static_cast<double>(m_spectrum.fillAlpha));
    spectrum.insert_or_assign("trace_thickness", static_cast<double>(m_spectrum.traceThickness));

    ::toml::table& waterfall = toml_util::ensureTable(root, "theme.waterfall");
    waterfall.insert_or_assign("colormap", m_waterfall.colorMap);

    if (!m_colorMaps.empty()) {
        ::toml::array colormaps;
        for (const ColorMap& map : m_colorMaps) {
            ::toml::table entry;
            entry.insert_or_assign("name", map.name());

            ::toml::array stops;
            for (const ColorStop& stop : map.stops()) {
                ::toml::table stopEntry;
                stopEntry.insert_or_assign("pos", static_cast<double>(stop.position));
                stopEntry.insert_or_assign("color", stop.color.toHex(stop.color.a < 1.0F));
                stops.push_back(std::move(stopEntry));
            }
            entry.insert_or_assign("stops", std::move(stops));
            colormaps.push_back(std::move(entry));
        }
        root.insert_or_assign("colormap", std::move(colormaps));
    }

    return toml_util::save(path, root, "Sweep++ theme");
}

std::vector<Theme> discoverThemes() {
    std::vector<Theme> themes = Theme::builtinThemes();

    const Paths& paths = Paths::instance();
    for (const std::string& name : paths.listResources("themes")) {
        auto path = paths.findResource("themes", name);
        if (!path) {
            continue;
        }

        auto theme = Theme::loadFromToml(*path);
        if (!theme) {
            logWarn("theme", "skipping {}: {}", path->string(), theme.error().message());
            continue;
        }

        // A user theme of the same name replaces the built-in, which is what
        // makes "edit the shipped dark theme" work without touching read-only
        // files.
        const auto existing = std::ranges::find_if(
            themes, [&theme](const Theme& candidate) { return candidate.name() == theme->name(); });
        if (existing != themes.end()) {
            *existing = std::move(*theme);
        } else {
            themes.push_back(std::move(*theme));
        }
    }

    return themes;
}

std::vector<ColorMap> discoverColorMaps() {
    std::vector<ColorMap> maps = builtinColorMaps();

    const Paths& paths = Paths::instance();
    for (const std::string& name : paths.listResources("colormaps")) {
        auto path = paths.findResource("colormaps", name);
        if (!path) {
            continue;
        }

        auto map = ColorMap::loadFromToml(*path);
        if (!map) {
            logWarn("colormap", "skipping {}: {}", path->string(), map.error().message());
            continue;
        }

        const auto existing = std::ranges::find_if(
            maps, [&map](const ColorMap& candidate) { return candidate.name() == map->name(); });
        if (existing != maps.end()) {
            *existing = std::move(*map);
        } else {
            maps.push_back(std::move(*map));
        }
    }

    return maps;
}

} // namespace sweeppp::ui
