// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/remote/RemoteServer.hpp"

#include "remote/StreamIo.hpp"
#include "sweeppp/core/Clock.hpp"
#include "sweeppp/core/Log.hpp"
#include "sweeppp/history/EventMapping.hpp"
#include "sweeppp/history/SessionRecorder.hpp"
#include "sweeppp/net/Socket.hpp"
#include "sweeppp/remote/FrameCodec.hpp"
#include "sweeppp/remote/Handshake.hpp"
#include "sweeppp/remote/Mdns.hpp"
#include "sweeppp/remote/Messages.hpp"
#include "sweeppp/remote/WireCodec.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sweeps/Records.hpp>
#include <sweeps/Stream.hpp>
#include <thread>
#include <utility>
#include <vector>

namespace sweeppp::remote {
namespace {

using namespace std::chrono_literals;
using io::asBytes;
using io::Clock;
using io::kReadSlice;
using io::kReceiveChunk;
using sweeps::Metadata;

constexpr auto kAcceptSlice = 200ms;
constexpr auto kControlInterval = 33ms;
constexpr auto kStateInterval = 100ms;
constexpr auto kTelemetryInterval = 250ms;
constexpr std::size_t kMaxPendingHandshakes = 8;
constexpr std::size_t kMaxQueuedEvents = 4096;

std::vector<std::byte> messageBytes(std::string_view name, const Metadata& body,
                                    std::uint64_t monotonicNs = 0) {
    std::vector<std::byte> out;
    appendMessage(out, name, body, monotonicNs);
    return out;
}

bool isControl(const sweeps::StreamRecord& record) {
    return record.header.type == static_cast<std::uint16_t>(sweeps::RecordType::PluginData);
}

/// An event record on its way out; a Retune is the first thing dropped when
/// the link is behind, since the next one supersedes it within milliseconds.
struct OutgoingEvent {
    std::vector<std::byte> bytes;
    bool droppable = false;
};

/// The ops whose last instance in a batch makes the earlier ones moot.
bool supersedes(std::string_view op) {
    return op == op::kApplySweepPlan || op == op::kSweepRange || op == op::kApplyPipelineConfig ||
           op == op::kSetCorrectionSettings || op == op::kSetUserAntennas ||
           op == op::kSetAssignments || op == op::kSetFftBackend;
}

} // namespace

// ---------------------------------------------------------------- the session

/// One authenticated client: its socket, the thread reading it, the thread
/// writing to it, and what is queued for the latter.
class Session {
public:
    Session(net::SecureChannel socket, sweeps::RecordFramer framer,
            std::chrono::milliseconds silenceTimeout)
        : m_socket(std::move(socket)), m_framer(std::move(framer)), m_peer(m_socket.peerAddress()),
          m_silenceTimeout(silenceTimeout) {}

    ~Session() { join(); }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    template <typename OnCommand, typename OnEnded>
    void run(OnCommand onCommand, OnEnded onEnded) {
        m_reader = std::thread([this, onCommand, onEnded] {
            readLoop(onCommand);
            end();
            onEnded();
        });
        m_sender = std::thread([this] { sendLoop(); });
    }

    [[nodiscard]] bool alive() const noexcept { return m_alive.load(); }

    /// Whether the client ended it, rather than the network.
    [[nodiscard]] bool saidGoodbye() const noexcept { return m_saidGoodbye.load(); }
    [[nodiscard]] const std::string& peer() const noexcept { return m_peer; }

    /// Ends the session from outside: both threads wake and leave.
    void end() {
        m_alive.store(false);
        m_socket.shutdown();
        m_wake.notify_all();
    }

    void join() {
        end();
        if (m_reader.joinable()) {
            m_reader.join();
        }
        if (m_sender.joinable()) {
            m_sender.join();
        }
    }

    /// Sends `bye`, what remains queued and EndOfStream, then closes. Waits
    /// for the sender to finish so the goodbye is on the wire before return.
    void finish(std::string_view reason) {
        {
            const std::lock_guard lock(m_mutex);
            m_control.push_back(
                messageBytes(msg::kBye, Bye{.reason = std::string(reason)}.toMetadata()));
            m_finishing = true;
        }
        m_wake.notify_all();
        if (m_sender.joinable()) {
            m_sender.join();
        }
        join();
    }

    void pushControl(std::vector<std::byte> bytes) {
        {
            const std::lock_guard lock(m_mutex);
            m_control.push_back(std::move(bytes));
        }
        m_wake.notify_all();
    }

    /// A download's chunk: sent after everything else, so a file never holds
    /// up the live view. False when too many are already waiting.
    bool pushBulk(std::vector<std::byte> bytes) {
        {
            const std::lock_guard lock(m_mutex);
            if (m_bulk.size() >= kMaxChunksInFlight * 2) {
                return false;
            }
            m_bulk.push_back(std::move(bytes));
        }
        m_wake.notify_all();
        return true;
    }

    void pushEvent(std::vector<std::byte> bytes, bool droppable) {
        {
            const std::lock_guard lock(m_mutex);
            if (m_events.size() >= kMaxQueuedEvents) {
                const auto victim = std::ranges::find_if(
                    m_events, [](const OutgoingEvent& event) { return event.droppable; });
                if (victim == m_events.end() && droppable) {
                    m_eventsDropped.fetch_add(1);
                    return;
                }
                m_events.erase(victim != m_events.end() ? victim : m_events.begin());
                m_eventsDropped.fetch_add(1);
            }
            m_events.push_back(OutgoingEvent{.bytes = std::move(bytes), .droppable = droppable});
        }
        m_wake.notify_all();
    }

    /// From the bus thread: one slot for a completed pass, one for the newest
    /// partial. A pass replaces any partial waiting, which is older than it.
    ///
    /// Each slot carries what changed since the frame before it went out,
    /// gathered over every frame it absorbed: the encoder then compares only
    /// that much of the grid, merged frames included.
    void offerFrame(const SpectrumFramePtr& frame) {
        {
            const std::lock_guard lock(m_mutex);
            m_partialChanged.add(ChangedBins::of(*frame));
            if (frame->passComplete) {
                if (m_pass) {
                    m_passesCoalesced.fetch_add(1);
                }
                if (m_partial) {
                    m_partialsCoalesced.fetch_add(1);
                    m_partial.reset();
                }
                m_pass = frame;
                m_passChanged.add(m_partialChanged);
                m_partialChanged = ChangedBins{};
            } else {
                if (m_partial) {
                    m_partialsCoalesced.fetch_add(1);
                }
                m_partial = frame;
            }
        }
        m_wake.notify_all();
    }

    /// Ends the open segment once what is queued has gone.
    void closeSegment() {
        {
            const std::lock_guard lock(m_mutex);
            m_closeSegment = true;
        }
        m_wake.notify_all();
    }

    /// Zero sends frames whole.
    void setMaxBins(std::uint32_t bins) noexcept { m_maxBins.store(bins); }
    [[nodiscard]] std::uint32_t maxBins() const noexcept { return m_maxBins.load(); }

    [[nodiscard]] LinkStats link() const {
        LinkStats link;
        link.framesSent = m_framesSent.load();
        link.passesCoalesced = m_passesCoalesced.load();
        link.partialsCoalesced = m_partialsCoalesced.load();
        link.eventsDropped = m_eventsDropped.load();
        link.encodeNs = m_encodeNs.load();
        return link;
    }

private:
    template <typename OnCommand>
    void readLoop(OnCommand& onCommand) {
        std::vector<std::byte> buffer(kReceiveChunk);
        auto lastHeard = Clock::now();
        sweeps::StreamRecord record;
        while (m_alive.load()) {
            while (true) {
                auto next = m_framer.next(record);
                if (!next) {
                    logWarn("remote", "{}: {}", m_peer, next.error().describe());
                    return;
                }
                if (!*next) {
                    break;
                }
                if (!handle(record, onCommand)) {
                    return;
                }
            }

            auto readable = m_socket.waitReadable(kReadSlice);
            if (!readable) {
                return;
            }
            if (!*readable) {
                if (Clock::now() - lastHeard > m_silenceTimeout) {
                    logWarn("remote", "{}: silent for {} ms, dropping it", m_peer,
                            m_silenceTimeout.count());
                    return;
                }
                continue;
            }
            auto got =
                m_socket.receive({reinterpret_cast<std::uint8_t*>(buffer.data()), buffer.size()});
            if (!got || *got == 0) {
                return;
            }
            m_framer.feed(buffer.data(), *got);
            lastHeard = Clock::now();
        }
    }

    /// False ends the session.
    template <typename OnCommand>
    bool handle(const sweeps::StreamRecord& record, OnCommand& onCommand) {
        if (!isControl(record)) {
            return true;
        }
        auto message = decodeMessage(record);
        if (!message) {
            logWarn("remote", "{}: {}", m_peer, message.error().describe());
            return false;
        }
        if (message->name == msg::kPing) {
            const Ping ping = Ping::from(message->body);
            pushControl(messageBytes(msg::kPong, Pong{.id = ping.id,
                                                      .clientNs = ping.clientNs,
                                                      .serverNs = monotonicNs(),
                                                      .serverWallNs = wallClockNs()}
                                                     .toMetadata()));
        } else if (message->name == msg::kCommand) {
            Command command = Command::from(message->body);
            const std::uint64_t seq = command.seq;
            if (!onCommand(std::move(command))) {
                pushControl(messageBytes(msg::kReply,
                                         Reply{.seq = seq,
                                               .ok = false,
                                               .code = ErrorCode::Unavailable,
                                               .message = "too many commands waiting; try again"}
                                             .toMetadata()));
            }
        } else if (message->name == msg::kBye) {
            logInfo("remote", "{} disconnected", m_peer);
            m_saidGoodbye.store(true);
            return false;
        }
        // Any other name is from a newer client, and ignored.
        return true;
    }

    void sendLoop() {
        FrameEncoder encoder;
        FrameReducer reducer;
        std::vector<std::byte> buffer;
        std::vector<std::vector<std::byte>> control;
        std::deque<OutgoingEvent> events;
        std::vector<std::vector<std::byte>> bulk;
        while (true) {
            SpectrumFramePtr pass;
            SpectrumFramePtr partial;
            ChangedBins passChanged;
            ChangedBins partialChanged;
            bool closeSegment = false;
            bool finishing = false;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait(lock, [this] {
                    return !m_alive.load() || m_finishing || !m_control.empty() ||
                           !m_events.empty() || m_pass || m_partial || m_closeSegment ||
                           !m_bulk.empty();
                });
                if (!m_alive.load() && !m_finishing) {
                    return;
                }
                control.swap(m_control);
                events.swap(m_events);
                bulk.swap(m_bulk);
                pass = std::exchange(m_pass, nullptr);
                partial = std::exchange(m_partial, nullptr);
                passChanged = std::exchange(m_passChanged, ChangedBins{});
                partialChanged = std::exchange(m_partialChanged, ChangedBins{});
                closeSegment = std::exchange(m_closeSegment, false);
                finishing = m_finishing;
            }

            buffer.clear();
            for (const std::vector<std::byte>& message : control) {
                buffer.insert(buffer.end(), message.begin(), message.end());
            }
            for (const OutgoingEvent& event : events) {
                buffer.insert(buffer.end(), event.bytes.begin(), event.bytes.end());
            }
            control.clear();
            events.clear();
            const std::uint64_t encodeStart = monotonicNs();
            const std::uint32_t cap = m_maxBins.load();
            const auto encode = [&](const SpectrumFrame& frame, ChangedBins changed) {
                if (cap > 0 && frame.binCount() > cap) {
                    const SpectrumFrame reduced = reducer.reduce(frame, cap, changed);
                    encoder.encode(reduced, buffer, changed);
                } else {
                    encoder.encode(frame, buffer, changed);
                }
                m_framesSent.fetch_add(1);
            };
            if (pass) {
                encode(*pass, passChanged);
            }
            if (partial) {
                encode(*partial, partialChanged);
            }
            if (pass || partial) {
                m_encodeNs.fetch_add(monotonicNs() - encodeStart);
            }
            for (const std::vector<std::byte>& chunk : bulk) {
                buffer.insert(buffer.end(), chunk.begin(), chunk.end());
            }
            bulk.clear();
            if (closeSegment || finishing) {
                encoder.close(monotonicNs(), buffer);
            }
            if (finishing) {
                sweeps::appendRecord(buffer,
                                     static_cast<std::uint16_t>(sweeps::RecordType::EndOfStream),
                                     nullptr, 0);
            }

            if (!buffer.empty() && !m_socket.sendAll(asBytes(buffer))) {
                end();
                return;
            }
            if (finishing) {
                m_socket.shutdown();
                return;
            }
        }
    }

    net::SecureChannel m_socket;
    sweeps::RecordFramer m_framer;
    std::string m_peer;
    std::chrono::milliseconds m_silenceTimeout;
    std::atomic<bool> m_alive{true};
    std::atomic<bool> m_saidGoodbye{false};

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::vector<std::vector<std::byte>> m_control;
    std::deque<OutgoingEvent> m_events;
    std::vector<std::vector<std::byte>> m_bulk;
    SpectrumFramePtr m_pass;
    SpectrumFramePtr m_partial;
    ChangedBins m_passChanged;
    ChangedBins m_partialChanged;
    bool m_closeSegment = false;
    bool m_finishing = false;

    std::atomic<std::uint64_t> m_framesSent{0};
    std::atomic<std::uint64_t> m_passesCoalesced{0};
    std::atomic<std::uint64_t> m_partialsCoalesced{0};
    std::atomic<std::uint64_t> m_eventsDropped{0};
    std::atomic<std::uint64_t> m_encodeNs{0};
    std::atomic<std::uint32_t> m_maxBins{0};

    std::thread m_reader;
    std::thread m_sender;
};

// ------------------------------------------------------------------- the server

struct RemoteServer::Impl {
    Impl(LocalInstrument& instrument, FrameBus& output, EventBus& events, Telemetry& telemetry,
         ServerConfig config)
        : instrument(instrument), output(output), events(events), telemetry(telemetry),
          config(std::move(config)) {}

    /// Hands every published frame to the session's mailbox.
    class Sink final : public IFrameConsumer {
    public:
        explicit Sink(Impl& owner) : m_owner(owner) {}
        void onFrame(const SpectrumFramePtr& frame) noexcept override {
            if (const std::shared_ptr<Session> current = m_owner.currentSession()) {
                current->offerFrame(frame);
            }
        }
        [[nodiscard]] std::string_view consumerName() const noexcept override {
            return "remote-server";
        }

    private:
        Impl& m_owner;
    };

    struct Handshake {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    struct Arrival {
        net::SecureChannel socket;
        sweeps::RecordFramer framer;
    };

    // ---- shared state ---------------------------------------------------------

    [[nodiscard]] std::shared_ptr<Session> currentSession() const {
        const std::lock_guard lock(sessionMutex);
        return activeSession;
    }

    void forwardEvent(const session::SessionEvent& event, bool droppable) {
        if (std::shared_ptr<Session> current = currentSession()) {
            std::vector<std::byte> payload;
            session::encodeEvent(payload, event);
            std::vector<std::byte> record;
            sweeps::appendRecord(record, static_cast<std::uint16_t>(sweeps::RecordType::Event),
                                 payload.data(), payload.size());
            current->pushEvent(std::move(record), droppable);
        }
    }

    bool enqueueCommand(Command command) {
        {
            const std::lock_guard lock(controlMutex);
            if (commands.size() >= kMaxQueuedCommands) {
                return false;
            }
            commands.push_back(std::move(command));
        }
        controlWake.notify_all();
        return true;
    }

    void wakeControl() { controlWake.notify_all(); }

    // ---- the handshake ------------------------------------------------------------

    void handshake(net::TcpSocket socket) {
        const std::string peer = socket.peerAddress();
        const auto deadline = Clock::now() + config.handshakeTimeout;
        (void)socket.setNoDelay(true);

        auto accepted =
            acceptChannel(std::move(socket), config.token,
                          std::format("{} {}", productName(), versionString()), deadline, stopping);
        if (!accepted) {
            if (accepted.error().code() == ErrorCode::PermissionDenied) {
                logWarn("remote", "{}: wrong token", peer);
                // Slows guessing to one try a second per connection; then the
                // connection simply ends, telling a guesser nothing.
                std::unique_lock lock(controlMutex);
                controlWake.wait_for(lock, config.refusalDelay, [this] { return stopping.load(); });
            } else {
                logInfo("remote", "{}: {}", peer, accepted.error().describe());
            }
            return;
        }

        net::SecureChannel& channel = accepted->channel;
        if (accepted->hello.protocolVersion != kProtocolVersion) {
            (void)channel.sendAll(asBytes(messageBytes(
                msg::kRefused,
                Refused{.reason = std::string(refusal::kVersion),
                        .message = std::format("this server speaks protocol {}, the client {}",
                                               kProtocolVersion, accepted->hello.protocolVersion)}
                    .toMetadata())));
            channel.shutdown();
            return;
        }

        {
            const std::lock_guard lock(controlMutex);
            arrivals.push_back(Arrival{.socket = std::move(channel),
                                       .framer = sweeps::RecordFramer(kMaxClientRecordBytes)});
        }
        controlWake.notify_all();
    }

    void listenLoop() {
        while (!stopping.load()) {
            std::erase_if(handshakes, [](Handshake& pending) {
                if (!pending.done->load()) {
                    return false;
                }
                pending.thread.join();
                return true;
            });

            auto accepted = listener.accept(kAcceptSlice);
            if (!accepted) {
                logWarn("remote", "{}", accepted.error().describe());
                std::this_thread::sleep_for(kAcceptSlice);
                continue;
            }
            if (!*accepted) {
                continue;
            }
            if (handshakes.size() >= kMaxPendingHandshakes) {
                (*accepted)->close();
                continue;
            }

            auto done = std::make_shared<std::atomic<bool>>(false);
            handshakes.push_back(Handshake{
                .thread = std::thread([this, socket = std::move(**accepted), done]() mutable {
                    handshake(std::move(socket));
                    done->store(true);
                }),
                .done = done});
        }
        for (Handshake& pending : handshakes) {
            pending.thread.join();
        }
        handshakes.clear();
    }

    // ---- the control thread ----------------------------------------------------------

    void controlLoop() {
        auto lastState = Clock::time_point{};
        auto lastTelemetry = Clock::time_point{};
        while (!stopping.load()) {
            std::vector<Arrival> arrived;
            std::vector<Command> batch;
            {
                std::unique_lock lock(controlMutex);
                controlWake.wait_for(lock, kControlInterval, [this] {
                    return stopping.load() || !arrivals.empty() || !commands.empty() ||
                           sessionEnded;
                });
                arrived.swap(arrivals);
                batch.assign(std::make_move_iterator(commands.begin()),
                             std::make_move_iterator(commands.end()));
                commands.clear();
            }
            if (stopping.load()) {
                break;
            }

            reapSession();
            for (Arrival& arrival : arrived) {
                admit(std::move(arrival));
            }
            if (lingerUntil && Clock::now() >= *lingerUntil && !currentSession()) {
                logInfo("remote", "nobody came back; acquisition stopped");
                idle();
            }

            const bool executed = execute(batch);
            instrument.tick(monotonicNs());
            if (recorder) {
                recorder->setCompletePassesOnly(instrument.sweeping());
            }
            const bool wasRunning = running;
            running = instrument.running();

            std::shared_ptr<Session> current = currentSession();
            const std::vector<InstrumentNotice> notices = instrument.takeNotices();
            if (!current) {
                continue;
            }
            for (const InstrumentNotice& notice : notices) {
                current->pushControl(messageBytes(msg::kNotice, encodeNotice(notice)));
            }
            if (wasRunning && !running) {
                current->closeSegment();
            }

            const auto now = Clock::now();
            if (executed || now - lastState >= kStateInterval) {
                sendState(*current, executed);
                lastState = now;
            }
            if (now - lastTelemetry >= kTelemetryInterval) {
                sendTelemetry(*current);
                lastTelemetry = now;
            }
        }

        if (std::shared_ptr<Session> current = currentSession()) {
            current->finish(refusal::kShutdown);
            const std::lock_guard lock(sessionMutex);
            activeSession.reset();
        }
        instrument.stop();
        (void)stopRecording();
        {
            const std::lock_guard lock(controlMutex);
            arrivals.clear();
        }
    }

    // ---- recordings ---------------------------------------------------------------

    Status startRecording(std::uint32_t bins) {
        if (config.sessionsDir.empty()) {
            return fail(ErrorCode::Unsupported, "this server has nowhere to record to");
        }
        if (recorder) {
            return ok();
        }
        std::error_code ec;
        std::filesystem::create_directories(config.sessionsDir, ec);
        const std::string name =
            std::format("server-{}.sweeps", formatWallClockCompact(wallClockNs()));
        session::RecorderConfig recorderConfig;
        recorderConfig.binsPerLine = bins;
        recorderConfig.sessionName = name;
        auto created = session::SessionRecorder::create(config.sessionsDir / name, recorderConfig);
        if (!created) {
            return std::unexpected(std::move(created).error());
        }
        recorder = std::move(*created);
        recorder->attachEvents(events);
        recorder->setCompletePassesOnly(instrument.sweeping());
        recorderSubscription = output.subscribe(recorder.get());
        recordingName = name;
        logInfo("remote", "recording to {}", (config.sessionsDir / name).string());
        listRecordings();
        return ok();
    }

    Status stopRecording() {
        if (!recorder) {
            return ok();
        }
        output.unsubscribe(recorderSubscription);
        Status closed = recorder->close();
        recorder.reset();
        recordingName.clear();
        listRecordings();
        return closed;
    }

    /// Only a name this server listed, so nothing from a client is ever a
    /// path.
    Result<std::filesystem::path> recordingPath(const std::string& name) {
        if (name.empty() || name.find_first_of("/\\") != std::string::npos ||
            name.find("..") != std::string::npos) {
            return fail<std::filesystem::path>(ErrorCode::InvalidArgument,
                                               "'{}' is not a recording's name", name);
        }
        listRecordings();
        if (std::ranges::find(recordings, name, &RecordingFile::name) == recordings.end()) {
            return fail<std::filesystem::path>(ErrorCode::NotFound, "no recording called '{}'",
                                               name);
        }
        if (name == recordingName) {
            return fail<std::filesystem::path>(ErrorCode::Unavailable,
                                               "'{}' is still being recorded", name);
        }
        return config.sessionsDir / name;
    }

    Status deleteRecording(const std::string& name) {
        auto path = recordingPath(name);
        if (!path) {
            return std::unexpected(std::move(path).error());
        }
        std::error_code ec;
        std::filesystem::remove(*path, ec);
        listRecordings();
        if (ec) {
            return fail(ErrorCode::IoError, "could not delete '{}': {}", name, ec.message());
        }
        return ok();
    }

    Status fetchRecording(const std::string& name, std::uint64_t offset) {
        auto path = recordingPath(name);
        if (!path) {
            return std::unexpected(std::move(path).error());
        }
        std::ifstream in(*path, std::ios::binary);
        std::error_code ec;
        const std::uint64_t total = std::filesystem::file_size(*path, ec);
        if (!in || ec) {
            return fail(ErrorCode::IoError, "could not read '{}'", name);
        }
        Chunk chunk{.name = name, .offset = std::min(offset, total), .totalBytes = total};
        chunk.data.resize(
            static_cast<std::size_t>(std::min<std::uint64_t>(kChunkBytes, total - chunk.offset)));
        in.seekg(static_cast<std::streamoff>(chunk.offset));
        in.read(reinterpret_cast<char*>(chunk.data.data()),
                static_cast<std::streamsize>(chunk.data.size()));
        if (!in) {
            return fail(ErrorCode::IoError, "could not read '{}'", name);
        }
        std::shared_ptr<Session> current = currentSession();
        if (current && !current->pushBulk(messageBytes(msg::kChunk, chunk.toMetadata()))) {
            return fail(ErrorCode::Unavailable, "too many pieces of '{}' asked for at once", name);
        }
        return ok();
    }

    void listRecordings() {
        recordings.clear();
        lastListing = Clock::now();
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(config.sessionsDir, ec)) {
            const std::string name = entry.path().filename().string();
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".sweeps" ||
                name.starts_with('.')) {
                continue;
            }
            // Through each clock's now: libc++ has no clock_cast, and a
            // second either way is nothing to a file listing.
            const auto modified = entry.last_write_time(ec);
            const auto wall = std::chrono::system_clock::now() +
                              std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                  modified - std::filesystem::file_time_type::clock::now());
            recordings.push_back(RecordingFile{
                .name = name,
                .bytes = entry.file_size(ec),
                .modifiedWallNs = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(wall.time_since_epoch())
                        .count())});
        }
        std::ranges::sort(recordings, std::greater<>(), &RecordingFile::name);
    }

    [[nodiscard]] ServerRecordings recordingsState() {
        // The folder is read again every few seconds rather than every state:
        // something else on the server may add or remove files.
        if (!config.sessionsDir.empty() && Clock::now() - lastListing > std::chrono::seconds(3)) {
            listRecordings();
        }
        ServerRecordings state{.available = !config.sessionsDir.empty(),
                               .active = recorder != nullptr,
                               .current = recordingName,
                               .files = recordings};
        if (recorder) {
            state.lines = recorder->linesWritten();
            state.bytes = recorder->bytesWritten();
        }
        return state;
    }

    void admit(Arrival arrival) {
        if (currentSession()) {
            const std::string peer = arrival.socket.peerAddress();
            logInfo("remote", "{} turned away: {} is connected", peer, currentSession()->peer());
            (void)arrival.socket.sendAll(asBytes(
                messageBytes(msg::kRefused, Refused{.reason = std::string(refusal::kBusy),
                                                    .message = "another client is connected"}
                                                .toMetadata())));
            arrival.socket.shutdown();
            return;
        }

        // Whoever arrives takes over a radio left running for a dropped
        // client: the same desktop reconnecting, nearly always.
        lingerUntil.reset();
        if (advertiser) {
            advertiser->setBusy(true);
        }

        auto fresh = std::make_shared<Session>(std::move(arrival.socket), std::move(arrival.framer),
                                               config.silenceTimeout);
        logInfo("remote", "{} connected", fresh->peer());
        fresh->pushControl(
            messageBytes(msg::kWelcome, Welcome{.serverName = serverName}.toMetadata()));

        sentSections.clear();
        lastRevision = ~std::uint64_t{0};
        lastAck = 0;
        lastSentAck = 0;
        sendState(*fresh, true);
        sendTelemetry(*fresh);

        {
            const std::lock_guard lock(sessionMutex);
            activeSession = fresh;
            sessionEnded = false;
        }
        fresh->run([this](Command command) { return enqueueCommand(std::move(command)); },
                   [this] {
                       {
                           const std::lock_guard lock(controlMutex);
                           sessionEnded = true;
                       }
                       wakeControl();
                   });
    }

    /// A session whose client went away: join it, and leave the radio idle.
    void reapSession() {
        std::shared_ptr<Session> ended;
        {
            const std::lock_guard lock(sessionMutex);
            if (!activeSession || activeSession->alive()) {
                return;
            }
            ended = std::move(activeSession);
        }
        {
            const std::lock_guard lock(controlMutex);
            sessionEnded = false;
            // Queued by a client that is no longer there to be answered.
            commands.clear();
        }
        ended->join();
        if (advertiser) {
            advertiser->setBusy(false);
        }

        // A goodbye is someone leaving; anything else may be a cable, and
        // the desktop at the other end is already trying to come back.
        if (!ended->saidGoodbye() && config.linger.count() > 0 && instrument.running()) {
            lingerUntil = Clock::now() + config.linger;
            logInfo("remote", "{} dropped; the radio keeps running for {} s", ended->peer(),
                    std::chrono::duration_cast<std::chrono::seconds>(config.linger).count());
            return;
        }
        logInfo("remote", "{} gone; acquisition stopped", ended->peer());
        idle();
    }

    /// The radio stopped with nobody to watch it.
    void idle() {
        lingerUntil.reset();
        instrument.cancelLearning();
        instrument.stop();
        running = false;
    }

    // ---- commands --------------------------------------------------------------------

    /// Runs a batch in order, skipping any edit a later one in the same batch
    /// replaces. True when anything ran.
    bool execute(std::vector<Command>& batch) {
        if (batch.empty()) {
            return false;
        }
        std::shared_ptr<Session> current = currentSession();
        for (std::size_t i = 0; i < batch.size(); ++i) {
            const Command& command = batch[i];
            const bool superseded = std::any_of(batch.begin() + static_cast<std::ptrdiff_t>(i) + 1,
                                                batch.end(), [&](const Command& later) {
                                                    if (later.op != command.op) {
                                                        return false;
                                                    }
                                                    if (command.op == op::kSetParameter) {
                                                        return later.args.getString("key") ==
                                                               command.args.getString("key");
                                                    }
                                                    return supersedes(command.op);
                                                });

            const Status status = superseded ? ok() : run(command);
            lastAck = std::max(lastAck, command.seq);
            // Sent again with the acknowledgement even if unchanged: the
            // client put its own edit there, and a refused one must be undone.
            for (const std::string_view touched : sectionsTouchedBy(command.op)) {
                sentSections.erase(std::string(touched));
            }
            if (current) {
                Reply reply{.seq = command.seq, .ok = status.has_value()};
                if (!status) {
                    reply.code = status.error().code();
                    reply.message = status.error().message();
                }
                current->pushControl(messageBytes(msg::kReply, reply.toMetadata()));
            }
        }
        return true;
    }

    Status run(const Command& command) {
        const Metadata& args = command.args;
        const std::string& name = command.op;

        if (name == op::kStart) {
            return instrument.start();
        }
        if (name == op::kStop) {
            instrument.stop();
            return ok();
        }
        if (name == op::kRestart) {
            return instrument.restart();
        }
        if (name == op::kSetSweeping) {
            return instrument.setSweeping(args.getBool("enabled"));
        }
        if (name == op::kApplySweepPlan) {
            return instrument.applySweepPlan(decodePlan(hashAt(args, "plan")));
        }
        if (name == op::kSweepRange) {
            return instrument.sweepRange(decodePlan(hashAt(args, "plan")));
        }
        if (name == op::kApplyPipelineConfig) {
            return instrument.applyPipelineConfig(decodePipeline(hashAt(args, "config")));
        }
        if (name == op::kSetFftBackend) {
            return instrument.setFftBackend(args.getString("name"));
        }
        if (name == op::kSetParameter) {
            const sweeps::Value* value = args.find("value");
            if (value == nullptr) {
                return fail(ErrorCode::InvalidArgument, "setParameter needs a value");
            }
            return instrument.setDeviceParameter(args.getString("key"), decodeValue(*value));
        }
        if (name == op::kResetTelemetry) {
            instrument.resetTelemetry();
            return ok();
        }
        if (name == op::kSetCorrectionSettings) {
            instrument.setCorrectionSettings(decodeCorrectionSettings(hashAt(args, "settings")));
            return ok();
        }
        if (name == op::kStartLearning) {
            return instrument.startLearning();
        }
        if (name == op::kCancelLearning) {
            instrument.cancelLearning();
            return ok();
        }
        if (name == op::kClearAutoSpurs) {
            instrument.clearAutoSpurs();
            return ok();
        }
        if (name == op::kClearCorrections) {
            instrument.clearCorrections();
            return ok();
        }
        if (name == op::kSetUserAntennas) {
            std::vector<Antenna> antennas = decodeAntennas(hashAt(args, "antennas"));
            for (Antenna& antenna : antennas) {
                antenna.builtin = false;
            }
            return instrument.setUserAntennas(std::move(antennas));
        }
        if (name == op::kSetAssignments) {
            return instrument.setAntennaAssignments(decodeAssignments(hashAt(args, "assignments")));
        }
        if (name == op::kRescanSwitchers) {
            instrument.rescanSwitchers();
            return ok();
        }
        if (name == op::kStartRecording) {
            const std::int64_t bins = args.getInt("maxBins", config.recordBins);
            return startRecording(bins > 0 && bins <= kMaxGridBins
                                      ? static_cast<std::uint32_t>(bins)
                                      : config.recordBins);
        }
        if (name == op::kStopRecording) {
            return stopRecording();
        }
        if (name == op::kDeleteRecording) {
            return deleteRecording(args.getString("name"));
        }
        if (name == op::kFetchRecording) {
            return fetchRecording(args.getString("name"),
                                  static_cast<std::uint64_t>(args.getInt("offset")));
        }
        if (name == op::kStartBenchmark) {
            return instrument.startBenchmark(decodeBenchmarkConfig(hashAt(args, "config")));
        }
        if (name == op::kCancelBenchmark) {
            instrument.cancelBenchmark();
            return ok();
        }
        if (name == op::kSetLinkResolution) {
            const std::int64_t bins = args.getInt("maxBins");
            if (bins != 0 && (bins < kMinLinkBins || bins > kMaxGridBins)) {
                return fail(ErrorCode::OutOfRange, "{} bins is outside {}..{}", bins, kMinLinkBins,
                            kMaxGridBins);
            }
            if (std::shared_ptr<Session> current = currentSession()) {
                current->setMaxBins(static_cast<std::uint32_t>(bins));
            }
            return ok();
        }
        return fail(ErrorCode::Unsupported, "this server does not know '{}'", name);
    }

    // ---- state and telemetry -----------------------------------------------------------

    /// Every section that differs from what this client last received. The
    /// bench sections are rebuilt only when the instrument's revision moved or
    /// a command ran; the rest are cheap enough to compare every time.
    void sendState(Session& target, bool afterCommand) {
        Metadata sections;
        const auto offer = [&](std::string_view name, Metadata body) {
            std::vector<std::byte> bytes;
            body.encode(bytes);
            auto& last = sentSections[std::string(name)];
            if (last == bytes) {
                return;
            }
            last = std::move(bytes);
            sections.setHash(std::string(name), std::move(body));
        };

        const DeviceDescriptor* device = instrument.device();
        const bool benchChanged = afterCommand || instrument.revision() != lastRevision;
        if (benchChanged) {
            lastRevision = instrument.revision();

            Metadata deviceSection;
            deviceSection.setBool("present", device != nullptr);
            if (device != nullptr) {
                deviceSection.setHash("descriptor", encodeDevice(*device));
            }
            deviceSection.setString("antennaKey", instrument.deviceAntennaKey());
            deviceSection.setString("profileDriver", instrument.profileDriver());
            deviceSection.setString("profileId", instrument.profileId());
            deviceSection.setString("displayLabel", instrument.displayLabel());
            offer(section::kDevice, std::move(deviceSection));

            Metadata backends = encodeBackends(instrument.fftBackends());
            backends.setString("current", instrument.fftBackendName());
            offer(section::kBackends, std::move(backends));

            offer(section::kAntennas, encodeAntennas(instrument.antennas().entries()));
            offer(section::kAssignments, encodeAssignments(instrument.antennaAssignments()));

            Metadata switchers;
            switchers.setHash("open", encodeSwitcherViews(instrument.openSwitchers()));
            switchers.setHash("available", encodeSwitcherInfos(instrument.availableSwitchers()));
            offer(section::kSwitchers, std::move(switchers));
        }

        Metadata values;
        Metadata parameters;
        if (device != nullptr) {
            for (const SdrParameter& parameter : device->parameters) {
                if (const std::optional<SdrValue> value = instrument.parameter(parameter.key)) {
                    parameters.set(parameter.key, encodeValue(*value));
                }
            }
        }
        values.setHash("parameters", std::move(parameters));
        values.setString("selectedRxPort", instrument.selectedRxPort());
        offer(section::kValues, std::move(values));

        Metadata run;
        run.setBool("running", instrument.running());
        run.setBool("sweeping", instrument.sweeping());
        run.setInt("startGeneration", static_cast<std::int64_t>(instrument.startGeneration()));
        run.setHash("engine", encodeEngineStats(instrument.engineStats()));
        offer(section::kRun, std::move(run));

        offer(section::kPlan, encodePlan(instrument.sweepPlan()));
        offer(section::kSchedule, encodeSchedule(instrument.schedule()));
        offer(section::kPipeline, encodePipeline(instrument.pipelineConfig()));

        Metadata corrections;
        corrections.setHash("settings", encodeCorrectionSettings(instrument.correctionSettings()));
        corrections.setHash("summary", encodeCorrectionSummary(instrument.correctionSummary()));
        offer(section::kCorrections, std::move(corrections));

        Metadata learning;
        learning.setBool("active", instrument.learning());
        learning.setString("label", instrument.learningLabel());
        offer(section::kLearning, std::move(learning));

        Metadata rfPath;
        rfPath.setHash("legs", encodeRfLegs(instrument.rfPath()));
        rfPath.setHash("coverage", encodeRanges(instrument.antennaCoverage()));
        offer(section::kRfPath, std::move(rfPath));

        Metadata link;
        link.setInt("maxBins", target.maxBins());
        offer(section::kLink, std::move(link));

        offer(section::kBenchmark, encodeBenchmarkStatus(instrument.benchmark()));
        offer(section::kRecordings, recordingsState().toMetadata());

        // An acknowledgement goes out even when nothing changed: the client
        // is holding its own copy of what it edited until it arrives.
        if (sections.empty() && lastAck == lastSentAck) {
            return;
        }
        lastSentAck = lastAck;
        target.pushControl(messageBytes(
            msg::kState, State{.ackSeq = lastAck, .sections = std::move(sections)}.toMetadata()));
    }

    void sendTelemetry(Session& target) {
        const TelemetrySnapshot& snapshot = telemetry.sample();
        TelemetryReport report;
        report.stream = snapshot.stream;
        report.process = snapshot.process;
        report.health = instrument.health();
        report.link = target.link();
        std::vector<std::byte> bytes;
        appendTelemetry(bytes, report, monotonicNs());
        target.pushControl(std::move(bytes));
    }

    // ---- members ------------------------------------------------------------------

    LocalInstrument& instrument;
    FrameBus& output;
    EventBus& events;
    Telemetry& telemetry;
    ServerConfig config;
    std::string serverName;

    net::TcpListener listener;
    std::atomic<bool> stopping{false};
    bool started = false;

    Sink sink{*this};
    FrameBus::SubscriptionId sinkSubscription = 0;
    std::vector<EventBus::SubscriptionId> eventSubscriptions;

    std::thread listenThread;
    std::vector<Handshake> handshakes; ///< The listener thread's alone
    std::thread controlThread;

    std::mutex controlMutex;
    std::condition_variable controlWake;
    std::vector<Arrival> arrivals;
    std::deque<Command> commands;
    bool sessionEnded = false;

    mutable std::mutex sessionMutex;
    std::shared_ptr<Session> activeSession;

    // The control thread's alone.
    std::map<std::string, std::vector<std::byte>> sentSections;
    std::uint64_t lastRevision = 0;
    std::uint64_t lastAck = 0;
    std::uint64_t lastSentAck = 0;
    bool running = false;
    std::optional<Clock::time_point> lingerUntil;

    std::unique_ptr<mdns::Advertiser> advertiser;
    std::unique_ptr<session::SessionRecorder> recorder;
    FrameBus::SubscriptionId recorderSubscription = 0;
    std::string recordingName;
    std::vector<RecordingFile> recordings;
    Clock::time_point lastListing{};
};

RemoteServer::RemoteServer(LocalInstrument& instrument, FrameBus& output, EventBus& events,
                           Telemetry& telemetry, ServerConfig config)
    : m_impl(std::make_unique<Impl>(instrument, output, events, telemetry, std::move(config))) {
}

RemoteServer::~RemoteServer() {
    stop();
}

Status RemoteServer::start() {
    Impl& impl = *m_impl;
    if (impl.started) {
        return ok();
    }
    if (impl.config.token.empty() && !net::isLoopbackAddress(impl.config.listenAddress)) {
        return fail(ErrorCode::PermissionDenied,
                    "listening on {} without a token would hand the radio to anyone who can "
                    "reach it; give --token, or listen on 127.0.0.1",
                    impl.config.listenAddress);
    }

    auto listener = net::TcpListener::listen(impl.config.listenAddress, impl.config.port);
    if (!listener) {
        return std::unexpected(std::move(listener).error());
    }
    impl.listener = std::move(*listener);
    impl.serverName = impl.config.serverName.empty() ? net::hostName() : impl.config.serverName;
    impl.stopping.store(false);
    impl.running = impl.instrument.running();

    impl.sinkSubscription = impl.output.subscribe(&impl.sink);
    const auto forward = [&impl]<typename Event>(bool droppable) {
        return impl.events.subscribe<Event>([&impl, droppable](const Event& event) {
            impl.forwardEvent(session::toSessionEvent(event, wallClockNs()), droppable);
        });
    };
    impl.eventSubscriptions = {
        forward.operator()<RetuneEvent>(true),
        forward.operator()<ParameterChangedEvent>(false),
        forward.operator()<SweepPassEvent>(false),
        forward.operator()<ThrottleChangedEvent>(false),
        forward.operator()<DeviceErrorEvent>(false),
    };

    // Before the control thread exists, which is the only other place the
    // recorder is touched.
    if (!impl.config.sessionsDir.empty()) {
        impl.listRecordings();
    }
    if (impl.config.recordAtStart) {
        if (auto recording = impl.startRecording(impl.config.recordBins); !recording) {
            logWarn("remote", "not recording: {}", recording.error().describe());
        }
    }

    if (impl.config.advertise) {
        // The machine's own name, its first label: "pi" whether the system
        // calls it pi, pi.local or pi.localdomain.
        std::string host = impl.serverName.substr(0, impl.serverName.find('.'));
        auto advertiser =
            mdns::Advertiser::start(mdns::Advert{.instance = std::format("Sweep++ on {}", host),
                                                 .host = host,
                                                 .port = impl.listener.port(),
                                                 .device = impl.instrument.displayLabel(),
                                                 .authRequired = !impl.config.token.empty()});
        if (advertiser) {
            impl.advertiser = std::move(*advertiser);
        } else {
            logWarn("remote", "not advertised on the LAN: {}", advertiser.error().describe());
        }
    }

    impl.controlThread = std::thread([&impl] { impl.controlLoop(); });
    impl.listenThread = std::thread([&impl] { impl.listenLoop(); });
    impl.started = true;
    logInfo("remote", "serving on {}:{}{}", impl.config.listenAddress, impl.listener.port(),
            impl.config.token.empty() ? " (no token)" : "");
    return ok();
}

void RemoteServer::stop() {
    Impl& impl = *m_impl;
    if (!impl.started) {
        return;
    }
    impl.stopping.store(true);
    impl.controlWake.notify_all();
    if (impl.listenThread.joinable()) {
        impl.listenThread.join();
    }
    if (impl.controlThread.joinable()) {
        impl.controlThread.join();
    }

    for (const EventBus::SubscriptionId id : impl.eventSubscriptions) {
        impl.events.unsubscribe(id);
    }
    impl.eventSubscriptions.clear();
    impl.output.unsubscribe(impl.sinkSubscription);
    impl.advertiser.reset();
    impl.listener.close();
    impl.started = false;
}

std::uint16_t RemoteServer::port() const noexcept {
    return m_impl->listener.port();
}

bool RemoteServer::clientConnected() const {
    const std::shared_ptr<Session> current = m_impl->currentSession();
    return current && current->alive();
}

std::string RemoteServer::clientAddress() const {
    const std::shared_ptr<Session> current = m_impl->currentSession();
    return current ? current->peer() : std::string{};
}

} // namespace sweeppp::remote
