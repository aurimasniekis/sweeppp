// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/EventBus.hpp"
#include "sweeppp/history/SessionFormat.hpp"

#include <cstdint>

/// Bus events to the session's Event records and back.
///
/// Field for field in both directions: a recording is replayed, and a remote
/// radio's events are republished, from these records alone, so a field left
/// behind here is one no reader can recover.
namespace sweeppp::session {

[[nodiscard]] SessionEvent toSessionEvent(const RetuneEvent& event, std::uint64_t wallNs);
[[nodiscard]] SessionEvent toSessionEvent(const ParameterChangedEvent& event, std::uint64_t wallNs);
[[nodiscard]] SessionEvent toSessionEvent(const SweepPassEvent& event, std::uint64_t wallNs);
[[nodiscard]] SessionEvent toSessionEvent(const ThrottleChangedEvent& event, std::uint64_t wallNs);
[[nodiscard]] SessionEvent toSessionEvent(const AnnotationEvent& event, std::uint64_t wallNs);
[[nodiscard]] SessionEvent toSessionEvent(const MarkerEvent& event, std::uint64_t wallNs);
[[nodiscard]] SessionEvent toSessionEvent(const DeviceErrorEvent& event, std::uint64_t wallNs);

/// Publishes the bus event `event` came from, stamped `monotonicNs`. False for
/// a kind no bus event maps to -- a segment boundary, an alert, a plugin's
/// event, a kind from a newer writer -- or a body that does not match it.
bool publishSessionEvent(EventBus& bus, const SessionEvent& event, std::uint64_t monotonicNs);

} // namespace sweeppp::session
