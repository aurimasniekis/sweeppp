// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <sweeppp/core/Result.hpp>

namespace sweeppp::ui {

/// Preferences that belong to the machine rather than to a measurement.
///
/// Deliberately not in `settings.toml`. That file **is** the Profile struct,
/// and loading a named profile replaces it wholesale -- so a UI scale kept
/// there would follow a sweep configuration around between machines and
/// change the size of the text on screen when an operator loaded one. Plugin
/// state was moved out of the profile for exactly this reason.
///
/// Lives beside the other application-level stores in the config directory:
/// `plugins.toml`, `sweep-presets.toml`, `marker-presets.toml`.
struct AppSettings {
    /// Multiplier on every size in the interface, or 0 to follow the display.
    ///
    /// Zero rather than a resolved number, because the right value is a
    /// property of the screen the window is on and that can change between
    /// runs -- an external monitor, a different machine sharing a config
    /// directory over a home directory. Resolving it at start-up keeps
    /// "whatever this display wants" true rather than true once.
    float uiScale = 0.0F;

    /// Interface text size in points, before any scaling.
    float fontSize = 15.0F;

    /// How heavily glyph coverage is boosted, against what the loaded face
    /// asks for. 1 is that face's own figure; below it is lighter.
    ///
    /// Relative rather than absolute because the figure it multiplies differs
    /// per face: a Medium cut needs almost none of this and a Regular needs a
    /// good deal, and which one is loaded depends on the platform. Keeping the
    /// preference relative means "a little lighter than normal" survives being
    /// carried to a machine that finds a different font.
    float fontWeight = 1.0F;

    /// Whether to ask GitHub about newer releases at start-up.
    bool checkForUpdates = true;

    /// The version whose notes were last shown in What's new; empty before the
    /// first run, which is what shows them on a fresh install.
    std::string whatsNewSeen;

    /// Reads the file, or returns the defaults. A missing file is the normal
    /// first-run case and not an error; a malformed one is logged and then
    /// treated the same way, because refusing to start over a preferences file
    /// would be a poor trade.
    [[nodiscard]] static AppSettings load(const std::filesystem::path& path);

    [[nodiscard]] Status save(const std::filesystem::path& path) const;

    /// Smallest and largest values the panel offers. Bounds rather than
    /// guidance: a scale of 0.1 leaves an interface nothing can be clicked in,
    /// and there is no way back from it through that same interface.
    static constexpr float kMinUiScale = 0.75F;
    static constexpr float kMaxUiScale = 3.0F;
    static constexpr float kMinFontSize = 9.0F;
    static constexpr float kMaxFontSize = 32.0F;
    static constexpr float kMinFontWeight = 0.7F;
    static constexpr float kMaxFontWeight = 1.4F;
};

} // namespace sweeppp::ui
