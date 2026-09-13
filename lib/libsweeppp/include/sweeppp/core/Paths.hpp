// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sweeppp {

/// Where Sweep++ reads and writes files.
///
/// Two roots, and the split matters: `resources/` ships with the build and is
/// read-only, while the per-user config directory is writable. A user file
/// shadows a built-in one *by name*, so "edit the built-in Jet colormap"
/// means "write jet.toml into the config dir" -- the original is never
/// modified and can always be recovered by deleting the override.
class Paths {
public:
    /// Resolves every directory once. Call at start-up; the result is cheap to
    /// copy and safe to keep.
    [[nodiscard]] static const Paths& instance();

    /// macOS   ~/Library/Application Support/<appId>
    /// Linux   $XDG_CONFIG_HOME/<appId>, else ~/.config/<appId>
    /// Windows %APPDATA%\<appId>
    ///
    /// `appId()` is "sweeppp" for a release and "sweeppp-nightly" for a
    /// nightly, so the two keep apart -- unless usesReleaseConfig(), in which
    /// case a nightly reads and writes the release's directory.
    [[nodiscard]] const std::filesystem::path& configDir() const noexcept { return m_configDir; }

    /// Read-only assets that ship with the build: fonts, built-in themes,
    /// colormaps and band plans.
    [[nodiscard]] const std::filesystem::path& resourcesDir() const noexcept {
        return m_resourcesDir;
    }

    [[nodiscard]] std::filesystem::path themesDir() const { return m_configDir / "themes"; }
    [[nodiscard]] std::filesystem::path colormapsDir() const { return m_configDir / "colormaps"; }
    [[nodiscard]] std::filesystem::path bandPlansDir() const { return m_configDir / "bandplans"; }
    [[nodiscard]] std::filesystem::path profilesDir() const { return m_configDir / "profiles"; }
    [[nodiscard]] std::filesystem::path sweepPlansDir() const { return m_configDir / "sweepplans"; }
    [[nodiscard]] std::filesystem::path pluginsDir() const { return m_configDir / "plugins"; }

    /// The antenna library, and the port assignments that reference it.
    ///
    /// Not in a profile: what is screwed onto RX1 is a fact about the bench,
    /// and loading a sweep profile must not silently rewire it.
    [[nodiscard]] std::filesystem::path antennasDir() const { return m_configDir / "antennas"; }

    /// Where live sessions and recordings land by default.
    [[nodiscard]] std::filesystem::path sessionsDir() const { return m_configDir / "sessions"; }

    [[nodiscard]] std::filesystem::path settingsFile() const {
        return m_configDir / "settings.toml";
    }
    [[nodiscard]] std::filesystem::path logFile() const { return m_configDir / "sweeppp.log"; }

    /// Built-in plugins that shipped alongside the executable.
    [[nodiscard]] const std::filesystem::path& bundledPluginsDir() const noexcept {
        return m_bundledPluginsDir;
    }

    /// Every directory a named asset of `kind` may live in, user overrides
    /// first. Callers walk this in order and take the first hit, which is what
    /// gives user files precedence over built-ins.
    [[nodiscard]] std::vector<std::filesystem::path> searchPath(std::string_view kind) const;

    /// Finds `name` under `kind` across the search path. `name` may carry the
    /// .toml suffix or not.
    [[nodiscard]] Result<std::filesystem::path> findResource(std::string_view kind,
                                                             std::string_view name) const;

    /// Lists every asset name available under `kind`, deduplicated, with user
    /// overrides masking built-ins. Sorted, so menus are stable.
    [[nodiscard]] std::vector<std::string> listResources(std::string_view kind) const;

    /// Creates the writable config tree. Safe to call repeatedly.
    [[nodiscard]] Status ensureConfigTree() const;

    /// Overrides the config root. For tests and for the `--config-dir` flag of
    /// `sweeppp` and `sweeppp-cli`; must be called before the first instance().
    static void setConfigDirOverride(std::filesystem::path dir);

    /// Whether a nightly has been told to use the release's configuration
    /// directory instead of its own. Always false for a release build, which
    /// has nothing to defer to. `--config-dir` outranks it either way.
    [[nodiscard]] static bool usesReleaseConfig();

    /// Records the choice above, as a marker file in the nightly's own
    /// directory -- the one place that stays the same whichever directory is
    /// in use. Takes effect on the next start: Paths is resolved once, and
    /// everything already open was read from where it was.
    [[nodiscard]] static Status setUsesReleaseConfig(bool enabled);

private:
    Paths();

    std::filesystem::path m_configDir;
    std::filesystem::path m_resourcesDir;
    std::filesystem::path m_bundledPluginsDir;
};

/// The running executable itself. Empty only where the platform would not say.
///
/// What a second copy of this process has to be started from, and not
/// something a caller can reconstruct from a directory and a name: on macOS
/// the GUI's binary inside its bundle is called after the product, "Sweep++"
/// or "Sweep++ Nightly", while everywhere else it is "sweeppp".
[[nodiscard]] std::filesystem::path executablePath();

/// Directory containing the running executable. Used to locate `resources/`
/// and bundled plugins relative to the binary, so a build tree works without
/// installation.
[[nodiscard]] std::filesystem::path executableDir();

/// A name as a file stem: "Diamond D-190" -> "diamond-d-190". Empty when the
/// name has no letters or digits.
[[nodiscard]] std::string slugify(std::string_view name);

} // namespace sweeppp
