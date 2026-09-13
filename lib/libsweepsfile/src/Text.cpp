// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include "sweeps/Text.hpp"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace sweeps {
namespace detail {
namespace {

/// A classic-locale stream, always.
///
/// Never the global locale: a user running under a comma-decimal locale would
/// otherwise write "0,5" into a session manifest, producing a file that no
/// TOML parser accepts and that nobody would connect to their locale setting.
std::ostringstream classicStream() {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    return stream;
}

} // namespace

std::string formatImpl(std::string_view fmt, const std::vector<std::string>& args) {
    std::string out;
    out.reserve(fmt.size() + args.size() * 8);

    std::size_t next = 0;
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        const char ch = fmt[i];

        if (ch == '{') {
            if (i + 1 < fmt.size() && fmt[i + 1] == '{') {
                out.push_back('{');
                ++i;
                continue;
            }
            // Only `{}` is understood. Anything else -- `{0}`, `{:.2f}` -- is
            // copied through verbatim rather than silently dropped, so a
            // mistake shows up in the message instead of vanishing.
            if (i + 1 < fmt.size() && fmt[i + 1] == '}') {
                if (next < args.size()) {
                    out += args[next++];
                }
                ++i;
                continue;
            }
            out.push_back(ch);
            continue;
        }

        if (ch == '}' && i + 1 < fmt.size() && fmt[i + 1] == '}') {
            out.push_back('}');
            ++i;
            continue;
        }

        out.push_back(ch);
    }

    return out;
}

std::string toText(bool value) {
    return value ? "true" : "false";
}
std::string toText(char value) {
    return std::string(1, value);
}
std::string toText(std::string value) {
    return value;
}
std::string toText(std::string_view value) {
    return std::string(value);
}
std::string toText(const char* value) {
    return value != nullptr ? std::string(value) : std::string();
}

} // namespace detail

std::string formatFrequencyShort(double hz) {
    // Four significant digits in general notation -- what `{:.4g}` produces,
    // and what a stream in default float notation at precision 4 produces,
    // because both are specified in terms of printf's %g.
    const auto render = [](double value, const char* unit) {
        std::ostringstream stream = detail::classicStream();
        stream << std::defaultfloat << std::setprecision(4) << value << ' ' << unit;
        return stream.str();
    };

    const double magnitude = std::abs(hz);
    if (magnitude >= 1e9) {
        return render(hz / 1e9, "GHz");
    }
    if (magnitude >= 1e6) {
        return render(hz / 1e6, "MHz");
    }
    if (magnitude >= 1e3) {
        return render(hz / 1e3, "kHz");
    }
    return render(hz, "Hz");
}

std::string formatBytes(std::uint64_t bytes) {
    constexpr double kKiB = 1024.0;
    const auto value = static_cast<double>(bytes);

    const auto render = [](double scaled, int decimals, const char* unit) {
        std::ostringstream stream = detail::classicStream();
        stream << std::fixed << std::setprecision(decimals) << scaled << ' ' << unit;
        return stream.str();
    };

    if (value >= kKiB * kKiB * kKiB * kKiB) {
        return render(value / (kKiB * kKiB * kKiB * kKiB), 2, "TiB");
    }
    if (value >= kKiB * kKiB * kKiB) {
        return render(value / (kKiB * kKiB * kKiB), 2, "GiB");
    }
    if (value >= kKiB * kKiB) {
        return render(value / (kKiB * kKiB), 1, "MiB");
    }
    if (value >= kKiB) {
        return render(value / kKiB, 1, "KiB");
    }
    return detail::format("{} B", bytes);
}

} // namespace sweeps
