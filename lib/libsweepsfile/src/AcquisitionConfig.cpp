// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include "sweeps/AcquisitionConfig.hpp"

#include <algorithm>
#include <cmath>

namespace sweeps {

namespace {

// At namespace scope rather than inside gridDiffers(): reading a constexpr
// local from a captureless lambda is well-formed, but MSVC rejects it with
// C3493 and there is nothing to gain from arguing with it.
constexpr double kEpsilon = 1e-6;

} // namespace

bool AcquisitionConfig::gridDiffers(const AcquisitionConfig& other) const noexcept {
    // Exactly the grid-affecting set from the session-segment rules. Gain and
    // reference level are deliberately absent: they shift the noise floor but
    // leave the frequency grid intact, so they are recorded as events rather
    // than forcing a new segment.
    const auto differs = [](double a, double b) {
        return std::abs(a - b) > kEpsilon * std::max({std::abs(a), std::abs(b), 1.0});
    };

    return differs(centerHz, other.centerHz) || differs(spanHz, other.spanHz) ||
           differs(sampleRate, other.sampleRate) || fftSize != other.fftSize ||
           window != other.window || differs(overlap, other.overlap) || deviceId != other.deviceId;
}

} // namespace sweeps
