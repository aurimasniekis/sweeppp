// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/sweep/SweepPlan.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace sweeppp {

/// A named frequency range the operator returns to.
///
/// Deliberately a range and not a whole plan. What an operator repeats is
/// "look at the 2.4 GHz band", and they expect their current resolution and
/// mode to carry over -- a preset that silently reset the RBW would be a
/// worse tool than typing the numbers again.
struct SweepPreset {
    std::string name;
    std::vector<SweepSegment> segments;

    /// Sorted to the front, so the handful in daily use stay reachable
    /// without scrolling past everything ever saved.
    bool favourite = false;

    /// True for presets that ship with the application. They can be hidden but
    /// not edited, so a built-in never silently becomes something else.
    bool builtin = false;

    [[nodiscard]] double lowestHz() const noexcept;
    [[nodiscard]] double highestHz() const noexcept;

    /// "88 - 108 MHz", or "3 ranges, 88 MHz - 6 GHz" when discontinuous.
    [[nodiscard]] std::string describeRange() const;
};

/// The operator's preset list, persisted as TOML.
class SweepPresetStore {
public:
    /// Loads the user's presets, merged over the built-ins.
    ///
    /// A missing file is not an error: it is what the first run looks like,
    /// and the built-ins alone are a usable list.
    static SweepPresetStore load(const std::filesystem::path& path);

    [[nodiscard]] Status save(const std::filesystem::path& path) const;

    [[nodiscard]] const std::vector<SweepPreset>& presets() const noexcept { return m_presets; }

    /// Adds a preset, replacing any user preset of the same name.
    ///
    /// Replacing rather than appending because "Add" on a name that already
    /// exists reads as "update this one"; ending up with two entries the
    /// operator cannot tell apart does not.
    void add(SweepPreset preset);

    /// Removes a user preset by name. Built-ins are hidden instead, so they
    /// can be restored without reinstalling.
    void remove(std::string_view name);

    void setFavourite(std::string_view name, bool favourite);

    /// Favourites first, then alphabetical within each group.
    void sort();

    /// The built-in list, used when no file exists yet.
    [[nodiscard]] static std::vector<SweepPreset> builtins();

private:
    std::vector<SweepPreset> m_presets;
    std::vector<std::string> m_hiddenBuiltins;
};

} // namespace sweeppp
