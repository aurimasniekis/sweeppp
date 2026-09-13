// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <sweeps/Clock.hpp>

namespace sweeppp {

/// Monotonic nanoseconds since an arbitrary origin.
///
/// Everything that measures *duration* -- frame timestamps, sweep dwell,
/// telemetry intervals, replay pacing -- uses this. It cannot jump backwards
/// when the system clock is corrected, which a session file's time axis very
/// much depends on.
[[nodiscard]] inline std::uint64_t monotonicNs() noexcept {
    return sweeps::monotonicNs();
}

/// Wall-clock nanoseconds since the Unix epoch.
///
/// Used only where a human needs to know *when* something happened: session
/// start time in the manifest, filenames, log lines. Never for measuring
/// intervals.
[[nodiscard]] inline std::uint64_t wallClockNs() noexcept {
    return sweeps::wallClockNs();
}

[[nodiscard]] inline double nsToSeconds(std::uint64_t ns) noexcept {
    return sweeps::nsToSeconds(ns);
}

[[nodiscard]] inline std::uint64_t secondsToNs(double seconds) noexcept {
    return sweeps::secondsToNs(seconds);
}

// The three formatters below live in libsweepsfile rather than here, and these
// are forwarders rather than copies. Their output reaches manifest bytes -- a
// session's `created` key is formatWallClockIso8601, and the default session
// name is formatWallClockCompact -- so a second implementation that drifted
// would change file content depending on which one a call site happened to
// reach.

/// RFC 3339 UTC, e.g. "2026-08-09T14:27:59Z". Session manifests and log lines.
[[nodiscard]] inline std::string formatWallClockIso8601(std::uint64_t wallNs) {
    return sweeps::formatWallClockIso8601(wallNs);
}

/// Suitable for a filename: "20260809-142759".
[[nodiscard]] inline std::string formatWallClockCompact(std::uint64_t wallNs) {
    return sweeps::formatWallClockCompact(wallNs);
}

/// "1.234 s", "12.3 ms", "456 us" -- picks the unit that keeps the number
/// readable, for latency and dwell readouts.
[[nodiscard]] inline std::string formatDuration(double seconds) {
    return sweeps::formatDuration(seconds);
}

} // namespace sweeppp
