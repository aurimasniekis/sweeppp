// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/ui/MarkerPreset.hpp"

#include "sweeppp/core/Log.hpp"
#include "sweeppp/core/Toml.hpp"

#include <algorithm>
#include <format>

namespace sweeppp::ui {

std::string MarkerPreset::describe() const {
    if (markers.empty()) {
        return "empty";
    }
    if (markers.size() == 1) {
        return toml_util::formatFrequencyShort(markers.front().frequencyHz);
    }

    double lowest = markers.front().frequencyHz;
    double highest = lowest;
    for (const MarkerPresetEntry& entry : markers) {
        lowest = std::min(lowest, entry.frequencyHz);
        highest = std::max(highest, entry.frequencyHz);
    }

    if (lowest == highest) {
        return std::format("{} markers, {}", markers.size(),
                           toml_util::formatFrequencyShort(lowest));
    }
    return std::format("{} markers, {} - {}", markers.size(),
                       toml_util::formatFrequencyShort(lowest),
                       toml_util::formatFrequencyShort(highest));
}

MarkerPresetStore MarkerPresetStore::load(const std::filesystem::path& path) {
    MarkerPresetStore store;

    auto table = toml_util::load(path);
    if (!table) {
        // Nothing saved yet is the normal first-run state; a file that will
        // not parse is worth a line in the log.
        if (table.error().code() != ErrorCode::NotFound) {
            logWarn("markers", "{}", table.error().describe());
        }
        return store;
    }

    const auto* array = toml_util::at(*table, "preset").as_array();
    if (array == nullptr) {
        return store;
    }

    for (const auto& node : *array) {
        const auto* entry = node.as_table();
        if (entry == nullptr) {
            continue;
        }

        MarkerPreset preset;
        preset.name = toml_util::getString(*entry, "name", "");
        if (preset.name.empty()) {
            continue;
        }
        preset.favourite = toml_util::getBool(*entry, "favourite", false);

        if (const auto* saved = toml_util::at(*entry, "markers").as_array()) {
            for (const auto& markerNode : *saved) {
                const auto* marker = markerNode.as_table();
                if (marker == nullptr) {
                    continue;
                }

                auto frequency =
                    toml_util::frequencyFrom((*marker)["frequency"], "preset.markers[].frequency");
                if (!frequency) {
                    logWarn("markers", "{}", frequency.error().describe());
                    continue;
                }

                preset.markers.push_back(MarkerPresetEntry{
                    .frequencyHz = *frequency,
                    .peakLocked = toml_util::getBool(*marker, "peak_locked", false),
                    .visible = toml_util::getBool(*marker, "visible", true)});
            }
        }

        if (preset.markers.empty()) {
            logWarn("markers", "preset '{}' places no markers; ignored", preset.name);
            continue;
        }

        store.add(std::move(preset));
    }

    store.sort();
    return store;
}

Status MarkerPresetStore::save(const std::filesystem::path& path) const {
    ::toml::table root;
    ::toml::array presets;

    for (const MarkerPreset& preset : m_presets) {
        ::toml::table entry;
        entry.insert_or_assign("name", preset.name);
        entry.insert_or_assign("favourite", preset.favourite);

        ::toml::array markers;
        for (const MarkerPresetEntry& marker : preset.markers) {
            ::toml::table item;
            item.insert_or_assign("frequency", marker.frequencyHz);
            item.insert_or_assign("peak_locked", marker.peakLocked);
            item.insert_or_assign("visible", marker.visible);
            markers.push_back(std::move(item));
        }
        entry.insert_or_assign("markers", std::move(markers));
        presets.push_back(std::move(entry));
    }

    root.insert_or_assign("preset", std::move(presets));
    return toml_util::save(path, root, "Sweep++ marker presets");
}

void MarkerPresetStore::add(MarkerPreset preset) {
    const auto existing = std::ranges::find(m_presets, preset.name, &MarkerPreset::name);
    if (existing != m_presets.end()) {
        // Keep the favourite mark: re-saving a set is not a reason to drop it
        // out of the operator's shortlist.
        preset.favourite = existing->favourite;
        *existing = std::move(preset);
    } else {
        m_presets.push_back(std::move(preset));
    }
    sort();
}

void MarkerPresetStore::remove(std::string_view name) {
    std::erase_if(m_presets,
                  [name](const MarkerPreset& candidate) { return candidate.name == name; });
}

void MarkerPresetStore::setFavourite(std::string_view name, bool favourite) {
    const auto match = std::ranges::find_if(
        m_presets, [name](const MarkerPreset& candidate) { return candidate.name == name; });
    if (match != m_presets.end()) {
        match->favourite = favourite;
        sort();
    }
}

void MarkerPresetStore::sort() {
    std::ranges::stable_sort(m_presets, [](const MarkerPreset& a, const MarkerPreset& b) {
        if (a.favourite != b.favourite) {
            return a.favourite;
        }
        return a.name < b.name;
    });
}

MarkerPreset presetFromMarkers(const MarkerSet& set, std::string name) {
    MarkerPreset preset;
    preset.name = std::move(name);
    preset.markers.reserve(set.items.size());

    for (const Marker& marker : set.items) {
        preset.markers.push_back(MarkerPresetEntry{.frequencyHz = marker.frequencyHz,
                                                   .peakLocked = marker.peakLocked,
                                                   .visible = marker.visible});
    }
    return preset;
}

void applyPreset(const MarkerPreset& preset, MarkerSet& set) {
    // Cleared first, so the names start at M1 rather than carrying on from
    // whatever the set had grown to: a preset restored twice in a session
    // should read the same both times.
    set.clear();
    addPresetToMarkers(preset, set);
}

void addPresetToMarkers(const MarkerPreset& preset, MarkerSet& set) {
    for (const MarkerPresetEntry& entry : preset.markers) {
        const bool alreadyPlaced = std::ranges::any_of(set.items, [&entry](const Marker& marker) {
            return marker.frequencyHz == entry.frequencyHz;
        });
        if (alreadyPlaced) {
            continue;
        }

        Marker& placed = set.add(entry.frequencyHz);
        placed.peakLocked = entry.peakLocked;
        placed.visible = entry.visible;
    }
}

} // namespace sweeppp::ui
