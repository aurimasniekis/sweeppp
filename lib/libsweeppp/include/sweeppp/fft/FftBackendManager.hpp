// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/fft/IFftBackend.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace sweeppp {

/// What the FFT panel shows for one backend.
///
/// Unavailable backends are listed too, with the reason. That is the whole
/// point: "see which backends are available and which are not" is only useful
/// if the not-available ones say *why* -- "no CUDA device", "requires Apple
/// silicon", whatever the library said when it declined to initialise. A
/// backend that silently vanishes teaches the operator nothing.
struct FftBackendInfo {
    std::string name;
    std::string displayName;
    std::string description;
    FftCapabilities capabilities;

    bool available = false;
    std::string unavailableReason;

    /// The backend the app recommends. Nothing is ever selected implicitly --
    /// the active backend is always an explicit user or profile value, and
    /// this only drives which one is preselected on a fresh install.
    bool isSuggestedDefault = false;
};

/// Registry of FFT backends.
///
/// Every backend arrives through a plugin, so this holds nothing of its own:
/// adding one is a module in `plugins/`, not an edit here. A backend that
/// cannot run where it was loaded registers an *unavailable* entry with the
/// reason instead of vanishing.
class FftBackendManager {
public:
    using Factory = std::function<Result<std::unique_ptr<IFftBackend>>()>;

    [[nodiscard]] static FftBackendManager& instance();

    /// Registers an available backend. `factory` is called lazily on first use
    /// so that merely listing backends does not initialise every library.
    void registerBackend(FftBackendInfo info, Factory factory);

    /// Registers a backend that cannot be used in this build or on this
    /// machine, with the reason shown to the operator.
    void registerUnavailable(std::string name, std::string displayName, std::string description,
                             std::string reason);

    /// Withdraws a backend, for a plugin being disabled.
    ///
    /// **Refuses while the backend is in use**, and that refusal is the point.
    /// `acquire()` hands out a raw `IFftBackend*` that the pipeline keeps, and
    /// every `IFftPlan` made from it has a vtable in the same image; dropping
    /// the instance out from under those would be a dangling pointer through
    /// the whole transform path. The caller turns the refusal into the
    /// "requires restart" marker in the UI rather than pretending it worked.
    [[nodiscard]] Status unregisterBackend(std::string_view name);

    /// Whether `name` has been instantiated -- which is what makes it
    /// un-withdrawable. Distinct from `isAvailable`: a backend can be
    /// available and never have been asked for.
    [[nodiscard]] bool isAcquired(std::string_view name) const;

    /// Every backend, available and not, sorted with available ones first.
    [[nodiscard]] std::vector<FftBackendInfo> enumerate() const;

    [[nodiscard]] Result<FftBackendInfo> info(std::string_view name) const;

    /// Instantiates (or returns the already-instantiated) backend. The
    /// returned pointer is owned by the manager and outlives any plan made
    /// from it.
    [[nodiscard]] Result<IFftBackend*> acquire(std::string_view name);

    /// Name of the suggested default, or empty when nothing is available.
    [[nodiscard]] std::string suggestedDefault() const;

    [[nodiscard]] bool isAvailable(std::string_view name) const;

    /// Convenience for the CLI and tests: acquires `name`, or the suggested
    /// default when `name` is empty.
    [[nodiscard]] Result<IFftBackend*> acquireOrDefault(std::string_view name);

private:
    FftBackendManager() = default;

    struct Entry {
        FftBackendInfo info;
        Factory factory;
        std::unique_ptr<IFftBackend> instance;
    };

    mutable std::mutex m_mutex;
    std::vector<Entry> m_entries;
};

} // namespace sweeppp
