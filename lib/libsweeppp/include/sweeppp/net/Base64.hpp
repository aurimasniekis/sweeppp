// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Base64 (RFC 4648, the standard alphabet, padded).
namespace sweeppp::net {

[[nodiscard]] std::string base64Encode(std::span<const std::uint8_t> bytes);

/// Nothing for text that is not padded base64 of the standard alphabet.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> base64Decode(std::string_view text);

} // namespace sweeppp::net
