// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>
#include <doctest/doctest.h>
#include <numbers>
#include <sweeppp/fft/Window.hpp>

using namespace sweeppp;

namespace {

/// Coherent gain and ENBW are measured from the generated coefficients rather
/// than tabulated, so these checks are what tie the generator to the published
/// figures. A window whose measured properties drift from nominal produces
/// silently wrong dBm readings and a wrong RBW -- nothing else would catch it.
constexpr double kPropertyTolerance = 0.01;

} // namespace

TEST_CASE("window coherent gain matches published values") {
    struct Expectation {
        WindowType type;
        double coherentGain;
    };

    const Expectation expectations[] = {
        {WindowType::Rectangular, 1.0},    {WindowType::Hann, 0.5},
        {WindowType::Hamming, 0.54},       {WindowType::BlackmanHarris, 0.35875},
        {WindowType::FlatTop, 0.21557895},
    };

    for (const Expectation& expectation : expectations) {
        CAPTURE(toString(expectation.type));
        const auto window = Window::create(expectation.type, 8192);
        REQUIRE(window.has_value());
        CHECK(window->properties().coherentGain ==
              doctest::Approx(expectation.coherentGain).epsilon(kPropertyTolerance));
    }
}

TEST_CASE("window ENBW matches published values") {
    struct Expectation {
        WindowType type;
        double enbw;
    };

    const Expectation expectations[] = {
        {WindowType::Rectangular, 1.0}, {WindowType::Hann, 1.5},
        {WindowType::Hamming, 1.3628},  {WindowType::BlackmanHarris, 2.0044},
        {WindowType::FlatTop, 3.77},
    };

    for (const Expectation& expectation : expectations) {
        CAPTURE(toString(expectation.type));
        const auto window = Window::create(expectation.type, 8192);
        REQUIRE(window.has_value());
        CHECK(window->properties().enbw ==
              doctest::Approx(expectation.enbw).epsilon(kPropertyTolerance));
    }
}

TEST_CASE("resolution bandwidth accounts for ENBW") {
    const auto hann = Window::create(WindowType::Hann, 4096);
    REQUIRE(hann.has_value());

    constexpr double kSampleRate = 20e6;
    const double binSpacing = kSampleRate / 4096.0;

    // The naive sampleRate/N figure understates RBW by exactly the ENBW
    // factor. Reporting bin spacing as RBW is a classic spectrum-analyser bug.
    CHECK(hann->resolutionBandwidth(kSampleRate) ==
          doctest::Approx(binSpacing * 1.5).epsilon(kPropertyTolerance));
    CHECK(hann->resolutionBandwidth(kSampleRate) > binSpacing);
}

TEST_CASE("window coefficients are symmetric and bounded") {
    for (const WindowType type : allWindowTypes()) {
        CAPTURE(toString(type));
        const auto window = Window::create(type, 1024);
        REQUIRE(window.has_value());

        const std::span<const float> coefficients = window->coefficients();
        REQUIRE(coefficients.size() == 1024);

        for (std::size_t i = 0; i < coefficients.size() / 2; ++i) {
            CHECK(coefficients[i] ==
                  doctest::Approx(coefficients[coefficients.size() - 1 - i]).epsilon(1e-5));
        }

        // Flat-top is the one window here with negative lobes -- that is what
        // buys its 0.01 dB amplitude flatness, not a generator artefact. Its
        // true minimum for these coefficients is -0.07056; anything meaningfully
        // below that means the coefficients have been mistyped. Every other
        // window is non-negative, and none may exceed unity.
        const float lowerBound = type == WindowType::FlatTop ? -0.0706F : -1e-5F;
        for (const float value : coefficients) {
            CHECK(value >= lowerBound);
            CHECK(value <= 1.0F + 1e-5F);
        }
    }
}

TEST_CASE("kaiser beta changes the resolution/leakage trade-off") {
    const auto narrow = Window::create(WindowType::Kaiser, 4096, 2.0);
    const auto wide = Window::create(WindowType::Kaiser, 4096, 14.0);
    REQUIRE(narrow.has_value());
    REQUIRE(wide.has_value());

    // Higher beta means a wider main lobe, so a larger ENBW.
    CHECK(wide->properties().enbw > narrow->properties().enbw);
}

TEST_CASE("window name round-trips through TOML spelling") {
    for (const WindowType type : allWindowTypes()) {
        const auto parsed = windowTypeFromString(toString(type));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == type);
    }

    CHECK(windowTypeFromString("hanning").value() == WindowType::Hann);
    CHECK(windowTypeFromString("FLATTOP").value() == WindowType::FlatTop);
    CHECK_FALSE(windowTypeFromString("gaussian").has_value());
}

TEST_CASE("amplitude scale recovers a known tone amplitude") {
    constexpr std::size_t kSize = 4096;
    const auto window = Window::create(WindowType::Hann, kSize);
    REQUIRE(window.has_value());

    // A windowed DC tone of amplitude A sums to A * N * coherentGain, so
    // multiplying by amplitudeScale must return exactly A.
    constexpr double kAmplitude = 0.25;
    double sum = 0.0;
    for (const float coefficient : window->coefficients()) {
        sum += static_cast<double>(coefficient) * kAmplitude;
    }

    CHECK(sum * static_cast<double>(window->amplitudeScale()) ==
          doctest::Approx(kAmplitude).epsilon(1e-5));
}

TEST_CASE("zero-size window is rejected") {
    CHECK_FALSE(Window::create(WindowType::Hann, 0).has_value());
    CHECK_FALSE(Window::create(WindowType::Kaiser, 1024, -1.0).has_value());
}
