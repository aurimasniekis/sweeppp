// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <sweeppp/plugin/PluginFft.hpp>

namespace sweeppp::accel {

/// This backend's log category.
///
/// Deliberately "fft" rather than the plugin's reverse-DNS id, and the same
/// category the FFTW backend uses: only one backend is live at a time, and
/// "fft" is the word an operator asking what the transform did will grep for.
///
/// Assigned once during activation, from the plugin's `Host`. Before that it
/// discards, which is what a message emitted before the host exists should do.
[[nodiscard]] plugin::Logger& log() noexcept;

/// Accelerate's vDSP, single precision. One setup shared across N workers.
///
/// vDSP.h states that its FFT and DFT setup structures need only read-only
/// access to their underlying memory and that execution routines may therefore
/// run concurrently on shared setups. That is what makes one setup per size
/// legitimate here, exactly as `fftwf_execute_dft` does for FFTW. Only
/// *create* and *destroy* concurrent with an execute is undefined, and this
/// backend passes `Previous = nullptr` everywhere so no two setups share
/// memory at all.
///
/// An ordinary `IFftBackend`: the plugin boundary is a vtable built over this
/// class, not something the class has to know about.
class AccelerateBackend final : public IFftBackend {
public:
    AccelerateBackend();

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] std::string displayName() const override;
    [[nodiscard]] const FftCapabilities& capabilities() const noexcept override;

    [[nodiscard]] Result<std::unique_ptr<IFftPlan>>
    createPlan(const FftPlanConfig& config) override;

    // `reset()` is deliberately not overridden. vDSP keeps no process-wide
    // wisdom to discard -- there is no counterpart to `fftwf_cleanup()`, and
    // everything a setup owns dies with the setup. The base's empty
    // implementation is the correct one.
    //
    // `snapSize` and `supportsSize` are not overridden either, and that is
    // load-bearing rather than lazy. vDSP accepts f * 2^n for f in
    // {2,3,5,9,15,25}, which `FftSizeConstraint` cannot express; the host's
    // `plugin_facets::FftBackend` forwards `snapSize` across the ABI but
    // computes `supportsSize` from the declared capabilities alone. A backend
    // that declared `Any` and snapped to 3*2^n would hand the pipeline a size
    // its own `supportsSize` then rejects. Declaring `PowerOfTwo` makes the
    // inherited `bit_ceil` snap exactly right, at the cost of coarser
    // achievable RBW.

private:
    FftCapabilities m_capabilities;
};

} // namespace sweeppp::accel
