// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// Accelerate/vDSP backend. Single precision, one setup shared across N workers.
//
// Two plan strategies, chosen once at plan time rather than branched per
// transform:
//
//   InterleavedPlan  vDSP_DFT_Interleaved_Execute straight onto the caller's
//                    buffers. The ABI already carries interleaved complex, so
//                    this path copies nothing. macOS 12 and later.
//   SplitPlan        vDSP_ctoz -> vDSP_DFT_Execute -> vDSP_ztoc.
//
// The fallback is NOT a rarity, and it is worth knowing which of these a given
// size actually gets. vDSP.h documents the interleaved setup as accepting
// f * 2^n for f in {2,3,5,9,15,25} and n >= 2, but the implementation declines
// everything above 4096 -- measured against the macOS 15 / SDK 26.2 library,
// where 8192 upwards return NULL and fall through here. So the interleaved
// path serves interactive and small-RBW sizes, and every large transform takes
// the split path. `log().debug` records which one a plan took; do not assume.
//
// Scaling: the modern complex-to-complex DFT applies no scale in either
// direction, which is FFTW's convention and the one `dsp::magnitudeToDbfs`
// assumes. The two routines that would break that are the legacy
// `vDSP_fft_zip`, whose inverse scales by 1/N, and the real-to-complex `zrop`,
// whose forward multiplies by 2. Neither is used here.
#include "AccelerateBackend.hpp"

#include <Accelerate/Accelerate.h>
#include <complex>
#include <cstdlib>
#include <string_view>
#include <sweeppp/core/Clock.hpp>
#include <vector>

namespace sweeppp::accel {
namespace {

/// Forces the split-complex fallback, so it is reachable on a machine where
/// the interleaved setup would otherwise always win.
///
/// Without it the fallback is dead code on every macOS 12+ machine and so is
/// never tested; with it the plugin's own suite runs the correctness cases
/// down both paths, and the benchmark can put a number on what the interleave
/// tax would have been.
///
/// Read per plan rather than cached, so a test can set it and immediately plan
/// against it. Planning is serialised by the registry, and a `getenv` per plan
/// is nothing beside building a setup.
[[nodiscard]] bool forceSplitPath() noexcept {
    const char* value = std::getenv("SWEEPPP_ACCELERATE_FORCE_SPLIT");
    return value != nullptr && *value != '\0' && std::string_view{value} != "0";
}

/// Split-path scratch: four N-float arrays laid end to end -- input real,
/// input imaginary, output real, output imaginary.
///
/// Thread-local rather than a member of the plan, and that is the whole reason
/// `threadSafeExecute` can be true: every worker shares one plan, so a buffer
/// hanging off the plan would be precisely the race the capability promises is
/// not there.
[[nodiscard]] float* splitWorkspace(std::size_t size) {
    thread_local std::vector<float> buffer;
    if (buffer.size() < size * 4) {
        buffer.resize(size * 4);
    }
    return buffer.data();
}

/// The no-copy path: vDSP reads and writes the caller's interleaved buffers.
class InterleavedPlan final : public IFftPlan {
public:
    InterleavedPlan(vDSP_DFT_Interleaved_Setup setup, std::size_t size, bool inverse) noexcept
        : m_setup(setup), m_size(size), m_inverse(inverse) {}

    ~InterleavedPlan() override {
        if (m_setup != nullptr) {
            // Safe unconditionally: `Previous` was null at creation, so this
            // setup shares memory with nothing and no other setup's execute
            // can be reading what it frees.
            vDSP_DFT_Interleaved_DestroySetup(m_setup);
        }
    }

    void execute(const std::complex<float>* input, std::complex<float>* output) noexcept override {
        // complex<float> is layout-compatible with float[2] and so with
        // DSPComplex, which is what lets the pipeline's buffers go straight in.
        vDSP_DFT_Interleaved_Execute(m_setup, reinterpret_cast<const DSPComplex*>(input),
                                     reinterpret_cast<DSPComplex*>(output));
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
    vDSP_DFT_Interleaved_Setup m_setup = nullptr;
    std::size_t m_size = 0;
    bool m_inverse = false;
};

/// The fallback: deinterleave, transform split-complex, re-interleave.
class SplitPlan final : public IFftPlan {
public:
    SplitPlan(vDSP_DFT_Setup setup, std::size_t size, bool inverse) noexcept
        : m_setup(setup), m_size(size), m_inverse(inverse) {}

    ~SplitPlan() override {
        if (m_setup != nullptr) {
            vDSP_DFT_DestroySetup(m_setup);
        }
    }

    void execute(const std::complex<float>* input, std::complex<float>* output) noexcept override {
        float* scratch = splitWorkspace(m_size);
        const DSPSplitComplex in{.realp = scratch, .imagp = scratch + m_size};
        const DSPSplitComplex out{.realp = scratch + m_size * 2, .imagp = scratch + m_size * 3};

        // The strides are the trap here. vDSP counts an *interleaved* stride in
        // floats rather than in DSPComplex, for backward compatibility with an
        // interface that predates the struct -- so contiguous complex data is
        // IC = 2, not 1. A stride of 1 would silently read every other float
        // and produce a plausible spectrum of the wrong signal. The split
        // stride IZ is an ordinary element stride and is 1.
        vDSP_ctoz(reinterpret_cast<const DSPComplex*>(input), 2, &in, 1, m_size);
        vDSP_DFT_Execute(m_setup, in.realp, in.imagp, out.realp, out.imagp);
        vDSP_ztoc(&out, 1, reinterpret_cast<DSPComplex*>(output), 2, m_size);
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
    vDSP_DFT_Setup m_setup = nullptr;
    std::size_t m_size = 0;
    bool m_inverse = false;
};

} // namespace

AccelerateBackend::AccelerateBackend() {
    m_capabilities = FftCapabilities{
        .type = FftBackendType::Cpu,
        // 2 * 2^2, the shortest length the interleaved setup accepts. The
        // split path goes lower, but a size only the fallback can serve buys
        // nothing -- no sweep plans an 8-point transform, let alone a 4-point
        // one -- and the floor is one fewer thing to be wrong about.
        .minSize = 8,
        // FFTW's ceiling, so the two backends are comparable and a profile
        // moved between them plans the same sizes.
        .maxSize = 1U << 24U,
        // vDSP actually accepts f * 2^n for f in {2,3,5,9,15,25}, which this
        // enum cannot express -- see the note in the header for why claiming
        // `Any` and snapping cleverly would be worse than claiming less.
        .sizeConstraint = FftSizeConstraint::PowerOfTwo,
        // Honest rather than lazy. Batching exists only in the legacy
        // `vDSP_fftm_*` family, which would force power-of-two-only
        // `vDSP_create_fftsetup` and forfeit the documented concurrency
        // guarantee this backend is built on. The pipeline plans with
        // batchCount = 1 and never calls executeBatch.
        .supportsBatch = false,
        .supportsInPlace = true,
        .threadSafeExecute = true,
        .threadSafePlanning = false,
    };
}

std::string_view AccelerateBackend::name() const noexcept {
    return "accelerate";
}

std::string AccelerateBackend::displayName() const {
    return "Accelerate / vDSP";
}

const FftCapabilities& AccelerateBackend::capabilities() const noexcept {
    return m_capabilities;
}

Result<std::unique_ptr<IFftPlan>> AccelerateBackend::createPlan(const FftPlanConfig& config) {
    if (!supportsSize(config.size)) {
        return fail<std::unique_ptr<IFftPlan>>(
            ErrorCode::InvalidArgument,
            "FFT size {} is not a power of two in the supported range {}..{}", config.size,
            m_capabilities.minSize, m_capabilities.maxSize);
    }
    if (config.batchCount == 0) {
        return fail<std::unique_ptr<IFftPlan>>(ErrorCode::InvalidArgument,
                                               "batch count must be at least 1");
    }

    // `config.quality` has no counterpart here: vDSP chooses its algorithm from
    // the length alone, so all three qualities produce the same setup. That is
    // not a gap to fill later -- it is why changing FFT size costs nothing
    // interactively, where FFTW's Balanced is FFTW_MEASURE.
    const vDSP_DFT_Direction direction = config.inverse ? vDSP_DFT_INVERSE : vDSP_DFT_FORWARD;
    const auto length = static_cast<vDSP_Length>(config.size);

    const std::uint64_t started = monotonicNs();

    // `Previous = nullptr` on both calls, deliberately: setups then share no
    // memory with each other, which removes the "do not destroy a setup while
    // another sharing its memory is executing" hazard entirely, at the cost of
    // some duplicated twiddle tables.
    if (!forceSplitPath()) {
        if (__builtin_available(macOS 12.0, *)) {
            // Signals failure by returning NULL with no error code of any
            // kind, so the null check is the whole diagnosis.
            vDSP_DFT_Interleaved_Setup setup = vDSP_DFT_Interleaved_CreateSetup(
                nullptr, length, direction, vDSP_DFT_Interleaved_ComplextoComplex);
            if (setup != nullptr) {
                log().debug("planned vDSP size {} ({}) on the interleaved path in {}", config.size,
                            config.inverse ? "inverse" : "forward",
                            formatDuration(nsToSeconds(monotonicNs() - started)));
                return std::make_unique<InterleavedPlan>(setup, config.size, config.inverse);
            }
        }
    }

    vDSP_DFT_Setup setup = vDSP_DFT_zop_CreateSetup(nullptr, length, direction);
    if (setup != nullptr) {
        log().debug("planned vDSP size {} ({}) on the split path in {}", config.size,
                    config.inverse ? "inverse" : "forward",
                    formatDuration(nsToSeconds(monotonicNs() - started)));
        return std::make_unique<SplitPlan>(setup, config.size, config.inverse);
    }

    return fail<std::unique_ptr<IFftPlan>>(
        ErrorCode::Unsupported, "vDSP has no implementation for a size-{} transform", config.size);
}

plugin::Logger& log() noexcept {
    static plugin::Logger logger;
    return logger;
}

} // namespace sweeppp::accel
