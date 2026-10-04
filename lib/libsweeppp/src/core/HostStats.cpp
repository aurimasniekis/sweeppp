// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/core/HostStats.hpp"

#include "sweeppp/core/Clock.hpp"
#include "sweeppp/core/SystemInfo.hpp"
#include "sweeppp/core/Telemetry.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <thread>
#include <utility>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <psapi.h>
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace sweeppp {
namespace {

/// Busy and total CPU ticks since boot, all cores together.
std::optional<std::pair<std::uint64_t, std::uint64_t>> cpuTicks() {
#if defined(__APPLE__)
    host_cpu_load_info_data_t load{};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&load),
                        &count) != KERN_SUCCESS) {
        return std::nullopt;
    }
    const std::uint64_t idle = load.cpu_ticks[CPU_STATE_IDLE];
    const std::uint64_t total = static_cast<std::uint64_t>(load.cpu_ticks[CPU_STATE_USER]) +
                                load.cpu_ticks[CPU_STATE_SYSTEM] + load.cpu_ticks[CPU_STATE_NICE] +
                                idle;
    return std::pair{total - idle, total};
#elif defined(_WIN32)
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetSystemTimes(&idle, &kernel, &user) == 0) {
        return std::nullopt;
    }
    const auto ticks = [](const FILETIME& t) {
        return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32U) | t.dwLowDateTime;
    };
    // Kernel time includes idle time on Windows.
    const std::uint64_t total = ticks(kernel) + ticks(user);
    return std::pair{total - ticks(idle), total};
#else
    std::ifstream in("/proc/stat");
    std::string cpu;
    std::uint64_t user = 0;
    std::uint64_t nice = 0;
    std::uint64_t system = 0;
    std::uint64_t idle = 0;
    std::uint64_t iowait = 0;
    std::uint64_t irq = 0;
    std::uint64_t softirq = 0;
    std::uint64_t steal = 0;
    if (!(in >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) ||
        cpu != "cpu") {
        return std::nullopt;
    }
    const std::uint64_t waiting = idle + iowait;
    const std::uint64_t total = user + nice + system + waiting + irq + softirq + steal;
    return std::pair{total - waiting, total};
#endif
}

/// Physical memory in use, as the system's own monitor counts it.
std::uint64_t memoryUsed([[maybe_unused]] std::uint64_t total) {
#if defined(__APPLE__)
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm),
                          &count) != KERN_SUCCESS) {
        return 0;
    }
    const auto page = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    return (static_cast<std::uint64_t>(vm.active_count) + vm.wire_count +
            vm.compressor_page_count) *
           page;
#elif defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) == 0) {
        return 0;
    }
    return status.ullTotalPhys - status.ullAvailPhys;
#else
    std::ifstream in("/proc/meminfo");
    std::string key;
    std::uint64_t value = 0;
    std::string unit;
    while (in >> key >> value >> unit) {
        if (key == "MemAvailable:") {
            return total > value * 1024 ? total - (value * 1024) : 0;
        }
    }
    return 0;
#endif
}

std::uint64_t processMemory() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) != KERN_SUCCESS) {
        return 0;
    }
    return info.resident_size;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
        return 0;
    }
    return counters.WorkingSetSize;
#else
    std::ifstream in("/proc/self/statm");
    std::uint64_t size = 0;
    std::uint64_t resident = 0;
    if (!(in >> size >> resident)) {
        return 0;
    }
    return resident * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
#endif
}

double uptimeSeconds() {
#if defined(__APPLE__)
    timeval boot{};
    std::size_t length = sizeof(boot);
    if (sysctlbyname("kern.boottime", &boot, &length, nullptr, 0) != 0) {
        return -1.0;
    }
    timeval now{};
    gettimeofday(&now, nullptr);
    return static_cast<double>(now.tv_sec - boot.tv_sec) +
           (static_cast<double>(now.tv_usec - boot.tv_usec) * 1e-6);
#elif defined(_WIN32)
    return static_cast<double>(GetTickCount64()) / 1000.0;
#else
    std::ifstream in("/proc/uptime");
    double seconds = -1.0;
    in >> seconds;
    return seconds;
#endif
}

double loadAverage() {
#if defined(_WIN32)
    return -1.0;
#else
    double load[1]{};
    return getloadavg(load, 1) == 1 ? load[0] : -1.0;
#endif
}

/// The hottest of the kernel's thermal zones -- on a Raspberry Pi the first
/// one is the SoC. Nothing on platforms without a portable answer.
std::optional<double> temperature() {
#if defined(__linux__)
    std::optional<double> hottest;
    for (int zone = 0; zone < 16; ++zone) {
        std::ifstream in(std::format("/sys/class/thermal/thermal_zone{}/temp", zone));
        long milli = 0;
        if (!(in >> milli)) {
            if (zone > 0) {
                break;
            }
            continue;
        }
        const double celsius = static_cast<double>(milli) / 1000.0;
        if (celsius > -50.0 && celsius < 200.0) {
            hottest = std::max(hottest.value_or(celsius), celsius);
        }
    }
    return hottest;
#else
    return std::nullopt;
#endif
}

std::string systemName() {
#if defined(_WIN32)
    return "Windows";
#else
    utsname name{};
    if (uname(&name) != 0) {
        return {};
    }
    // As pointers: formatted as arrays they keep every trailing NUL.
    return std::format("{} {} {}", static_cast<const char*>(name.sysname),
                       static_cast<const char*>(name.release),
                       static_cast<const char*>(name.machine));
#endif
}

} // namespace

HostStats HostSampler::sample(const std::filesystem::path& disk) {
    HostStats stats;
    static const std::string kSystem = systemName();
    stats.system = kSystem;
    stats.cores = std::thread::hardware_concurrency();

    if (const auto ticks = cpuTicks()) {
        const auto [busy, total] = *ticks;
        if (m_totalTicks != 0 && total > m_totalTicks && busy >= m_busyTicks) {
            stats.cpuPercent = 100.0 * static_cast<double>(busy - m_busyTicks) /
                               static_cast<double>(total - m_totalTicks);
        }
        m_busyTicks = busy;
        m_totalTicks = total;
    }

    const double processSeconds = processCpuSeconds();
    const std::uint64_t wallNs = monotonicNs();
    if (processSeconds >= 0.0 && m_processSeconds >= 0.0 && wallNs > m_wallNs) {
        stats.processCpuPercent = 100.0 * (processSeconds - m_processSeconds) /
                                  (static_cast<double>(wallNs - m_wallNs) * 1e-9);
    }
    m_processSeconds = processSeconds;
    m_wallNs = wallNs;

    stats.memoryTotalBytes = systemInfo().totalMemoryBytes;
    stats.memoryUsedBytes = memoryUsed(stats.memoryTotalBytes);
    stats.processMemoryBytes = processMemory();
    stats.load1 = loadAverage();
    stats.uptimeSeconds = uptimeSeconds();
    stats.temperatureC = temperature();

    std::error_code ec;
    const std::filesystem::space_info space =
        std::filesystem::space(disk.empty() ? std::filesystem::current_path(ec) : disk, ec);
    if (!ec) {
        stats.diskTotalBytes = space.capacity;
        stats.diskFreeBytes = space.available;
    }
    return stats;
}

} // namespace sweeppp
