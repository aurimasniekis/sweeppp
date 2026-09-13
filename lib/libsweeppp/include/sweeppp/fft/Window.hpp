// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <sweeps/WindowType.hpp>
#include <vector>

namespace sweeppp {

/// The enumeration belongs to the session format -- its numeric values are
/// written into every SegmentOpen record -- so libsweepsfile owns it and this
/// is a re-export.
using WindowType = sweeps::WindowType;

// `toString(WindowType)` is deliberately NOT declared here.
//
// It lives in namespace sweeps, and argument-dependent lookup finds it from
// inside namespace sweeppp because its argument is a sweeps type. Declaring a
// second one here would not shadow it -- unqualified lookup would find this
// one and ADL would add that one, making all dozen-odd unqualified `toString`
// call sites ambiguous. Deleting it is what keeps them compiling unchanged.

[[nodiscard]] std::string_view displayName(WindowType type) noexcept;
[[nodiscard]] std::span<const WindowType> allWindowTypes() noexcept;

/// Parses a window name. Forwards to the library and adopts its result, so the
/// accepted spellings cannot drift from the ones a `.sweeps` reader accepts.
[[nodiscard]] inline Result<WindowType> windowTypeFromString(std::string_view name) {
    return adopt(sweeps::windowTypeFromString(name));
}

/// One-line explanation of what each window trades away, for the tooltip in
/// the FFT panel. Choosing a window is a real measurement decision and the UI
/// should say so rather than presenting six equivalent-looking names.
[[nodiscard]] std::string_view description(WindowType type) noexcept;

/// A window's measurement properties.
///
/// These are not decoration: without coherent gain the dBm readout of a tone
/// is wrong by up to 13 dB, and without ENBW the displayed RBW is wrong by up
/// to a factor of 3.8. Any window added later must carry both.
struct WindowProperties {
    /// Mean of the window coefficients. Divide a coherent tone's magnitude by
    /// this to recover its true amplitude.
    double coherentGain = 1.0;

    /// Equivalent Noise Bandwidth, in bins. The actual resolution bandwidth is
    /// `sampleRate * enbw / fftSize`, not `sampleRate / fftSize`.
    double enbw = 1.0;

    /// Worst-case amplitude error, in dB, for a tone falling between bins.
    /// The reason flat-top exists.
    double scallopLossDb = 0.0;

    /// Highest sidelobe relative to the main lobe, in dB. How well a weak
    /// signal survives next to a strong one.
    double sidelobeDb = 0.0;
};

/// A generated window, with the properties measured from the coefficients it
/// actually produced rather than from a table.
///
/// Computing them from the samples means the numbers can never drift out of
/// step with the generator, including for Kaiser where they depend on beta.
class Window {
public:
    Window() = default;

    /// `beta` applies to Kaiser only and is ignored otherwise.
    [[nodiscard]] static Result<Window> create(WindowType type, std::size_t size,
                                               double beta = 8.6);

    [[nodiscard]] WindowType type() const noexcept { return m_type; }
    [[nodiscard]] std::size_t size() const noexcept { return m_coefficients.size(); }
    [[nodiscard]] double beta() const noexcept { return m_beta; }
    [[nodiscard]] std::span<const float> coefficients() const noexcept { return m_coefficients; }
    [[nodiscard]] const WindowProperties& properties() const noexcept { return m_properties; }

    /// Reciprocal of `size * coherentGain`. Precomputed because the FFT worker
    /// multiplies by it once per bin, per frame.
    [[nodiscard]] float amplitudeScale() const noexcept { return m_amplitudeScale; }

    /// Resolution bandwidth in Hz for this window at a given sample rate.
    [[nodiscard]] double resolutionBandwidth(double sampleRate) const noexcept {
        return size() == 0 ? 0.0 : sampleRate * m_properties.enbw / static_cast<double>(size());
    }

    [[nodiscard]] bool empty() const noexcept { return m_coefficients.empty(); }

private:
    WindowType m_type = WindowType::Hann;
    double m_beta = 8.6;
    std::vector<float> m_coefficients;
    WindowProperties m_properties;
    float m_amplitudeScale = 1.0F;
};

/// Textbook properties for a window, independent of length.
///
/// Used for the tooltip and for the unit tests that check the generator
/// against published figures -- a generated window whose measured ENBW drifts
/// from this is a bug in the generator.
[[nodiscard]] WindowProperties nominalProperties(WindowType type, double kaiserBeta = 8.6) noexcept;

} // namespace sweeppp
