// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ReferenceFft.hpp"

#include <bit>
#include <cmath>
#include <complex>
#include <mutex>
#include <numbers>
#include <sweeppp/fft/FftBackendManager.hpp>
#include <vector>

namespace sweeppp {
namespace {

constexpr double kPi = std::numbers::pi;

/// Arithmetic in double throughout, whatever the interface carries.
///
/// The suite compares against closed-form expectations to a few parts in 1e4,
/// and a single-precision accumulator over a 2^20-point transform does not
/// leave that much room.
using Complex = std::complex<double>;

[[nodiscard]] Complex widen(std::complex<float> value) noexcept {
    return {static_cast<double>(value.real()), static_cast<double>(value.imag())};
}

/// Iterative Cooley-Tukey, in place, for a power-of-two length.
///
/// `inverse` flips the exponent's sign and nothing else: the result is
/// unnormalised, so a round trip scales by the length. That matches what every
/// backend the application talks to does.
void transformRadix2(std::vector<Complex>& data, bool inverse) noexcept {
    const std::size_t n = data.size();

    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1U;
        for (; (j & bit) != 0; bit >>= 1U) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }

    for (std::size_t span = 2; span <= n; span <<= 1U) {
        const double angle = (inverse ? 2.0 : -2.0) * kPi / static_cast<double>(span);
        const Complex step(std::cos(angle), std::sin(angle));
        for (std::size_t base = 0; base < n; base += span) {
            Complex twiddle(1.0, 0.0);
            for (std::size_t k = 0; k < span / 2; ++k) {
                const Complex even = data[base + k];
                const Complex odd = data[base + k + (span / 2)] * twiddle;
                data[base + k] = even + odd;
                data[base + k + (span / 2)] = even - odd;
                twiddle *= step;
            }
        }
    }
}

/// One prepared transform.
///
/// A power-of-two size runs the radix-2 core directly. Anything else goes
/// through Bluestein's chirp-z, which rewrites the size-N DFT as a convolution
/// of length M -- the next power of two at or above 2N-1 -- so an arbitrary N
/// still costs O(M log M) rather than O(N^2).
///
/// Everything precomputed here is const afterwards and every buffer a call
/// needs is thread-local, which is what makes `threadSafeExecute` true: the
/// pipeline runs one plan from several workers at once.
class ReferencePlan final : public IFftPlan {
public:
    ReferencePlan(std::size_t size, bool inverse)
        : m_size(size), m_inverse(inverse), m_radix2(std::has_single_bit(size)) {
        if (m_radix2) {
            return;
        }

        m_convolutionSize = std::bit_ceil((2 * size) - 1);

        // chirp[j] = exp(sign * i * pi * j^2 / N), with j^2 reduced modulo 2N
        // first: at N near the maximum size j^2 is large enough that taking
        // the sine of it directly loses most of the mantissa.
        const double sign = inverse ? 1.0 : -1.0;
        const std::size_t period = 2 * size;
        m_chirp.resize(size);
        for (std::size_t j = 0; j < size; ++j) {
            const double angle =
                sign * kPi * static_cast<double>((j * j) % period) / static_cast<double>(size);
            m_chirp[j] = Complex(std::cos(angle), std::sin(angle));
        }

        // The convolution kernel is the conjugate chirp, laid out symmetrically
        // so that a cyclic convolution of length M reproduces the linear one
        // over the indices that matter.
        m_kernel.assign(m_convolutionSize, Complex(0.0, 0.0));
        m_kernel[0] = std::conj(m_chirp[0]);
        for (std::size_t j = 1; j < size; ++j) {
            m_kernel[j] = std::conj(m_chirp[j]);
            m_kernel[m_convolutionSize - j] = std::conj(m_chirp[j]);
        }
        transformRadix2(m_kernel, false);
    }

    void execute(const std::complex<float>* input, std::complex<float>* output) noexcept override {
        std::vector<Complex>& scratch = workspace();

        if (m_radix2) {
            scratch.resize(m_size);
            for (std::size_t i = 0; i < m_size; ++i) {
                scratch[i] = widen(input[i]);
            }
            transformRadix2(scratch, m_inverse);
            store(scratch, output);
            return;
        }

        scratch.assign(m_convolutionSize, Complex(0.0, 0.0));
        for (std::size_t i = 0; i < m_size; ++i) {
            scratch[i] = widen(input[i]) * m_chirp[i];
        }

        transformRadix2(scratch, false);
        for (std::size_t i = 0; i < m_convolutionSize; ++i) {
            scratch[i] *= m_kernel[i];
        }
        transformRadix2(scratch, true);

        // The inverse leg is unnormalised, so the 1/M belongs here.
        const double scale = 1.0 / static_cast<double>(m_convolutionSize);
        for (std::size_t k = 0; k < m_size; ++k) {
            scratch[k] *= m_chirp[k] * scale;
        }
        store(scratch, output);
    }

    void executeBatch(const std::complex<float>* input, std::complex<float>* output,
                      std::size_t count) noexcept override {
        for (std::size_t i = 0; i < count; ++i) {
            execute(input + (i * m_size), output + (i * m_size));
        }
    }

    [[nodiscard]] std::size_t size() const noexcept override { return m_size; }
    [[nodiscard]] bool inverse() const noexcept override { return m_inverse; }

private:
    /// Per thread, not per plan: several workers execute one plan at once, and
    /// a buffer on the plan would be the race the capability flag promises is
    /// not there.
    [[nodiscard]] static std::vector<Complex>& workspace() {
        thread_local std::vector<Complex> buffer;
        return buffer;
    }

    void store(const std::vector<Complex>& from, std::complex<float>* output) const noexcept {
        for (std::size_t k = 0; k < m_size; ++k) {
            output[k] = {static_cast<float>(from[k].real()), static_cast<float>(from[k].imag())};
        }
    }

    std::size_t m_size;
    bool m_inverse;
    bool m_radix2;

    std::size_t m_convolutionSize = 0;
    std::vector<Complex> m_chirp;
    std::vector<Complex> m_kernel;
};

FftCapabilities referenceCapabilities() noexcept {
    return FftCapabilities{
        .type = FftBackendType::Cpu,
        .minSize = 2,
        // Well above anything the suite plans, and low enough that a plan's
        // Bluestein buffers stay small.
        .maxSize = 1U << 20U,
        .sizeConstraint = FftSizeConstraint::Any,
        .supportsBatch = true,
        .supportsInPlace = false,
        .threadSafeExecute = true,
        // Nothing is shared between plans, so two threads may build one at
        // once.
        .threadSafePlanning = true,
    };
}

class ReferenceBackend final : public IFftBackend {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "reference"; }

    [[nodiscard]] std::string displayName() const override { return "Reference FFT"; }

    [[nodiscard]] const FftCapabilities& capabilities() const noexcept override {
        return m_capabilities;
    }

    Result<std::unique_ptr<IFftPlan>> createPlan(const FftPlanConfig& config) override {
        if (!supportsSize(config.size)) {
            return fail<std::unique_ptr<IFftPlan>>(
                ErrorCode::InvalidArgument, "FFT size {} is outside the supported range {}..{}",
                config.size, m_capabilities.minSize, m_capabilities.maxSize);
        }
        if (config.batchCount == 0) {
            return fail<std::unique_ptr<IFftPlan>>(ErrorCode::InvalidArgument,
                                                   "batch count must be at least 1");
        }
        return std::make_unique<ReferencePlan>(config.size, config.inverse);
    }

private:
    FftCapabilities m_capabilities = referenceCapabilities();
};

} // namespace

void registerReferenceFftBackend() {
    static std::once_flag once;
    std::call_once(once, [] {
        FftBackendManager::instance().registerBackend(
            FftBackendInfo{.name = "reference",
                           .displayName = "Reference FFT",
                           .description = "Radix-2 with a Bluestein wrapper for arbitrary sizes. "
                                          "Compiled into the test binary, so no host test "
                                          "depends on which plugins were built.",
                           .capabilities = referenceCapabilities(),
                           .available = true,
                           .unavailableReason = {},
                           .isSuggestedDefault = false},
            [] -> Result<std::unique_ptr<IFftBackend>> {
                return std::make_unique<ReferenceBackend>();
            });
    });
}

} // namespace sweeppp
