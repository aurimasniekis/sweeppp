// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The tuner table, without a dongle.
//
// It decides the range the panel offers and whether an HF port exists at all,
// and there are six tuners behind one USB id -- no single dongle on a desk
// exercises more than one row of it.
#include "RtlSdrTuners.hpp"

#include <array>
#include <doctest/doctest.h>
#include <string_view>

using namespace sweeppp::rtl;

TEST_CASE("each known tuner is named and has a usable range") {
    constexpr std::array kKnown{RTLSDR_TUNER_E4000,  RTLSDR_TUNER_FC0012, RTLSDR_TUNER_FC0013,
                                RTLSDR_TUNER_FC2580, RTLSDR_TUNER_R820T,  RTLSDR_TUNER_R828D};

    for (const rtlsdr_tuner tuner : kKnown) {
        const TunerSpec spec = tunerSpec(tuner);
        CHECK_FALSE(spec.name.empty());
        CHECK(spec.name != "unknown tuner");
        CHECK(spec.minHz > 0.0);
        CHECK(spec.maxHz > spec.minHz);
        // Direct sampling exists to reach below the tuners, so every one of
        // them must start above where it starts.
        CHECK(spec.minHz > kDirectSamplingMinHz);
    }
}

TEST_CASE("the ranges are the ones the tuners actually cover") {
    CHECK(tunerSpec(RTLSDR_TUNER_R820T).minHz == doctest::Approx(24e6));
    CHECK(tunerSpec(RTLSDR_TUNER_R820T).maxHz == doctest::Approx(1766e6));
    CHECK(tunerSpec(RTLSDR_TUNER_R828D).maxHz == doctest::Approx(1766e6));
    CHECK(tunerSpec(RTLSDR_TUNER_E4000).minHz == doctest::Approx(52e6));
    CHECK(tunerSpec(RTLSDR_TUNER_E4000).maxHz == doctest::Approx(2200e6));
    CHECK(tunerSpec(RTLSDR_TUNER_FC0012).maxHz == doctest::Approx(948.6e6));
    CHECK(tunerSpec(RTLSDR_TUNER_FC0013).maxHz == doctest::Approx(1100e6));
    CHECK(tunerSpec(RTLSDR_TUNER_FC2580).minHz == doctest::Approx(146e6));
}

TEST_CASE("direct sampling is offered everywhere but the R828D") {
    CHECK(tunerSpec(RTLSDR_TUNER_R820T).directSampling);
    CHECK(tunerSpec(RTLSDR_TUNER_E4000).directSampling);
    CHECK_FALSE(tunerSpec(RTLSDR_TUNER_R828D).directSampling);
}

TEST_CASE("a tuner librtlsdr could not identify still gets a range") {
    // A zero range would leave the panel with nothing to tune.
    const TunerSpec spec = tunerSpec(RTLSDR_TUNER_UNKNOWN);
    CHECK(spec.name == "unknown tuner");
    CHECK(spec.maxHz > spec.minHz);
}

TEST_CASE("direct sampling stops at the ADC clock") {
    CHECK(kDirectSamplingMinHz > 0.0);
    CHECK(kDirectSamplingMaxHz == doctest::Approx(28.8e6));
}
