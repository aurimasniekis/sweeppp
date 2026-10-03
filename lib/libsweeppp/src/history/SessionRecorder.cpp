// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/history/SessionRecorder.hpp"

#include "sweeppp/core/Clock.hpp"
#include "sweeppp/core/Log.hpp"
#include "sweeppp/core/Version.hpp"
#include "sweeppp/history/EventMapping.hpp"
#include "sweeppp/history/SweepsLog.hpp"

#include <utility>

namespace fs = std::filesystem;

namespace sweeppp::session {

Result<std::unique_ptr<SessionRecorder>> SessionRecorder::create(const fs::path& path,
                                                                 RecorderConfig config) {
    const std::size_t queueDepth = config.queueDepth;
    if (!config.log.enabled()) {
        config.log = sweepsLogSink();
    }
    // The manifest's `app_version` names the application that recorded the
    // session, not the library that encoded it. Left empty, the writer would
    // honestly but unhelpfully report libsweepsfile's own version.
    if (config.applicationVersion.empty()) {
        config.applicationVersion = std::string(versionString());
    }

    auto writer = sweeps::SessionWriter::create(path, static_cast<sweeps::WriterConfig>(config));
    if (!writer) {
        return std::unexpected(writer.error());
    }

    auto recorder =
        std::unique_ptr<SessionRecorder>(new SessionRecorder(std::move(*writer), queueDepth));

    // Started only after the file header is on disk, so the worker thread never
    // races the construction it depends on.
    recorder->startWorker();
    return recorder;
}

SessionRecorder::SessionRecorder(std::unique_ptr<sweeps::SessionWriter> writer,
                                 std::size_t queueDepth)
    : AsyncFrameConsumer("session-writer", queueDepth), m_writer(std::move(writer)) {
}

SessionRecorder::~SessionRecorder() {
    // Detached from the bus before anything is torn down. A publisher on
    // another thread must not be able to reach a recorder that is midway
    // through being destroyed.
    if (m_eventBus != nullptr) {
        for (const EventBus::SubscriptionId id : m_subscriptions) {
            m_eventBus->unsubscribe(id);
        }
        m_subscriptions.clear();
        m_eventBus = nullptr;
    }

    shutdown();
    if (!m_closed) {
        (void)close();
    }
}

void SessionRecorder::processFrame(const SpectrumFramePtr& frame) {
    if (m_closed) {
        return;
    }

    // Ahead of the frame, so an event that describes a change lands in the file
    // before the first frame taken under it.
    drainEvents();

    if (!frame) {
        return;
    }

    FrameView view;
    view.bins = frame->binsDbfs.data();
    view.count = frame->binsDbfs.size();
    view.startHz = frame->startHz;
    view.binWidthHz = frame->binWidthHz;
    view.monotonicNs = frame->hostTimeNs;
    view.wallNs = frame->wallTimeNs;
    view.config = &frame->config;

    auto outcome = m_writer->writeFrame(view);
    if (!outcome) {
        logError("session", "{}", outcome.error().describe());
        return;
    }

    if (outcome->segmentOpened) {
        // Queued rather than written here, which is what keeps a segment
        // boundary event where it has always been in the byte stream: after
        // this frame's tiles, at the head of the next frame's drain.
        recordEvent(SessionEvent::of(SessionEvent::Kind::SegmentBoundary, frame->hostTimeNs,
                                     frame->wallTimeNs,
                                     SegmentBoundaryData{.reason = outcome->reason}));
    }
}

Status SessionRecorder::close() {
    if (m_closed) {
        return ok();
    }

    // Drain queued frames first, so the tail of a recording is not lost to a
    // shutdown race.
    flush();
    shutdown();

    // The worker is gone, so this thread is now the only one that can touch the
    // writer -- and the only one that can write out whatever was still queued
    // when it stopped.
    drainEvents();

    const Status written = adopt(m_writer->close());
    m_closed = true;

    // Dropping is the right behaviour for a live display consumer, but for a
    // recording it is a real loss -- so it is stated plainly rather than left
    // for the operator to infer from a short file.
    if (const std::uint64_t dropped = m_eventsDropped.load(std::memory_order_relaxed);
        dropped > 0) {
        logWarn("session", "{}: {} events were dropped; replay will be missing them",
                m_writer->path().filename().string(), dropped);
    }

    if (const std::uint64_t dropped = droppedFrames(); dropped > 0) {
        logWarn("session",
                "{}: {} frames were dropped because the writer could not keep up "
                "({} recorded). Raise queueDepth or lower the frame rate.",
                m_writer->path().filename().string(), dropped, processedFrames());
    }

    return written;
}

void SessionRecorder::recordEvent(const SessionEvent& event) {
    // Deliberately no writer access: it belongs to the recorder thread, and
    // this runs on whichever thread published the event.
    constexpr std::size_t kMaxPending = 8192;

    const std::lock_guard lock(m_eventMutex);
    if (m_pendingEvents.size() >= kMaxPending) {
        m_eventsDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    m_pendingEvents.push_back(event);
}

void SessionRecorder::recordPluginData(std::string pluginId, std::string recordName,
                                       std::uint32_t schemaVersion, std::uint64_t monotonicNs,
                                       std::vector<std::byte> body) {
    // Bounded for the same reason the event queue is: a session whose frames
    // have stopped must not accumulate a plugin's output without limit.
    constexpr std::size_t kMaxPending = 4096;

    const std::lock_guard lock(m_eventMutex);
    if (m_pendingPluginData.size() >= kMaxPending) {
        m_eventsDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    m_pendingPluginData.push_back(PendingPluginData{.pluginId = std::move(pluginId),
                                                    .recordName = std::move(recordName),
                                                    .schemaVersion = schemaVersion,
                                                    .monotonicNs = monotonicNs,
                                                    .body = std::move(body)});
}

void SessionRecorder::drainEvents() {
    std::vector<SessionEvent> events;
    std::vector<PendingPluginData> records;
    {
        const std::lock_guard lock(m_eventMutex);
        events.swap(m_pendingEvents);
        records.swap(m_pendingPluginData);
    }

    for (const SessionEvent& event : events) {
        if (auto written = m_writer->recordEvent(event); !written) {
            logWarn("session", "could not record event: {}", written.error().describe());
            return;
        }
    }

    for (const PendingPluginData& record : records) {
        if (auto written = m_writer->writePluginData(record.pluginId, record.recordName,
                                                     record.schemaVersion, record.monotonicNs,
                                                     record.body.data(), record.body.size());
            !written) {
            logWarn("session", "could not record {}'s data: {}", record.pluginId,
                    written.error().describe());
            return;
        }
    }
}

void SessionRecorder::attachEvents(EventBus& bus) {
    m_eventBus = &bus;

    // Field for field: replay reconstructs these events from the recording
    // alone. Calibration-affecting parameter changes matter especially -- the
    // noise floor shifts, and later analysis of these tiles must know it did.
    const auto record = [this]<typename Event>(const Event& event) {
        recordEvent(toSessionEvent(event, wallClockNs()));
    };
    m_subscriptions.push_back(bus.subscribe<RetuneEvent>(record));
    m_subscriptions.push_back(bus.subscribe<ParameterChangedEvent>(record));
    m_subscriptions.push_back(bus.subscribe<SweepPassEvent>(record));
    m_subscriptions.push_back(bus.subscribe<ThrottleChangedEvent>(record));
    m_subscriptions.push_back(bus.subscribe<AnnotationEvent>(record));
    m_subscriptions.push_back(bus.subscribe<MarkerEvent>(record));
}

} // namespace sweeppp::session
