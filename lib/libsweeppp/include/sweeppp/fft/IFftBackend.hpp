// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace sweeppp {

enum class FftBackendType : std::uint8_t { Cpu, Gpu };

[[nodiscard]] std::string_view toString(FftBackendType type) noexcept;

enum class FftSizeConstraint : std::uint8_t {
    PowerOfTwo, ///< Only 2^k accepted.
    Any,        ///< Any size, though composite sizes may be faster.
};

/// How much time to spend choosing an algorithm before running it.
///
/// Matters more than it looks: at sweep rates a plan is built per FFT size and
/// reused for millions of transforms, so Measure pays for itself -- but a user
/// changing FFT size interactively must not wait seconds for it.
enum class FftPlanQuality : std::uint8_t {
    Fast,     ///< Heuristic only. Instant planning; interactive changes.
    Balanced, ///< Brief measurement. The default.
    Thorough, ///< Exhaustive. For a long unattended run.
};

[[nodiscard]] std::string_view toString(FftPlanQuality quality) noexcept;

struct FftCapabilities {
    FftBackendType type = FftBackendType::Cpu;
    std::size_t minSize = 2;
    std::size_t maxSize = 1U << 24U;
    FftSizeConstraint sizeConstraint = FftSizeConstraint::Any;

    /// Can transform many blocks in one call. Matters at sweep rates, where
    /// per-call overhead across thousands of small FFTs is measurable.
    bool supportsBatch = false;

    bool supportsInPlace = true;

    /// Whether one plan may be executed concurrently from several threads with
    /// separate buffers. This is what lets N workers share a plan instead of
    /// each building its own; FFTW guarantees it for fftwf_execute_dft.
    bool threadSafeExecute = false;

    /// Plan construction itself is usually NOT thread-safe even where
    /// execution is, so the manager serialises it.
    bool threadSafePlanning = false;
};

struct FftPlanConfig {
    std::size_t size = 4096;
    std::size_t batchCount = 1;
    bool inverse = false;
    bool inPlace = false;
    FftPlanQuality quality = FftPlanQuality::Balanced;
};

/// A prepared transform.
///
/// `execute` takes explicit input and output pointers rather than owning
/// buffers, precisely so one plan can serve every worker thread. A plan that
/// owned its scratch would force one plan per worker and multiply planning
/// cost by the worker count.
class IFftPlan {
public:
    virtual ~IFftPlan() = default;

    IFftPlan(const IFftPlan&) = delete;
    IFftPlan& operator=(const IFftPlan&) = delete;

    /// One transform of `size()` complex samples. Thread-safe when the
    /// backend advertises `threadSafeExecute` and the buffers are per-thread.
    virtual void execute(const std::complex<float>* input,
                         std::complex<float>* output) noexcept = 0;

    /// `count` consecutive transforms. Backends without native batching loop
    /// over execute(), so this is always callable.
    virtual void executeBatch(const std::complex<float>* input, std::complex<float>* output,
                              std::size_t count) noexcept = 0;

    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual bool inverse() const noexcept = 0;

protected:
    IFftPlan() = default;
};

class IFftBackend {
public:
    virtual ~IFftBackend() = default;

    IFftBackend(const IFftBackend&) = delete;
    IFftBackend& operator=(const IFftBackend&) = delete;

    /// Stable identifier used in profiles and on the command line, e.g "fftw".
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// Human-readable name for the UI, e.g. "FFTW 3.3.10".
    [[nodiscard]] virtual std::string displayName() const = 0;

    [[nodiscard]] virtual const FftCapabilities& capabilities() const noexcept = 0;

    [[nodiscard]] virtual Result<std::unique_ptr<IFftPlan>>
    createPlan(const FftPlanConfig& config) = 0;

    /// Nearest size this backend accepts, at or above `desired`. The sweep
    /// planner uses it to snap `sampleRate / RBW` onto a legal size instead of
    /// failing on an awkward number.
    [[nodiscard]] virtual std::size_t snapSize(std::size_t desired) const noexcept;

    /// Whether `size` is directly usable.
    [[nodiscard]] virtual bool supportsSize(std::size_t size) const noexcept;

    /// Discards cached plans and wisdom. Called when the user switches
    /// backends so an unused one stops holding memory.
    virtual void reset() {}

protected:
    IFftBackend() = default;
};

/// Builds a plan, serialising construction process-wide when the backend
/// declares `threadSafePlanning == false`.
///
/// This is where the promise `FftCapabilities::threadSafePlanning` makes is
/// actually kept, and it is the call every planner should use in place of
/// `backend.createPlan`. It stopped being theoretical once the FFT benchmark
/// existed: that runs on a thread of its own, while the operator can still
/// change FFT size and set the pipeline planning on another.
///
/// A free function rather than a member of `FftBackendManager`, so that
/// planning does not drag the registry singleton into `Pipeline` -- which
/// takes an `IFftBackend&` precisely so that it needs no registry at all.
[[nodiscard]] Result<std::unique_ptr<IFftPlan>> createPlanSerialised(IFftBackend& backend,
                                                                     const FftPlanConfig& config);

} // namespace sweeppp
