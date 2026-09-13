// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// FFTW backend. Single precision (fftwf), one plan shared across N workers.
#include "FftwBackend.hpp"

#include <cstdint>
#include <fftw3.h>
#include <format>
#include <mutex>
#include <sweeppp/core/Clock.hpp>

namespace sweeppp::fftw {
namespace {

/// FFTW's planner mutates global state and is explicitly documented as not
/// thread-safe, even though plan *execution* via fftwf_execute_dft is. One
/// process-wide mutex around planning is the whole cost of that guarantee.
std::mutex& plannerMutex() {
    static std::mutex mutex;
    return mutex;
}

int plannerFlags(FftPlanQuality quality) noexcept {
    switch (quality) {
    case FftPlanQuality::Fast:
        return FFTW_ESTIMATE;
    case FftPlanQuality::Balanced:
        return FFTW_MEASURE;
    case FftPlanQuality::Thorough:
        return FFTW_PATIENT;
    }
    return FFTW_MEASURE;
}

/// FFTW-allocated buffer, so plans can rely on SIMD alignment.
class FftwBuffer {
public:
    explicit FftwBuffer(std::size_t count)
        : m_data(static_cast<fftwf_complex*>(fftwf_malloc(sizeof(fftwf_complex) * count))) {}

    ~FftwBuffer() {
        if (m_data != nullptr) {
            fftwf_free(m_data);
        }
    }

    FftwBuffer(const FftwBuffer&) = delete;
    FftwBuffer& operator=(const FftwBuffer&) = delete;

    [[nodiscard]] fftwf_complex* get() const noexcept { return m_data; }
    [[nodiscard]] bool valid() const noexcept { return m_data != nullptr; }

private:
    fftwf_complex* m_data = nullptr;
};

class FftwPlan final : public IFftPlan {
public:
    FftwPlan(fftwf_plan plan, fftwf_plan batchPlan, std::size_t size, std::size_t batchCount,
             bool inverse) noexcept
        : m_plan(plan), m_batchPlan(batchPlan), m_size(size), m_batchCount(batchCount),
          m_inverse(inverse) {}

    ~FftwPlan() override {
        const std::lock_guard lock(plannerMutex());
        if (m_batchPlan != nullptr) {
            fftwf_destroy_plan(m_batchPlan);
        }
        if (m_plan != nullptr) {
            fftwf_destroy_plan(m_plan);
        }
    }

    void execute(const std::complex<float>* input, std::complex<float>* output) noexcept override {
        // The new-array execute variant: this is the call FFTW documents as
        // thread-safe, and the reason every worker can share one plan instead
        // of building its own.
        fftwf_execute_dft(m_plan,
                          reinterpret_cast<fftwf_complex*>(const_cast<std::complex<float>*>(input)),
                          reinterpret_cast<fftwf_complex*>(output));
    }

    void executeBatch(const std::complex<float>* input, std::complex<float>* output,
                      std::size_t count) noexcept override {
        // Use the batched plan for whole multiples of its batch size, then
        // finish the remainder one at a time.
        std::size_t done = 0;
        if (m_batchPlan != nullptr && m_batchCount > 1) {
            while (count - done >= m_batchCount) {
                fftwf_execute_dft(m_batchPlan,
                                  reinterpret_cast<fftwf_complex*>(
                                      const_cast<std::complex<float>*>(input + done * m_size)),
                                  reinterpret_cast<fftwf_complex*>(output + done * m_size));
                done += m_batchCount;
            }
        }
        for (; done < count; ++done) {
            execute(input + done * m_size, output + done * m_size);
        }
    }

    [[nodiscard]] std::size_t size() const noexcept override { return m_size; }
    [[nodiscard]] bool inverse() const noexcept override { return m_inverse; }

private:
    fftwf_plan m_plan = nullptr;
    fftwf_plan m_batchPlan = nullptr;
    std::size_t m_size = 0;
    std::size_t m_batchCount = 1;
    bool m_inverse = false;
};

/// FFTW exposes its version as a string in fftwf_version, but that symbol is
/// not declared in the public header. The build pins the tag, so report that
/// rather than guessing at runtime.
[[nodiscard]] std::string_view fftwVersion() noexcept {
#ifdef SWEEPPP_FFTW_VERSION
    return SWEEPPP_FFTW_VERSION;
#else
    return "3";
#endif
}

} // namespace

FftwBackend::FftwBackend() {
    m_capabilities = FftCapabilities{
        .type = FftBackendType::Cpu,
        .minSize = 2,
        // FFTW handles any size, but composite sizes with large prime factors
        // fall back to a slow path. The planner snaps to powers of two anyway;
        // this is the hard ceiling, chosen so a plan's buffers stay under
        // 256 MiB.
        .maxSize = 1U << 24U,
        .sizeConstraint = FftSizeConstraint::Any,
        .supportsBatch = true,
        .supportsInPlace = true,
        .threadSafeExecute = true,
        .threadSafePlanning = false,
    };
}

std::string_view FftwBackend::name() const noexcept {
    return "fftw";
}

std::string FftwBackend::displayName() const {
    return std::format("FFTW {}", fftwVersion());
}

const FftCapabilities& FftwBackend::capabilities() const noexcept {
    return m_capabilities;
}

Result<std::unique_ptr<IFftPlan>> FftwBackend::createPlan(const FftPlanConfig& config) {
    if (!supportsSize(config.size)) {
        return fail<std::unique_ptr<IFftPlan>>(
            ErrorCode::InvalidArgument, "FFT size {} is outside the supported range {}..{}",
            config.size, m_capabilities.minSize, m_capabilities.maxSize);
    }
    if (config.batchCount == 0) {
        return fail<std::unique_ptr<IFftPlan>>(ErrorCode::InvalidArgument,
                                               "batch count must be at least 1");
    }

    const int sign = config.inverse ? FFTW_BACKWARD : FFTW_FORWARD;
    const int flags = plannerFlags(config.quality);
    const auto n = static_cast<int>(config.size);

    const std::uint64_t started = monotonicNs();
    const std::lock_guard lock(plannerMutex());

    // FFTW_MEASURE and above overwrite their input while planning, so the plan
    // is built against scratch buffers and only ever executed against the
    // caller's via fftwf_execute_dft.
    FftwBuffer scratchIn(config.size * config.batchCount);
    FftwBuffer scratchOut(config.size * config.batchCount);
    if (!scratchIn.valid() || !scratchOut.valid()) {
        return fail<std::unique_ptr<IFftPlan>>(ErrorCode::OutOfMemory,
                                               "could not allocate FFTW scratch for size {} x {}",
                                               config.size, config.batchCount);
    }

    fftwf_complex* out = config.inPlace ? scratchIn.get() : scratchOut.get();

    fftwf_plan plan = fftwf_plan_dft_1d(n, scratchIn.get(), out, sign, flags);
    if (plan == nullptr) {
        return fail<std::unique_ptr<IFftPlan>>(
            ErrorCode::Unknown, "FFTW could not plan a size-{} transform", config.size);
    }

    fftwf_plan batchPlan = nullptr;
    if (config.batchCount > 1) {
        const auto howMany = static_cast<int>(config.batchCount);
        batchPlan = fftwf_plan_many_dft(1, &n, howMany, scratchIn.get(), nullptr, 1, n, out,
                                        nullptr, 1, n, sign, flags);
        // A failed batch plan is not fatal: executeBatch falls back to looping
        // the single-transform plan.
        if (batchPlan == nullptr) {
            log().debug("FFTW declined a batched plan for {} x {}; using the scalar path",
                        config.batchCount, config.size);
        }
    }

    log().debug("planned FFTW size {} ({}, batch {}) in {}", config.size, toString(config.quality),
                config.batchCount, formatDuration(nsToSeconds(monotonicNs() - started)));

    return std::make_unique<FftwPlan>(plan, batchPlan, config.size, config.batchCount,
                                      config.inverse);
}

void FftwBackend::reset() {
    const std::lock_guard lock(plannerMutex());
    fftwf_cleanup();
}

plugin::Logger& log() noexcept {
    static plugin::Logger logger;
    return logger;
}

} // namespace sweeppp::fftw
