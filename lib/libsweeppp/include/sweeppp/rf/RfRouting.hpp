// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/rf/Antenna.hpp"
#include "sweeppp/rf/AntennaAssignments.hpp"
#include "sweeppp/rf/IRfPath.hpp"
#include "sweeppp/sdr/SdrDeviceInfo.hpp"
#include "sweeppp/sdr/SdrRxPort.hpp"
#include "sweeppp/sweep/SweepPlan.hpp"

#include <span>
#include <utility>
#include <vector>

namespace sweeppp {

/// One resolved path from the tuner to an antenna.
///
/// `route` is the part the planner sees; the rest is what the engine and the
/// panel need and the planner must not.
struct RfLeg {
    RoutePort route;
    IRfPath* switcher = nullptr;      ///< Null when the antenna is on the connector
    const Antenna* antenna = nullptr; ///< Never null
    std::string portLabel;            ///< "RX1", or "RX1 via J3"
};

/// Every antenna this bench can hear through, with the band each can reach.
///
/// The one definition of what is in front of the tuner. Three callers need it
/// -- the sweep engine plans against it, the device panel reports coverage
/// from it, and the range panel offers it as a range to sweep -- and three
/// copies of "walk port -> switcher -> input -> antenna" would answer the same
/// question three ways within a release.
///
/// A band is the antenna's coverage intersected with everything between it and
/// the tuner: the switcher input's limits where the box narrows them, the
/// port's own, and the device's. A port with nothing assigned, or a switcher
/// named but not connected, contributes nothing -- which is what makes an
/// unassigned connector show up as spectrum the operator is told about rather
/// than as a silent choice to measure through it.
[[nodiscard]] std::vector<RfLeg> resolveRfPath(const SdrDeviceInfo& info,
                                               std::span<const SdrRxPort> ports,
                                               const AntennaLibrary& antennas,
                                               const AntennaAssignments& assignments,
                                               std::span<const OpenRfPath> switchers);

/// The frequencies `legs` can reach, merged and in ascending order.
[[nodiscard]] std::vector<std::pair<double, double>> coveredRanges(std::span<const RfLeg> legs);

} // namespace sweeppp
