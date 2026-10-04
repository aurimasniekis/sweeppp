// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/plugin/PluginAbi.h"

#include <cstdint>
#include <string_view>

namespace sweeppp {

/// Application version, as reported in session manifests and on the wire.
///
/// Bare and stable -- "0.1.0". It is compared against a plugin's declared
/// `minHostVersion` and embedded in every session manifest, so the commit does
/// not belong in it. `buildString()` is the one to show a person.
[[nodiscard]] std::string_view versionString() noexcept;

/// The version with the commit it was built from -- "0.1.0+abc12345", or
/// "+abc12345.dirty" from a modified tree, or just the version where there was
/// no git repository to ask.
///
/// Spelled exactly as CI names a nightly archive, so the file someone
/// downloaded and the string in its About box are the same text. That is the
/// whole point: a report against a bare "0.1.0" names any of several hundred
/// commits.
[[nodiscard]] std::string_view buildString() noexcept;

/// "release" or "nightly": which application this build is, fixed at
/// configure time by SWEEPPP_CHANNEL. A nightly is a separate application
/// from the release, with its own name, identifier and configuration
/// directory, so the two can be installed and run side by side.
[[nodiscard]] std::string_view channel() noexcept;

/// The name a person reads: "Sweep++", or "Sweep++ Nightly". Window titles,
/// the .app, the desktop entry.
[[nodiscard]] std::string_view productName() noexcept;

/// The name a machine reads: "sweeppp", or "sweeppp-nightly". The
/// configuration directory, the window class, the paths a package installs
/// under -- everywhere "++" would be illegal or a space would be.
[[nodiscard]] std::string_view appId() noexcept;

/// When this binary was compiled, as "Sep 12 2026".
[[nodiscard]] std::string_view buildDate() noexcept;

/// The compiler and version that produced it, e.g. "AppleClang 17.0.0".
[[nodiscard]] std::string_view buildCompiler() noexcept;

/// The platform it was built for, e.g. "Darwin arm64". What it was built
/// *for*, not what it is running on: a universal question a crash report
/// needs answered before anything else.
[[nodiscard]] std::string_view buildPlatform() noexcept;

// The `.sweeps` container version is deliberately absent from this header.
// It belongs to the format, not to the application, and lives in exactly one
// place: `sweeps::kMajorVersion` in <sweeps/FileFormat.hpp>. A second constant
// here was unenforced -- nothing tied the two together, so they could disagree
// and the file would carry whichever one the writer happened to read.

/// Version of the remote streaming protocol. Shares the container's record
/// encoding, but negotiates independently of the file format.
inline constexpr std::uint32_t kRemoteProtocolVersion = 2;

/// Plugin ABI version. Bumped on any change to `sweeppp_host_api_t`'s layout,
/// or to any other struct in <sweeppp/plugin/PluginAbi.h>; the host refuses to
/// load a plugin built against a different value.
///
/// Derived from the macro rather than declared alongside it. The C header is
/// the one both sides compile against -- a plugin cannot include this file, it
/// is C++ -- so a second constant here would be a second thing to keep in step,
/// and the failure of letting them drift is a host that accepts a layout it
/// cannot read.
inline constexpr std::uint32_t kPluginAbiVersion = SWEEPPP_PLUGIN_ABI_VERSION;

} // namespace sweeppp
