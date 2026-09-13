// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "BandPlan.hpp"

#include <algorithm>
#include <format>
#include <sweeppp/core/Toml.hpp>

namespace bandplan {

using sweeppp::ErrorCode;
using sweeppp::fail;
namespace toml_util = sweeppp::toml_util;

namespace {

ContributionKind parseKind(std::string_view text) noexcept {
    if (text == "channel") {
        return ContributionKind::Channel;
    }
    if (text == "spot") {
        return ContributionKind::Spot;
    }
    return ContributionKind::Band;
}

} // namespace

Result<BandPlan> BandPlan::load(const std::filesystem::path& path) {
    auto table = toml_util::load(path);
    if (!table) {
        return std::unexpected(table.error());
    }

    BandPlan plan;
    plan.m_name = toml_util::getString(*table, "bandplan.name", path.stem().string());
    plan.m_description = toml_util::getString(*table, "bandplan.description", "");

    // Group colours first: a band names its group, and the group carries the
    // colour, so recolouring a whole service is one edit rather than thirty.
    std::vector<std::pair<std::string, Color>> groups;
    std::string colorProblem;
    if (const auto* colors = toml_util::at(*table, "bandplan.colors").as_table()) {
        for (const auto& [key, node] : *colors) {
            if (auto text = node.value<std::string>()) {
                if (auto color = Color::fromHex(*text)) {
                    groups.emplace_back(std::string(key.str()), *color);
                } else if (colorProblem.empty()) {
                    colorProblem =
                        std::format("{} is not a colour: {}", std::string(key.str()), *text);
                }
            }
        }
    }

    const auto* bands = toml_util::at(*table, "band").as_array();
    if (bands == nullptr) {
        return fail<BandPlan>(ErrorCode::ParseError, "{} defines no bands", path.string());
    }

    for (const auto& node : *bands) {
        const auto* entry = node.as_table();
        if (entry == nullptr) {
            continue;
        }

        Band band;
        band.name = toml_util::getString(*entry, "name", "");
        band.description = toml_util::getString(*entry, "description", "");
        band.group = toml_util::getString(*entry, "group", "");
        band.kind = parseKind(toml_util::getString(*entry, "type", ""));

        // Frequencies go through the same parser as every other config file,
        // so "148.5 kHz" and 148500.0 both work and mean the same thing. One
        // at a time, because a spot gives a start and no stop and the two must
        // not be able to spoil each other's parse.
        const auto start = toml_util::parseFrequency(toml_util::getString(*entry, "start", ""));
        band.startHz = start ? *start : toml_util::getDouble(*entry, "start", 0.0);

        const auto stop = toml_util::parseFrequency(toml_util::getString(*entry, "stop", ""));
        band.stopHz = stop ? *stop : toml_util::getDouble(*entry, "stop", band.startHz);

        // A spot is one frequency and so has no stop to give; everything else
        // must span something, or it is a typo rather than an allocation.
        const bool spot = band.kind == ContributionKind::Spot;
        if (band.name.empty() ||
            (spot ? band.stopHz < band.startHz : band.stopHz <= band.startHz)) {
            continue;
        }

        const auto group = std::ranges::find_if(
            groups, [&band](const auto& candidate) { return candidate.first == band.group; });
        band.color = group != groups.end() ? group->second : Color{0.5F, 0.5F, 0.5F, 1.0F};

        plan.m_bands.push_back(std::move(band));
    }

    if (plan.m_bands.empty()) {
        return fail<BandPlan>(ErrorCode::ParseError, "{} defines no usable bands{}", path.string(),
                              colorProblem.empty() ? "" : std::format(" ({})", colorProblem));
    }

    std::ranges::sort(plan.m_bands,
                      [](const Band& a, const Band& b) { return a.startHz < b.startHz; });

    // The service categories actually used, sorted, with each band pointed at
    // its own. Sorted rather than in file order because this is what the
    // settings tree branches on, and a tree whose branches move when a band is
    // added in the middle of a file is a tree nobody can learn.
    for (const Band& band : plan.m_bands) {
        plan.m_groups.push_back(band.group);
    }
    std::ranges::sort(plan.m_groups);
    plan.m_groups.erase(std::ranges::unique(plan.m_groups).begin(), plan.m_groups.end());

    for (Band& band : plan.m_bands) {
        band.groupIndex = static_cast<std::size_t>(
            std::ranges::lower_bound(plan.m_groups, band.group) - plan.m_groups.begin());
    }

    return plan;
}

std::vector<BandPlan> BandPlan::discover(std::span<const std::filesystem::path> directories,
                                         std::vector<std::string>* problems) {
    std::vector<BandPlan> plans;

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
            auto plan = BandPlan::load(file);
            if (!plan) {
                // Reported and skipped: one unparseable file must not cost the
                // operator every other plan in the directory.
                if (problems != nullptr) {
                    problems->push_back(plan.error().describe());
                }
                continue;
            }

            // A user file replaces a built-in of the same name rather than
            // adding to it. Someone correcting their region's allocations
            // wants their version, not both.
            const auto existing = std::ranges::find_if(
                plans, [&plan](const BandPlan& other) { return other.name() == plan->name(); });
            if (existing != plans.end()) {
                *existing = std::move(*plan);
            } else {
                plans.push_back(std::move(*plan));
            }
        }
    }

    std::ranges::sort(plans,
                      [](const BandPlan& a, const BandPlan& b) { return a.name() < b.name(); });
    return plans;
}

std::string BandPlan::bandKey(std::string_view group, std::string_view name) {
    return std::format("{}#{}", group, name);
}

std::vector<std::uint8_t>
BandPlan::resolveEnabled(const std::set<std::string, std::less<>>& disabled) const {
    std::vector<std::uint8_t> on(m_groups.size(), 1);
    for (std::size_t i = 0; i < m_groups.size(); ++i) {
        on[i] = static_cast<std::uint8_t>(disabled.contains(m_groups[i]) ? 0 : 1);
    }
    return on;
}

std::vector<std::uint8_t>
BandPlan::resolveBands(std::span<const std::uint8_t> groupOn,
                       const std::set<std::string, std::less<>>& disabled) const {
    std::vector<std::uint8_t> on(m_bands.size(), 1);
    for (std::size_t i = 0; i < m_bands.size(); ++i) {
        const Band& band = m_bands[i];
        const bool groupIsOn = groupOn.empty() || groupOn[band.groupIndex] != 0;
        on[i] = static_cast<std::uint8_t>(
            groupIsOn && !disabled.contains(bandKey(band.group, band.name)) ? 1 : 0);
    }
    return on;
}

std::vector<const Band*> BandPlan::bandsAt(double hz, std::span<const std::uint8_t> bandOn) const {
    std::vector<const Band*> found;
    for (const Band& band : m_bands) {
        if (!bandOn.empty() && bandOn[static_cast<std::size_t>(&band - m_bands.data())] == 0) {
            continue;
        }
        if (band.contains(hz)) {
            found.push_back(&band);
        }
    }

    // Narrowest first: the most specific claim leads, and whoever reads only
    // the first entry gets the answer the old narrowest-wins rule gave.
    std::ranges::sort(found,
                      [](const Band* a, const Band* b) { return a->widthHz() < b->widthHz(); });
    return found;
}

std::vector<const Band*> BandPlan::bandsIn(double fromHz, double toHz,
                                           std::span<const std::uint8_t> bandOn) const {
    std::vector<const Band*> found;
    for (const Band& band : m_bands) {
        if (!bandOn.empty() && bandOn[static_cast<std::size_t>(&band - m_bands.data())] == 0) {
            continue;
        }
        if (band.stopHz > fromHz && band.startHz < toHz) {
            found.push_back(&band);
        }
    }
    return found;
}

} // namespace bandplan
