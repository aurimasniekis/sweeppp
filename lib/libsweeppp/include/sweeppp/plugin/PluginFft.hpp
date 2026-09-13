// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/// Writing a Sweep++ FFT backend as an ordinary C++ class.
///
/// A backend derives from `IFftBackend` and its plans from `IFftPlan` -- the
/// application's own interfaces, unchanged. There is no plugin-side mirror of
/// them because there is nothing to mirror: both are pure value interfaces with
/// no singleton behind them, so what a plugin adds is this vtable and nothing
/// else.
///
/// This header DOES include libsweeppp's FFT interface header, which the one
/// rule permits: `IFftBackend`, `IFftPlan` and `FftCapabilities` are pure
/// computation over their arguments, with no per-image state behind them. It
/// still touches no singleton -- in particular not
/// `FftBackendManager::instance()`, which inside a plugin is a registry the
/// host never sees.
///
/// The shape of a backend:
///
///     class MyFft : public sweeppp::IFftBackend { ... };
///
///     MyFft& backend() { static MyFft instance; return instance; }
///     const auto kVtable = sweeppp::plugin::makeFftBackendVtable<MyFft>();
///     // facet(SWEEPPP_FACET_FFT_BACKEND, "myfft", ..., &kVtable)
///     // host.registerFacet(facet, &backend());

#include "sweeppp/core/Result.hpp"
#include "sweeppp/fft/IFftBackend.hpp"
#include "sweeppp/plugin/Plugin.hpp"
#include "sweeppp/plugin/PluginStatus.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace sweeppp::plugin {

// ----------------------------------------------------------- conversions

/// Whether a struct the host built reaches as far as the end of a field.
///
/// The `struct_size` rule, on the plugin's side of it: both sides read only
/// `min(their size, the other's)`. A `struct_size` of 0 fails every check,
/// which is what makes a zeroed struct inert rather than one full of plausible
/// defaults.
#define SWEEPPP_PLUGIN_ABI_HAS(value, field)                                                       \
    ((value).struct_size >=                                                                        \
     offsetof(std::remove_cvref_t<decltype(value)>, field) + sizeof((value).field))

[[nodiscard]] constexpr sweeppp_fft_capabilities_t
toAbiCapabilities(const FftCapabilities& capabilities) noexcept {
    return sweeppp_fft_capabilities_t{
        .struct_size = sizeof(sweeppp_fft_capabilities_t),
        .is_gpu = capabilities.type == FftBackendType::Gpu ? 1 : 0,
        .min_size = capabilities.minSize,
        .max_size = capabilities.maxSize,
        .power_of_two_only = capabilities.sizeConstraint == FftSizeConstraint::PowerOfTwo ? 1 : 0,
        .supports_batch = capabilities.supportsBatch ? 1 : 0,
        .supports_in_place = capabilities.supportsInPlace ? 1 : 0,
        .thread_safe_execute = capabilities.threadSafeExecute ? 1 : 0,
        .thread_safe_planning = capabilities.threadSafePlanning ? 1 : 0,
    };
}

/// The ABI carries the quality as 0 fast / 1 balanced / 2 thorough, which is
/// `FftPlanQuality`'s own numbering. Switched rather than cast anyway, so that
/// a value from a host built against a later ABI lands on the default instead
/// of on an enumerator that does not exist here.
[[nodiscard]] constexpr FftPlanQuality fromAbiQuality(std::int32_t quality) noexcept {
    switch (quality) {
    case 0:
        return FftPlanQuality::Fast;
    case 2:
        return FftPlanQuality::Thorough;
    default:
        break;
    }
    return FftPlanQuality::Balanced;
}

/// Reads only as far as the host's own struct reaches; anything beyond that
/// keeps `FftPlanConfig`'s default.
[[nodiscard]] inline FftPlanConfig fromAbiConfig(const sweeppp_fft_plan_config_t& config) noexcept {
    FftPlanConfig wanted;
    if (SWEEPPP_PLUGIN_ABI_HAS(config, size)) {
        wanted.size = config.size;
    }
    if (SWEEPPP_PLUGIN_ABI_HAS(config, batch_count)) {
        wanted.batchCount = config.batch_count;
    }
    if (SWEEPPP_PLUGIN_ABI_HAS(config, inverse)) {
        wanted.inverse = config.inverse != 0;
    }
    if (SWEEPPP_PLUGIN_ABI_HAS(config, in_place)) {
        wanted.inPlace = config.in_place != 0;
    }
    if (SWEEPPP_PLUGIN_ABI_HAS(config, quality)) {
        wanted.quality = fromAbiQuality(config.quality);
    }
    return wanted;
}

#undef SWEEPPP_PLUGIN_ABI_HAS

// ---------------------------------------------------------------- thunks

namespace detail {

/// `void* plan` IS the `IFftPlan*`, with nothing wrapped around it.
///
/// A plan writes its answers into the caller's own buffers, so there is no
/// scratch of ours to park beside it. The base has a virtual destructor, so
/// `delete` through it is well-founded.
[[nodiscard]] inline IFftPlan* planOf(void* plan) noexcept {
    return static_cast<IFftPlan*>(plan);
}

inline void planDestroyThunk(void* plan) {
    guard([plan] { delete planOf(plan); });
}

inline void planExecuteThunk(void* plan, const float* input, float* output) {
    guard([plan, input, output] {
        // complex<float> is specified to be layout-compatible with float[2],
        // which is what lets the ABI carry interleaved floats without a copy.
        planOf(plan)->execute(reinterpret_cast<const std::complex<float>*>(input),
                              reinterpret_cast<std::complex<float>*>(output));
    });
}

inline void planExecuteBatchThunk(void* plan, const float* input, float* output,
                                  std::size_t count) {
    guard([plan, input, output, count] {
        planOf(plan)->executeBatch(reinterpret_cast<const std::complex<float>*>(input),
                                   reinterpret_cast<std::complex<float>*>(output), count);
    });
}

} // namespace detail

/// The one plan vtable, shared by every backend written against `IFftPlan`.
///
/// One table rather than one per backend because every entry dispatches through
/// the base class anyway.
[[nodiscard]] inline const sweeppp_fft_plan_vtable_t& fftPlanVtable() noexcept {
    static const sweeppp_fft_plan_vtable_t kVtable{
        .struct_size = sizeof(sweeppp_fft_plan_vtable_t),
        .destroy = &detail::planDestroyThunk,
        .execute = &detail::planExecuteThunk,
        .execute_batch = &detail::planExecuteBatchThunk,
    };
    return kVtable;
}

/// Builds a backend vtable forwarding to `T`, which must derive from
/// `IFftBackend`.
///
/// Store the result in something with static storage duration: the host keeps
/// the pointer for the life of the process.
///
/// One thing does not cross: `create_plan` answers with a status and nothing
/// else, so the sentence in `createPlan`'s `Result` stops here. Nothing is
/// silently swallowed -- the host formats "FFT backend '{}' could not plan size
/// {}: {}" from the status name -- but a backend with something specific to say
/// about a size it refused should log it as well as returning it.
template <typename T>
[[nodiscard]] sweeppp_fft_backend_vtable_t makeFftBackendVtable() {
    static_assert(std::is_base_of_v<IFftBackend, T>,
                  "an FFT facet's instance must be an IFftBackend");

    sweeppp_fft_backend_vtable_t vtable{};
    vtable.struct_size = sizeof(sweeppp_fft_backend_vtable_t);

    vtable.capabilities = [](void* instance,
                             sweeppp_fft_capabilities_t* out) -> sweeppp_plugin_status_t {
        auto status = SWEEPPP_PLUGIN_ERR_UNKNOWN;
        detail::guard([instance, out, &status] {
            if (out == nullptr) {
                status = SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
                return;
            }
            *out = toAbiCapabilities(static_cast<const T*>(instance)->capabilities());
            status = SWEEPPP_PLUGIN_OK;
        });
        return status;
    };

    vtable.create_plan =
        [](void* instance, const sweeppp_fft_plan_config_t* config, void** plan,
           const sweeppp_fft_plan_vtable_t** planVtable) -> sweeppp_plugin_status_t {
        auto status = SWEEPPP_PLUGIN_ERR_UNKNOWN;
        detail::guard([instance, config, plan, planVtable, &status] {
            if (config == nullptr || plan == nullptr || planVtable == nullptr) {
                status = SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
                return;
            }

            Result<std::unique_ptr<IFftPlan>> created =
                static_cast<T*>(instance)->createPlan(fromAbiConfig(*config));
            if (!created) {
                status = toAbiStatus(created.error().code());
                return;
            }
            if (*created == nullptr) {
                // Success with no plan. The thunks below dereference what goes
                // out through `plan`, so this is refused here rather than
                // handed over to be dereferenced by the host's first execute.
                status = SWEEPPP_PLUGIN_ERR_UNKNOWN;
                return;
            }

            *plan = created->release();
            *planVtable = &fftPlanVtable();
            status = SWEEPPP_PLUGIN_OK;
        });
        return status;
    };

    vtable.snap_size = [](void* instance, std::size_t desired) -> std::size_t {
        // The caller's own figure if anything goes wrong: a snap that answered
        // zero would be planned against, and a plan of size zero fails much
        // further from the cause.
        std::size_t size = desired;
        detail::guard([instance, desired, &size] {
            size = static_cast<const T*>(instance)->snapSize(desired);
        });
        return size;
    };

    vtable.reset = [](void* instance) {
        detail::guard([instance] { static_cast<T*>(instance)->reset(); });
    };

    return vtable;
}

} // namespace sweeppp::plugin
