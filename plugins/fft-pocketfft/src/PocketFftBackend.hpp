// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <sweeppp/plugin/PluginFft.hpp>

namespace sweeppp::pocket {

/// This backend's log category.
///
/// "fft", the category the FFTW and Accelerate backends use: only one backend
/// is live at a time, and "fft" is the word an operator will grep for.
///
/// Assigned once during activation, from the plugin's `Host`. Before that it
/// discards, which is what a message emitted before the host exists should do.
[[nodiscard]] plugin::Logger& log() noexcept;

/// PocketFFT, single precision. One plan shared across N workers.
///
/// A plan is FFTPACK's factorisation and twiddle tables, or a Bluestein setup
/// for lengths with a large prime factor, and `exec` only reads it. Scratch is
/// allocated inside each call, so concurrent executes share nothing mutable --
/// the same promise `fftwf_execute_dft` makes.
class PocketFftBackend final : public IFftBackend {
public:
    PocketFftBackend();

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] std::string displayName() const override;
    [[nodiscard]] const FftCapabilities& capabilities() const noexcept override;

    [[nodiscard]] Result<std::unique_ptr<IFftPlan>>
    createPlan(const FftPlanConfig& config) override;

    // `snapSize` and `supportsSize` are not overridden: every length in range
    // is accepted, which is what the base implementations assume for `Any`.
    // `reset()` neither -- with POCKETFFT_CACHE_SIZE at 0 there is no plan
    // cache to discard.

private:
    FftCapabilities m_capabilities;
};

} // namespace sweeppp::pocket
