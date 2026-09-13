// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include "sweeps/WindowType.hpp"

#include <cctype>
#include <string>

namespace sweeps {

using detail::fail;

std::string_view toString(WindowType type) noexcept {
    switch (type) {
    case WindowType::Rectangular:
        return "rectangular";
    case WindowType::Hann:
        return "hann";
    case WindowType::Hamming:
        return "hamming";
    case WindowType::BlackmanHarris:
        return "blackman-harris";
    case WindowType::FlatTop:
        return "flat-top";
    case WindowType::Kaiser:
        return "kaiser";
    }
    return "hann";
}

Result<WindowType> windowTypeFromString(std::string_view name) {
    std::string lowered;
    lowered.reserve(name.size());
    for (const char ch : name) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    if (lowered == "rectangular" || lowered == "rect" || lowered == "none" || lowered == "boxcar" ||
        lowered == "uniform") {
        return WindowType::Rectangular;
    }
    if (lowered == "hann" || lowered == "hanning") {
        return WindowType::Hann;
    }
    if (lowered == "hamming") {
        return WindowType::Hamming;
    }
    if (lowered == "blackman-harris" || lowered == "blackmanharris" ||
        lowered == "blackman_harris" || lowered == "bh" || lowered == "blackman") {
        return WindowType::BlackmanHarris;
    }
    if (lowered == "flat-top" || lowered == "flattop" || lowered == "flat_top") {
        return WindowType::FlatTop;
    }
    if (lowered == "kaiser") {
        return WindowType::Kaiser;
    }

    return fail<WindowType>(ErrorCode::InvalidArgument, "unknown window '{}'", name);
}

} // namespace sweeps
