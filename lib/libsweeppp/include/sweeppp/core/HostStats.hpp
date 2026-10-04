// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace sweeppp {

/// What the machine running the instrument is doing: what a server's operator,
/// who is not at it, needs to see. A negative number or zero total is "this
/// platform does not say".
struct HostStats {
    std::string system; ///< "Linux 6.6.31 aarch64"
    std::uint32_t cores = 0;
    double cpuPercent = -1.0;        ///< All cores together, 0..100
    double processCpuPercent = -1.0; ///< This process, of one core
    std::uint64_t memoryTotalBytes = 0;
    std::uint64_t memoryUsedBytes = 0; ///< Not reclaimable on demand
    std::uint64_t processMemoryBytes = 0;
    double load1 = -1.0;
    double uptimeSeconds = -1.0;
    std::optional<double> temperatureC; ///< The hottest sensor that answered
    std::uint64_t diskTotalBytes = 0;   ///< Where recordings are written
    std::uint64_t diskFreeBytes = 0;
};

/// Reads `HostStats`. Rates are over the interval since the previous call, so
/// the first answer has none.
class HostSampler {
public:
    [[nodiscard]] HostStats sample(const std::filesystem::path& disk);

private:
    std::uint64_t m_busyTicks = 0;
    std::uint64_t m_totalTicks = 0;
    double m_processSeconds = -1.0;
    std::uint64_t m_wallNs = 0;
};

} // namespace sweeppp
