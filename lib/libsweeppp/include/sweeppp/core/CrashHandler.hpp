// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string_view>

namespace sweeppp {

/// Writes a stack trace when the process dies, then lets the platform's own
/// crash reporting run as it would have.
///
/// The report is the whole point. A crash on somebody else's bench is
/// otherwise a window that vanishes and a log that ends wherever the last
/// flush happened to be -- and the interesting failures here are the ones that
/// take minutes of sweeping to reproduce, so "run it again under a debugger"
/// is not an answer.
class CrashHandler {
public:
    /// Installs the handlers. `directory` receives
    /// `sweeppp-crash-<timestamp>.log`, and `program` names the binary on the
    /// report's first line, so a GUI crash and a CLI one are told apart.
    ///
    /// Everything the handler needs is composed here, where allocating and
    /// formatting are still allowed. On the signal path they are not: a
    /// handler runs on a thread whose stack may be exhausted and whose
    /// allocator may be mid-update, so it calls nothing but `open`, `write`,
    /// `close` and `raise`. That is why the path and the header are fixed
    /// buffers filled in advance rather than built when they are needed.
    ///
    /// Calling this twice is harmless; the second call is ignored.
    static void install(const std::filesystem::path& directory, std::string_view program);

    /// Where a report would be written. Empty before `install`.
    [[nodiscard]] static std::string_view reportPath() noexcept;
};

} // namespace sweeppp
