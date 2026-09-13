// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace sweeppp {

/// One RF input the tuner can listen on.
///
/// A first-class object rather than a naming convention over an Enum
/// parameter: the sweep planner routes by it, the assignment store keys on it,
/// and both need the frequency limits and the switching cost that a parameter
/// value cannot carry.
///
/// Beside `SdrDeviceInfo`'s value types in spirit and for the same reason -- a
/// plugin driver names this without naming `ISdrDevice`, because it implements
/// a different base class and the host converts.
struct SdrRxPort {
    std::string id;        ///< Stable; what an assignment stores. "rx1"
    std::string label;     ///< "RX1"
    std::string connector; ///< What is printed on the case: "SMA (J1)". May be empty.

    /// Where this port can tune, when it is narrower than the device's own
    /// range. Zero means the device's limits apply -- `info()` reports the
    /// union across ports, so a plan inside it may still be unreachable on a
    /// particular one.
    double minHz = 0.0;
    double maxHz = 0.0;

    bool biasTee = false; ///< This port can supply DC on the connector

    /// Selecting it needs the stream stopped and restarted. The engine cycles
    /// the device's stream around the switch rather than the driver doing it
    /// silently, because the samples in flight belong to the *old* antenna and
    /// something has to account for them.
    bool requiresStop = false;

    /// Settle after selecting, beyond a retune. Charged once per transition by
    /// the planner and waited out by the engine, exactly as the retune settle
    /// is -- a predicted sweep rate that ignores a stream cycle per pass is a
    /// number the Analysis panel would print in bold and be wrong about.
    double switchSeconds = 0.0;
};

} // namespace sweeppp
