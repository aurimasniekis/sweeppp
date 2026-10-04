// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/core/Clock.hpp"
#include "sweeppp/core/Log.hpp"
#include "sweeppp/history/EventMapping.hpp"
#include "sweeppp/history/IFrameSource.hpp"
#include "sweeppp/history/SweepsLog.hpp"

#include <algorithm>
#include <cmath>

namespace sweeppp {
namespace {

using namespace sweeppp::session;

} // namespace

Result<std::unique_ptr<SessionReplay>> SessionReplay::open(const std::filesystem::path& path,
                                                           FrameBus& output, EventBus& events) {
    auto reader = SessionReader::open(path, sweepsLogSink());
    if (!reader) {
        return std::unexpected(reader.error());
    }
    if ((*reader)->segments().empty()) {
        return fail<std::unique_ptr<SessionReplay>>(ErrorCode::Corrupt, "{} contains no segments",
                                                    path.string());
    }

    return std::unique_ptr<SessionReplay>(new SessionReplay(std::move(*reader), output, events));
}

SessionReplay::SessionReplay(std::unique_ptr<SessionReader> reader, FrameBus& output,
                             EventBus& events)
    : m_reader(std::move(reader)), m_output(output), m_events(events) {
    m_positionNs.store(m_reader->summary().firstLineNs, std::memory_order_relaxed);
}

SessionReplay::~SessionReplay() {
    SessionReplay::stop();
}

double SessionReplay::durationSeconds() const noexcept {
    return m_reader->summary().durationSeconds();
}

double SessionReplay::positionSeconds() const noexcept {
    const std::uint64_t position = m_positionNs.load(std::memory_order_relaxed);
    const std::uint64_t first = m_reader->summary().firstLineNs;
    return position > first ? nsToSeconds(position - first) : 0.0;
}

Status SessionReplay::start() {
    if (running()) {
        return fail(ErrorCode::AlreadyExists, "replay is already running");
    }
    m_running.store(true, std::memory_order_release);
    m_thread = std::jthread([this](std::stop_token stop) { replayLoop(stop); });
    return ok();
}

void SessionReplay::stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    m_thread.request_stop();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void SessionReplay::seek(double seconds) {
    const std::uint64_t target =
        m_reader->summary().firstLineNs + secondsToNs(std::max(0.0, seconds));
    m_seekTargetNs.store(target, std::memory_order_relaxed);
    m_seekRequested.store(true, std::memory_order_release);
}

void SessionReplay::setPaused(bool paused) {
    m_paused.store(paused, std::memory_order_relaxed);
}

void SessionReplay::setSpeed(double multiplier) {
    m_speed.store(std::clamp(multiplier, kMinSpeed, kMaxSpeed), std::memory_order_relaxed);
}

void SessionReplay::step() {
    m_pendingSteps.fetch_add(1, std::memory_order_relaxed);
}

void SessionReplay::applyEventsUpTo(std::uint64_t monotonicNs) {
    // Re-publishing the recorded events is what makes replay faithful: the
    // readouts around the waterfall change at the same relative moments they
    // did live, instead of being frozen at whatever the file opened with.
    const std::vector<SessionEvent>& events = m_reader->events();

    while (m_nextEventIndex < events.size() &&
           events[m_nextEventIndex].monotonicNs <= monotonicNs) {
        const SessionEvent& event = events[m_nextEventIndex++];

        // A recorded device error is history rather than a live condition.
        if (event.kindEnum() != SessionEvent::Kind::DeviceError) {
            publishSessionEvent(m_events, event, event.monotonicNs);
        }
    }
}

void SessionReplay::emitLine(std::uint32_t segmentId, std::uint64_t lineNs) {
    if (segmentId >= m_reader->segments().size()) {
        return;
    }
    const SegmentInfo& segment = m_reader->segments()[segmentId];

    auto spectrum = m_reader->spectrumAt(lineNs, segmentId);
    if (!spectrum) {
        return;
    }

    // A replayed frame is a SpectrumFrame like any other, carrying the
    // segment's recorded configuration -- which is exactly why consumers
    // cannot tell replay from live.
    auto frame = std::make_shared<SpectrumFrame>();
    frame->sequence = m_frameSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    frame->hostTimeNs = lineNs;
    frame->wallTimeNs = segment.startWallNs + (lineNs - segment.startMonotonicNs);
    frame->binsDbfs = std::move(*spectrum);
    frame->config = segment.config;
    frame->startHz = segment.grid.startHz;
    frame->binWidthHz = segment.grid.binWidthHz;

    m_output.publish(frame);
}

void SessionReplay::emitLineForPosition(std::uint64_t positionNs) {
    // Whichever segment covers this instant. A session that changed parameters
    // mid-run has several, and replay walks through them in the same order and
    // at the same moments as the original.
    for (const SegmentInfo& segment : m_reader->segments()) {
        const std::uint64_t segmentEnd =
            segment.endMonotonicNs != 0 ? segment.endMonotonicNs : m_reader->summary().lastLineNs;
        if (positionNs >= segment.startMonotonicNs && positionNs <= segmentEnd) {
            emitLine(segment.id, positionNs);
            return;
        }
    }
}

void SessionReplay::replayLoop(std::stop_token stop) {
    const SessionSummary& summary = m_reader->summary();
    const std::uint64_t firstNs = summary.firstLineNs;
    const std::uint64_t lastNs = summary.lastLineNs;

    if (lastNs <= firstNs) {
        m_running.store(false, std::memory_order_release);
        return;
    }

    // Line interval taken from the recorded density, so playback runs at the
    // session's own rate rather than a guess.
    std::uint64_t totalLines = 0;
    for (const SegmentInfo& segment : m_reader->segments()) {
        totalLines += segment.lineCount;
    }
    const std::uint64_t lineIntervalNs =
        totalLines > 0 ? std::max<std::uint64_t>((lastNs - firstNs) / totalLines, 1'000)
                       : 1'000'000;

    std::uint64_t positionNs = m_positionNs.load(std::memory_order_relaxed);
    std::uint64_t wallAnchorNs = monotonicNs();
    std::uint64_t sessionAnchorNs = positionNs;

    while (!stop.stop_requested()) {
        if (m_seekRequested.exchange(false, std::memory_order_acquire)) {
            positionNs =
                std::clamp(m_seekTargetNs.load(std::memory_order_relaxed), firstNs, lastNs);
            // Rewind the event cursor so a backward seek re-applies the events
            // between the new position and wherever playback goes next.
            m_nextEventIndex = 0;
            const std::vector<SessionEvent>& events = m_reader->events();
            while (m_nextEventIndex < events.size() &&
                   events[m_nextEventIndex].monotonicNs < positionNs) {
                ++m_nextEventIndex;
            }
            wallAnchorNs = monotonicNs();
            sessionAnchorNs = positionNs;

            // Published immediately. Otherwise a seek made while paused would
            // leave the transport bar showing the old position until playback
            // resumed -- the scrub would look broken.
            m_positionNs.store(positionNs, std::memory_order_relaxed);
            emitLineForPosition(positionNs);
        }

        const bool isPaused = m_paused.load(std::memory_order_relaxed);
        const std::int64_t steps = m_pendingSteps.load(std::memory_order_relaxed);

        if (isPaused && steps <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            wallAnchorNs = monotonicNs();
            sessionAnchorNs = positionNs;
            continue;
        }

        if (isPaused && steps > 0) {
            m_pendingSteps.fetch_sub(1, std::memory_order_relaxed);
            positionNs = std::min(positionNs + lineIntervalNs, lastNs);
        } else {
            // Pace against wall time so the speed multiplier is honest and a
            // slow consumer cannot stretch playback.
            const double multiplier = m_speed.load(std::memory_order_relaxed);
            const std::uint64_t elapsedWallNs = monotonicNs() - wallAnchorNs;
            const auto targetNs =
                sessionAnchorNs +
                static_cast<std::uint64_t>(static_cast<double>(elapsedWallNs) * multiplier);

            if (targetNs <= positionNs) {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                continue;
            }
            positionNs = std::min(targetNs, lastNs);
        }

        applyEventsUpTo(positionNs);
        emitLineForPosition(positionNs);
        m_positionNs.store(positionNs, std::memory_order_relaxed);

        if (positionNs >= lastNs) {
            logInfo("replay", "reached the end of the session");
            if (m_onComplete) {
                m_onComplete();
            }
            break;
        }
    }

    m_running.store(false, std::memory_order_release);
}

} // namespace sweeppp
