// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/profile/Profile.hpp"

#include "sweeppp/core/Log.hpp"
#include "sweeppp/core/Toml.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

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

std::string_view panelModeName(ui::PanelMode mode) {
    return mode == ui::PanelMode::Spans ? "spans" : "mirror";
}

std::string_view arrangementName(ui::PanelArrangement arrangement) {
    switch (arrangement) {
    case ui::PanelArrangement::Single:
        return "single";
    case ui::PanelArrangement::Columns:
        return "columns";
    case ui::PanelArrangement::Rows:
        return "rows";
    case ui::PanelArrangement::Three:
        return "three";
    case ui::PanelArrangement::Grid:
        return "grid";
    case ui::PanelArrangement::Six:
        return "six";
    case ui::PanelArrangement::Nine:
        return "nine";
    }
    return "single";
}

::toml::array pairOf(const std::array<float, 2>& values) {
    return ::toml::array{static_cast<double>(values[0]), static_cast<double>(values[1])};
}

/// A two-number array, or `fallback` when the key is missing or malformed.
std::array<float, 2> readPair(const ::toml::table& table, std::string_view key,
                              const std::array<float, 2>& fallback) {
    const ::toml::array* values = toml_util::at(table, key).as_array();
    if (values == nullptr || values->size() != 2) {
        return fallback;
    }
    const std::optional<double> first = (*values)[0].value<double>();
    const std::optional<double> second = (*values)[1].value<double>();
    if (!first || !second) {
        return fallback;
    }
    return {static_cast<float>(*first), static_cast<float>(*second)};
}

void writeLayout(::toml::table& display, const ui::PanelLayout& layout) {
    display.insert_or_assign("panel_mode", std::string(panelModeName(layout.mode)));
    display.insert_or_assign("panel_layout", std::string(arrangementName(ui::arrangementFor(
                                                 layout.attachedCount(), layout.rowsForTwo))));
    display.insert_or_assign("panel_split_x", static_cast<double>(layout.splits.x));
    display.insert_or_assign("panel_split_y", static_cast<double>(layout.splits.y));
    display.insert_or_assign("panel_thirds_x", pairOf(layout.splits.thirdsX));
    display.insert_or_assign("panel_thirds_y", pairOf(layout.splits.thirdsY));
    display.insert_or_assign("overview", layout.overview);

    // Mirror panels side by side at different zooms are the point of that
    // layout, so their windows are kept. A single panel's is not, for the same
    // reason it never was: it is where the operator happened to be looking.
    const bool keepViews = layout.mode == ui::PanelMode::Mirror && layout.panels.size() > 1;
    const bool keepSegments = layout.mode == ui::PanelMode::Spans;

    ::toml::array panels;
    for (const ui::PanelView& panel : layout.panels) {
        ::toml::table entry;
        entry.insert_or_assign("id", static_cast<std::int64_t>(panel.id));
        entry.insert_or_assign("y_min", static_cast<double>(panel.yMinDb));
        entry.insert_or_assign("y_max", static_cast<double>(panel.yMaxDb));
        entry.insert_or_assign("gradient_min", static_cast<double>(panel.gradientMinDb));
        entry.insert_or_assign("gradient_max", static_cast<double>(panel.gradientMaxDb));
        entry.insert_or_assign("waterfall_fraction", static_cast<double>(panel.waterfallFraction));
        entry.insert_or_assign("detached", panel.detached);
        if (keepViews && panel.viewStopHz > panel.viewStartHz) {
            entry.insert_or_assign("view_start", panel.viewStartHz);
            entry.insert_or_assign("view_stop", panel.viewStopHz);
        }
        if (keepSegments && panel.segment.valid()) {
            entry.insert_or_assign("segment_start", panel.segment.startHz);
            entry.insert_or_assign("segment_stop", panel.segment.stopHz);
        }
        panels.push_back(std::move(entry));
    }
    display.insert_or_assign("panels", std::move(panels));
}

/// Levels are clamped on the way in. A file edited by hand, or written by a
/// build with different bounds, must not be able to put a panel somewhere the
/// controls cannot bring it back from.
void clampLevels(ui::PanelView& panel) {
    panel.yMaxDb =
        std::clamp(panel.yMaxDb, ui::kScaleFloorDbfs + ui::kMinScaleSpanDb, ui::kScaleCeilingDbfs);
    panel.yMinDb =
        std::clamp(panel.yMinDb, ui::kScaleFloorDbfs, panel.yMaxDb - ui::kMinScaleSpanDb);
    panel.gradientMaxDb =
        std::clamp(panel.gradientMaxDb, ui::kScaleFloorDbfs + 1.0F, ui::kScaleCeilingDbfs);
    panel.gradientMinDb =
        std::clamp(panel.gradientMinDb, ui::kScaleFloorDbfs, panel.gradientMaxDb - 1.0F);
    panel.waterfallFraction = std::clamp(panel.waterfallFraction, 0.05F, 0.95F);
}

ui::PanelLayout readLayout(const ::toml::table& table) {
    ui::PanelLayout layout;
    const ui::PanelView defaults;

    layout.mode = toml_util::getString(table, "display.panel_mode", "mirror") == "spans"
                      ? ui::PanelMode::Spans
                      : ui::PanelMode::Mirror;
    layout.rowsForTwo = toml_util::getString(table, "display.panel_layout", "") == "rows";
    const ui::PanelSplits splitDefaults;
    ui::PanelSplits splits;
    splits.x = toml_util::getFloat(table, "display.panel_split_x", splitDefaults.x);
    splits.y = toml_util::getFloat(table, "display.panel_split_y", splitDefaults.y);
    splits.thirdsX = readPair(table, "display.panel_thirds_x", splitDefaults.thirdsX);
    splits.thirdsY = readPair(table, "display.panel_thirds_y", splitDefaults.thirdsY);
    layout.splits = ui::clampSplits(splits);
    layout.overview = toml_util::getBool(table, "display.overview", true);

    std::vector<ui::PanelView> panels;
    if (const ::toml::array* entries = toml_util::at(table, "display.panels").as_array()) {
        for (const ::toml::node& node : *entries) {
            const ::toml::table* entry = node.as_table();
            if (entry == nullptr) {
                continue;
            }
            ui::PanelView panel;
            panel.id = static_cast<int>((*entry)["id"].value_or(std::int64_t{0}));
            panel.yMinDb = static_cast<float>(
                (*entry)["y_min"].value_or(static_cast<double>(defaults.yMinDb)));
            panel.yMaxDb = static_cast<float>(
                (*entry)["y_max"].value_or(static_cast<double>(defaults.yMaxDb)));
            panel.gradientMinDb = static_cast<float>(
                (*entry)["gradient_min"].value_or(static_cast<double>(defaults.gradientMinDb)));
            panel.gradientMaxDb = static_cast<float>(
                (*entry)["gradient_max"].value_or(static_cast<double>(defaults.gradientMaxDb)));
            panel.waterfallFraction = static_cast<float>((*entry)["waterfall_fraction"].value_or(
                static_cast<double>(defaults.waterfallFraction)));
            panel.detached = (*entry)["detached"].value_or(false);
            panel.viewStartHz = (*entry)["view_start"].value_or(0.0);
            panel.viewStopHz = (*entry)["view_stop"].value_or(0.0);
            panel.segment.startHz = (*entry)["segment_start"].value_or(0.0);
            panel.segment.stopHz = (*entry)["segment_stop"].value_or(0.0);
            panels.push_back(panel);
        }
    }

    // Repaired the way markers are: an id that is zero or already used would
    // be a panel the window cannot tell apart from its twin.
    std::vector<int> seen;
    std::erase_if(panels, [&seen](const ui::PanelView& panel) {
        if (panel.id <= 0 || std::ranges::find(seen, panel.id) != seen.end()) {
            return true;
        }
        seen.push_back(panel.id);
        return false;
    });
    if (panels.size() > ui::kMaxPanels) {
        panels.resize(ui::kMaxPanels);
    }

    const bool keepViews = layout.mode == ui::PanelMode::Mirror && panels.size() > 1;
    for (ui::PanelView& panel : panels) {
        clampLevels(panel);
        const bool usable = std::isfinite(panel.viewStartHz) && std::isfinite(panel.viewStopHz) &&
                            panel.viewStartHz >= 0.0 && panel.viewStopHz > panel.viewStartHz;
        if (!keepViews || !usable) {
            panel.viewStartHz = 0.0;
            panel.viewStopHz = 0.0;
        }
        if (layout.mode != ui::PanelMode::Spans || !std::isfinite(panel.segment.startHz) ||
            !std::isfinite(panel.segment.stopHz) || !panel.segment.valid()) {
            panel.segment = {};
        }
    }

    if (panels.empty()) {
        return layout;
    }
    // Every panel torn off would leave the main window with nothing in it.
    if (std::ranges::none_of(panels, [](const ui::PanelView& panel) { return !panel.detached; })) {
        panels.front().detached = false;
    }

    layout.panels = std::move(panels);
    layout.focusedId = layout.panels.front().id;
    layout.resetNextId();
    return layout;
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
    analysis.insert_or_assign("dc_removal", self.corrections.dcRemoval);
    analysis.insert_or_assign("flatten", self.corrections.flatten);
    analysis.insert_or_assign("spur_mask", self.corrections.spurMask);
    analysis.insert_or_assign("auto_spurs", self.corrections.autoSpurs);

    ::toml::table& display = toml_util::ensureTable(root, "display");
    display.insert_or_assign("theme", self.view.themeName);
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

    writeLayout(display, self.view.layout);

    // The measured level is deliberately absent: it is re-read from the live
    // trace every frame, so a saved one would be a reading from a session that
    // has ended.

    // Deliberately not saved: a lone panel's visible window, whether a
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

    const CorrectionSettings correctionDefaults;
    profile.corrections.dcRemoval =
        toml_util::getBool(*table, "analysis.dc_removal", correctionDefaults.dcRemoval);
    profile.corrections.flatten =
        toml_util::getBool(*table, "analysis.flatten", correctionDefaults.flatten);
    profile.corrections.spurMask =
        toml_util::getBool(*table, "analysis.spur_mask", correctionDefaults.spurMask);
    profile.corrections.autoSpurs =
        toml_util::getBool(*table, "analysis.auto_spurs", correctionDefaults.autoSpurs);

    const ui::ViewSettings viewDefaults;
    profile.view.themeName = toml_util::getString(*table, "display.theme", viewDefaults.themeName);
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

    profile.view.layout = readLayout(*table);
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
