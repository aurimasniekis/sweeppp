// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/sdr/ISdrDevice.hpp"

#include <memory>

namespace sweeppp {

/// Registers the synthetic signal generator driver.
///
/// The primary development target: it honours the configured sample rate, so
/// it can genuinely simulate a 100 MS/s load and exercise the drop, throttle
/// and telemetry paths without any hardware attached. It also emits tones at
/// known frequencies, which is what makes sweep correctness testable.
void registerSyntheticDevice();

/// Registers the IQ file replay driver -- reproducible regression input.
void registerIqFileDevice();

} // namespace sweeppp
