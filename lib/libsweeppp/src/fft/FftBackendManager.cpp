// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/fft/FftBackendManager.hpp"

#include "sweeppp/core/Log.hpp"

#include <algorithm>

namespace sweeppp {

FftBackendManager& FftBackendManager::instance() {
    static FftBackendManager manager;
    return manager;
}

void FftBackendManager::registerBackend(FftBackendInfo info, Factory factory) {
    const std::lock_guard lock(m_mutex);

    const auto existing = std::ranges::find_if(
        m_entries, [&info](const Entry& entry) { return entry.info.name == info.name; });
    if (existing != m_entries.end()) {
        // Re-registration replaces: a plugin providing a better build of a
        // backend already present should win rather than be ignored.
        existing->info = std::move(info);
        existing->factory = std::move(factory);
        existing->instance.reset();
        return;
    }

    info.available = true;
    m_entries.push_back(Entry{.info = std::move(info), .factory = std::move(factory)});
}

void FftBackendManager::registerUnavailable(std::string name, std::string displayName,
                                            std::string description, std::string reason) {
    const std::lock_guard lock(m_mutex);

    const auto existing = std::ranges::find_if(
        m_entries, [&name](const Entry& entry) { return entry.info.name == name; });
    if (existing != m_entries.end()) {
        // An available registration always beats an unavailable one, whichever
        // order they arrive in.
        return;
    }

    m_entries.push_back(Entry{.info = FftBackendInfo{.name = std::move(name),
                                                     .displayName = std::move(displayName),
                                                     .description = std::move(description),
                                                     .capabilities = {},
                                                     .available = false,
                                                     .unavailableReason = std::move(reason),
                                                     .isSuggestedDefault = false},
                              .factory = nullptr});
}

Status FftBackendManager::unregisterBackend(std::string_view name) {
    const std::lock_guard lock(m_mutex);

    const auto entry = std::ranges::find_if(
        m_entries, [name](const Entry& candidate) { return candidate.info.name == name; });
    if (entry == m_entries.end()) {
        return fail(ErrorCode::NotFound, "no FFT backend named '{}'", name);
    }
    if (entry->instance) {
        return fail(ErrorCode::Unavailable,
                    "FFT backend '{}' has been acquired and cannot be withdrawn while the "
                    "pipeline may still hold plans made from it",
                    name);
    }

    m_entries.erase(entry);
    return ok();
}

bool FftBackendManager::isAcquired(std::string_view name) const {
    const std::lock_guard lock(m_mutex);
    const auto entry = std::ranges::find_if(
        m_entries, [name](const Entry& candidate) { return candidate.info.name == name; });
    return entry != m_entries.end() && entry->instance != nullptr;
}

std::vector<FftBackendInfo> FftBackendManager::enumerate() const {
    const std::lock_guard lock(m_mutex);

    std::vector<FftBackendInfo> result;
    result.reserve(m_entries.size());
    for (const Entry& entry : m_entries) {
        result.push_back(entry.info);
    }

    // Available first, suggested default at the very top, then alphabetical.
    // Unavailable entries stay in the list -- they are informative, not noise.
    std::ranges::stable_sort(result, [](const FftBackendInfo& a, const FftBackendInfo& b) {
        if (a.available != b.available) {
            return a.available;
        }
        if (a.isSuggestedDefault != b.isSuggestedDefault) {
            return a.isSuggestedDefault;
        }
        return a.displayName < b.displayName;
    });

    return result;
}

Result<FftBackendInfo> FftBackendManager::info(std::string_view name) const {
    const std::lock_guard lock(m_mutex);

    const auto entry = std::ranges::find_if(
        m_entries, [name](const Entry& candidate) { return candidate.info.name == name; });
    if (entry == m_entries.end()) {
        return fail<FftBackendInfo>(ErrorCode::NotFound, "no FFT backend named '{}'", name);
    }
    return entry->info;
}

bool FftBackendManager::isAvailable(std::string_view name) const {
    const std::lock_guard lock(m_mutex);
    const auto entry = std::ranges::find_if(
        m_entries, [name](const Entry& candidate) { return candidate.info.name == name; });
    return entry != m_entries.end() && entry->info.available;
}

Result<IFftBackend*> FftBackendManager::acquire(std::string_view name) {
    const std::lock_guard lock(m_mutex);

    const auto entry = std::ranges::find_if(
        m_entries, [name](const Entry& candidate) { return candidate.info.name == name; });
    if (entry == m_entries.end()) {
        return fail<IFftBackend*>(ErrorCode::NotFound, "no FFT backend named '{}'", name);
    }
    if (!entry->info.available) {
        return fail<IFftBackend*>(ErrorCode::Unsupported, "FFT backend '{}' is unavailable: {}",
                                  name, entry->info.unavailableReason);
    }
    if (entry->instance) {
        return entry->instance.get();
    }

    auto created = entry->factory();
    if (!created) {
        // A backend that advertised itself as available but then failed to
        // initialise becomes unavailable-with-reason rather than an error the
        // caller sees repeatedly.
        entry->info.available = false;
        entry->info.unavailableReason = created.error().message();
        logWarn("fft", "backend '{}' failed to initialise: {}", name, created.error().describe());
        return std::unexpected(created.error());
    }

    entry->instance = std::move(*created);
    return entry->instance.get();
}

std::string FftBackendManager::suggestedDefault() const {
    const std::lock_guard lock(m_mutex);

    for (const Entry& entry : m_entries) {
        if (entry.info.available && entry.info.isSuggestedDefault) {
            return entry.info.name;
        }
    }
    for (const Entry& entry : m_entries) {
        if (entry.info.available) {
            return entry.info.name;
        }
    }
    return {};
}

Result<IFftBackend*> FftBackendManager::acquireOrDefault(std::string_view name) {
    if (!name.empty()) {
        return acquire(name);
    }

    const std::string fallback = suggestedDefault();
    if (fallback.empty()) {
        // The sentence the GUI and the CLI show when start-up fails, so it
        // names the missing piece rather than the empty registry: every
        // backend arrives through a plugin, and with none loaded there is no
        // transform to run.
        return fail<IFftBackend*>(
            ErrorCode::Unavailable,
            "no FFT backend is available -- install an FFT backend plugin such as "
            "sweeppp-plugin-fft-pocketfft or sweeppp-plugin-fft-fftw");
    }
    return acquire(fallback);
}

} // namespace sweeppp
