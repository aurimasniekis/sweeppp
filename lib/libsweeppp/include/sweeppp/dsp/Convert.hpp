// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/sdr/SampleFormat.hpp"

#include <complex>
#include <cstddef>
#include <span>

namespace sweeppp::dsp {

/// Converts native-format samples to normalised complex<float> in [-1, 1].
///
/// Standalone form, for the pull-mode adapter and for tests. The pipeline does
/// not call this -- it calls convertAndWindow(), which does the same work
/// fused with the window multiply in a single pass.
void convertToComplexFloat(const std::byte* input, SampleFormat format, std::complex<float>* output,
                           std::size_t frames) noexcept;

/// Encodes normalised samples into a native format: the inverse of the above.
///
/// `input` is `2 * frames` interleaved I/Q components in [-1, 1]; anything
/// outside that saturates for the integer formats and passes through for the
/// float ones, which is what the two kinds of hardware would do. `output` must
/// hold `frames * bytesPerFrame(format)` bytes.
///
/// Not an acquisition path -- nothing streaming converts in this direction.
/// It exists so the synthetic device can produce every format the reader
/// understands, which is what makes those conversions testable with no radio
/// attached, and it shares the reader's own scale factors rather than
/// restating them.
void convertFromComplexFloat(const float* input, SampleFormat format, std::byte* output,
                             std::size_t frames) noexcept;

/// Converts *and* applies the window in one pass.
///
/// This is the pipeline's hot path and the reason SampleFormat exists. Two
/// separate passes over a 100 MS/s stream would touch memory twice and destroy
/// the cache locality that makes the rate reachable at all; fused, each sample
/// is read once, scaled, multiplied and written once. The loop is written to
/// vectorise cleanly on NEON and AVX.
///
/// `window` must hold `frames` coefficients.
void convertAndWindow(const std::byte* input, SampleFormat format, std::span<const float> window,
                      std::complex<float>* output, std::size_t frames) noexcept;

/// The same pass with a DC offset subtracted before the window multiply.
///
/// `dcOffset` is in normalised units, as `blockStats` reports it. The
/// subtraction happens in the format's own raw units so the loop stays one
/// fused multiply-add per component and still vectorises. A carrier sitting
/// exactly on the LO is removed along with the receiver's own leak -- there is
/// no way to tell the two apart from one block -- which is the trade-off of
/// turning this on.
void convertAndWindow(const std::byte* input, SampleFormat format, std::span<const float> window,
                      std::complex<float>* output, std::size_t frames,
                      std::complex<float> dcOffset) noexcept;

/// What one pass over a block can say about it before it is converted.
struct BlockStats {
    /// Fraction of samples at or beyond full scale; see `clippedFraction`.
    float clippedFraction = 0.0F;
    /// Mean I and Q in normalised units: the receiver's DC offset, which a
    /// direct-conversion tuner leaks into every block as a spike at its LO.
    std::complex<float> mean{0.0F, 0.0F};
};

/// Clipping and mean in one pass. The pipeline reads both per block, and two
/// passes over a 100 MS/s stream would touch memory twice for two numbers.
[[nodiscard]] BlockStats blockStats(const std::byte* input, SampleFormat format,
                                    std::size_t frames) noexcept;

/// Mean power of a native-format block, without converting it.
///
/// Used for the settle-detection heuristic and for a cheap overload check on
/// the acquisition side, where a full conversion would be wasteful.
[[nodiscard]] float meanPower(const std::byte* input, SampleFormat format,
                              std::size_t frames) noexcept;

/// Fraction of samples at or beyond full scale. A non-zero value means the
/// front end is clipping and every level reading downstream is wrong, so the
/// UI surfaces it rather than letting it look like a strong signal.
[[nodiscard]] float clippedFraction(const std::byte* input, SampleFormat format,
                                    std::size_t frames) noexcept;

/// Converts an FFT output to power in dBFS, with the window's amplitude scale
/// already applied.
///
/// `amplitudeScale` is `1 / (N * coherentGain)` -- see Window. Passing it in
/// rather than recomputing keeps the per-bin cost to one multiply.
void magnitudeToDbfs(const std::complex<float>* spectrum, float* output, std::size_t bins,
                     float amplitudeScale) noexcept;

/// Reorders an FFT result so bin 0 is the lowest frequency rather than DC.
///
/// A complex FFT returns [DC .. +Fs/2, -Fs/2 .. -bin], which plots as two
/// halves in the wrong order. Every display and every stored tile wants
/// ascending frequency, so the swap happens once here.
void fftShift(float* bins, std::size_t count) noexcept;

/// In-place accumulation for averaging across FFTs within one dwell.
/// `weight` of 1/n gives a running mean.
void accumulateMax(float* accumulator, const float* values, std::size_t count) noexcept;
void accumulateMin(float* accumulator, const float* values, std::size_t count) noexcept;
void accumulateMean(float* accumulator, const float* values, std::size_t count,
                    float weight) noexcept;

} // namespace sweeppp::dsp
