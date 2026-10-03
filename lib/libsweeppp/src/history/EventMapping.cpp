// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/history/EventMapping.hpp"

namespace sweeppp::session {

SessionEvent toSessionEvent(const RetuneEvent& event, std::uint64_t wallNs) {
    return SessionEvent::of(SessionEvent::Kind::Retune, event.monotonicNs, wallNs,
                            RetuneData{.centerHz = event.centerHz, .stepIndex = event.stepIndex});
}

SessionEvent toSessionEvent(const ParameterChangedEvent& event, std::uint64_t wallNs) {
    return SessionEvent::of(
        SessionEvent::Kind::ParameterChanged, event.monotonicNs, wallNs,
        ParameterChangedData{.key = event.key,
                             .value = event.value,
                             .gridAffecting = event.gridAffecting,
                             .calibrationAffecting = event.calibrationAffecting});
}

SessionEvent toSessionEvent(const SweepPassEvent& event, std::uint64_t wallNs) {
    return SessionEvent::of(SessionEvent::Kind::SweepPass, event.monotonicNs, wallNs,
                            SweepPassData{.passId = event.passId,
                                          .startHz = event.startHz,
                                          .stopHz = event.stopHz,
                                          .durationSeconds = event.durationSeconds});
}

SessionEvent toSessionEvent(const ThrottleChangedEvent& event, std::uint64_t wallNs) {
    return SessionEvent::of(
        SessionEvent::Kind::ThrottleChanged, event.monotonicNs, wallNs,
        ThrottleChangedData{.reason = event.reason, .processedFraction = event.processedFraction});
}

SessionEvent toSessionEvent(const AnnotationEvent& event, std::uint64_t wallNs) {
    return SessionEvent::of(
        SessionEvent::Kind::Annotation, event.monotonicNs, wallNs,
        AnnotationData{.text = event.text, .startHz = event.startHz, .stopHz = event.stopHz});
}

SessionEvent toSessionEvent(const MarkerEvent& event, std::uint64_t wallNs) {
    return SessionEvent::of(SessionEvent::Kind::Marker, event.monotonicNs, wallNs,
                            MarkerData{.label = event.label,
                                       .frequencyHz = event.frequencyHz,
                                       .levelDbm = event.levelDbm});
}

SessionEvent toSessionEvent(const DeviceErrorEvent& event, std::uint64_t wallNs) {
    return SessionEvent::of(SessionEvent::Kind::DeviceError, event.monotonicNs, wallNs,
                            DeviceErrorData{.deviceId = event.deviceId, .message = event.message});
}

bool publishSessionEvent(EventBus& bus, const SessionEvent& event, std::uint64_t monotonicNs) {
    switch (event.kindEnum()) {
    case SessionEvent::Kind::Retune:
        if (const auto* body = event.as<RetuneData>()) {
            bus.publish(RetuneEvent{.monotonicNs = monotonicNs,
                                    .centerHz = body->centerHz,
                                    .stepIndex = body->stepIndex});
            return true;
        }
        return false;

    case SessionEvent::Kind::ParameterChanged:
        if (const auto* body = event.as<ParameterChangedData>()) {
            bus.publish(ParameterChangedEvent{.monotonicNs = monotonicNs,
                                              .key = body->key,
                                              .value = body->value,
                                              .gridAffecting = body->gridAffecting,
                                              .calibrationAffecting = body->calibrationAffecting});
            return true;
        }
        return false;

    case SessionEvent::Kind::SweepPass:
        if (const auto* body = event.as<SweepPassData>()) {
            bus.publish(SweepPassEvent{.monotonicNs = monotonicNs,
                                       .passId = body->passId,
                                       .startHz = body->startHz,
                                       .stopHz = body->stopHz,
                                       .durationSeconds = body->durationSeconds});
            return true;
        }
        return false;

    case SessionEvent::Kind::Marker:
        if (const auto* body = event.as<MarkerData>()) {
            bus.publish(MarkerEvent{.monotonicNs = monotonicNs,
                                    .label = body->label,
                                    .frequencyHz = body->frequencyHz,
                                    .levelDbm = body->levelDbm});
            return true;
        }
        return false;

    case SessionEvent::Kind::Annotation:
        if (const auto* body = event.as<AnnotationData>()) {
            bus.publish(AnnotationEvent{.monotonicNs = monotonicNs,
                                        .text = body->text,
                                        .startHz = body->startHz,
                                        .stopHz = body->stopHz});
            return true;
        }
        return false;

    case SessionEvent::Kind::ThrottleChanged:
        if (const auto* body = event.as<ThrottleChangedData>()) {
            bus.publish(ThrottleChangedEvent{.monotonicNs = monotonicNs,
                                             .reason = body->reason,
                                             .processedFraction = body->processedFraction});
            return true;
        }
        return false;

    case SessionEvent::Kind::DeviceError:
        if (const auto* body = event.as<DeviceErrorData>()) {
            bus.publish(DeviceErrorEvent{
                .monotonicNs = monotonicNs, .deviceId = body->deviceId, .message = body->message});
            return true;
        }
        return false;

    case SessionEvent::Kind::SegmentBoundary:
    case SessionEvent::Kind::Alert:
    case SessionEvent::Kind::Plugin:
    default:
        return false;
    }
}

} // namespace sweeppp::session
