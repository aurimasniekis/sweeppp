// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/sweep/SweepPreset.hpp"

#include "sweeppp/core/Log.hpp"
#include "sweeppp/core/Toml.hpp"

#include <algorithm>
#include <format>
#include <limits>

namespace sweeppp {

double SweepPreset::lowestHz() const noexcept {
    double lowest = std::numeric_limits<double>::max();
    for (const SweepSegment& segment : segments) {
        lowest = std::min(lowest, segment.startHz);
    }
    return segments.empty() ? 0.0 : lowest;
}

double SweepPreset::highestHz() const noexcept {
    double highest = std::numeric_limits<double>::lowest();
    for (const SweepSegment& segment : segments) {
        highest = std::max(highest, segment.stopHz);
    }
    return segments.empty() ? 0.0 : highest;
}

std::string SweepPreset::describeRange() const {
    if (segments.empty()) {
        return "empty";
    }
    if (segments.size() == 1) {
        return std::format("{} - {}", toml_util::formatFrequencyShort(segments.front().startHz),
                           toml_util::formatFrequencyShort(segments.front().stopHz));
    }
    // The extent alone would claim coverage the preset does not have, so the
    // count comes first: a discontinuous preset must not read like a wide one.
    return std::format("{} ranges, {} - {}", segments.size(),
                       toml_util::formatFrequencyShort(lowestHz()),
                       toml_util::formatFrequencyShort(highestHz()));
}

std::vector<SweepPreset> SweepPresetStore::builtins() {
    const auto one = [](const char* name, double startHz, double stopHz) {
        return SweepPreset{.name = name,
                           .segments = {SweepSegment{.startHz = startHz, .stopHz = stopHz}},
                           .favourite = false,
                           .builtin = true};
    };

    // Bands that are populated almost everywhere, so a new install has
    // somewhere to point the radio that will actually show something. Regional
    // allocations differ; these are the widely-shared ones.
    return {
        one("FM broadcast", 88e6, 108e6),   one("Airband", 118e6, 137e6),
        one("2 m amateur", 144e6, 148e6),   one("70 cm amateur", 430e6, 440e6),
        one("ISM 433", 433.05e6, 434.79e6), one("LTE downlink", 1.805e9, 1.88e9),
        one("GPS L1", 1.5e9, 1.6e9),        one("ISM 868", 863e6, 870e6),
        one("ISM 915", 902e6, 928e6),       one("Wi-Fi 2.4G", 2.4e9, 2.5e9),
        one("Wi-Fi 5G", 5.15e9, 5.85e9),    one("Full range", 1e6, 6e9),
    };
}

SweepPresetStore SweepPresetStore::load(const std::filesystem::path& path) {
    SweepPresetStore store;
    store.m_presets = builtins();

    auto table = toml_util::load(path);
    if (!table) {
        // Absent is the normal first-run state, and the built-ins alone are a
        // usable list. A malformed file is worth saying something about.
        if (table.error().code() != ErrorCode::NotFound) {
            logWarn("presets", "{}", table.error().describe());
        }
        store.sort();
        return store;
    }

    if (const auto* hidden = toml_util::at(*table, "presets.hidden_builtins").as_array()) {
        for (const auto& entry : *hidden) {
            if (auto name = entry.value<std::string>()) {
                store.m_hiddenBuiltins.push_back(*name);
            }
        }
    }

    std::erase_if(store.m_presets, [&store](const SweepPreset& preset) {
        return preset.builtin && std::ranges::find(store.m_hiddenBuiltins, preset.name) !=
                                     store.m_hiddenBuiltins.end();
    });

    // Favourites are recorded separately from the preset itself so marking a
    // built-in does not require copying it into the user's file.
    std::vector<std::string> favourites;
    if (const auto* marked = toml_util::at(*table, "presets.favourites").as_array()) {
        for (const auto& entry : *marked) {
            if (auto name = entry.value<std::string>()) {
                favourites.push_back(*name);
            }
        }
    }

    if (const auto* array = toml_util::at(*table, "preset").as_array()) {
        for (const auto& node : *array) {
            const auto* entry = node.as_table();
            if (entry == nullptr) {
                continue;
            }

            SweepPreset preset;
            preset.name = toml_util::getString(*entry, "name", "");
            if (preset.name.empty()) {
                continue;
            }

            if (const auto* segments = toml_util::at(*entry, "segments").as_array()) {
                for (const auto& segmentNode : *segments) {
                    const auto* segment = segmentNode.as_table();
                    if (segment == nullptr) {
                        continue;
                    }
                    const double startHz = toml_util::getDouble(*segment, "start", 0.0);
                    const double stopHz = toml_util::getDouble(*segment, "stop", 0.0);
                    if (stopHz > startHz) {
                        preset.segments.push_back(
                            SweepSegment{.startHz = startHz, .stopHz = stopHz});
                    }
                }
            }

            if (preset.segments.empty()) {
                logWarn("presets", "preset '{}' covers no range; ignored", preset.name);
                continue;
            }

            store.add(std::move(preset));
        }
    }

    for (SweepPreset& preset : store.m_presets) {
        preset.favourite = std::ranges::find(favourites, preset.name) != favourites.end();
    }

    store.sort();
    return store;
}

Status SweepPresetStore::save(const std::filesystem::path& path) const {
    ::toml::table root;

    ::toml::array userPresets;
    ::toml::array favourites;
    ::toml::array hidden;

    for (const SweepPreset& preset : m_presets) {
        if (preset.favourite) {
            favourites.push_back(preset.name);
        }
        if (preset.builtin) {
            continue;
        }

        ::toml::table entry;
        entry.insert_or_assign("name", preset.name);

        ::toml::array segments;
        for (const SweepSegment& segment : preset.segments) {
            ::toml::table range;
            range.insert_or_assign("start", segment.startHz);
            range.insert_or_assign("stop", segment.stopHz);
            segments.push_back(std::move(range));
        }
        entry.insert_or_assign("segments", std::move(segments));
        userPresets.push_back(std::move(entry));
    }

    for (const std::string& name : m_hiddenBuiltins) {
        hidden.push_back(name);
    }

    ::toml::table& presets = toml_util::ensureTable(root, "presets");
    presets.insert_or_assign("favourites", std::move(favourites));
    presets.insert_or_assign("hidden_builtins", std::move(hidden));
    root.insert_or_assign("preset", std::move(userPresets));

    return toml_util::save(path, root, "Sweep++ sweep range presets");
}

void SweepPresetStore::add(SweepPreset preset) {
    const auto existing = std::ranges::find_if(m_presets, [&preset](const SweepPreset& candidate) {
        return !candidate.builtin && candidate.name == preset.name;
    });

    if (existing != m_presets.end()) {
        // Keep the favourite mark: updating a preset's range is not a reason
        // to drop it out of the operator's shortlist.
        preset.favourite = existing->favourite;
        *existing = std::move(preset);
    } else {
        m_presets.push_back(std::move(preset));
    }
    sort();
}

void SweepPresetStore::remove(std::string_view name) {
    const auto match = std::ranges::find_if(
        m_presets, [name](const SweepPreset& candidate) { return candidate.name == name; });
    if (match == m_presets.end()) {
        return;
    }

    if (match->builtin) {
        m_hiddenBuiltins.emplace_back(name);
    }
    m_presets.erase(match);
}

void SweepPresetStore::setFavourite(std::string_view name, bool favourite) {
    const auto match = std::ranges::find_if(
        m_presets, [name](const SweepPreset& candidate) { return candidate.name == name; });
    if (match != m_presets.end()) {
        match->favourite = favourite;
        sort();
    }
}

void SweepPresetStore::sort() {
    std::ranges::stable_sort(m_presets, [](const SweepPreset& a, const SweepPreset& b) {
        if (a.favourite != b.favourite) {
            return a.favourite;
        }
        return a.name < b.name;
    });
}

} // namespace sweeppp
