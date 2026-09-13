// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <sweeppp/plugin/PluginSdr.hpp>

namespace sweeppp::fobos {

/// This driver's log category.
///
/// Deliberately "fobos" rather than the plugin's reverse-DNS id: the log is
/// read by an operator looking for what their radio did, and that is the word
/// they will be grepping for.
///
/// Assigned once during activation, from the plugin's `Host`. Before that it
/// discards, which is what a message emitted before the host exists should do.
[[nodiscard]] plugin::Logger& log() noexcept;

/// The single factory instance the facet registers.
[[nodiscard]] plugin::Driver& driver() noexcept;

} // namespace sweeppp::fobos
