// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/fft/IFftBackend.hpp"

#include <bit>
#include <mutex>

namespace sweeppp {
namespace {

/// One process-wide planner lock.
///
/// Global rather than per backend, and that is not laziness: what a planner
/// mutates is usually its library's own state, which two backend *instances*
/// of the same library share anyway. Planning happens once per FFT size and
/// costs FFTW half a second at 65536, so the lock is contended about as often
/// as an operator changes size -- and when it is contended, serialising is
/// exactly the required behaviour rather than a cost.
std::mutex& plannerMutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace

std::string_view toString(FftBackendType type) noexcept {
    switch (type) {
    case FftBackendType::Cpu:
        return "CPU";
    case FftBackendType::Gpu:
        return "GPU";
    }
    return "CPU";
}

std::string_view toString(FftPlanQuality quality) noexcept {
    switch (quality) {
    case FftPlanQuality::Fast:
        return "fast";
    case FftPlanQuality::Balanced:
        return "balanced";
    case FftPlanQuality::Thorough:
        return "thorough";
    }
    return "balanced";
}

bool IFftBackend::supportsSize(std::size_t size) const noexcept {
    const FftCapabilities& caps = capabilities();
    if (size < caps.minSize || size > caps.maxSize) {
        return false;
    }
    if (caps.sizeConstraint == FftSizeConstraint::PowerOfTwo) {
        return std::has_single_bit(size);
    }
    return true;
}

std::size_t IFftBackend::snapSize(std::size_t desired) const noexcept {
    const FftCapabilities& caps = capabilities();
    std::size_t size = std::max(desired, caps.minSize);

    if (caps.sizeConstraint == FftSizeConstraint::PowerOfTwo && !std::has_single_bit(size)) {
        size = std::bit_ceil(size);
    }

    return std::min(size, caps.maxSize);
}

Result<std::unique_ptr<IFftPlan>> createPlanSerialised(IFftBackend& backend,
                                                       const FftPlanConfig& config) {
    if (backend.capabilities().threadSafePlanning) {
        return backend.createPlan(config);
    }
    const std::lock_guard lock(plannerMutex());
    return backend.createPlan(config);
}

} // namespace sweeppp
