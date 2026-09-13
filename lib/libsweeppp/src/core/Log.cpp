// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/core/Log.hpp"

#include "sweeppp/core/Clock.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>

namespace sweeppp {
namespace {

struct LogState {
    std::mutex mutex;
    Log::Sink sink;
    std::ofstream file;
};

LogState& state() {
    static LogState instance;
    return instance;
}

std::atomic<LogLevel> g_level{LogLevel::Info};

void writeToStderr(const LogRecord& record) {
    std::fputs(std::format("{} [{}] {}: {}\n", formatWallClockIso8601(record.wallNs),
                           toString(record.level), record.category, record.message)
                   .c_str(),
               stderr);
}

} // namespace

std::string_view toString(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    }
    return "?";
}

void Log::setLevel(LogLevel level) noexcept {
    g_level.store(level, std::memory_order_relaxed);
}

LogLevel Log::level() noexcept {
    return g_level.load(std::memory_order_relaxed);
}

void Log::setSink(Sink sink) {
    LogState& s = state();
    const std::lock_guard lock(s.mutex);
    s.sink = std::move(sink);
}

void Log::setLogFile(const std::string& path, std::uintmax_t maxBytes) {
    LogState& s = state();
    const std::lock_guard lock(s.mutex);
    s.file.close();
    s.file.clear();

    if (path.empty()) {
        return;
    }

    // Rotate before opening, and only here. Renaming a file the process has
    // open works on POSIX and fails on Windows, so the one moment both agree
    // on is the moment before it is opened.
    std::error_code ec;
    const std::filesystem::path current(path);
    if (maxBytes > 0 && std::filesystem::file_size(current, ec) >= maxBytes && !ec) {
        std::filesystem::path previous = current;
        previous += ".1";
        // Removed first: on Windows rename onto an existing file fails, so
        // without this the log would simply stop rotating after the first
        // generation and grow without bound.
        std::filesystem::remove(previous, ec);
        std::filesystem::rename(current, previous, ec);
    }

    s.file.open(path, std::ios::app);
    if (!s.file) {
        writeToStderr({.level = LogLevel::Warn,
                       .category = "log",
                       .message = std::format("could not open log file {}", path),
                       .wallNs = wallClockNs()});
    }
}

void Log::emit(LogLevel level, std::string_view category, std::string message) {
    const LogRecord record{.level = level,
                           .category = category,
                           .message = std::move(message),
                           .wallNs = wallClockNs()};

    LogState& s = state();
    const std::lock_guard lock(s.mutex);

    if (s.sink) {
        s.sink(record);
    } else {
        writeToStderr(record);
    }

    if (s.file.is_open()) {
        s.file << std::format("{} [{}] {}: {}\n", formatWallClockIso8601(record.wallNs),
                              toString(record.level), record.category, record.message);
        s.file.flush();
    }
}

} // namespace sweeppp
