// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/dsp/Convert.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

#if defined(__STDCPP_FLOAT16_T__)
#include <stdfloat>
#endif

namespace sweeppp::dsp {
namespace {

/// Floor for the dB conversion.
///
/// -300 dBFS is far below any real signal and below float denormal territory,
/// so a zero bin becomes a very negative number rather than -inf. Infinities
/// would propagate through averaging, min/max hold and the uint8 quantiser,
/// turning one silent bin into a corrupted trace.
constexpr float kPowerFloor = 1e-30F;

template <typename Sample>
[[nodiscard]] const Sample* as(const std::byte* input) noexcept {
    // The pool guarantees cache-line alignment, which exceeds the alignment of
    // every sample type here, so this cast is always well-founded.
    return reinterpret_cast<const Sample*>(input);
}

/* ---------------------------------------------------------------------------
 * binary16
 *
 * Three ways to spell it and the compiler decides which exists. Apple clang on
 * arm64 has `_Float16` but no <stdfloat>, so the two native cases are separate
 * rather than one; the third is the arithmetic, for a compiler with neither.
 * ------------------------------------------------------------------------- */

#if defined(__STDCPP_FLOAT16_T__)
#define SWEEPPP_NATIVE_FLOAT16 1
using NativeHalf = std::float16_t;
#elif defined(__FLT16_MAX__)
#define SWEEPPP_NATIVE_FLOAT16 1
using NativeHalf = _Float16;
#else
#define SWEEPPP_NATIVE_FLOAT16 0
#endif

#if !SWEEPPP_NATIVE_FLOAT16
[[nodiscard]] inline std::uint16_t floatToHalfBits(float value) noexcept {
    // Truncating rather than round-to-nearest-even. This path only runs on a
    // compiler with no binary16 of its own, and only to *generate* test signal;
    // half an LSB at 11 bits of mantissa is far below the noise it is encoding.
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const auto sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
    const auto exponent = static_cast<std::int32_t>((bits >> 23U) & 0xFFU) - 127;
    const std::uint32_t mantissa = bits & 0x7FFFFFU;

    if (exponent == 128) { // infinity or NaN
        return static_cast<std::uint16_t>(sign | 0x7C00U | (mantissa != 0U ? 0x200U : 0U));
    }
    if (exponent > 15) { // saturates binary16
        return static_cast<std::uint16_t>(sign | 0x7C00U);
    }
    if (exponent < -24) { // underflows it
        return sign;
    }
    if (exponent < -14) { // subnormal binary16, normal float
        const auto shift = static_cast<std::uint32_t>(-exponent - 14) + 13U;
        return static_cast<std::uint16_t>(sign | ((mantissa | 0x800000U) >> shift));
    }
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent + 15) << 10U) |
                                      (mantissa >> 13U));
}

[[nodiscard]] constexpr float halfBitsToFloat(std::uint16_t bits) noexcept {
    const std::uint32_t sign = static_cast<std::uint32_t>(bits & 0x8000U) << 16U;
    const std::uint32_t exponent = (bits >> 10U) & 0x1FU;
    const std::uint32_t mantissa = bits & 0x3FFU;

    std::uint32_t out = sign;
    if (exponent == 0U) {
        if (mantissa != 0U) {
            // Subnormal half, normal float: shift the mantissa up until its
            // leading one is implicit, and pay for it in the exponent.
            std::uint32_t shifted = mantissa;
            std::uint32_t shifts = 0U;
            while ((shifted & 0x400U) == 0U) {
                shifted <<= 1U;
                ++shifts;
            }
            out |= ((113U - shifts) << 23U) | ((shifted & 0x3FFU) << 13U);
        }
    } else if (exponent == 0x1FU) {
        out |= 0x7F800000U | (mantissa << 13U); // infinity or NaN
    } else {
        out |= ((exponent + 112U) << 23U) | (mantissa << 13U);
    }
    return std::bit_cast<float>(out);
}
#endif

/* ---------------------------------------------------------------------------
 * Codecs
 *
 * One trait per format, and one `visitFormat` that switches once and hands a
 * generic lambda the trait it selected. Written this way because the four
 * functions below need the same eleven loops each, and forty-four hand-written
 * loops is forty-four places for a scale factor to be wrong in one of them.
 *
 * Each instantiation is still the flat typed loop it always was -- no inner
 * conditionals, no indirect call -- which is what keeps the fused
 * convert-and-window pass vectorising on NEON and AVX. That property is the
 * reason this file exists, so it is the one to check has not regressed.
 *
 * `load` returns one component already centred on zero and still at the
 * format's own scale: `raw - zeroOffset`, in [-fullScale, fullScale). Callers
 * multiply by `kScale` to normalise, so sign never reaches a call site.
 *
 * `store` is the inverse and shares the same constants, which is the point of
 * it being here rather than in the synthetic device: an encoder that carried
 * its own copy of a scale factor would be an encoder that eventually disagreed
 * with the decoder, and the two are only ever checked against each other.
 * ------------------------------------------------------------------------- */

[[nodiscard]] constexpr bool isFloatFormat(SampleFormat format) noexcept {
    return format == SampleFormat::Cf16 || format == SampleFormat::Cf32 ||
           format == SampleFormat::Cf64;
}

template <SampleFormat F>
struct CodecBase {
    static constexpr SampleFormat kFormat = F;
    static constexpr float kScale = 1.0F / fullScale(F);
    static constexpr float kBias = zeroOffset(F);

    /// Where a component counts as clipped: one count below full scale, which
    /// is what a saturating ADC actually produces, and 1.0 for the float
    /// formats where full scale is the limit itself.
    ///
    /// At 32 bits this threshold is not exactly representable in a float, so it
    /// rounds up to full scale and the topmost handful of codes out of 2^32 are
    /// counted as clipped when they are one step short. Immaterial next to what
    /// the reading is for.
    static constexpr float kClip = isFloatFormat(F) ? 1.0F : fullScale(F) - 1.0F;
};

template <SampleFormat F>
struct Codec;

/// Clamps into the raw type, then converts. The clamp is not decoration: at 32
/// bits, full scale plus the bias lands one ULP above UINT32_MAX once it has
/// been through a float, and converting that to the integer type is undefined
/// rather than merely wrong.
///
/// Callers truncate *before* biasing. Doing it here instead would make
/// truncation a floor for the unsigned formats -- every negative sample a count
/// low -- so cu8 would carry a half-LSB rattle where cs8, at the same
/// resolution and fed the same signal, is properly silent.
template <typename Raw>
[[nodiscard]] Raw quantise(double value) noexcept {
    constexpr auto kLowest = static_cast<double>(std::numeric_limits<Raw>::lowest());
    constexpr auto kHighest = static_cast<double>(std::numeric_limits<Raw>::max());
    return static_cast<Raw>(std::clamp(value, kLowest, kHighest));
}

/// The eight formats that are a plain interleaved array of one type.
template <SampleFormat F, typename Raw>
struct ArrayCodec : CodecBase<F> {
    [[nodiscard]] static float load(const std::byte* input, std::size_t component) noexcept {
        if constexpr (CodecBase<F>::kBias == 0.0F) {
            return static_cast<float>(as<Raw>(input)[component]);
        } else {
            return static_cast<float>(as<Raw>(input)[component]) - CodecBase<F>::kBias;
        }
    }

    static void store(std::byte* output, std::size_t component, float value) noexcept {
        auto* raw = reinterpret_cast<Raw*>(output);
        if constexpr (isFloatFormat(F)) {
            // A float format has no saturation point, so it takes the value as
            // it stands rather than being clipped to one.
            raw[component] = static_cast<Raw>(value);
        } else {
            constexpr auto kLimit = static_cast<double>(CodecBase<F>::kClip);
            const double scaled = std::clamp(static_cast<double>(value) * kLimit, -kLimit, kLimit);
            raw[component] =
                quantise<Raw>(std::trunc(scaled) + static_cast<double>(CodecBase<F>::kBias));
        }
    }
};

template <>
struct Codec<SampleFormat::Cu8> : ArrayCodec<SampleFormat::Cu8, std::uint8_t> {};
template <>
struct Codec<SampleFormat::Cs8> : ArrayCodec<SampleFormat::Cs8, std::int8_t> {};
template <>
struct Codec<SampleFormat::Cu16> : ArrayCodec<SampleFormat::Cu16, std::uint16_t> {};
template <>
struct Codec<SampleFormat::Cs16> : ArrayCodec<SampleFormat::Cs16, std::int16_t> {};
template <>
struct Codec<SampleFormat::Cu32> : ArrayCodec<SampleFormat::Cu32, std::uint32_t> {};
template <>
struct Codec<SampleFormat::Cs32> : ArrayCodec<SampleFormat::Cs32, std::int32_t> {};
template <>
struct Codec<SampleFormat::Cf32> : ArrayCodec<SampleFormat::Cf32, float> {};
template <>
struct Codec<SampleFormat::Cf64> : ArrayCodec<SampleFormat::Cf64, double> {};

template <>
struct Codec<SampleFormat::Cf16> : CodecBase<SampleFormat::Cf16> {
    [[nodiscard]] static float load(const std::byte* input, std::size_t component) noexcept {
#if SWEEPPP_NATIVE_FLOAT16
        return static_cast<float>(as<NativeHalf>(input)[component]);
#else
        return halfBitsToFloat(as<std::uint16_t>(input)[component]);
#endif
    }

    static void store(std::byte* output, std::size_t component, float value) noexcept {
#if SWEEPPP_NATIVE_FLOAT16
        reinterpret_cast<NativeHalf*>(output)[component] = static_cast<NativeHalf>(value);
#else
        reinterpret_cast<std::uint16_t*>(output)[component] = floatToHalfBits(value);
#endif
    }
};

/// One packed 12-bit component, little-endian nibble order:
///
///     byte0 = I[7:0]
///     byte1 = Q[3:0] << 4 | I[11:8]
///     byte2 = Q[11:4]
///
/// The one shape here that cannot be a typed loop -- a component has no address
/// of its own -- so it unpacks scalar and does not vectorise. Acceptable, and
/// worth stating rather than discovering: no device in this tree produces
/// 12-bit, and every format that actually carries throughput is a plain array.
[[nodiscard]] inline unsigned loadPacked12(const std::byte* input, std::size_t component) noexcept {
    const std::uint8_t* bytes = as<std::uint8_t>(input) + (component >> 1U) * 3U;
    if ((component & 1U) == 0U) {
        return static_cast<unsigned>(bytes[0]) | ((static_cast<unsigned>(bytes[1]) & 0x0FU) << 8U);
    }
    return (static_cast<unsigned>(bytes[1]) >> 4U) | (static_cast<unsigned>(bytes[2]) << 4U);
}

/// The inverse. The I component of a frame MUST be stored before its Q, which
/// the encoder below does: the two share a byte, and writing Q first would mean
/// reading back the half of it that I has not written yet.
inline void storePacked12(std::byte* output, std::size_t component, unsigned raw) noexcept {
    std::uint8_t* bytes = reinterpret_cast<std::uint8_t*>(output) + (component >> 1U) * 3U;
    raw &= 0xFFFU;
    if ((component & 1U) == 0U) {
        bytes[0] = static_cast<std::uint8_t>(raw & 0xFFU);
        bytes[1] = static_cast<std::uint8_t>(raw >> 8U);
    } else {
        bytes[1] = static_cast<std::uint8_t>(bytes[1] | ((raw & 0x0FU) << 4U));
        bytes[2] = static_cast<std::uint8_t>(raw >> 4U);
    }
}

template <>
struct Codec<SampleFormat::Cu12> : CodecBase<SampleFormat::Cu12> {
    [[nodiscard]] static float load(const std::byte* input, std::size_t component) noexcept {
        return static_cast<float>(loadPacked12(input, component)) - kBias;
    }

    static void store(std::byte* output, std::size_t component, float value) noexcept {
        constexpr auto kLimit = static_cast<double>(kClip);
        const double scaled = std::clamp(static_cast<double>(value) * kLimit, -kLimit, kLimit);
        storePacked12(output, component,
                      static_cast<unsigned>(quantise<std::uint16_t>(std::trunc(scaled) +
                                                                    static_cast<double>(kBias))));
    }
};

template <>
struct Codec<SampleFormat::Cs12> : CodecBase<SampleFormat::Cs12> {
    [[nodiscard]] static float load(const std::byte* input, std::size_t component) noexcept {
        const unsigned raw = loadPacked12(input, component);
        const int value =
            (raw & 0x800U) != 0U ? static_cast<int>(raw) - 4096 : static_cast<int>(raw);
        return static_cast<float>(value);
    }

    static void store(std::byte* output, std::size_t component, float value) noexcept {
        constexpr auto kLimit = static_cast<double>(kClip);
        const double scaled = std::clamp(static_cast<double>(value) * kLimit, -kLimit, kLimit);
        storePacked12(output, component,
                      static_cast<unsigned>(quantise<std::int16_t>(scaled)) & 0xFFFU);
    }
};

/// Switches once, then calls `fn` with the selected `Codec<F>` as a tag.
///
/// An unrecognised value falls through and does nothing, which leaves the
/// caller's output as it found it. That can only happen when a format ordinal
/// arrives from outside this image, and doing nothing is the answer that cannot
/// read past the end of a buffer sized for something else.
template <typename Fn>
void visitFormat(SampleFormat format, Fn&& fn) {
    switch (format) {
    case SampleFormat::Cu8:
        fn(Codec<SampleFormat::Cu8>{});
        return;
    case SampleFormat::Cs8:
        fn(Codec<SampleFormat::Cs8>{});
        return;
    case SampleFormat::Cu12:
        fn(Codec<SampleFormat::Cu12>{});
        return;
    case SampleFormat::Cs12:
        fn(Codec<SampleFormat::Cs12>{});
        return;
    case SampleFormat::Cu16:
        fn(Codec<SampleFormat::Cu16>{});
        return;
    case SampleFormat::Cs16:
        fn(Codec<SampleFormat::Cs16>{});
        return;
    case SampleFormat::Cu32:
        fn(Codec<SampleFormat::Cu32>{});
        return;
    case SampleFormat::Cs32:
        fn(Codec<SampleFormat::Cs32>{});
        return;
    case SampleFormat::Cf16:
        fn(Codec<SampleFormat::Cf16>{});
        return;
    case SampleFormat::Cf32:
        fn(Codec<SampleFormat::Cf32>{});
        return;
    case SampleFormat::Cf64:
        fn(Codec<SampleFormat::Cf64>{});
        return;
    }
}

} // namespace

void convertToComplexFloat(const std::byte* input, SampleFormat format, std::complex<float>* output,
                           std::size_t frames) noexcept {
    if (format == SampleFormat::Cf32) {
        // Already in the destination format and layout.
        std::memcpy(output, input, frames * sizeof(std::complex<float>));
        return;
    }

    visitFormat(format, [&]<typename C>(C) noexcept {
        for (std::size_t i = 0; i < frames; ++i) {
            output[i] = {C::load(input, 2 * i) * C::kScale, C::load(input, 2 * i + 1) * C::kScale};
        }
    });
}

void convertFromComplexFloat(const float* input, SampleFormat format, std::byte* output,
                             std::size_t frames) noexcept {
    if (format == SampleFormat::Cf32) {
        std::memcpy(output, input, frames * sizeof(std::complex<float>));
        return;
    }

    visitFormat(format, [&]<typename C>(C) noexcept {
        for (std::size_t i = 0; i < frames; ++i) {
            // I before Q, per frame: the packed 12-bit formats share a byte
            // between the two and depend on this order.
            C::store(output, 2 * i, input[2 * i]);
            C::store(output, 2 * i + 1, input[2 * i + 1]);
        }
    });
}

namespace {

/// One body for both public forms. The DC branch is a compile-time choice so
/// each instantiation stays a flat loop: with it, one subtract folds into the
/// multiply as a fused multiply-add; without it, nothing is added at all.
template <bool kRemoveDc>
void convertAndWindowImpl(const std::byte* input, SampleFormat format,
                          std::span<const float> window, std::complex<float>* output,
                          std::size_t frames, std::complex<float> dcOffset) noexcept {
    const std::size_t count = std::min(frames, window.size());
    const float* coefficients = window.data();

    // One pass: read, scale, window, write. Each instantiation is a flat loop
    // with no inner conditionals so the vectoriser can take it.
    visitFormat(format, [&]<typename C>(C) noexcept {
        if constexpr (kRemoveDc) {
            // The offset arrives normalised; `load` yields raw units, so it is
            // scaled back once here rather than every sample scaled forward.
            const float dcRe = dcOffset.real() / C::kScale;
            const float dcIm = dcOffset.imag() / C::kScale;
            for (std::size_t i = 0; i < count; ++i) {
                const float w = coefficients[i] * C::kScale;
                output[i] = {(C::load(input, 2 * i) - dcRe) * w,
                             (C::load(input, 2 * i + 1) - dcIm) * w};
            }
        } else {
            for (std::size_t i = 0; i < count; ++i) {
                const float w = coefficients[i] * C::kScale;
                output[i] = {C::load(input, 2 * i) * w, C::load(input, 2 * i + 1) * w};
            }
        }
    });

    // A short block is zero-padded rather than left as stale data, so a
    // partial final block cannot smear the previous one into the spectrum.
    for (std::size_t i = count; i < frames; ++i) {
        output[i] = {0.0F, 0.0F};
    }
}

} // namespace

void convertAndWindow(const std::byte* input, SampleFormat format, std::span<const float> window,
                      std::complex<float>* output, std::size_t frames) noexcept {
    convertAndWindowImpl<false>(input, format, window, output, frames, {});
}

void convertAndWindow(const std::byte* input, SampleFormat format, std::span<const float> window,
                      std::complex<float>* output, std::size_t frames,
                      std::complex<float> dcOffset) noexcept {
    convertAndWindowImpl<true>(input, format, window, output, frames, dcOffset);
}

BlockStats blockStats(const std::byte* input, SampleFormat format, std::size_t frames) noexcept {
    if (frames == 0) {
        return {};
    }

    // Double accumulators: a 262144-frame block of 16-bit samples sums past
    // what a float carries without losing the low bits the mean lives in.
    double sumRe = 0.0;
    double sumIm = 0.0;
    std::size_t clipped = 0;
    visitFormat(format, [&]<typename C>(C) noexcept {
        for (std::size_t i = 0; i < frames; ++i) {
            const float re = C::load(input, 2 * i);
            const float im = C::load(input, 2 * i + 1);
            sumRe += static_cast<double>(re);
            sumIm += static_cast<double>(im);
            if (std::fabs(re) >= C::kClip || std::fabs(im) >= C::kClip) {
                ++clipped;
            }
        }
        const double scale = static_cast<double>(C::kScale) / static_cast<double>(frames);
        sumRe *= scale;
        sumIm *= scale;
    });

    return {.clippedFraction = static_cast<float>(clipped) / static_cast<float>(frames),
            .mean = {static_cast<float>(sumRe), static_cast<float>(sumIm)}};
}

float meanPower(const std::byte* input, SampleFormat format, std::size_t frames) noexcept {
    if (frames == 0) {
        return 0.0F;
    }

    double total = 0.0;
    visitFormat(format, [&]<typename C>(C) noexcept {
        for (std::size_t i = 0; i < frames; ++i) {
            const double re = static_cast<double>(C::load(input, 2 * i) * C::kScale);
            const double im = static_cast<double>(C::load(input, 2 * i + 1) * C::kScale);
            total += re * re + im * im;
        }
    });

    return static_cast<float>(total / static_cast<double>(frames));
}

float clippedFraction(const std::byte* input, SampleFormat format, std::size_t frames) noexcept {
    if (frames == 0) {
        return 0.0F;
    }

    std::size_t clipped = 0;
    visitFormat(format, [&]<typename C>(C) noexcept {
        for (std::size_t i = 0; i < frames; ++i) {
            if (std::fabs(C::load(input, 2 * i)) >= C::kClip ||
                std::fabs(C::load(input, 2 * i + 1)) >= C::kClip) {
                ++clipped;
            }
        }
    });

    return static_cast<float>(clipped) / static_cast<float>(frames);
}

void magnitudeToDbfs(const std::complex<float>* spectrum, float* output, std::size_t bins,
                     float amplitudeScale) noexcept {
    // Squaring the scale once outside the loop turns two multiplies per bin
    // into one, which is worth it at megabin sweep widths.
    const float powerScale = amplitudeScale * amplitudeScale;

    for (std::size_t i = 0; i < bins; ++i) {
        const float re = spectrum[i].real();
        const float im = spectrum[i].imag();
        const float power = (re * re + im * im) * powerScale;
        output[i] = 10.0F * std::log10(std::max(power, kPowerFloor));
    }
}

void fftShift(float* bins, std::size_t count) noexcept {
    const std::size_t half = count / 2;
    if (half == 0) {
        return;
    }

    if (count % 2 == 0) {
        // Even sizes swap two equal halves in place -- no scratch needed.
        for (std::size_t i = 0; i < half; ++i) {
            std::swap(bins[i], bins[i + half]);
        }
        return;
    }

    // Odd sizes have unequal halves, so an in-place swap would not line up.
    // FFT sizes are effectively always even here; this branch exists so an odd
    // size is merely slower rather than wrong.
    std::rotate(bins, bins + half + 1, bins + count);
}

void accumulateMax(float* accumulator, const float* values, std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        accumulator[i] = std::max(accumulator[i], values[i]);
    }
}

void accumulateMin(float* accumulator, const float* values, std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        accumulator[i] = std::min(accumulator[i], values[i]);
    }
}

void accumulateMean(float* accumulator, const float* values, std::size_t count,
                    float weight) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        accumulator[i] += (values[i] - accumulator[i]) * weight;
    }
}

} // namespace sweeppp::dsp
