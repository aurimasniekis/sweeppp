// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ChannelSet.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <sweeppp/core/Toml.hpp>
#include <utility>

namespace channels {

using sweeppp::ErrorCode;
using sweeppp::fail;
namespace toml_util = sweeppp::toml_util;

namespace {

constexpr Color kFallbackColor{0.5F, 0.5F, 0.5F, 1.0F};

/// Nullopt rather than a default, so "the file said nothing" stays
/// distinguishable from "the file said band" -- the first inherits, the second
/// overrides the width rule below.
std::optional<ChannelKind> parseKind(std::string_view text) noexcept {
    if (text == "band") {
        return ChannelKind::Band;
    }
    if (text == "channel") {
        return ChannelKind::Channel;
    }
    if (text == "spot") {
        return ChannelKind::Spot;
    }
    return std::nullopt;
}

/// The id up to the last '/', empty for a root.
///
/// '/' rather than '.': the id is also the persistence key, and a profile is
/// flattened to dotted keys that would swallow a dot here.
std::string_view parentId(std::string_view id) noexcept {
    const std::size_t slash = id.rfind('/');
    return slash == std::string_view::npos ? std::string_view{} : id.substr(0, slash);
}

/// A frequency from a node that may be a number or a suffixed string, or
/// nullopt when the key is absent. Everything goes through the same parser as
/// every other config file, so "5658 MHz", "5.658G" and 5658000000.0 are one
/// entry written three ways.
std::optional<double> frequencyAt(const ::toml::node_view<const ::toml::node>& node,
                                  std::string_view key) {
    auto parsed = toml_util::frequencyFrom(node, key);
    return parsed ? std::optional{*parsed} : std::nullopt;
}

} // namespace

Result<ChannelSet> ChannelSet::load(const std::filesystem::path& path,
                                    std::vector<std::string>* problems) {
    auto table = toml_util::load(path);
    if (!table) {
        return std::unexpected(table.error());
    }

    const auto report = [problems, &path](std::string text) {
        if (problems != nullptr) {
            problems->push_back(std::format("{}: {}", path.string(), text));
        }
    };

    ChannelSet set;
    set.m_path = path;
    set.m_id = toml_util::getString(*table, "channels.id", path.stem().string());
    set.m_name = toml_util::getString(*table, "channels.name", set.m_id);
    set.m_description = toml_util::getString(*table, "channels.description", "");

    // Named colours first: a group names one and every group under it inherits
    // it, so recolouring a whole family is one edit rather than ninety.
    std::vector<std::pair<std::string, Color>> palette;
    if (const auto* colors = toml_util::at(*table, "channels.colors").as_table()) {
        for (const auto& [key, node] : *colors) {
            const auto text = node.value<std::string>();
            if (!text) {
                continue;
            }
            if (auto color = Color::fromHex(*text)) {
                palette.emplace_back(std::string(key.str()), *color);
            } else {
                report(std::format("{} is not a colour: {}", key.str(), *text));
            }
        }
    }

    const Color fileColor = palette.empty() ? kFallbackColor : palette.front().second;

    const auto colorFor = [&palette](std::string_view text) -> std::optional<Color> {
        const auto named = std::ranges::find_if(
            palette, [text](const auto& candidate) { return candidate.first == text; });
        if (named != palette.end()) {
            return named->second;
        }
        if (auto literal = Color::fromHex(text)) {
            return *literal;
        }
        return std::nullopt;
    };

    const std::optional<double> fileWidth =
        frequencyAt(toml_util::at(*table, "channels.width"), "width");

    const auto* declaredGroups = toml_util::at(*table, "group").as_array();
    if (declaredGroups == nullptr) {
        return fail<ChannelSet>(ErrorCode::ParseError, "{} defines no groups", path.string());
    }

    // Parallel to `set.m_groups`: the default width each group hands its
    // channels, resolved once as the groups are read.
    std::vector<std::optional<double>> widths;

    for (const auto& node : *declaredGroups) {
        const auto* declared = node.as_table();
        if (declared == nullptr) {
            continue;
        }

        ChannelGroup group;
        group.id = toml_util::getString(*declared, "id", "");
        if (group.id.empty()) {
            report("a group has no id");
            continue;
        }
        if (set.findGroup(group.id) != kNoParent) {
            report(std::format("group '{}' is declared twice", group.id));
            continue;
        }

        // The parent comes from the id path, so there is no `parent` key to
        // keep in sync with anything. A path whose parent was never declared
        // is a typo rather than a second root, and saying so is how the typo
        // gets found -- silently rooting it would put the branch in the tree
        // looking almost right.
        const std::string_view parent = parentId(group.id);
        group.parent = parent.empty() ? kNoParent : set.findGroup(parent);
        if (!parent.empty() && group.parent == kNoParent) {
            report(
                std::format("group '{}' has no group '{}' declared before it", group.id, parent));
            continue;
        }

        group.name = toml_util::getString(*declared, "name", group.id);
        group.description = toml_util::getString(*declared, "description", "");
        group.defaultOn = toml_util::getBool(*declared, "default", false);

        const Color inherited =
            group.parent != kNoParent ? set.m_groups[group.parent].color : fileColor;

        const std::string colorText = toml_util::getString(*declared, "color", "");
        group.color = inherited;
        if (!colorText.empty()) {
            if (auto color = colorFor(colorText)) {
                group.color = *color;
            } else {
                report(std::format("group '{}' names no colour '{}'", group.id, colorText));
            }
        }

        // Width inherits the way colour does, from the nearest ancestor that
        // names one: LoRaWAN's 125 kHz is a property of the plan rather than
        // of each of its six regions, and repeating it per leaf group is how
        // one leaf ends up silently a spot.
        std::optional<double> groupWidth = frequencyAt((*declared)["width"], "width");
        if (!groupWidth) {
            groupWidth = group.parent != kNoParent ? widths[group.parent] : fileWidth;
        }

        const std::size_t index = set.m_groups.size();
        set.m_groups.push_back(std::move(group));
        widths.push_back(groupWidth);

        const auto* rows = (*declared)["channel"].as_array();
        if (rows == nullptr) {
            continue;
        }

        for (const auto& row : *rows) {
            const auto* declaredEntry = row.as_table();
            if (declaredEntry == nullptr) {
                continue;
            }

            ChannelEntry entry;
            entry.group = index;
            entry.name = toml_util::getString(*declaredEntry, "name", "");
            entry.description = toml_util::getString(*declaredEntry, "description", "");
            if (entry.name.empty()) {
                report(std::format("a channel in group '{}' has no name", set.m_groups[index].id));
                continue;
            }

            const std::optional<double> start = frequencyAt((*declaredEntry)["start"], "start");
            const std::optional<double> stop = frequencyAt((*declaredEntry)["stop"], "stop");
            const std::optional<double> center = frequencyAt((*declaredEntry)["center"], "center");

            std::optional<double> width = frequencyAt((*declaredEntry)["width"], "width");
            if (!width) {
                width = groupWidth;
            }

            if (start && stop) {
                entry.startHz = *start;
                entry.stopHz = *stop;
            } else if (center) {
                // No width anywhere makes this one frequency rather than a
                // span, which is how ADS-B 1090 and GPS L1 are written and
                // what `stop_hz == start_hz` means in the ABI.
                const double span = width.value_or(0.0);
                entry.startHz = *center - span * 0.5;
                entry.stopHz = *center + span * 0.5;
            } else {
                report(std::format("channel '{}' gives neither a centre nor a start and stop",
                                   entry.name));
                continue;
            }

            if (entry.startHz <= 0.0 || entry.stopHz < entry.startHz) {
                report(std::format("channel '{}' spans nothing", entry.name));
                continue;
            }

            entry.kind = entry.stopHz > entry.startHz ? ChannelKind::Channel : ChannelKind::Spot;
            if (const auto declaredKind =
                    parseKind(toml_util::getString(*declaredEntry, "type", ""))) {
                entry.kind = *declaredKind;
            }

            set.m_entries.push_back(std::move(entry));
        }
    }

    if (set.m_entries.empty()) {
        return fail<ChannelSet>(ErrorCode::ParseError, "{} defines no usable channels",
                                path.string());
    }

    // Sorted by start, so a range query is a scan. Stable, so two entries on
    // the same frequency stay in the order the file wrote them.
    std::ranges::stable_sort(set.m_entries, [](const ChannelEntry& a, const ChannelEntry& b) {
        return a.startHz < b.startHz;
    });

    return set;
}

std::vector<ChannelSet> ChannelSet::discover(std::span<const std::filesystem::path> directories,
                                             std::vector<std::string>* problems) {
    std::vector<ChannelSet> sets;

    for (const std::filesystem::path& directory : directories) {
        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec)) {
            continue;
        }

        // Sorted, so what an operator sees does not depend on the order the
        // filesystem happens to hand entries back in.
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".toml") {
                files.push_back(entry.path());
            }
        }
        std::ranges::sort(files);

        for (const std::filesystem::path& file : files) {
            auto set = ChannelSet::load(file, problems);
            if (!set) {
                // Reported and skipped: one unparseable file must not cost the
                // operator every other set in the directory.
                if (problems != nullptr) {
                    problems->push_back(set.error().describe());
                }
                continue;
            }

            // A user file replaces a shipped one of the same id outright --
            // the override rule every other asset here follows. A file with a
            // different id is a branch of its own beside it.
            const auto existing = std::ranges::find_if(
                sets, [&set](const ChannelSet& other) { return other.id() == set->id(); });
            if (existing != sets.end()) {
                *existing = std::move(*set);
            } else {
                sets.push_back(std::move(*set));
            }
        }
    }

    std::ranges::sort(sets,
                      [](const ChannelSet& a, const ChannelSet& b) { return a.name() < b.name(); });
    return sets;
}

std::size_t ChannelSet::findGroup(std::string_view id) const {
    const auto match =
        std::ranges::find_if(m_groups, [id](const ChannelGroup& group) { return group.id == id; });
    return match != m_groups.end() ? static_cast<std::size_t>(match - m_groups.begin()) : kNoParent;
}

std::vector<std::uint8_t>
ChannelSet::resolveEnabled(const std::set<std::string, std::less<>>& disabled) const {
    // One forward pass: a group's parent is always declared before it, so the
    // parent's answer is settled by the time this reaches the child.
    std::vector<std::uint8_t> on(m_groups.size(), 1);
    for (std::size_t i = 0; i < m_groups.size(); ++i) {
        const ChannelGroup& group = m_groups[i];
        const bool parentOn = group.parent == kNoParent || on[group.parent] != 0;
        on[i] = static_cast<std::uint8_t>(parentOn && !disabled.contains(group.id) ? 1 : 0);
    }
    return on;
}

std::vector<std::uint8_t> ChannelSet::resolveDefaults() const {
    std::vector<std::uint8_t> on(m_groups.size(), 0);
    for (std::size_t i = 0; i < m_groups.size(); ++i) {
        const ChannelGroup& group = m_groups[i];
        const bool inherited = group.parent != kNoParent && on[group.parent] != 0;
        on[i] = static_cast<std::uint8_t>(group.defaultOn || inherited ? 1 : 0);
    }
    return on;
}

std::string ChannelSet::entryKey(std::string_view groupId, std::string_view name) {
    return std::format("{}#{}", groupId, name);
}

std::vector<std::uint8_t>
ChannelSet::resolveEntries(std::span<const std::uint8_t> groupOn,
                           const std::set<std::string, std::less<>>& disabled) const {
    std::vector<std::uint8_t> on(m_entries.size(), 1);
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        const ChannelEntry& entry = m_entries[i];
        const bool groupIsOn = groupOn.empty() || groupOn[entry.group] != 0;
        on[i] = static_cast<std::uint8_t>(
            groupIsOn && !disabled.contains(entryKey(m_groups[entry.group].id, entry.name)) ? 1
                                                                                            : 0);
    }
    return on;
}

std::vector<const ChannelEntry*>
ChannelSet::entriesIn(double fromHz, double toHz, std::span<const std::uint8_t> entryOn) const {
    std::vector<const ChannelEntry*> found;
    for (const ChannelEntry& entry : m_entries) {
        if (!entryOn.empty() && entryOn[static_cast<std::size_t>(&entry - m_entries.data())] == 0) {
            continue;
        }
        // Overlap rather than containment, and a spot is a point: an entry
        // wider than the view still has to be drawn, or zooming into the
        // middle of one would make it disappear.
        if (entry.stopHz > entry.startHz ? (entry.stopHz > fromHz && entry.startHz < toHz)
                                         : (entry.startHz >= fromHz && entry.startHz <= toHz)) {
            found.push_back(&entry);
        }
    }
    return found;
}

std::vector<const ChannelEntry*>
ChannelSet::entriesAt(double hz, std::span<const std::uint8_t> entryOn) const {
    std::vector<const ChannelEntry*> found;
    for (const ChannelEntry& entry : m_entries) {
        if (!entryOn.empty() && entryOn[static_cast<std::size_t>(&entry - m_entries.data())] == 0) {
            continue;
        }
        if (entry.contains(hz)) {
            found.push_back(&entry);
        }
    }

    std::ranges::stable_sort(found, [](const ChannelEntry* a, const ChannelEntry* b) {
        return a->widthHz() < b->widthHz();
    });
    return found;
}

} // namespace channels
