// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace sweeppp {

/// Typed publish/subscribe for things that *happen*, as distinct from data
/// that *flows*.
///
/// Frames go through FrameBus; discrete occurrences -- a device connecting, a
/// retune, a sweep pass completing, a parameter change, and later an alert
/// firing -- go through here. Keeping them apart matters because the session
/// container records both, and it records them differently: frames become
/// tiles, events become the event stream that makes replay faithful.
///
/// Ships now with a logging subscriber only. The alert evaluator will publish
/// here and the alert actions will subscribe, with no change to this class.
class EventBus {
public:
    using SubscriptionId = std::uint64_t;

    /// Subscribes to one event type. The handler runs on the publishing
    /// thread, so it must be quick and must not block; anything expensive
    /// belongs on its own thread behind a queue.
    template <typename Event>
    SubscriptionId subscribe(std::function<void(const Event&)> handler) {
        const std::lock_guard lock(m_mutex);
        const SubscriptionId id = ++m_nextId;

        m_handlers[std::type_index(typeid(Event))].push_back(
            Entry{.id = id, .invoke = [handler = std::move(handler)](const void* event) {
                      handler(*static_cast<const Event*>(event));
                  }});
        return id;
    }

    /// Delivers to every subscriber of `Event`. Handlers are copied out from
    /// under the lock before being called, so a handler may subscribe or
    /// unsubscribe without deadlocking.
    template <typename Event>
    void publish(const Event& event) const {
        std::vector<Entry> handlers;
        {
            const std::lock_guard lock(m_mutex);
            const auto it = m_handlers.find(std::type_index(typeid(Event)));
            if (it == m_handlers.end()) {
                return;
            }
            handlers = it->second;
        }

        for (const Entry& entry : handlers) {
            entry.invoke(&event);
        }
    }

    void unsubscribe(SubscriptionId id);

    /// Number of live subscriptions. Tests, and the plugin panel.
    [[nodiscard]] std::size_t subscriberCount() const;

private:
    struct Entry {
        SubscriptionId id = 0;
        std::function<void(const void*)> invoke;
    };

    mutable std::mutex m_mutex;
    std::unordered_map<std::type_index, std::vector<Entry>> m_handlers;
    SubscriptionId m_nextId = 0;
};

// ---------------------------------------------------------------------------
// Core event types.
//
// These are the occurrences the session event stream records, which is why
// they carry a monotonic timestamp: on replay each is re-applied at the same
// relative moment so the RBW / gain / span readouts change exactly when they
// changed live.
// ---------------------------------------------------------------------------

struct DeviceOpenedEvent {
    std::uint64_t monotonicNs = 0;
    std::string deviceId;
    std::string label;
    std::string serial;
};

struct DeviceClosedEvent {
    std::uint64_t monotonicNs = 0;
    std::string deviceId;
    std::string reason;
};

struct DeviceErrorEvent {
    std::uint64_t monotonicNs = 0;
    std::string deviceId;
    std::string message;
};

/// A device parameter changed. `grid` is true when the change redefines the
/// frequency grid (sample rate, FFT size, span) and therefore must close the
/// current session segment and open a new one -- see history/Segment.
struct ParameterChangedEvent {
    std::uint64_t monotonicNs = 0;
    std::string key;
    std::string value;
    bool gridAffecting = false;
    /// Gain, reference level: does not change the grid, but does shift the
    /// noise floor, so analysis of old tiles must know it happened.
    bool calibrationAffecting = false;
};

struct RetuneEvent {
    std::uint64_t monotonicNs = 0;
    double centerHz = 0.0;
    std::uint32_t stepIndex = 0;
};

struct SweepPassEvent {
    std::uint64_t monotonicNs = 0;
    std::uint64_t passId = 0;
    double startHz = 0.0;
    double stopHz = 0.0;
    double durationSeconds = 0.0;
};

struct MarkerEvent {
    std::uint64_t monotonicNs = 0;
    std::string label;
    double frequencyHz = 0.0;
    double levelDbm = 0.0;
};

/// Free-text annotation attached to a moment, written by the operator or by a
/// plugin analysing history.
struct AnnotationEvent {
    std::uint64_t monotonicNs = 0;
    std::string text;
    double startHz = 0.0;
    double stopHz = 0.0;
};

/// Published when the pipeline changes why (or whether) it is skipping data,
/// so the reason shown to the operator is never stale.
struct ThrottleChangedEvent {
    std::uint64_t monotonicNs = 0;
    std::string reason;
    double processedFraction = 0.0;
};

/// Installs a subscriber that logs every core event. The default wiring, and
/// the placeholder for the alert evaluator that will subscribe here later.
void installLoggingSubscribers(EventBus& bus);

} // namespace sweeppp
