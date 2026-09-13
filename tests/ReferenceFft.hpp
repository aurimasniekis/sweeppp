// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace sweeppp {

/// Registers the host suite's own FFT backend, under the name "reference".
///
/// The suite needs a transform, and every real one is a plugin: a host test
/// that acquired a plugin-provided backend would be testing whether that module
/// happened to be built, which is the plugin suite's business. This one is
/// compiled into the test binary and is always there.
///
/// It advertises `FftSizeConstraint::Any` rather than power-of-two, and that is
/// load-bearing: `SweepPlanner::plan` derives a size from RBW and snaps it
/// through `snapSize`, so a power-of-two backend would round that up and shift
/// `actualRbwHz` and everything the sweep tests derive from it.
///
/// Idempotent -- call it from any test that needs a backend.
void registerReferenceFftBackend();

} // namespace sweeppp
