// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/profile/Profile.hpp"

#include "sweeppp/core/Log.hpp"
#include "sweeppp/core/Toml.hpp"

#include <algorithm>
#include <cmath>

namespace sweeppp {
namespace {

/// Writes an SdrValue keeping its type.
///
/// The variant maps onto TOML's own scalars with no encoding, which is most of
/// why TOML was chosen: a gain of 20 round-trips as an integer and a frequency
/// as a float, so a hand-edited file reads like what it configures rather than
/// like a list of quoted strings.
void writeValue(::toml::table& table, const std::string& key, const SdrValue& value) {
    std::visit([&table, &key](const auto& held) { table.insert_or_assign(key, held); }, value);
}

/// Reads a TOML scalar back into an SdrValue, preserving whichever type the
/// file used. The device coerces it to the parameter's declared type on the
/// way in, so a file written by an older build still applies.
std::optional<SdrValue> readValue(const ::toml::node& node) {
    // Dispatched on what the node *is*, not on what it can be converted to.
    //
    // `value<T>()` converts between the arithmetic types, so asking for a bool
    // first succeeds on every number in the file and a gain of 24 comes back
    // as `true`. Each alternative has to survive as itself: the device coerces
    // to its declared type on the way in, and it can only do that correctly if
    // it is handed the type that was written.
    if (const auto* boolean = node.as_boolean()) {
        return SdrValue{boolean->get()};
    }
    if (const auto* integer = node.as_integer()) {
        return SdrValue{integer->get()};
    }
    if (const auto* real = node.as_floating_point()) {
        return SdrValue{real->get()};
    }
    if (const auto* text = node.as_string()) {
        return SdrValue{text->get()};
    }
    return std::nullopt;
}

/// The profile as one table.
///
/// Shared by `save()` and `entries()` so the two cannot disagree about a key's
/// name: what a plugin reads through the host is byte for byte what an
/// operator sees in their own settings.toml.
::toml::table buildTable(const Profile& self) {
    ::toml::table root;

    ::toml::table& profile = toml_util::ensureTable(root, "profile");
    profile.insert_or_assign("name", self.name);
    profile.insert_or_assign("sweeping", self.sweeping);
    profile.insert_or_assign("waterfall_fraction", static_cast<double>(self.waterfallFraction));

    ::toml::table& device = toml_util::ensureTable(root, "device");
    device.insert_or_assign("driver", self.deviceDriver);
    device.insert_or_assign("id", self.deviceId);
    device.insert_or_assign("label", self.deviceLabel);

    ::toml::table& parameters = toml_util::ensureTable(root, "device.parameters");
    for (const auto& [key, value] : self.deviceParameters) {
        writeValue(parameters, key, value);
    }

    // The plan writes itself into the same table, so its schema lives in one
    // place and a profile cannot drift from a standalone plan file.
    self.sweepPlan.writeInto(root);

    ::toml::table& analysis = toml_util::ensureTable(root, "analysis");
    analysis.insert_or_assign("fft_size", static_cast<std::int64_t>(self.pipeline.fftSize));
    analysis.insert_or_assign("window", std::string(toString(self.pipeline.window)));
    analysis.insert_or_assign("window_beta", self.pipeline.windowBeta);
    analysis.insert_or_assign("overlap", self.pipeline.overlap);
    analysis.insert_or_assign("workers", static_cast<std::int64_t>(self.pipeline.workerCount));
    analysis.insert_or_assign("throttle", static_cast<std::int64_t>(self.pipeline.throttleMode));
    analysis.insert_or_assign("every_nth", static_cast<std::int64_t>(self.pipeline.everyNth));
    analysis.insert_or_assign("average", static_cast<std::int64_t>(self.pipeline.averageCount));
    analysis.insert_or_assign("frame_rate", self.pipeline.targetFrameRate);

    ::toml::table& display = toml_util::ensureTable(root, "display");
    display.insert_or_assign("theme", self.view.themeName);
    display.insert_or_assign("y_min", static_cast<double>(self.view.yMinDb));
    display.insert_or_assign("y_max", static_cast<double>(self.view.yMaxDb));
    display.insert_or_assign("gradient_min", static_cast<double>(self.view.gradientMinDb));
    display.insert_or_assign("gradient_max", static_cast<double>(self.view.gradientMaxDb));
    display.insert_or_assign("auto_points", self.view.autoPoints);
    display.insert_or_assign("display_points", static_cast<std::int64_t>(self.view.displayPoints));
    display.insert_or_assign("smoothing", static_cast<double>(self.view.smoothing));
    display.insert_or_assign("max_hold", self.view.showMaxHold);
    display.insert_or_assign("max_hold_decay", static_cast<double>(self.view.maxHoldDecayDbPerSec));
    display.insert_or_assign("min_hold", self.view.showMinHold);
    display.insert_or_assign("average", self.view.showAverage);
    display.insert_or_assign("average_window", static_cast<std::int64_t>(self.view.averageWindow));
    display.insert_or_assign("heatmap_fill", self.view.showHeatmapFill);
    display.insert_or_assign("fill_style", static_cast<std::int64_t>(self.view.fillStyle));
    display.insert_or_assign("grid", self.view.showGrid);
    display.insert_or_assign("contribution_bands", self.view.showBandContributions);
    display.insert_or_assign("antenna_ranges", self.view.showAntennaRanges);
    display.insert_or_assign("contribution_channels", self.view.showChannelContributions);
    display.insert_or_assign("waterfall_lines",
                             static_cast<std::int64_t>(self.view.waterfallLines));
    display.insert_or_assign("waterfall_peak", self.view.waterfallPeakDetect);
    display.insert_or_assign("waterfall_time_axis",
                             static_cast<std::int64_t>(self.view.waterfallTimeAxis));
    display.insert_or_assign("waterfall_time_lines", self.view.waterfallTimeLines);
    display.insert_or_assign("marker_readout", static_cast<std::int64_t>(self.view.markerReadout));

    // An array of tables, following the sweep plan's segments: an operator
    // reading their settings.toml sees one block per marker with the id it is
    // called on screen, rather than a set of parallel lists to line up by eye.
    {
        ::toml::array markers;
        for (const ui::Marker& marker : self.view.markers.items) {
            ::toml::table entry;
            entry.insert_or_assign("id", static_cast<std::int64_t>(marker.id));
            entry.insert_or_assign("frequency", marker.frequencyHz);
            entry.insert_or_assign("visible", marker.visible);
            entry.insert_or_assign("peak", marker.peakLocked);
            markers.push_back(std::move(entry));
        }
        display.insert_or_assign("markers", std::move(markers));
    }

    // The measured level is deliberately absent: it is re-read from the live
    // trace every frame, so a saved one would be a reading from a session that
    // has ended.

    // Deliberately not saved: the visible frequency window, whether the
    // waterfall was paused, and which marker was selected. All three are where
    // the operator happened to be looking when they quit, not how they want the
    // instrument set up -- and restoring a paused waterfall would look like the
    // application had failed to start.
    //
    // The selection is the one that would do damage quietly: it is what the
    // right button moves and what backspace deletes, so a marker restored as
    // selected turns the first gesture of a new session into an edit of an old
    // one. The markers themselves come back; which of them is under the hand
    // is a decision for this session.

    // Whatever the plugins put in, under the keys they used. Written last and
    // read back whole, so a profile survives a round trip through a build that
    // has none of the plugins that wrote it.
    for (const auto& [key, value] : self.pluginValues) {
        const std::size_t dot = key.rfind('.');
        if (dot == std::string::npos) {
            continue;
        }
        ::toml::table& parent = toml_util::ensureTable(root, key.substr(0, dot));
        writeValue(parent, key.substr(dot + 1), value);
    }

    return root;
}

/// Walks a table into dotted keys, depth first, in the order toml++ holds
/// them -- which is sorted, so a listing is stable between calls.
void flatten(const ::toml::table& table, const std::string& prefix,
             std::vector<std::pair<std::string, SdrValue>>& out) {
    for (const auto& [key, node] : table) {
        const std::string path =
            prefix.empty() ? std::string(key.str()) : std::format("{}.{}", prefix, key.str());

        if (const auto* nested = node.as_table()) {
            flatten(*nested, path, out);
            continue;
        }
        if (auto value = readValue(node)) {
            out.emplace_back(path, std::move(*value));
        }
        // Arrays are skipped rather than encoded: the sweep plan's segments and
        // the markers are the only ones, and a plugin that wants either wants
        // them as ranges and cursors, not as "sweep.segments.0.start".
    }
}

} // namespace

Status Profile::save(const std::filesystem::path& path) const {
    return toml_util::save(path, buildTable(*this), "Sweep++ profile");
}

std::vector<std::pair<std::string, SdrValue>> Profile::entries() const {
    std::vector<std::pair<std::string, SdrValue>> flat;
    flatten(buildTable(*this), {}, flat);
    return flat;
}

Result<Profile> Profile::load(const std::filesystem::path& path) {
    auto table = toml_util::load(path);
    if (!table) {
        return std::unexpected(table.error());
    }

    Profile profile;
    profile.name = toml_util::getString(*table, "profile.name", path.stem().string());
    profile.sweeping = toml_util::getBool(*table, "profile.sweeping", true);
    profile.waterfallFraction = toml_util::getFloat(*table, "profile.waterfall_fraction", 0.45F);

    profile.deviceDriver = toml_util::getString(*table, "device.driver", "");
    profile.deviceId = toml_util::getString(*table, "device.id", "");
    profile.deviceLabel = toml_util::getString(*table, "device.label", "");

    if (const auto* parameters = toml_util::at(*table, "device.parameters").as_table()) {
        for (const auto& [key, node] : *parameters) {
            if (auto value = readValue(node)) {
                profile.deviceParameters.emplace_back(std::string(key.str()), *value);
            }
        }
    }

    // Round-tripped through the plan's own parser rather than re-read field by
    // field here, so there is exactly one definition of what a plan is.
    if (auto plan = SweepPlan::fromTable(*table)) {
        profile.sweepPlan = std::move(*plan);
    } else {
        logWarn("profile", "{}: {}", path.string(), plan.error().describe());
    }

    const PipelineConfig defaults;
    profile.pipeline.fftSize = static_cast<std::uint32_t>(
        toml_util::getInt(*table, "analysis.fft_size", defaults.fftSize));
    if (auto window = windowTypeFromString(toml_util::getString(
            *table, "analysis.window", std::string(toString(defaults.window))))) {
        profile.pipeline.window = *window;
    }
    profile.pipeline.windowBeta =
        toml_util::getDouble(*table, "analysis.window_beta", defaults.windowBeta);
    profile.pipeline.overlap = toml_util::getDouble(*table, "analysis.overlap", defaults.overlap);
    profile.pipeline.workerCount = static_cast<std::uint32_t>(
        toml_util::getInt(*table, "analysis.workers", defaults.workerCount));
    profile.pipeline.throttleMode = static_cast<ThrottleMode>(
        toml_util::getInt(*table, "analysis.throttle", static_cast<int>(defaults.throttleMode)));
    profile.pipeline.everyNth = static_cast<std::uint32_t>(
        toml_util::getInt(*table, "analysis.every_nth", defaults.everyNth));
    profile.pipeline.averageCount = static_cast<std::uint32_t>(
        toml_util::getInt(*table, "analysis.average", defaults.averageCount));
    profile.pipeline.targetFrameRate =
        toml_util::getDouble(*table, "analysis.frame_rate", defaults.targetFrameRate);

    const ui::ViewSettings viewDefaults;
    profile.view.themeName = toml_util::getString(*table, "display.theme", viewDefaults.themeName);
    profile.view.yMinDb = toml_util::getFloat(*table, "display.y_min", viewDefaults.yMinDb);
    profile.view.yMaxDb = toml_util::getFloat(*table, "display.y_max", viewDefaults.yMaxDb);
    profile.view.gradientMinDb =
        toml_util::getFloat(*table, "display.gradient_min", viewDefaults.gradientMinDb);
    profile.view.gradientMaxDb =
        toml_util::getFloat(*table, "display.gradient_max", viewDefaults.gradientMaxDb);
    profile.view.autoPoints =
        toml_util::getBool(*table, "display.auto_points", viewDefaults.autoPoints);
    profile.view.displayPoints = static_cast<int>(
        toml_util::getInt(*table, "display.display_points", viewDefaults.displayPoints));
    profile.view.smoothing =
        toml_util::getFloat(*table, "display.smoothing", viewDefaults.smoothing);
    profile.view.showMaxHold =
        toml_util::getBool(*table, "display.max_hold", viewDefaults.showMaxHold);
    profile.view.maxHoldDecayDbPerSec =
        toml_util::getFloat(*table, "display.max_hold_decay", viewDefaults.maxHoldDecayDbPerSec);
    profile.view.showMinHold =
        toml_util::getBool(*table, "display.min_hold", viewDefaults.showMinHold);
    profile.view.showAverage =
        toml_util::getBool(*table, "display.average", viewDefaults.showAverage);
    profile.view.averageWindow = static_cast<int>(
        toml_util::getInt(*table, "display.average_window", viewDefaults.averageWindow));
    profile.view.showHeatmapFill =
        toml_util::getBool(*table, "display.heatmap_fill", viewDefaults.showHeatmapFill);
    profile.view.fillStyle =
        static_cast<int>(toml_util::getInt(*table, "display.fill_style", viewDefaults.fillStyle));
    profile.view.showGrid = toml_util::getBool(*table, "display.grid", viewDefaults.showGrid);
    profile.view.showBandContributions = toml_util::getBool(*table, "display.contribution_bands",
                                                            viewDefaults.showBandContributions);
    profile.view.showAntennaRanges =
        toml_util::getBool(*table, "display.antenna_ranges", viewDefaults.showAntennaRanges);
    profile.view.showChannelContributions = toml_util::getBool(
        *table, "display.contribution_channels", viewDefaults.showChannelContributions);
    profile.view.waterfallLines = static_cast<int>(
        toml_util::getInt(*table, "display.waterfall_lines", viewDefaults.waterfallLines));
    profile.view.waterfallPeakDetect =
        toml_util::getBool(*table, "display.waterfall_peak", viewDefaults.waterfallPeakDetect);
    profile.view.waterfallTimeAxis = static_cast<int>(
        toml_util::getInt(*table, "display.waterfall_time_axis", viewDefaults.waterfallTimeAxis));
    profile.view.waterfallTimeLines =
        toml_util::getBool(*table, "display.waterfall_time_lines", viewDefaults.waterfallTimeLines);
    profile.view.markerReadout = static_cast<int>(
        toml_util::getInt(*table, "display.marker_readout", viewDefaults.markerReadout));

    if (const ::toml::array* markers = toml_util::at(*table, "display.markers").as_array()) {
        for (const ::toml::node& node : *markers) {
            const ::toml::table* entry = node.as_table();
            if (entry == nullptr) {
                continue;
            }
            profile.view.markers.items.push_back(
                ui::Marker{.id = static_cast<int>((*entry)["id"].value_or(std::int64_t{0})),
                           .frequencyHz = (*entry)["frequency"].value_or(0.0),
                           .visible = (*entry)["visible"].value_or(true),
                           .peakLocked = (*entry)["peak"].value_or(false)});
        }
    }

    // Levels are clamped on the way in. A file edited by hand, or written by a
    // build with different bounds, must not be able to put the display
    // somewhere the controls cannot bring it back from.
    profile.view.yMaxDb = std::clamp(profile.view.yMaxDb, ui::kScaleFloorDbfs + ui::kMinScaleSpanDb,
                                     ui::kScaleCeilingDbfs);
    profile.view.yMinDb = std::clamp(profile.view.yMinDb, ui::kScaleFloorDbfs,
                                     profile.view.yMaxDb - ui::kMinScaleSpanDb);
    profile.view.gradientMaxDb =
        std::clamp(profile.view.gradientMaxDb, ui::kScaleFloorDbfs + 1.0F, ui::kScaleCeilingDbfs);
    profile.view.gradientMinDb = std::clamp(profile.view.gradientMinDb, ui::kScaleFloorDbfs,
                                            profile.view.gradientMaxDb - 1.0F);
    profile.waterfallFraction = std::clamp(profile.waterfallFraction, 0.05F, 0.95F);
    profile.view.markerReadout = std::clamp(profile.view.markerReadout, 0, 8);

    // Markers are repaired rather than rejected, for the same reason the levels
    // are clamped: a hand-edited file must not be able to strand the UI. A
    // marker at zero or with an id shared with another one would be a row the
    // panel cannot tell apart from its twin.
    {
        ui::MarkerSet& markers = profile.view.markers;
        std::vector<int> seen;
        std::erase_if(markers.items, [&seen](const ui::Marker& marker) {
            if (marker.id <= 0 || !std::isfinite(marker.frequencyHz) || marker.frequencyHz <= 0.0) {
                return true;
            }
            if (std::ranges::find(seen, marker.id) != seen.end()) {
                return true;
            }
            seen.push_back(marker.id);
            return false;
        });

        markers.resetNextId();
    }

    // Everything under [plugins], whether or not the plugin that wrote it is
    // installed here. Read back into the same flat keys it was written from,
    // so a profile that travels between machines does not lose the settings of
    // a plugin one of them happens not to have.
    if (const auto* plugins = toml_util::at(*table, "plugins").as_table()) {
        flatten(*plugins, "plugins", profile.pluginValues);
    }

    return profile;
}

} // namespace sweeppp
