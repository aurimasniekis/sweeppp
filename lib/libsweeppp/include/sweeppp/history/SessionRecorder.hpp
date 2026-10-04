// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/EventBus.hpp"
#include "sweeppp/core/Result.hpp"
#include "sweeppp/history/SessionFormat.hpp"
#include "sweeppp/pipeline/AsyncFrameConsumer.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sweeps/SessionWriter.hpp>
#include <vector>

namespace sweeppp::session {

using sweeps::FrameOutcome;
using sweeps::FrameView;

/// Recording configuration: the library writer's, plus what only a threaded
/// consumer needs.
struct RecorderConfig : sweeps::WriterConfig {
    /// Frames buffered before the recorder starts dropping. The recorder is an
    /// AsyncFrameConsumer, so a slow disk costs frames here rather than
    /// stalling acquisition.
    std::size_t queueDepth = 256;
};

/// Records a live session to a `.sweeps` file.
///
/// This is the threading and event-plumbing that libsweepsfile deliberately
/// does not contain. The library's writer is synchronous and owns no thread;
/// this attaches it to the FrameBus as an ordinary consumer, and because it
/// derives from AsyncFrameConsumer, a slow disk can never stall the radio.
class SessionRecorder final : public AsyncFrameConsumer {
public:
    [[nodiscard]] static Result<std::unique_ptr<SessionRecorder>>
    create(const std::filesystem::path& path, RecorderConfig config = {});

    ~SessionRecorder() override;

    /// Queues an event for the stream. Events are what make replay faithful:
    /// they reproduce the readouts around the waterfall, not just the
    /// waterfall.
    ///
    /// Callable from any thread, and deliberately does not write: retune events
    /// arrive on the sweep thread thousands of times a second, and a
    /// std::ofstream driven from two threads at once walks its put pointers
    /// past the end of its own buffer. The event is serialised later, on the
    /// recorder's thread, which is the only one that ever touches the writer.
    void recordEvent(const SessionEvent& event);

    /// Queues an opaque producer record -- a plugin's own analysis, alongside
    /// the sweep it analysed rather than in a sidecar file.
    ///
    /// Same threading contract as recordEvent, and for the same reason: a
    /// plugin calling this from its frame-processor thread must not become the
    /// second thread touching the writer. `body` is copied here and serialised
    /// later, on the recorder's own thread.
    ///
    /// A `monotonicNs` of 0 means the record is not tied to a moment, which is
    /// what keeps it in an extraction whatever time range was asked for.
    void recordPluginData(std::string pluginId, std::string recordName, std::uint32_t schemaVersion,
                          std::uint64_t monotonicNs, std::vector<std::byte> body);

    /// Subscribes to an EventBus so core events land in the session
    /// automatically.
    void attachEvents(EventBus& bus);

    /// Keeps only frames that complete a sweep pass, for a recording at a
    /// resolution where a sweep's partial frames would be most of the file.
    /// Off while tuned, where every frame is a line. Callable from any thread.
    void setCompletePassesOnly(bool enabled) noexcept { m_completePassesOnly.store(enabled); }

    /// Finishes the file. Idempotent, and called by the destructor -- but a
    /// session closed by power loss is still readable, by design.
    [[nodiscard]] Status close();

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_writer->path(); }
    /// As of the recorder thread's last write. Callable from any thread.
    [[nodiscard]] std::uint64_t bytesWritten() const noexcept { return m_bytesWritten.load(); }
    [[nodiscard]] std::uint64_t linesWritten() const noexcept { return m_linesWritten.load(); }
    [[nodiscard]] std::uint32_t segmentCount() const noexcept { return m_writer->segmentCount(); }

    [[nodiscard]] bool retentionReached() const noexcept { return m_writer->retentionReached(); }
    [[nodiscard]] const std::string& retentionReason() const noexcept {
        return m_writer->retentionReason();
    }

    /// Events dropped because the pending queue was full.
    [[nodiscard]] std::uint64_t droppedEvents() const noexcept {
        return m_pending->dropped.load(std::memory_order_relaxed);
    }

protected:
    void processFrame(const SpectrumFramePtr& frame) override;

private:
    SessionRecorder(std::unique_ptr<sweeps::SessionWriter> writer, std::size_t queueDepth);

    /// Serialises everything recordEvent() and recordPluginData() have queued.
    /// Recorder thread only.
    void drainEvents();

    /// An opaque record waiting to be serialised.
    struct PendingPluginData {
        std::string pluginId;
        std::string recordName;
        std::uint32_t schemaVersion = 0;
        std::uint64_t monotonicNs = 0;
        std::vector<std::byte> body;
    };

    std::unique_ptr<sweeps::SessionWriter> m_writer;
    bool m_closed = false;

    std::vector<EventBus::SubscriptionId> m_subscriptions;
    EventBus* m_eventBus = nullptr;

    /// What waits to be serialised by the recorder thread. Bounded, because a
    /// session whose frames have stopped must not accumulate retunes without
    /// limit; overflow is counted rather than allowed to grow.
    ///
    /// Shared with the bus handlers rather than reached through `this`: a bus
    /// calls a handler outside its lock, so one already under way when the
    /// recorder unsubscribes still runs -- into this, which it keeps alive,
    /// and not into a recorder being destroyed.
    struct Pending {
        std::mutex mutex;
        std::vector<SessionEvent> events;
        std::vector<PendingPluginData> pluginData;
        std::atomic<std::uint64_t> dropped{0};

        void push(const SessionEvent& event);
    };
    std::shared_ptr<Pending> m_pending = std::make_shared<Pending>();
    std::atomic<bool> m_completePassesOnly{false};

    /// The writer's counts, published by whichever thread wrote last: the
    /// writer's own are plain fields that thread alone may read.
    void publishCounts() noexcept;
    std::atomic<std::uint64_t> m_bytesWritten{0};
    std::atomic<std::uint64_t> m_linesWritten{0};
};

} // namespace sweeppp::session
