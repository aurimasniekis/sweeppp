// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/rf/RfRouting.hpp"

#include "sweeppp/core/Log.hpp"

#include <algorithm>
#include <format>

namespace sweeppp {

std::vector<RfLeg> resolveRfPath(const SdrDeviceInfo& info, std::span<const SdrRxPort> ports,
                                 const AntennaLibrary& antennas,
                                 const AntennaAssignments& assignments,
                                 std::span<const OpenRfPath> switchers) {
    std::vector<RfLeg> legs;
    const std::string deviceKey = AntennaAssignments::deviceKey(info);

    for (std::size_t i = 0; i < ports.size(); ++i) {
        const double portMin = ports[i].minHz > 0.0 ? ports[i].minHz : info.minFrequencyHz;
        const double portMax = ports[i].maxHz > 0.0 ? ports[i].maxHz : info.maxFrequencyHz;

        // One leg per reachable antenna, added by the same lambda whether it
        // hangs off the connector or off a switcher input, so the two cases
        // cannot drift in how they clip the band or charge the switch.
        const auto addLeg = [&](const Antenna& antenna, std::size_t inputIndex, double inputMinHz,
                                double inputMaxHz, double inputSwitchSeconds, IRfPath* switcher,
                                std::string label) {
            legs.push_back(RfLeg{
                .route = RoutePort{.portIndex = i,
                                   .id = ports[i].id,
                                   .inputIndex = inputIndex,
                                   .startHz = std::max({antenna.startHz, portMin, inputMinHz}),
                                   .stopHz = std::min({antenna.stopHz, portMax,
                                                       inputMaxHz > 0.0 ? inputMaxHz : portMax}),
                                   .gainDbi = antenna.gainDbi,
                                   .switchSeconds = ports[i].switchSeconds,
                                   .inputSwitchSeconds = inputSwitchSeconds},
                .switcher = switcher,
                .antenna = &antenna,
                .portLabel = std::move(label)});
        };

        if (const std::string_view switcherKey = assignments.switcherFor(deviceKey, ports[i].id);
            !switcherKey.empty()) {
            const auto found =
                std::ranges::find_if(switchers, [switcherKey](const OpenRfPath& open) {
                    return open.key == switcherKey;
                });
            if (found == switchers.end() || found->path == nullptr) {
                // The box is named but not attached. Its inputs are wherever
                // they were left, so measuring through it would be measuring
                // through an unknown antenna and calling it a known one.
                continue;
            }

            IRfPath* switcher = found->path;
            const std::span<const RfPathInput> inputs = switcher->inputs();
            for (std::size_t in = 0; in < inputs.size(); ++in) {
                const std::string_view antennaId =
                    assignments.antennaOnInput(switcherKey, inputs[in].id);
                const Antenna* antenna = antennaId.empty() ? nullptr : antennas.find(antennaId);
                if (antenna == nullptr) {
                    continue;
                }
                addLeg(*antenna, in, inputs[in].minHz, inputs[in].maxHz,
                       switcher->info().switchSeconds, switcher,
                       std::format("{} via {}", ports[i].label, inputs[in].label));
            }
            continue;
        }

        const std::string_view antennaId = assignments.antennaFor(deviceKey, ports[i].id);
        const Antenna* antenna = antennaId.empty() ? nullptr : antennas.find(antennaId);
        if (antenna == nullptr) {
            continue;
        }
        addLeg(*antenna, kNoInput, 0.0, 0.0, 0.0, nullptr, ports[i].label);
    }

    return legs;
}

std::vector<std::pair<double, double>> coveredRanges(std::span<const RfLeg> legs) {
    std::vector<std::pair<double, double>> ranges;
    ranges.reserve(legs.size());
    for (const RfLeg& leg : legs) {
        if (leg.route.stopHz > leg.route.startHz) {
            ranges.emplace_back(leg.route.startHz, leg.route.stopHz);
        }
    }

    // Merged, because two antennas covering the same band cover it once. A
    // total that counted the overlap twice would read as more coverage than
    // the bench has.
    std::ranges::sort(ranges);

    std::vector<std::pair<double, double>> merged;
    for (const auto& [startHz, stopHz] : ranges) {
        if (!merged.empty() && startHz <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, stopHz);
        } else {
            merged.emplace_back(startHz, stopHz);
        }
    }
    return merged;
}

} // namespace sweeppp
