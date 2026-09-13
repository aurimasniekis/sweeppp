// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/fft/Window.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace sweeppp {
namespace {

constexpr double kPi = std::numbers::pi;

/// Zeroth-order modified Bessel function of the first kind, by series.
///
/// Kaiser needs it and there is no portable std:: equivalent
/// (std::cyl_bessel_i is <cmath> special-functions, absent from libc++).
/// The series converges quickly for the beta range in use (0..20).
double besselI0(double x) {
    double sum = 1.0;
    double term = 1.0;
    const double halfXSquared = (x * x) / 4.0;

    for (int k = 1; k < 64; ++k) {
        term *= halfXSquared / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-17) {
            break;
        }
    }
    return sum;
}

/// Generic cosine-sum window: w[n] = sum_i (-1)^i * a[i] * cos(2*pi*i*n/(N-1)).
///
/// Hann, Hamming, Blackman-Harris and flat-top are all this with different
/// coefficients, so they share one generator and cannot disagree about
/// endpoint handling.
void fillCosineSum(std::span<float> out, std::span<const double> a) {
    const std::size_t size = out.size();
    if (size == 1) {
        out[0] = 1.0F;
        return;
    }

    // Symmetric (denominator N-1) rather than periodic (N). Symmetric is the
    // correct choice for spectral *analysis*; periodic is for overlap-add
    // synthesis, which this project does not do.
    const double denominator = static_cast<double>(size - 1);

    for (std::size_t n = 0; n < size; ++n) {
        const double phase = 2.0 * kPi * static_cast<double>(n) / denominator;
        double value = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const double sign = (i % 2 == 0) ? 1.0 : -1.0;
            value += sign * a[i] * std::cos(static_cast<double>(i) * phase);
        }
        out[n] = static_cast<float>(value);
    }
}

/// Measures coherent gain and ENBW from the coefficients themselves.
///
///   CG   = (1/N) * sum(w)
///   ENBW = N * sum(w^2) / (sum(w))^2      [in bins]
///
/// Deriving these rather than tabulating them means a new window type gets
/// correct dBm scaling and a correct RBW readout for free, and cannot ship
/// with a transcription error in a constant.
void measure(std::span<const float> coefficients, WindowProperties& out) {
    double sum = 0.0;
    double sumSquares = 0.0;
    for (const float coefficient : coefficients) {
        const auto value = static_cast<double>(coefficient);
        sum += value;
        sumSquares += value * value;
    }

    const auto n = static_cast<double>(coefficients.size());
    if (n == 0.0 || sum == 0.0) {
        out.coherentGain = 1.0;
        out.enbw = 1.0;
        return;
    }

    out.coherentGain = sum / n;
    out.enbw = n * sumSquares / (sum * sum);
}

// Coefficient sets. Sources noted because getting one digit wrong here shifts
// every dBm reading and nothing else would flag it.
constexpr std::array<double, 2> kHann{0.5, 0.5};
constexpr std::array<double, 2> kHamming{0.54, 0.46};
// Harris (1978), 4-term minimum-sidelobe Blackman-Harris, -92 dB.
constexpr std::array<double, 4> kBlackmanHarris{0.35875, 0.48829, 0.14128, 0.01168};
// 5-term flat-top as used by SRS/HP analysers; amplitude-flat to ~0.01 dB.
constexpr std::array<double, 5> kFlatTop{0.21557895, 0.41663158, 0.277263158, 0.083578947,
                                         0.006947368};

constexpr std::array kAllWindowTypes{
    WindowType::Rectangular,    WindowType::Hann,    WindowType::Hamming,
    WindowType::BlackmanHarris, WindowType::FlatTop, WindowType::Kaiser,
};

} // namespace

std::string_view displayName(WindowType type) noexcept {
    switch (type) {
    case WindowType::Rectangular:
        return "Rectangular";
    case WindowType::Hann:
        return "Hann";
    case WindowType::Hamming:
        return "Hamming";
    case WindowType::BlackmanHarris:
        return "Blackman-Harris";
    case WindowType::FlatTop:
        return "Flat-top";
    case WindowType::Kaiser:
        return "Kaiser";
    }
    return "Hann";
}

std::string_view description(WindowType type) noexcept {
    switch (type) {
    case WindowType::Rectangular:
        return "No windowing. Sharpest resolution, worst leakage -- only for "
               "signals that are exactly periodic in the window.";
    case WindowType::Hann:
        return "General purpose. Good leakage suppression at modest resolution "
               "cost. The sensible default.";
    case WindowType::Hamming:
        return "Lower first sidelobe than Hann, but the far sidelobes fall off "
               "more slowly.";
    case WindowType::BlackmanHarris:
        return "Very low sidelobes (-92 dB). Use when a weak signal sits close "
               "to a strong one. Costs resolution.";
    case WindowType::FlatTop:
        return "Amplitude-accurate to ~0.01 dB regardless of where a tone falls "
               "between bins. Use for level measurement, not for resolution.";
    case WindowType::Kaiser:
        return "Adjustable resolution/leakage trade-off via beta. Higher beta "
               "means lower sidelobes and a wider main lobe.";
    }
    return "";
}

std::span<const WindowType> allWindowTypes() noexcept {
    return kAllWindowTypes;
}

WindowProperties nominalProperties(WindowType type, double kaiserBeta) noexcept {
    switch (type) {
    case WindowType::Rectangular:
        return {.coherentGain = 1.0, .enbw = 1.0, .scallopLossDb = 3.92, .sidelobeDb = -13.3};
    case WindowType::Hann:
        return {.coherentGain = 0.5, .enbw = 1.5, .scallopLossDb = 1.42, .sidelobeDb = -31.5};
    case WindowType::Hamming:
        return {.coherentGain = 0.54, .enbw = 1.3628, .scallopLossDb = 1.75, .sidelobeDb = -42.7};
    case WindowType::BlackmanHarris:
        return {
            .coherentGain = 0.35875, .enbw = 2.0044, .scallopLossDb = 0.83, .sidelobeDb = -92.0};
    case WindowType::FlatTop:
        return {
            .coherentGain = 0.21557895, .enbw = 3.77, .scallopLossDb = 0.01, .sidelobeDb = -93.0};
    case WindowType::Kaiser: {
        // Kaiser's properties are a function of beta. These approximations
        // track the standard curves closely enough for a tooltip; the values
        // actually used in measurement are measured from the coefficients.
        const double alpha = kaiserBeta / kPi;
        return {.coherentGain = 0.0,
                .enbw = 1.0 + 0.5 * alpha,
                .scallopLossDb = 1.0,
                .sidelobeDb = -(7.95 + 7.18 * kaiserBeta) / 1.0};
    }
    }
    return {};
}

Result<Window> Window::create(WindowType type, std::size_t size, double beta) {
    if (size == 0) {
        return fail<Window>(ErrorCode::InvalidArgument, "window size must be non-zero");
    }
    if (type == WindowType::Kaiser && (beta < 0.0 || beta > 40.0)) {
        return fail<Window>(ErrorCode::InvalidArgument,
                            "Kaiser beta {} is outside the usable range 0..40", beta);
    }

    Window window;
    window.m_type = type;
    window.m_beta = beta;
    window.m_coefficients.resize(size);

    switch (type) {
    case WindowType::Rectangular:
        std::ranges::fill(window.m_coefficients, 1.0F);
        break;
    case WindowType::Hann:
        fillCosineSum(window.m_coefficients, kHann);
        break;
    case WindowType::Hamming:
        fillCosineSum(window.m_coefficients, kHamming);
        break;
    case WindowType::BlackmanHarris:
        fillCosineSum(window.m_coefficients, kBlackmanHarris);
        break;
    case WindowType::FlatTop:
        fillCosineSum(window.m_coefficients, kFlatTop);
        break;
    case WindowType::Kaiser: {
        const double denominator = size > 1 ? static_cast<double>(size - 1) : 1.0;
        const double i0Beta = besselI0(beta);
        for (std::size_t n = 0; n < size; ++n) {
            const double ratio = (2.0 * static_cast<double>(n) / denominator) - 1.0;
            const double argument = beta * std::sqrt(std::max(0.0, 1.0 - ratio * ratio));
            window.m_coefficients[n] = static_cast<float>(besselI0(argument) / i0Beta);
        }
        break;
    }
    }

    measure(window.m_coefficients, window.m_properties);

    const WindowProperties nominal = nominalProperties(type, beta);
    window.m_properties.scallopLossDb = nominal.scallopLossDb;
    window.m_properties.sidelobeDb = nominal.sidelobeDb;

    const double scale = static_cast<double>(size) * window.m_properties.coherentGain;
    window.m_amplitudeScale = scale > 0.0 ? static_cast<float>(1.0 / scale) : 1.0F;

    return window;
}

} // namespace sweeppp
