// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/ui/Marker.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sweeppp::ui {

/// One marker in a preset: where it goes and how it behaves, but not its name.
///
/// A name is handed out by the set the marker is placed into -- M1 in one
/// session is M4 in the next -- so a preset carrying ids would either collide
/// with what is already on the plot or quietly rename it.
struct MarkerPresetEntry {
    double frequencyHz = 0.0;
    bool peakLocked = false; ///< See Marker::peakLocked.
    bool visible = true;     ///< See Marker::visible.
};

/// A named set of markers the operator returns to.
///
/// The other half of "look here again" from SweepPreset: a range says what to
/// sweep, a marker set says which frequencies in it are being watched. Kept
/// apart from the sweep presets because the two are reused independently --
/// the same three channel centres are worth marking whether the radio is
/// sweeping the whole band or parked on one of them.
struct MarkerPreset {
    std::string name;
    std::vector<MarkerPresetEntry> markers;

    /// Sorted to the front, so the handful in daily use stay reachable
    /// without scrolling past everything ever saved.
    bool favourite = false;

    /// "100.5 MHz", or "4 markers, 88 MHz - 2.4 GHz".
    [[nodiscard]] std::string describe() const;
};

/// The operator's saved marker sets, persisted as TOML.
///
/// No built-ins, unlike SweepPresetStore: which bands are worth looking at is
/// much the same everywhere, but which frequencies inside one are worth
/// marking is entirely the operator's own.
class MarkerPresetStore {
public:
    /// Loads the saved sets. A missing file is what a first run looks like,
    /// not an error.
    static MarkerPresetStore load(const std::filesystem::path& path);

    [[nodiscard]] Status save(const std::filesystem::path& path) const;

    [[nodiscard]] const std::vector<MarkerPreset>& presets() const noexcept { return m_presets; }

    /// Adds a preset, replacing any of the same name.
    ///
    /// Replacing rather than appending because saving under a name already
    /// used reads as "update this one"; ending up with two entries the
    /// operator cannot tell apart does not.
    void add(MarkerPreset preset);

    void remove(std::string_view name);
    void setFavourite(std::string_view name, bool favourite);

    /// Favourites first, then alphabetical within each group.
    void sort();

private:
    std::vector<MarkerPreset> m_presets;
};

/// The set as it stands, ready to be saved under `name`.
[[nodiscard]] MarkerPreset presetFromMarkers(const MarkerSet& set, std::string name);

/// Puts a preset's markers on the plot in place of whatever is there.
void applyPreset(const MarkerPreset& preset, MarkerSet& set);

/// Puts a preset's markers on the plot alongside the ones already placed.
///
/// A frequency already marked is skipped rather than doubled: two markers on
/// one frequency cannot be told apart on the plot or picked apart by a click,
/// and applying the same preset twice should leave the set as it was. Exact
/// equality is the right test for that -- the frequencies being compared came
/// from the same preset, so they are the same double, and two markers a
/// hertz apart are two markers the operator meant to place.
void addPresetToMarkers(const MarkerPreset& preset, MarkerSet& set);

} // namespace sweeppp::ui
