// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <doctest/doctest.h>
#include <string>
#include <sweeppp/backends/sdr/SyntheticDevice.hpp>
#include <sweeppp/dsp/Convert.hpp>
#include <sweeppp/sdr/ISdrDevice.hpp>
#include <sweeppp/sdr/SampleFormat.hpp>
#include <utility>
#include <vector>

using namespace sweeppp;

namespace {

/* ---------------------------------------------------------------------------
 * A reference codec, hand-written from each format's definition.
 *
 * Deliberately NOT sharing anything with dsp::Convert. A test that encoded its
 * fixtures through the code under test would agree with it however wrong both
 * were; the whole value here is that these bytes were laid out by reading the
 * format's description rather than by calling the thing being checked.
 * ------------------------------------------------------------------------- */

/// binary16 both ways, in arithmetic, on every compiler. The converter uses the
/// hardware type where there is one, so this is a genuinely separate opinion
/// about what the bits mean.
[[nodiscard]] float referenceHalfToFloat(std::uint16_t bits) {
    const std::uint32_t sign = (bits & 0x8000U) << 16U;
    const std::uint32_t exponent = (bits >> 10U) & 0x1FU;
    const std::uint32_t mantissa = bits & 0x3FFU;

    if (exponent == 0U) {
        if (mantissa == 0U) {
            return std::bit_cast<float>(sign);
        }
        // Subnormal: value is mantissa * 2^-24, built directly rather than
        // renormalised, which is a different route to the same number.
        const float magnitude = static_cast<float>(mantissa) * 0x1p-24F;
        return (sign != 0U) ? -magnitude : magnitude;
    }
    if (exponent == 0x1FU) {
        return std::bit_cast<float>(sign | 0x7F800000U | (mantissa << 13U));
    }
    return std::bit_cast<float>(sign | ((exponent + 112U) << 23U) | (mantissa << 13U));
}

[[nodiscard]] std::uint16_t referenceFloatToHalf(float value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const auto sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
    const auto exponent = static_cast<std::int32_t>((bits >> 23U) & 0xFFU) - 127;
    const std::uint32_t mantissa = bits & 0x7FFFFFU;

    if (exponent < -14) {
        return sign; // every value this test encodes is normal or zero
    }
    if (exponent > 15) {
        return static_cast<std::uint16_t>(sign | 0x7C00U);
    }
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent + 15) << 10U) |
                                      (mantissa >> 13U));
}

/// The raw code a component holds, as an integer for the fixed-point formats
/// and as the value itself for the floating ones.
struct Codes {
    double mostNegative;
    double zero;
    double mostPositive;
};

[[nodiscard]] Codes codesFor(SampleFormat format) {
    switch (format) {
    case SampleFormat::Cu8:
        return {0.0, 128.0, 255.0};
    case SampleFormat::Cs8:
        return {-128.0, 0.0, 127.0};
    case SampleFormat::Cu12:
        return {0.0, 2048.0, 4095.0};
    case SampleFormat::Cs12:
        return {-2048.0, 0.0, 2047.0};
    case SampleFormat::Cu16:
        return {0.0, 32768.0, 65535.0};
    case SampleFormat::Cs16:
        return {-32768.0, 0.0, 32767.0};
    case SampleFormat::Cu32:
        return {0.0, 2147483648.0, 4294967295.0};
    case SampleFormat::Cs32:
        return {-2147483648.0, 0.0, 2147483647.0};
    case SampleFormat::Cf16:
    case SampleFormat::Cf32:
    case SampleFormat::Cf64:
        return {-1.0, 0.0, 1.0};
    }
    return {0.0, 0.0, 0.0};
}

/// Places one component's raw code into a buffer, spelled out per format.
///
/// The 12-bit case writes the I component before the Q of the same frame,
/// because they share a byte; every caller here fills components in order.
void putCode(std::byte* buffer, SampleFormat format, std::size_t component, double code) {
    const auto integer = static_cast<std::int64_t>(code);
    auto* bytes = reinterpret_cast<std::uint8_t*>(buffer);

    switch (format) {
    case SampleFormat::Cu8:
        bytes[component] = static_cast<std::uint8_t>(integer);
        return;
    case SampleFormat::Cs8: {
        const auto value = static_cast<std::int8_t>(integer);
        std::memcpy(bytes + component, &value, 1);
        return;
    }
    case SampleFormat::Cu12:
    case SampleFormat::Cs12: {
        const auto packed = static_cast<std::uint32_t>(integer) & 0xFFFU;
        std::uint8_t* frame = bytes + (component / 2) * 3;
        if (component % 2 == 0) {
            frame[0] = static_cast<std::uint8_t>(packed & 0xFFU);
            frame[1] = static_cast<std::uint8_t>(packed >> 8U);
        } else {
            frame[1] = static_cast<std::uint8_t>(frame[1] | ((packed & 0x0FU) << 4U));
            frame[2] = static_cast<std::uint8_t>(packed >> 4U);
        }
        return;
    }
    case SampleFormat::Cu16: {
        const auto value = static_cast<std::uint16_t>(integer);
        std::memcpy(bytes + component * 2, &value, 2);
        return;
    }
    case SampleFormat::Cs16: {
        const auto value = static_cast<std::int16_t>(integer);
        std::memcpy(bytes + component * 2, &value, 2);
        return;
    }
    case SampleFormat::Cu32: {
        const auto value = static_cast<std::uint32_t>(integer);
        std::memcpy(bytes + component * 4, &value, 4);
        return;
    }
    case SampleFormat::Cs32: {
        const auto value = static_cast<std::int32_t>(integer);
        std::memcpy(bytes + component * 4, &value, 4);
        return;
    }
    case SampleFormat::Cf16: {
        const std::uint16_t value = referenceFloatToHalf(static_cast<float>(code));
        std::memcpy(bytes + component * 2, &value, 2);
        return;
    }
    case SampleFormat::Cf32: {
        const auto value = static_cast<float>(code);
        std::memcpy(bytes + component * 4, &value, 4);
        return;
    }
    case SampleFormat::Cf64: {
        std::memcpy(bytes + component * 8, &code, 8);
        return;
    }
    }
}

/// Reads one back, likewise by hand, and centres it: `raw - zeroOffset`, still
/// at the format's own scale.
[[nodiscard]] double getCentred(const std::byte* buffer, SampleFormat format,
                                std::size_t component) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(buffer);
    double raw = 0.0;

    switch (format) {
    case SampleFormat::Cu8:
        raw = static_cast<double>(bytes[component]);
        break;
    case SampleFormat::Cs8: {
        std::int8_t value = 0;
        std::memcpy(&value, bytes + component, 1);
        raw = static_cast<double>(value);
        break;
    }
    case SampleFormat::Cu12:
    case SampleFormat::Cs12: {
        const std::uint8_t* frame = bytes + (component / 2) * 3;
        std::uint32_t packed = 0;
        if (component % 2 == 0) {
            packed = frame[0] | ((static_cast<std::uint32_t>(frame[1]) & 0x0FU) << 8U);
        } else {
            packed = (static_cast<std::uint32_t>(frame[1]) >> 4U) |
                     (static_cast<std::uint32_t>(frame[2]) << 4U);
        }
        if (format == SampleFormat::Cs12 && (packed & 0x800U) != 0U) {
            raw = static_cast<double>(packed) - 4096.0;
        } else {
            raw = static_cast<double>(packed);
        }
        break;
    }
    case SampleFormat::Cu16: {
        std::uint16_t value = 0;
        std::memcpy(&value, bytes + component * 2, 2);
        raw = static_cast<double>(value);
        break;
    }
    case SampleFormat::Cs16: {
        std::int16_t value = 0;
        std::memcpy(&value, bytes + component * 2, 2);
        raw = static_cast<double>(value);
        break;
    }
    case SampleFormat::Cu32: {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes + component * 4, 4);
        raw = static_cast<double>(value);
        break;
    }
    case SampleFormat::Cs32: {
        std::int32_t value = 0;
        std::memcpy(&value, bytes + component * 4, 4);
        raw = static_cast<double>(value);
        break;
    }
    case SampleFormat::Cf16: {
        std::uint16_t value = 0;
        std::memcpy(&value, bytes + component * 2, 2);
        raw = static_cast<double>(referenceHalfToFloat(value));
        break;
    }
    case SampleFormat::Cf32: {
        float value = 0.0F;
        std::memcpy(&value, bytes + component * 4, 4);
        raw = static_cast<double>(value);
        break;
    }
    case SampleFormat::Cf64:
        std::memcpy(&raw, bytes + component * 8, 8);
        break;
    }

    // Exactly the definition in SampleFormat.hpp, restated rather than called.
    return raw - static_cast<double>(zeroOffset(format));
}

[[nodiscard]] double getNormalised(const std::byte* buffer, SampleFormat format,
                                   std::size_t component) {
    return getCentred(buffer, format, component) / static_cast<double>(fullScale(format));
}

/// A block whose components sweep the format's whole range, so the conversion
/// is checked somewhere other than at its three landmark codes.
[[nodiscard]] std::vector<std::byte> rampBlock(SampleFormat format, std::size_t frames) {
    const Codes codes = codesFor(format);
    std::vector<std::byte> buffer(frames * bytesPerFrame(format), std::byte{0});

    const std::size_t components = frames * 2;
    for (std::size_t i = 0; i < components; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(components - 1);
        // Landmark codes at both ends, and a straight line between them, so
        // clipping is exercised at each extreme rather than only at one.
        const double code = codes.mostNegative + t * (codes.mostPositive - codes.mostNegative);
        putCode(buffer.data(), format, i,
                (format == SampleFormat::Cf16 || format == SampleFormat::Cf32 ||
                 format == SampleFormat::Cf64)
                    ? code
                    : std::trunc(code));
    }
    return buffer;
}

constexpr double kTolerance = 1e-6;

} // namespace

TEST_CASE("every sample format is described consistently") {
    for (const SampleFormat format : allSampleFormats()) {
        CAPTURE(std::string(toString(format)));

        CHECK(bytesPerFrame(format) > 0);
        CHECK(fullScale(format) > 0.0F);
        CHECK(zeroOffset(format) >= 0.0F);
        CHECK_FALSE(displayName(format).empty());

        // An unsigned format is centred on half its span; every other format
        // is centred on zero. Nothing in between is meaningful.
        const bool unsignedFormat = zeroOffset(format) != 0.0F;
        if (unsignedFormat) {
            CHECK(zeroOffset(format) == fullScale(format));
        }
    }
}

TEST_CASE("format sizes match the raw types they name") {
    CHECK(bytesPerFrame(SampleFormat::Cu8) == 2 * sizeof(std::uint8_t));
    CHECK(bytesPerFrame(SampleFormat::Cs8) == 2 * sizeof(std::int8_t));
    CHECK(bytesPerFrame(SampleFormat::Cu16) == 2 * sizeof(std::uint16_t));
    CHECK(bytesPerFrame(SampleFormat::Cs16) == 2 * sizeof(std::int16_t));
    CHECK(bytesPerFrame(SampleFormat::Cu32) == 2 * sizeof(std::uint32_t));
    CHECK(bytesPerFrame(SampleFormat::Cs32) == 2 * sizeof(std::int32_t));
    CHECK(bytesPerFrame(SampleFormat::Cf32) == 2 * sizeof(float));
    CHECK(bytesPerFrame(SampleFormat::Cf64) == 2 * sizeof(double));

    // The two that have no raw type of their own.
    CHECK(bytesPerFrame(SampleFormat::Cf16) == 4);
    CHECK(bytesPerFrame(SampleFormat::Cu12) == 3);
    CHECK(bytesPerFrame(SampleFormat::Cs12) == 3);
}

TEST_CASE("format names round-trip") {
    // This is the persisted vocabulary: toString() feeds the synthetic
    // device's `format` parameter into a saved profile, and
    // sampleFormatFromString() reads it back. A typo here is a profile that
    // stops loading, which is why it is checked rather than assumed.
    for (const SampleFormat format : allSampleFormats()) {
        const std::string name(toString(format));
        CAPTURE(name);

        const auto parsed = sampleFormatFromString(name);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == format);
    }

    // The three spellings that were already in profiles before the set grew.
    CHECK(toString(SampleFormat::Cs8) == "cs8");
    CHECK(toString(SampleFormat::Cs16) == "cs16");
    CHECK(toString(SampleFormat::Cf32) == "cf32");

    CHECK_FALSE(sampleFormatFromString("cs9").has_value());
    CHECK_FALSE(sampleFormatFromString("").has_value());
}

TEST_CASE("aliases resolve to the canonical format") {
    const auto expect = [](std::string_view text, SampleFormat format) {
        CAPTURE(std::string(text));
        const auto parsed = sampleFormatFromString(text);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == format);
    };

    expect("SC8", SampleFormat::Cs8);
    expect("int16", SampleFormat::Cs16);
    expect("FC32", SampleFormat::Cf32);
    expect("complex64", SampleFormat::Cf32);
    expect("complex128", SampleFormat::Cf64);
    expect("uint8", SampleFormat::Cu8);
    expect("half", SampleFormat::Cf16);
}

TEST_CASE("conversion maps the landmark codes to -1, 0 and +1") {
    for (const SampleFormat format : allSampleFormats()) {
        CAPTURE(std::string(toString(format)));

        const Codes codes = codesFor(format);
        std::vector<std::byte> buffer(3 * bytesPerFrame(format), std::byte{0});

        // One frame per landmark, I and Q the same, so a codec that confused
        // the two would still be caught by the ramp case below.
        const double landmarks[3] = {codes.mostNegative, codes.zero, codes.mostPositive};
        for (std::size_t frame = 0; frame < 3; ++frame) {
            putCode(buffer.data(), format, 2 * frame, landmarks[frame]);
            putCode(buffer.data(), format, 2 * frame + 1, landmarks[frame]);
        }

        std::vector<std::complex<float>> out(3);
        dsp::convertToComplexFloat(buffer.data(), format, out.data(), 3);

        // The most negative code is exactly -1: there is one more code below
        // the centre than above it, in every one of these formats.
        CHECK(out[0].real() == doctest::Approx(-1.0).epsilon(kTolerance));
        CHECK(out[0].imag() == doctest::Approx(-1.0).epsilon(kTolerance));

        // Zero is exact, and for the unsigned formats that is the whole point
        // of zeroOffset: a missing bias puts the entire stream at full-scale
        // DC, which shows up as a spike in bin 0 and nothing else.
        CHECK(out[1].real() == 0.0F);
        CHECK(out[1].imag() == 0.0F);

        // The most positive code is one step short of +1.
        const auto expected =
            static_cast<float>((codes.mostPositive - static_cast<double>(zeroOffset(format))) /
                               static_cast<double>(fullScale(format)));
        CHECK(out[2].real() == doctest::Approx(expected).epsilon(kTolerance));
        CHECK(out[2].imag() == doctest::Approx(expected).epsilon(kTolerance));
        CHECK(out[2].real() <= 1.0F);
    }
}

TEST_CASE("conversion agrees with the reference decoder across the range") {
    constexpr std::size_t kFrames = 257; // odd, so the tail of a vectorised loop runs

    for (const SampleFormat format : allSampleFormats()) {
        CAPTURE(std::string(toString(format)));

        const std::vector<std::byte> buffer = rampBlock(format, kFrames);
        std::vector<std::complex<float>> out(kFrames);
        dsp::convertToComplexFloat(buffer.data(), format, out.data(), kFrames);

        for (std::size_t i = 0; i < kFrames; ++i) {
            CAPTURE(i);
            CHECK(static_cast<double>(out[i].real()) ==
                  doctest::Approx(getNormalised(buffer.data(), format, 2 * i)).epsilon(kTolerance));
            CHECK(static_cast<double>(out[i].imag()) ==
                  doctest::Approx(getNormalised(buffer.data(), format, 2 * i + 1))
                      .epsilon(kTolerance));
        }
    }
}

TEST_CASE("convertAndWindow is convertToComplexFloat times the window") {
    constexpr std::size_t kFrames = 129;

    std::vector<float> window(kFrames);
    for (std::size_t i = 0; i < kFrames; ++i) {
        window[i] = 0.25F + 0.75F * static_cast<float>(i) / static_cast<float>(kFrames);
    }

    for (const SampleFormat format : allSampleFormats()) {
        CAPTURE(std::string(toString(format)));

        const std::vector<std::byte> buffer = rampBlock(format, kFrames);

        std::vector<std::complex<float>> plain(kFrames);
        dsp::convertToComplexFloat(buffer.data(), format, plain.data(), kFrames);

        std::vector<std::complex<float>> windowed(kFrames);
        dsp::convertAndWindow(buffer.data(), format, window, windowed.data(), kFrames);

        for (std::size_t i = 0; i < kFrames; ++i) {
            CAPTURE(i);
            CHECK(windowed[i].real() ==
                  doctest::Approx(plain[i].real() * window[i]).epsilon(kTolerance));
            CHECK(windowed[i].imag() ==
                  doctest::Approx(plain[i].imag() * window[i]).epsilon(kTolerance));
        }
    }
}

TEST_CASE("convertAndWindow zero-pads a short window") {
    // A partial final block must not smear the previous one into the spectrum.
    constexpr std::size_t kFrames = 32;
    const std::vector<std::byte> buffer = rampBlock(SampleFormat::Cs16, kFrames);

    const std::vector<float> window(8, 1.0F);
    std::vector<std::complex<float>> out(kFrames, std::complex<float>{9.0F, 9.0F});
    dsp::convertAndWindow(buffer.data(), SampleFormat::Cs16, window, out.data(), kFrames);

    for (std::size_t i = 8; i < kFrames; ++i) {
        CAPTURE(i);
        CHECK(out[i].real() == 0.0F);
        CHECK(out[i].imag() == 0.0F);
    }
}

TEST_CASE("meanPower agrees with a scalar reference") {
    constexpr std::size_t kFrames = 193;

    for (const SampleFormat format : allSampleFormats()) {
        CAPTURE(std::string(toString(format)));

        const std::vector<std::byte> buffer = rampBlock(format, kFrames);

        double total = 0.0;
        for (std::size_t i = 0; i < kFrames; ++i) {
            const double re = getNormalised(buffer.data(), format, 2 * i);
            const double im = getNormalised(buffer.data(), format, 2 * i + 1);
            total += re * re + im * im;
        }
        const double expected = total / static_cast<double>(kFrames);

        CHECK(static_cast<double>(dsp::meanPower(buffer.data(), format, kFrames)) ==
              doctest::Approx(expected).epsilon(kTolerance));
    }

    CHECK(dsp::meanPower(nullptr, SampleFormat::Cs16, 0) == 0.0F);
}

TEST_CASE("clippedFraction agrees with a scalar reference") {
    constexpr std::size_t kFrames = 193;

    for (const SampleFormat format : allSampleFormats()) {
        CAPTURE(std::string(toString(format)));

        const std::vector<std::byte> buffer = rampBlock(format, kFrames);

        // One count short of full scale for the fixed-point formats, and full
        // scale itself for the floating ones -- and compared in float, because
        // that is what the reading is defined over. At 32 bits neither the
        // threshold nor the sample survives a float intact, so a reference
        // computed in double would disagree by a factor of two at the rails
        // and be measuring the wrong thing.
        const bool floating = fullScale(format) == 1.0F;
        const float threshold = floating ? 1.0F : fullScale(format) - 1.0F;

        std::size_t clipped = 0;
        for (std::size_t i = 0; i < kFrames; ++i) {
            const float re =
                std::fabs(static_cast<float>(getCentred(buffer.data(), format, 2 * i)));
            const float im =
                std::fabs(static_cast<float>(getCentred(buffer.data(), format, 2 * i + 1)));
            if (re >= threshold || im >= threshold) {
                ++clipped;
            }
        }
        const auto expected = static_cast<float>(clipped) / static_cast<float>(kFrames);

        CHECK(dsp::clippedFraction(buffer.data(), format, kFrames) ==
              doctest::Approx(expected).epsilon(1e-5));

        // The ramp reaches both rails, so something must have been counted.
        CHECK(clipped > 0);
    }

    CHECK(dsp::clippedFraction(nullptr, SampleFormat::Cs16, 0) == 0.0F);
}

TEST_CASE("encoding round-trips through the converter") {
    constexpr std::size_t kFrames = 64;

    std::vector<float> source(kFrames * 2);
    for (std::size_t i = 0; i < source.size(); ++i) {
        // Inside the rails, so no format's saturation is in play and the only
        // error left is quantisation.
        source[i] = 0.9F * std::sin(static_cast<float>(i) * 0.37F);
    }

    for (const SampleFormat format : allSampleFormats()) {
        CAPTURE(std::string(toString(format)));

        std::vector<std::byte> buffer(kFrames * bytesPerFrame(format), std::byte{0});
        dsp::convertFromComplexFloat(source.data(), format, buffer.data(), kFrames);

        std::vector<std::complex<float>> out(kFrames);
        dsp::convertToComplexFloat(buffer.data(), format, out.data(), kFrames);

        // One count of the format's own quantiser, floored at what the float
        // carrying the result can resolve. Binary16 is the coarse end -- eleven
        // bits of mantissa, worse than any fixed-point format here bar the
        // 8-bit ones -- and the 32-bit formats are the other: their quantiser
        // is finer than a float, so the float is what limits the round trip.
        const double quantiser = (format == SampleFormat::Cf16)
                                     ? 1.0 / 512.0
                                     : 2.0 / static_cast<double>(fullScale(format));
        const double step = std::max(quantiser, 1e-7);

        for (std::size_t i = 0; i < kFrames; ++i) {
            CAPTURE(i);
            CHECK(std::abs(static_cast<double>(out[i].real()) -
                           static_cast<double>(source[2 * i])) <= step);
            CHECK(std::abs(static_cast<double>(out[i].imag()) -
                           static_cast<double>(source[2 * i + 1])) <= step);
        }
    }
}

TEST_CASE("a format and its unsigned twin encode identically") {
    // Same width, same resolution, same quantiser -- so the only difference
    // between them may be where zero sits. Getting this wrong is quiet: it
    // costs every negative sample one count, which is a half-LSB rattle on the
    // unsigned format and silence on the signed one, and it only shows up as
    // an unexplained noise floor several stages downstream.
    constexpr std::size_t kFrames = 96;

    std::vector<float> source(kFrames * 2);
    for (std::size_t i = 0; i < source.size(); ++i) {
        source[i] = 0.8F * std::sin(static_cast<float>(i) * 0.21F);
    }

    const std::pair<SampleFormat, SampleFormat> twins[] = {
        {SampleFormat::Cs8, SampleFormat::Cu8},
        {SampleFormat::Cs12, SampleFormat::Cu12},
        {SampleFormat::Cs16, SampleFormat::Cu16},
        {SampleFormat::Cs32, SampleFormat::Cu32},
    };

    for (const auto& [signedFormat, unsignedFormat] : twins) {
        CAPTURE(std::string(toString(signedFormat)));

        std::vector<std::byte> a(kFrames * bytesPerFrame(signedFormat), std::byte{0});
        std::vector<std::byte> b(kFrames * bytesPerFrame(unsignedFormat), std::byte{0});
        dsp::convertFromComplexFloat(source.data(), signedFormat, a.data(), kFrames);
        dsp::convertFromComplexFloat(source.data(), unsignedFormat, b.data(), kFrames);

        for (std::size_t i = 0; i < kFrames * 2; ++i) {
            CAPTURE(i);
            CHECK(getCentred(a.data(), signedFormat, i) == getCentred(b.data(), unsignedFormat, i));
        }
    }
}

TEST_CASE("the synthetic device produces every format") {
    // What makes the conversions above testable with no radio attached: the
    // generator emits each format through the same path a driver would, so
    // eight formats with no hardware in this tree still get run.
    registerBuiltinSdrDevices();

    for (const SampleFormat format : allSampleFormats()) {
        CAPTURE(std::string(toString(format)));

        auto device = SdrDeviceManager::instance().open("synthetic", "");
        REQUIRE(device.has_value());

        REQUIRE((*device)
                    ->setParameter("sample_format", SdrValue{std::string(toString(format))})
                    .has_value());
        CHECK((*device)->nativeFormat() == format);

        const auto readBack = (*device)->getParameter("sample_format");
        REQUIRE(readBack.has_value());
        CHECK(asString(*readBack) == toString(format));

        // readSamples() runs the push stream and converts what comes back, so
        // this exercises the generator's encoder and the converter's decoder
        // against each other over a real block.
        constexpr std::size_t kFrames = 4096;
        std::vector<std::complex<float>> samples(kFrames);
        const auto read = (*device)->readSamples(samples.data(), kFrames);
        REQUIRE(read.has_value());
        CHECK(*read == kFrames);

        // Noise at the default floor: not silence, and for the fixed-point
        // formats not past the rails either. The float formats are exempt
        // because they have no rails -- an encoder that clamped them would be
        // inventing a saturation the hardware does not have.
        const bool floating = fullScale(format) == 1.0F;
        double power = 0.0;
        for (const std::complex<float>& sample : samples) {
            CHECK(std::isfinite(sample.real()));
            CHECK(std::isfinite(sample.imag()));
            if (!floating) {
                CHECK(std::abs(sample.real()) <= 1.0F);
                CHECK(std::abs(sample.imag()) <= 1.0F);
            }
            power += static_cast<double>(std::norm(sample));
        }
        CHECK(power > 0.0);
    }
}
