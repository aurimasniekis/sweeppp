// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace sweeps {

enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warn, Error };

/// Where the library's diagnostics go.
///
/// A value, not a process-wide logger: the library holds no global state, so
/// two readers in one process can log to different places, a test can capture
/// output without disturbing anything else, and nothing has to be initialised
/// before a file can be opened. A default-constructed `Log` discards
/// everything, which is what a caller that does not care wants.
///
/// The sink is called on the thread that emitted the record, so a sink that
/// blocks blocks that thread.
class Log {
public:
    using Sink =
        std::function<void(LogLevel level, std::string_view category, std::string message)>;

    Log() = default;
    explicit Log(Sink sink) : m_sink(std::move(sink)) {}

    [[nodiscard]] bool enabled() const noexcept { return static_cast<bool>(m_sink); }

    void emit(LogLevel level, std::string_view category, std::string message) const {
        if (m_sink) {
            m_sink(level, category, std::move(message));
        }
    }

private:
    Sink m_sink;
};

} // namespace sweeps
