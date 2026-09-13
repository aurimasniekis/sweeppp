// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include "sweeps/Clock.hpp"

#include "sweeps/Text.hpp"

#include <cmath>
#include <ctime>
#include <iomanip>
#include <locale>
#include <sstream>

namespace sweeps {
namespace {

std::tm toUtcTm(std::uint64_t wallNs) {
    const auto seconds = static_cast<std::time_t>(wallNs / 1'000'000'000ULL);
    std::tm out{};
#if defined(_WIN32)
    gmtime_s(&out, &seconds);
#else
    gmtime_r(&seconds, &out);
#endif
    return out;
}

std::ostringstream classicStream() {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    return stream;
}

/// Zero-padded to a fixed width, which is what `{:04}` and `{:02}` mean.
std::ostream& padded(std::ostream& stream, int value, int width) {
    return stream << std::setw(width) << std::setfill('0') << value;
}

std::string fixed(double value, int decimals, const char* suffix) {
    std::ostringstream stream = classicStream();
    stream << std::fixed << std::setprecision(decimals) << value << suffix;
    return stream.str();
}

} // namespace

std::string formatWallClockIso8601(std::uint64_t wallNs) {
    const std::tm utc = toUtcTm(wallNs);
    std::ostringstream stream = classicStream();
    padded(stream, utc.tm_year + 1900, 4) << '-';
    padded(stream, utc.tm_mon + 1, 2) << '-';
    padded(stream, utc.tm_mday, 2) << 'T';
    padded(stream, utc.tm_hour, 2) << ':';
    padded(stream, utc.tm_min, 2) << ':';
    padded(stream, utc.tm_sec, 2) << 'Z';
    return stream.str();
}

std::string formatWallClockCompact(std::uint64_t wallNs) {
    const std::tm utc = toUtcTm(wallNs);
    std::ostringstream stream = classicStream();
    padded(stream, utc.tm_year + 1900, 4);
    padded(stream, utc.tm_mon + 1, 2);
    padded(stream, utc.tm_mday, 2) << '-';
    padded(stream, utc.tm_hour, 2);
    padded(stream, utc.tm_min, 2);
    padded(stream, utc.tm_sec, 2);
    return stream.str();
}

std::string formatDuration(double seconds) {
    const double magnitude = std::abs(seconds);
    if (magnitude >= 60.0) {
        const auto totalSeconds = static_cast<long long>(seconds);
        const long long hours = totalSeconds / 3600;
        const long long minutes = (totalSeconds % 3600) / 60;
        const long long secs = totalSeconds % 60;

        std::ostringstream stream = classicStream();
        if (hours > 0) {
            stream << hours << ':';
            padded(stream, static_cast<int>(minutes), 2) << ':';
            padded(stream, static_cast<int>(secs), 2);
        } else {
            stream << minutes << ':';
            padded(stream, static_cast<int>(secs), 2);
        }
        return stream.str();
    }
    if (magnitude >= 1.0) {
        return fixed(seconds, 3, " s");
    }
    if (magnitude >= 1e-3) {
        return fixed(seconds * 1e3, 2, " ms");
    }
    if (magnitude >= 1e-6) {
        return fixed(seconds * 1e6, 1, " us");
    }
    return fixed(seconds * 1e9, 0, " ns");
}

} // namespace sweeps
