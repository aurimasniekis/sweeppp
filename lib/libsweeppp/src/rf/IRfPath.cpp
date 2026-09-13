// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/rf/IRfPath.hpp"

#include "sweeppp/core/Log.hpp"

#include <algorithm>
#include <format>

namespace sweeppp {

std::string rfPathKey(const RfPathInfo& info) {
    const std::string& stable = info.serial.empty() ? info.id : info.serial;
    return std::format("{}:{}", info.driver, stable);
}

RfPathManager& RfPathManager::instance() {
    static RfPathManager manager;
    return manager;
}

void RfPathManager::registerFactory(std::unique_ptr<IRfPathFactory> factory) {
    if (!factory) {
        return;
    }

    std::shared_ptr<IRfPathFactory> shared(std::move(factory));

    const std::lock_guard lock(m_mutex);
    const auto existing =
        std::ranges::find_if(m_factories, [&shared](const std::shared_ptr<IRfPathFactory>& c) {
            return c->driver() == shared->driver();
        });
    if (existing != m_factories.end()) {
        *existing = std::move(shared);
        return;
    }
    m_factories.push_back(std::move(shared));
}

bool RfPathManager::hasDriver(std::string_view driver) const {
    const std::lock_guard lock(m_mutex);
    return std::ranges::any_of(m_factories,
                               [driver](const std::shared_ptr<IRfPathFactory>& candidate) {
                                   return candidate->driver() == driver;
                               });
}

Status RfPathManager::unregisterFactory(std::string_view driver) {
    const std::lock_guard lock(m_mutex);

    const auto found =
        std::ranges::find_if(m_factories, [driver](const std::shared_ptr<IRfPathFactory>& c) {
            return c->driver() == driver;
        });
    if (found == m_factories.end()) {
        return fail(ErrorCode::NotFound, "no RF path driver named '{}'", driver);
    }

    if (const std::string blocker = (*found)->withdrawalBlocker(); !blocker.empty()) {
        return fail(ErrorCode::Unavailable, "RF path driver '{}' is still in use: {}", driver,
                    blocker);
    }

    m_factories.erase(found);
    return ok();
}

std::vector<RfPathInfo> RfPathManager::enumerateAll() const {
    // Snapshot under the lock, enumerate outside it: each of these is a bus
    // scan, and holding the registry mutex across all of them would block
    // every other caller for the sum of them.
    std::vector<std::shared_ptr<IRfPathFactory>> factories;
    {
        const std::lock_guard lock(m_mutex);
        factories = m_factories;
    }

    std::vector<RfPathInfo> paths;
    for (const std::shared_ptr<IRfPathFactory>& factory : factories) {
        try {
            std::vector<RfPathInfo> found = factory->enumerate();
            for (RfPathInfo& info : found) {
                if (info.driver.empty()) {
                    info.driver = factory->driver();
                }
            }
            paths.insert(paths.end(), std::make_move_iterator(found.begin()),
                         std::make_move_iterator(found.end()));
        } catch (const std::exception& error) {
            logWarn("rfpath", "driver '{}' failed to enumerate: {}", factory->driver(),
                    error.what());
        }
    }
    return paths;
}

Result<std::unique_ptr<IRfPath>> RfPathManager::open(std::string_view driver, std::string_view id) {
    std::shared_ptr<IRfPathFactory> factory;
    {
        const std::lock_guard lock(m_mutex);
        const auto found =
            std::ranges::find_if(m_factories, [driver](const std::shared_ptr<IRfPathFactory>& c) {
                return c->driver() == driver;
            });
        if (found == m_factories.end()) {
            return fail<std::unique_ptr<IRfPath>>(ErrorCode::NotFound,
                                                  "no RF path driver named '{}'", driver);
        }
        factory = *found;
    }

    // Called with the lock released. The copied shared_ptr is what makes that
    // safe: a plugin withdrawn on another thread meanwhile drops its entry
    // from the vector, not the object this call is standing on.
    return factory->open(id);
}

Result<std::unique_ptr<IRfPath>> RfPathManager::openSpecifier(std::string_view specifier) {
    const std::size_t colon = specifier.find(':');
    const std::string_view driver = specifier.substr(0, colon);
    const std::string_view id =
        colon == std::string_view::npos ? std::string_view{} : specifier.substr(colon + 1);
    return open(driver, id);
}

std::vector<std::string> RfPathManager::drivers() const {
    const std::lock_guard lock(m_mutex);
    std::vector<std::string> names;
    names.reserve(m_factories.size());
    for (const std::shared_ptr<IRfPathFactory>& factory : m_factories) {
        names.emplace_back(factory->driver());
    }
    return names;
}

} // namespace sweeppp
