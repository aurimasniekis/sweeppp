// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/core/SystemInfo.hpp"

#include <cstddef>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace sweeppp {

SystemInfo systemInfo() {
    SystemInfo info;

#if defined(__APPLE__)
    std::uint64_t memory = 0;
    std::size_t length = sizeof(memory);
    if (sysctlbyname("hw.memsize", &memory, &length, nullptr, 0) == 0) {
        info.totalMemoryBytes = memory;
    }

    // Apple silicon shares one pool between CPU and GPU. Intel Macs with a
    // discrete card do not, and there is no reliable runtime test that
    // distinguishes them without pulling in Metal, so this follows the
    // architecture the binary was built for.
#if defined(__aarch64__)
    info.unifiedMemory = true;
#endif

#elif defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) != 0) {
        info.totalMemoryBytes = status.ullTotalPhys;
    }

#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0) {
        info.totalMemoryBytes =
            static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(pageSize);
    }
#endif

    return info;
}

} // namespace sweeppp
