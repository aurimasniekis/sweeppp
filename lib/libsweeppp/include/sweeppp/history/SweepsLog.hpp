// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Log.hpp"

#include <sweeps/Log.hpp>
#include <type_traits>

namespace sweeppp::session {

// The two level enumerations are separate types with the same shape. Nothing
// forces them to stay that way, so this does -- a value added to one and not
// the other would otherwise silently remap every level above it.
static_assert(static_cast<int>(sweeps::LogLevel::Trace) == static_cast<int>(LogLevel::Trace));
static_assert(static_cast<int>(sweeps::LogLevel::Debug) == static_cast<int>(LogLevel::Debug));
static_assert(static_cast<int>(sweeps::LogLevel::Info) == static_cast<int>(LogLevel::Info));
static_assert(static_cast<int>(sweeps::LogLevel::Warn) == static_cast<int>(LogLevel::Warn));
static_assert(static_cast<int>(sweeps::LogLevel::Error) == static_cast<int>(LogLevel::Error));
static_assert(
    std::is_same_v<std::underlying_type_t<sweeps::LogLevel>, std::underlying_type_t<LogLevel>>);

/// Routes libsweepsfile's diagnostics into Sweep++'s log.
///
/// The library holds no global logger, so this is how a session reader or
/// writer built here gets heard. The category the library emits -- "session" --
/// arrives as the sink's `category` argument, so per-category filtering keeps
/// working exactly as it did when the code lived in this project.
[[nodiscard]] inline sweeps::Log sweepsLogSink() {
    return sweeps::Log([](sweeps::LogLevel level, std::string_view category, std::string message) {
        const auto mapped = static_cast<LogLevel>(level);
        if (Log::enabled(mapped)) {
            Log::emit(mapped, category, std::move(message));
        }
    });
}

} // namespace sweeppp::session
