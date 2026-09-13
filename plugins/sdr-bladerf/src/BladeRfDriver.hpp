// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <sweeppp/plugin/PluginSdr.hpp>

/// `blade` rather than `bladerf`, and not by preference: libbladeRF's device
/// handle is `struct bladerf`, so a namespace of that name hides the type
/// inside itself and every `bladerf*` in the driver stops compiling.
namespace sweeppp::blade {

/// This driver's log category.
///
/// Deliberately "bladerf" rather than the plugin's reverse-DNS id: the log is
/// read by an operator looking for what their radio did, and it is the same
/// word they were grepping for when the driver was built in.
///
/// Assigned once during activation, from the plugin's `Host`. Before that it
/// discards, which is what a message emitted before the host exists should do.
[[nodiscard]] plugin::Logger& log() noexcept;

/// The single factory instance the facet registers.
[[nodiscard]] plugin::Driver& driver() noexcept;

} // namespace sweeppp::blade
