// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <sweeppp/plugin/PluginFft.hpp>

namespace sweeppp::fftw {

/// This backend's log category.
///
/// Deliberately "fft" rather than the plugin's reverse-DNS id: the log is read
/// by an operator asking what the transform did, and "fft" is the word they
/// will grep for.
///
/// Assigned once during activation, from the plugin's `Host`. Before that it
/// discards, which is what a message emitted before the host exists should do.
[[nodiscard]] plugin::Logger& log() noexcept;

/// FFTW, single precision (fftwf). One plan shared across N workers.
///
/// An ordinary `IFftBackend`: the plugin boundary is a vtable built over this
/// class, not something the class has to know about.
class FftwBackend final : public IFftBackend {
public:
    FftwBackend();

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] std::string displayName() const override;
    [[nodiscard]] const FftCapabilities& capabilities() const noexcept override;

    [[nodiscard]] Result<std::unique_ptr<IFftPlan>>
    createPlan(const FftPlanConfig& config) override;

    void reset() override;

private:
    FftCapabilities m_capabilities;
};

} // namespace sweeppp::fftw
