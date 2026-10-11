// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <sweeppp/plugin/PluginSdr.hpp>

namespace sweeppp::pluto {

/// This driver's log category, "pluto". Discards until activation assigns it
/// from the plugin's `Host`.
[[nodiscard]] plugin::Logger& log() noexcept;

/// The single factory instance the facet registers.
[[nodiscard]] plugin::Driver& driver() noexcept;

} // namespace sweeppp::pluto
