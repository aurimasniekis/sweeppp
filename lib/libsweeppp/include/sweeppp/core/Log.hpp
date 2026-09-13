// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace sweeppp {

// GCC's -Wshadow treats a scoped enumerator like an unscoped one, so `Error`
// here is reported as shadowing the `sweeppp::Error` type wherever Result.hpp
// is visible first. A scoped enumerator cannot shadow anything -- it is only
// ever reached through `LogLevel::` -- and the name mirrors
// `sweeps::LogLevel::Error`, which SweepsLog.hpp asserts it stays in step with.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warn, Error };
#pragma GCC diagnostic pop

[[nodiscard]] std::string_view toString(LogLevel level) noexcept;

struct LogRecord {
    LogLevel level = LogLevel::Info;
    std::string_view category;
    std::string message;
    std::uint64_t wallNs = 0;
};

/// Process-wide log. Deliberately minimal: a level, a category and a sink.
///
/// The sink exists so the GUI can mirror log lines into a panel and the CLI
/// can write them to stderr, without either knowing about the other. Sinks are
/// called on the emitting thread, so a sink that blocks blocks that thread --
/// nothing on the acquisition path logs per-sample for exactly that reason.
class Log {
public:
    using Sink = std::function<void(const LogRecord&)>;

    static void setLevel(LogLevel level) noexcept;
    [[nodiscard]] static LogLevel level() noexcept;

    /// Replaces the sink. Passing nullptr restores the default stderr sink.
    static void setSink(Sink sink);

    /// How large the log may grow before opening it rotates it.
    ///
    /// One generation is kept. A few megabytes is days of ordinary running
    /// and still small enough to attach to a report, which is the only thing
    /// this file is for.
    static constexpr std::uintmax_t kMaxLogBytes = std::uintmax_t{4} * 1024 * 1024;

    /// Also append every record to a file. Errors opening it are reported
    /// through the existing sink and otherwise ignored -- failing to log is
    /// never a reason to fail the operation being logged.
    ///
    /// A file already at or over `maxBytes` is renamed to `<path>.1`, one
    /// generation deep, before this one is opened. Checked here rather than
    /// per record: `emit` flushes every line and is called from every thread,
    /// and a `stat` on that path would cost more than the writing does.
    /// The bound is therefore on where a run *starts*, not on the size a
    /// single very long run can reach.
    ///
    /// An empty path stops writing to a file at all.
    static void setLogFile(const std::string& path, std::uintmax_t maxBytes = kMaxLogBytes);

    static void emit(LogLevel level, std::string_view category, std::string message);

    [[nodiscard]] static bool enabled(LogLevel level) noexcept {
        return static_cast<std::uint8_t>(level) >= static_cast<std::uint8_t>(Log::level());
    }
};

namespace detail {

template <typename... Args>
void logIf(LogLevel level, std::string_view category, std::format_string<Args...> fmt,
           Args&&... args) {
    if (Log::enabled(level)) {
        Log::emit(level, category, std::format(fmt, std::forward<Args>(args)...));
    }
}

} // namespace detail

template <typename... Args>
void logTrace(std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
    detail::logIf(LogLevel::Trace, category, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void logDebug(std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
    detail::logIf(LogLevel::Debug, category, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void logInfo(std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
    detail::logIf(LogLevel::Info, category, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void logWarn(std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
    detail::logIf(LogLevel::Warn, category, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void logError(std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
    detail::logIf(LogLevel::Error, category, fmt, std::forward<Args>(args)...);
}

} // namespace sweeppp
