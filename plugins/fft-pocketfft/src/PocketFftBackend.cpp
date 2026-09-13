// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// PocketFFT backend. Single precision, one plan shared across N workers.
//
// Any length: FFTPACK's factorisation handles lengths built from small primes,
// and a length with a large prime factor takes the Bluestein path, which
// PocketFFT chooses by its own cost estimate at plan time.
//
// Scaling: `exec` with a factor of 1 applies no scale in either direction,
// which is FFTW's convention and the one `dsp::magnitudeToDbfs` assumes.
//
// Cost: `exec` allocates its scratch on every call -- one N-element array on
// the FFTPACK path, and an array of the padded Bluestein length on the other.
// That is upstream's design and is left unpatched; the FFT panel's Benchmark
// button shows what it costs against the other backends.
#include "PocketFftBackend.hpp"

#include <algorithm>
#include <complex>
#include <cstdint>
#include <exception>
#include <new>
#include <pocketfft_hdronly.h>
#include <sweeppp/core/Clock.hpp>
#include <utility>

namespace sweeppp::pocket {
namespace {

using Transform = pocketfft::detail::pocketfft_c<float>;
using Complex = pocketfft::detail::cmplx<float>;

class PocketFftPlan final : public IFftPlan {
public:
    PocketFftPlan(std::unique_ptr<Transform> transform, std::size_t size, bool inverse) noexcept
        : m_transform(std::move(transform)), m_size(size), m_inverse(inverse) {}

    void execute(const std::complex<float>* input, std::complex<float>* output) noexcept override {
        if (input != output) {
            std::copy_n(input, m_size, output);
        }
        try {
            // complex<float> is layout-compatible with float[2], and so with
            // PocketFFT's {r, i} pair -- the pipeline's buffers go straight in.
            m_transform->exec(reinterpret_cast<Complex*>(output), 1.0F, !m_inverse);
        } catch (...) {
            // Only the scratch allocation can throw. A zeroed block reads as
            // no signal, where letting it out of a noexcept would terminate.
            std::fill_n(output, m_size, std::complex<float>{});
        }
    }

    void executeBatch(const std::complex<float>* input, std::complex<float>* output,
                      std::size_t count) noexcept override {
        for (std::size_t i = 0; i < count; ++i) {
            execute(input + i * m_size, output + i * m_size);
        }
    }

    [[nodiscard]] std::size_t size() const noexcept override { return m_size; }
    [[nodiscard]] bool inverse() const noexcept override { return m_inverse; }

private:
    std::unique_ptr<Transform> m_transform;
    std::size_t m_size = 0;
    bool m_inverse = false;
};

} // namespace

PocketFftBackend::PocketFftBackend() {
    m_capabilities = FftCapabilities{
        .type = FftBackendType::Cpu,
        .minSize = 2,
        // FFTW's ceiling, so a profile moved between backends plans the same
        // sizes.
        .maxSize = 1U << 24U,
        .sizeConstraint = FftSizeConstraint::Any,
        .supportsBatch = false,
        .supportsInPlace = true,
        .threadSafeExecute = true,
        // No global state is touched while planning: the plan cache is
        // compiled out (POCKETFFT_CACHE_SIZE 0) and so is the thread pool.
        .threadSafePlanning = true,
    };
}

std::string_view PocketFftBackend::name() const noexcept {
    return "pocketfft";
}

std::string PocketFftBackend::displayName() const {
    return "PocketFFT";
}

const FftCapabilities& PocketFftBackend::capabilities() const noexcept {
    return m_capabilities;
}

Result<std::unique_ptr<IFftPlan>> PocketFftBackend::createPlan(const FftPlanConfig& config) {
    if (!supportsSize(config.size)) {
        return fail<std::unique_ptr<IFftPlan>>(
            ErrorCode::InvalidArgument, "FFT size {} is outside the supported range {}..{}",
            config.size, m_capabilities.minSize, m_capabilities.maxSize);
    }
    if (config.batchCount == 0) {
        return fail<std::unique_ptr<IFftPlan>>(ErrorCode::InvalidArgument,
                                               "batch count must be at least 1");
    }

    // `config.quality` has no counterpart: PocketFFT picks FFTPACK or
    // Bluestein from the length alone, so every quality builds the same plan.
    const std::uint64_t started = monotonicNs();

    std::unique_ptr<Transform> transform;
    try {
        transform = std::make_unique<Transform>(config.size);
    } catch (const std::bad_alloc&) {
        // Logged as well as returned: the ABI carries only the status code,
        // so the sentence would otherwise stop at the plugin boundary.
        log().warn("out of memory planning a size-{} PocketFFT transform", config.size);
        return fail<std::unique_ptr<IFftPlan>>(
            ErrorCode::OutOfMemory, "out of memory planning a size-{} transform", config.size);
    } catch (const std::exception& e) {
        log().warn("PocketFFT could not plan size {}: {}", config.size, e.what());
        return fail<std::unique_ptr<IFftPlan>>(
            ErrorCode::Unknown, "PocketFFT could not plan size {}: {}", config.size, e.what());
    }

    log().debug("planned PocketFFT size {} ({}) in {}", config.size,
                config.inverse ? "inverse" : "forward",
                formatDuration(nsToSeconds(monotonicNs() - started)));
    return std::make_unique<PocketFftPlan>(std::move(transform), config.size, config.inverse);
}

plugin::Logger& log() noexcept {
    static plugin::Logger logger;
    return logger;
}

} // namespace sweeppp::pocket
