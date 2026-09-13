// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace sweeps {

/// Monotonic nanoseconds since an arbitrary origin.
///
/// Everything that measures *duration* uses this -- every time axis in a
/// `.sweeps` file is expressed in it. It cannot jump backwards when the system
/// clock is corrected, which a session's time axis very much depends on.
[[nodiscard]] inline std::uint64_t monotonicNs() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

/// Wall-clock nanoseconds since the Unix epoch.
///
/// Used only where a human needs to know *when* something happened: the file
/// header's creation stamp, the manifest, filenames. Never for measuring
/// intervals.
[[nodiscard]] inline std::uint64_t wallClockNs() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

[[nodiscard]] inline double nsToSeconds(std::uint64_t ns) noexcept {
    return static_cast<double>(ns) * 1e-9;
}

[[nodiscard]] inline std::uint64_t secondsToNs(double seconds) noexcept {
    return static_cast<std::uint64_t>(seconds * 1e9);
}

/// RFC 3339 UTC, e.g. "2026-08-09T14:27:59Z".
///
/// This one lands in manifest bytes, so its exact output is part of the file
/// format rather than a presentation choice.
[[nodiscard]] std::string formatWallClockIso8601(std::uint64_t wallNs);

/// Suitable for a filename: "20260809-142759". Also reaches the manifest, as
/// the default session name.
[[nodiscard]] std::string formatWallClockCompact(std::uint64_t wallNs);

/// "1.234 s", "12.30 ms", "456.0 us" -- picks the unit that keeps the number
/// readable. Above a minute, "m:ss" or "h:mm:ss".
[[nodiscard]] std::string formatDuration(double seconds);

} // namespace sweeps
