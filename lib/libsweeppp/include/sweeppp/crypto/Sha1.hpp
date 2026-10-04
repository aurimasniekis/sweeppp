// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace sweeppp::crypto {

using Sha1Digest = std::array<std::uint8_t, 20>;

/// SHA-1 (RFC 3174), for the one place a protocol still names it: the
/// WebSocket handshake's accept key. Nothing here relies on it for security.
[[nodiscard]] Sha1Digest sha1(std::span<const std::uint8_t> data);
[[nodiscard]] Sha1Digest sha1(std::string_view text);

} // namespace sweeppp::crypto
