// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace sweeps {
namespace detail {

/// The subset of `std::format` this library uses: `{}` substitution, plus
/// `{{` and `}}` for literal braces. No format specifications.
///
/// A C++17 library cannot have `std::format`, and a dependency on {fmt} to
/// interpolate a filename into an error message would cost more than it is
/// worth. Every message this library produces was checked to use a bare `{}`,
/// and `formatSpecUnsupported` below turns a future one that does not into a
/// compile error rather than a silently mis-rendered string.
///
/// Deliberately no floating-point support: `std::format`'s `{}` for a double is
/// shortest-round-trip, an `ostringstream`'s default is six significant digits,
/// and quietly picking the second would change strings that are pinned by
/// tests -- and, for the segment `reason` string, written into the file. A
/// caller with a number to render formats it first, with one of the functions
/// below or with `formatDuration`.
[[nodiscard]] std::string formatImpl(std::string_view fmt, const std::vector<std::string>& args);

[[nodiscard]] std::string toText(bool value);
[[nodiscard]] std::string toText(char value);
[[nodiscard]] std::string toText(std::string value);
[[nodiscard]] std::string toText(std::string_view value);
[[nodiscard]] std::string toText(const char* value);

template <typename T>
[[nodiscard]] std::enable_if_t<std::is_integral_v<T>, std::string> toText(T value) {
    return std::to_string(value);
}

template <typename T>
[[nodiscard]] std::enable_if_t<std::is_enum_v<T>, std::string> toText(T value) {
    return std::to_string(static_cast<std::underlying_type_t<T>>(value));
}

template <typename T>
std::enable_if_t<std::is_floating_point_v<T>, std::string> toText(T) = delete;

template <typename... Args>
[[nodiscard]] std::string format(std::string_view fmt, Args&&... args) {
    return formatImpl(fmt, std::vector<std::string>{toText(std::forward<Args>(args))...});
}

} // namespace detail

// ---------------------------------------------------------------------------
// Human-readable quantities.
//
// These are not decoration: `formatFrequencyShort` builds the segment `reason`
// string, which is written into the file. A change in its output is a change in
// file content, which is why it lives here beside the format rather than in an
// application's UI layer.
// ---------------------------------------------------------------------------

/// Compact form for labels and chips: "2.4 GHz", "868.3 MHz", "12.5 kHz".
/// Four significant digits, unit chosen from the magnitude.
[[nodiscard]] std::string formatFrequencyShort(double hz);

/// "12.4 GiB", "1.5 MiB", "512 B" -- session file sizes.
///
/// Binary units, because storage is conventionally binary.
[[nodiscard]] std::string formatBytes(std::uint64_t bytes);

} // namespace sweeps
