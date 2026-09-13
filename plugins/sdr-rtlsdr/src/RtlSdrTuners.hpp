// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What librtlsdr's tuner type means for the radio in front of it.
//
// Header-only so the table is testable without a dongle: librtlsdr reports
// which tuner it found and nothing else, so the name the device chip shows and
// the range the panel offers both come from here.

#include <rtl-sdr.h>
#include <string_view>

namespace sweeppp::rtl {

struct TunerSpec {
    std::string_view name;
    double minHz = 0.0;
    double maxHz = 0.0;
    bool directSampling = false; ///< Offers HF through the RTL2832U's own ADC
};

/// Where direct sampling reaches. librtlsdr tunes the demodulator's DDC from
/// zero to the 28.8 MHz ADC clock; the floor is lifted off zero so a centre
/// there does not put half of every span below 0 Hz.
inline constexpr double kDirectSamplingMinHz = 500e3;
inline constexpr double kDirectSamplingMaxHz = 28.8e6;

[[nodiscard]] constexpr TunerSpec tunerSpec(rtlsdr_tuner tuner) noexcept {
    switch (tuner) {
    case RTLSDR_TUNER_E4000:
        // With a hole around 1.1 - 1.25 GHz that the PLL cannot reach. Named
        // as one range because the panel has no way to express a gap, and a
        // retune into it fails with the library's own error.
        return {.name = "E4000", .minHz = 52e6, .maxHz = 2200e6, .directSampling = true};
    case RTLSDR_TUNER_FC0012:
        return {.name = "FC0012", .minHz = 22e6, .maxHz = 948.6e6, .directSampling = true};
    case RTLSDR_TUNER_FC0013:
        return {.name = "FC0013", .minHz = 22e6, .maxHz = 1100e6, .directSampling = true};
    case RTLSDR_TUNER_FC2580:
        // Two bands, 146 - 308 and 438 - 924 MHz, for the same reason as the
        // E4000 above.
        return {.name = "FC2580", .minHz = 146e6, .maxHz = 924e6, .directSampling = true};
    case RTLSDR_TUNER_R820T:
        // The R820T2 reports as this too, and covers the same range.
        return {.name = "R820T", .minHz = 24e6, .maxHz = 1766e6, .directSampling = true};
    case RTLSDR_TUNER_R828D:
        // Boards built around the R828D cover HF through the tuner path
        // instead, so direct sampling is not offered on them.
        return {.name = "R828D", .minHz = 24e6, .maxHz = 1766e6, .directSampling = false};
    case RTLSDR_TUNER_UNKNOWN:
        break;
    }
    // The R820T's range: by far the commonest tuner, so the likeliest guess
    // for one librtlsdr could not identify, and a retune outside what the
    // hardware really does still fails with the library's own error.
    return {.name = "unknown tuner", .minHz = 24e6, .maxHz = 1766e6, .directSampling = true};
}

} // namespace sweeppp::rtl
