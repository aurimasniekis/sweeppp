// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/core/EventBus.hpp"

#include "sweeppp/core/Log.hpp"

#include <algorithm>

namespace sweeppp {

void EventBus::unsubscribe(SubscriptionId id) {
    const std::lock_guard lock(m_mutex);
    for (auto& [type, entries] : m_handlers) {
        std::erase_if(entries, [id](const Entry& entry) { return entry.id == id; });
    }
}

std::size_t EventBus::subscriberCount() const {
    const std::lock_guard lock(m_mutex);
    std::size_t total = 0;
    for (const auto& [type, entries] : m_handlers) {
        total += entries.size();
    }
    return total;
}

void installLoggingSubscribers(EventBus& bus) {
    bus.subscribe<DeviceOpenedEvent>([](const DeviceOpenedEvent& event) {
        logInfo("device", "opened {} ({}{})", event.label, event.deviceId,
                event.serial.empty() ? "" : std::format(", serial {}", event.serial));
    });

    bus.subscribe<DeviceClosedEvent>([](const DeviceClosedEvent& event) {
        logInfo("device", "closed {}{}", event.deviceId,
                event.reason.empty() ? "" : std::format(": {}", event.reason));
    });

    bus.subscribe<DeviceErrorEvent>([](const DeviceErrorEvent& event) {
        logError("device", "{}: {}", event.deviceId, event.message);
    });

    bus.subscribe<ParameterChangedEvent>([](const ParameterChangedEvent& event) {
        logDebug("param", "{} = {}{}", event.key, event.value,
                 event.gridAffecting ? " (grid-affecting: new segment)" : "");
    });

    bus.subscribe<SweepPassEvent>([](const SweepPassEvent& event) {
        logDebug("sweep", "pass {} covered {:.3f}-{:.3f} MHz in {:.3f} s", event.passId,
                 event.startHz / 1e6, event.stopHz / 1e6, event.durationSeconds);
    });

    bus.subscribe<ThrottleChangedEvent>([](const ThrottleChangedEvent& event) {
        logInfo("pipeline", "throttle now '{}' ({:.1f}% of samples processed)", event.reason,
                event.processedFraction * 100.0);
    });

    bus.subscribe<AnnotationEvent>(
        [](const AnnotationEvent& event) { logInfo("annotation", "{}", event.text); });
}

} // namespace sweeppp
