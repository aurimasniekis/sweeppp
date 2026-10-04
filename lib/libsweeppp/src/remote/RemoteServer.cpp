// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/remote/RemoteServer.hpp"

#include "remote/StreamIo.hpp"
#include "sweeppp/core/Clock.hpp"
#include "sweeppp/core/Log.hpp"
#include "sweeppp/history/EventMapping.hpp"
#include "sweeppp/history/SessionRecorder.hpp"
#include "sweeppp/net/Socket.hpp"
#include "sweeppp/plugin/PluginHost.hpp"
#include "sweeppp/remote/FrameCodec.hpp"
#include "sweeppp/remote/Handshake.hpp"
#include "sweeppp/remote/Mdns.hpp"
#include "sweeppp/remote/Messages.hpp"
#include "sweeppp/remote/WireCodec.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sweeps/Records.hpp>
#include <sweeps/SessionReader.hpp>
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

/// One authenticated client: its stream, the thread reading it, the thread
/// writing to it, and what is queued for the latter.
class Session {
public:
    Session(std::uint64_t id, std::unique_ptr<net::ByteStream> socket, sweeps::RecordFramer framer,
            const Hello& hello, std::chrono::milliseconds silenceTimeout)
        : m_id(id), m_socket(std::move(socket)), m_framer(std::move(framer)),
          m_peer(m_socket->peerAddress()),
          m_kind(hello.kind.empty() ? std::string(client::kDesktop) : hello.kind),
          m_name(hello.name.empty() ? m_peer : hello.name), m_clientId(hello.clientId),
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
    [[nodiscard]] std::uint64_t id() const noexcept { return m_id; }
    [[nodiscard]] const std::string& peer() const noexcept { return m_peer; }
    [[nodiscard]] const std::string& kind() const noexcept { return m_kind; }
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] const std::string& clientId() const noexcept { return m_clientId; }

    /// What this client was last sent, which the next state is the
    /// difference from. The control thread's alone.
    struct Ledger {
        std::map<std::string, std::vector<std::byte>, std::less<>> sent;
        std::uint64_t lastAck = 0;
        std::uint64_t lastSentAck = 0;
        /// Recordings this client is reading, by the handle it was given.
        std::map<std::int64_t, std::unique_ptr<sweeps::SessionReader>> readers;
        std::int64_t nextReader = 0;
    };
    [[nodiscard]] Ledger& ledger() noexcept { return m_ledger; }

    /// Ends the session from outside: both threads wake and leave.
    void end() {
        m_alive.store(false);
        m_socket->shutdown();
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

    /// Passes kept while this client was away, sent before anything live.
    void offerReplay(std::vector<SpectrumFramePtr> frames) {
        {
            const std::lock_guard lock(m_mutex);
            m_replay.insert(m_replay.end(), std::make_move_iterator(frames.begin()),
                            std::make_move_iterator(frames.end()));
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

            auto readable = m_socket->waitReadable(kReadSlice);
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
                m_socket->receive({reinterpret_cast<std::uint8_t*>(buffer.data()), buffer.size()});
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
        std::vector<SpectrumFramePtr> replay;
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
                           !m_bulk.empty() || !m_replay.empty();
                });
                if (!m_alive.load() && !m_finishing) {
                    return;
                }
                control.swap(m_control);
                events.swap(m_events);
                bulk.swap(m_bulk);
                replay.swap(m_replay);
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
            for (const SpectrumFramePtr& kept : replay) {
                encode(*kept, ChangedBins::unknown());
            }
            replay.clear();
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

            if (!buffer.empty() && !m_socket->sendAll(asBytes(buffer))) {
                end();
                return;
            }
            if (finishing) {
                m_socket->shutdown();
                return;
            }
        }
    }

    std::uint64_t m_id;
    std::unique_ptr<net::ByteStream> m_socket;
    sweeps::RecordFramer m_framer;
    std::string m_peer;
    std::string m_kind;
    std::string m_name;
    std::string m_clientId;
    std::chrono::milliseconds m_silenceTimeout;
    Ledger m_ledger;
    std::atomic<bool> m_alive{true};
    std::atomic<bool> m_saidGoodbye{false};

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::vector<std::vector<std::byte>> m_control;
    std::deque<OutgoingEvent> m_events;
    std::vector<std::vector<std::byte>> m_bulk;
    std::vector<SpectrumFramePtr> m_replay;
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

    using SessionPtr = std::shared_ptr<Session>;

    /// Hands every published frame to each client's mailbox.
    class Sink final : public IFrameConsumer {
    public:
        explicit Sink(Impl& owner) : m_owner(owner) {}
        void onFrame(const SpectrumFramePtr& frame) noexcept override {
            m_owner.backlog.keep(frame);
            const std::lock_guard lock(m_owner.sessionMutex);
            for (const SessionPtr& session : m_owner.sessions) {
                session->offerFrame(frame);
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

    /// Completed passes kept for a controller that dropped, newest last,
    /// bounded by age and by bytes. Filled on the bus thread, emptied on the
    /// control thread.
    class Backlog {
    public:
        void begin(std::chrono::milliseconds span, std::size_t bytes) {
            const std::lock_guard lock(m_mutex);
            m_frames.clear();
            m_bytes = 0;
            m_spanNs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(span).count());
            m_limit = bytes;
            m_active = m_spanNs > 0 && bytes > 0;
        }

        void keep(const SpectrumFramePtr& frame) noexcept {
            if (!frame->passComplete) {
                return;
            }
            const std::lock_guard lock(m_mutex);
            if (!m_active) {
                return;
            }
            m_frames.push_back(frame);
            m_bytes += frame->binCount() * sizeof(float);
            while (!m_frames.empty() &&
                   (m_bytes > m_limit ||
                    frame->hostTimeNs - m_frames.front()->hostTimeNs > m_spanNs)) {
                m_bytes -= m_frames.front()->binCount() * sizeof(float);
                m_frames.pop_front();
            }
        }

        /// What was kept, marked replayed; nothing more is kept after.
        std::vector<SpectrumFramePtr> take() {
            const std::lock_guard lock(m_mutex);
            std::vector<SpectrumFramePtr> out;
            out.reserve(m_frames.size());
            for (const SpectrumFramePtr& frame : m_frames) {
                auto copy = std::make_shared<SpectrumFrame>(*frame);
                copy->replayed = true;
                out.push_back(std::move(copy));
            }
            m_frames.clear();
            m_bytes = 0;
            m_active = false;
            return out;
        }

        void drop() {
            const std::lock_guard lock(m_mutex);
            m_frames.clear();
            m_bytes = 0;
            m_active = false;
        }

    private:
        std::mutex m_mutex;
        std::deque<SpectrumFramePtr> m_frames;
        std::size_t m_bytes = 0;
        std::size_t m_limit = 0;
        std::uint64_t m_spanNs = 0;
        bool m_active = false;
    };

    struct Arrival {
        std::unique_ptr<net::ByteStream> socket;
        sweeps::RecordFramer framer;
        Hello hello;
    };

    /// A command and the client it came from, which is who is answered.
    struct Queued {
        SessionPtr from;
        Command command;
    };

    /// A state section as last built, and its encoding to compare against.
    struct Built {
        Metadata body;
        std::vector<std::byte> bytes;
    };

    // ---- shared state ---------------------------------------------------------

    [[nodiscard]] std::vector<SessionPtr> currentSessions() const {
        const std::lock_guard lock(sessionMutex);
        return sessions;
    }

    [[nodiscard]] std::size_t clientLimit() const noexcept {
        return config.shared ? std::max<std::size_t>(config.maxClients, 1) : 1;
    }

    void forwardEvent(const session::SessionEvent& event, bool droppable) {
        const std::lock_guard lock(sessionMutex);
        if (sessions.empty()) {
            return;
        }
        std::vector<std::byte> payload;
        session::encodeEvent(payload, event);
        std::vector<std::byte> record;
        sweeps::appendRecord(record, static_cast<std::uint16_t>(sweeps::RecordType::Event),
                             payload.data(), payload.size());
        for (const SessionPtr& session : sessions) {
            session->pushEvent(record, droppable);
        }
    }

    bool enqueueCommand(SessionPtr from, Command command) {
        if (!from) {
            return true;
        }
        {
            const std::lock_guard lock(controlMutex);
            if (commands.size() >= kMaxQueuedCommands) {
                return false;
            }
            commands.push_back(Queued{.from = std::move(from), .command = std::move(command)});
        }
        controlWake.notify_all();
        return true;
    }

    void wakeControl() { controlWake.notify_all(); }

    /// A client through its handshake, for the control thread to admit or
    /// turn away.
    void arrive(std::unique_ptr<net::ByteStream> socket, const Hello& hello,
                sweeps::RecordFramer framer = sweeps::RecordFramer(kMaxClientRecordBytes)) {
        {
            const std::lock_guard lock(controlMutex);
            arrivals.push_back(
                Arrival{.socket = std::move(socket), .framer = std::move(framer), .hello = hello});
        }
        controlWake.notify_all();
    }

    /// A stream someone else authenticated: stream headers both ways, then
    /// the client's hello as its first message.
    void greet(std::unique_ptr<net::ByteStream> stream) {
        const std::string peer = stream->peerAddress();
        const auto deadline = Clock::now() + config.handshakeTimeout;
        if (auto sent = io::sendStreamHeader(*stream); !sent) {
            return;
        }
        if (auto header = io::readStreamHeader(*stream, deadline, stopping); !header) {
            logInfo("remote", "{}: {}", peer, header.error().describe());
            return;
        }
        sweeps::RecordFramer framer(kMaxClientRecordBytes);
        auto record = io::readRecord(*stream, framer, deadline, stopping);
        if (!record) {
            logInfo("remote", "{}: {}", peer, record.error().describe());
            return;
        }
        auto message = isControl(*record) ? decodeMessage(*record)
                                          : fail<Message>(ErrorCode::ProtocolError, "no hello");
        if (!message || message->name != msg::kHello) {
            const bool otherVersion = !message && message.error().code() == ErrorCode::Unsupported;
            (void)stream->sendAll(asBytes(messageBytes(
                msg::kRefused,
                Refused{
                    .reason = std::string(otherVersion ? refusal::kVersion : refusal::kProtocol),
                    .message = otherVersion
                                   ? std::format("this server speaks protocol {}", kProtocolVersion)
                                   : std::string("the stream must open with a hello")}
                    .toMetadata())));
            stream->shutdown();
            return;
        }
        arrive(std::move(stream), Hello::from(message->body), std::move(framer));
    }

    void adopt(std::unique_ptr<net::ByteStream> stream) {
        const std::lock_guard lock(adoptMutex);
        std::erase_if(greetings, [](Handshake& pending) {
            if (!pending.done->load()) {
                return false;
            }
            pending.thread.join();
            return true;
        });
        if (stopping.load() || greetings.size() >= kMaxPendingHandshakes) {
            stream->close();
            return;
        }
        auto done = std::make_shared<std::atomic<bool>>(false);
        greetings.push_back(
            Handshake{.thread = std::thread([this, stream = std::move(stream), done]() mutable {
                          greet(std::move(stream));
                          done->store(true);
                      }),
                      .done = done});
    }

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
        arrive(std::make_unique<net::SecureChannel>(std::move(channel)), accepted->hello);
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
            std::vector<Queued> batch;
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

            reapSessions();
            for (Arrival& arrival : arrived) {
                admit(std::move(arrival));
            }
            if (lingerUntil && Clock::now() >= *lingerUntil) {
                endLinger();
                if (currentSessions().empty()) {
                    logInfo("remote", "nobody came back; acquisition stopped");
                    idle();
                }
            }

            const bool executed = execute(batch);
            instrument.tick(monotonicNs());
            if (recorder) {
                recorder->setCompletePassesOnly(instrument.sweeping());
            }
            const bool wasRunning = running;
            running = instrument.running();

            const std::vector<SessionPtr> current = currentSessions();
            const std::vector<InstrumentNotice> notices = instrument.takeNotices();
            if (current.empty()) {
                continue;
            }
            for (const InstrumentNotice& notice : notices) {
                const std::vector<std::byte> bytes =
                    messageBytes(msg::kNotice, encodeNotice(notice));
                for (const SessionPtr& session : current) {
                    session->pushControl(bytes);
                }
            }
            if (wasRunning && !running) {
                for (const SessionPtr& session : current) {
                    session->closeSegment();
                }
            }

            const auto now = Clock::now();
            if (executed || stateDue || now - lastState >= kStateInterval) {
                refreshSections(executed);
                for (const SessionPtr& session : current) {
                    sendState(*session, current);
                }
                lastState = now;
                stateDue = false;
            }
            if (now - lastTelemetry >= kTelemetryInterval) {
                const TelemetryReport report = telemetryReport();
                for (const SessionPtr& session : current) {
                    sendTelemetry(*session, report);
                }
                lastTelemetry = now;
            }
        }

        for (const SessionPtr& session : currentSessions()) {
            session->finish(refusal::kShutdown);
        }
        {
            const std::lock_guard lock(sessionMutex);
            sessions.clear();
        }
        instrument.stop();
        (void)stopRecording();
        {
            const std::lock_guard lock(controlMutex);
            arrivals.clear();
            commands.clear();
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
        {
            const std::lock_guard lock(recordingMutex);
            activeRecording = name;
        }
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
        {
            const std::lock_guard lock(recordingMutex);
            activeRecording.clear();
        }
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

    Status fetchRecording(Session& target, const std::string& name, std::uint64_t offset) {
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
        if (!target.pushBulk(messageBytes(msg::kChunk, chunk.toMetadata()))) {
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

    // ---- clients and control -----------------------------------------------------

    void admit(Arrival arrival) {
        const std::size_t connected = currentSessions().size();
        if (connected >= clientLimit()) {
            const std::string peer = arrival.socket->peerAddress();
            const bool exclusive = !config.shared;
            logInfo("remote", "{} turned away: {}", peer,
                    exclusive ? "another client is connected"
                              : std::format("{} clients are connected", connected));
            (void)arrival.socket->sendAll(asBytes(messageBytes(
                msg::kRefused,
                Refused{.reason = std::string(exclusive ? refusal::kBusy : refusal::kLimit),
                        .message = exclusive ? std::string("another client is connected")
                                             : std::format("this server takes {} clients at once",
                                                           clientLimit())}
                    .toMetadata())));
            arrival.socket->shutdown();
            return;
        }

        auto fresh = std::make_shared<Session>(++lastSessionId, std::move(arrival.socket),
                                               std::move(arrival.framer), arrival.hello,
                                               config.silenceTimeout);
        logInfo("remote", "{} ({}, {}) connected", fresh->name(), fresh->kind(), fresh->peer());

        // Control that is free goes to whoever arrives -- except while a
        // dropped controller's radio is being kept for it, when it waits for
        // that client to come back. A server for one client has nobody else
        // to wait for.
        const bool returning = !lingerClientId.empty() && fresh->clientId() == lingerClientId;
        std::vector<SpectrumFramePtr> missed;
        if (returning) {
            missed = backlog.take();
        }
        if (returning || !lingerUntil) {
            endLinger();
        }
        if (controllerId.load() == 0 && (!lingerUntil || !config.shared)) {
            endLinger();
            setController(fresh->id());
        }

        fresh->pushControl(messageBytes(
            msg::kWelcome,
            Welcome{.serverName = serverName, .shared = config.shared, .serverNs = monotonicNs()}
                .toMetadata()));
        {
            const std::lock_guard lock(sessionMutex);
            sessions.push_back(fresh);
            sessionEnded = false;
        }
        refreshSections(false);
        sendState(*fresh, currentSessions());
        sendTelemetry(*fresh, telemetryReport());
        stateDue = true;
        if (!missed.empty()) {
            logInfo("remote", "{} is back; sending the {} passes it missed", fresh->name(),
                    missed.size());
            fresh->offerReplay(std::move(missed));
        }

        const std::weak_ptr<Session> self = fresh;
        fresh->run([this, self](
                       Command command) { return enqueueCommand(self.lock(), std::move(command)); },
                   [this] {
                       {
                           const std::lock_guard lock(controlMutex);
                           sessionEnded = true;
                       }
                       wakeControl();
                   });
    }

    /// Clients that went away: joined and dropped from the list. When none
    /// is left, the radio stops, unless it is being kept for a controller
    /// that dropped.
    void reapSessions() {
        std::vector<SessionPtr> ended;
        {
            const std::lock_guard lock(sessionMutex);
            for (auto it = sessions.begin(); it != sessions.end();) {
                if ((*it)->alive()) {
                    ++it;
                    continue;
                }
                ended.push_back(std::move(*it));
                it = sessions.erase(it);
            }
        }
        {
            const std::lock_guard lock(controlMutex);
            sessionEnded = false;
            // Queued by a client that is no longer there to be answered.
            std::erase_if(commands, [&ended](const Queued& queued) {
                return std::ranges::find(ended, queued.from) != ended.end();
            });
        }
        if (ended.empty()) {
            return;
        }

        for (const SessionPtr& gone : ended) {
            gone->join();
            stateDue = true;
            if (gone->id() != controllerId.load()) {
                logInfo("remote", "{} left", gone->name());
                continue;
            }
            setController(0);
            // A goodbye is someone leaving; anything else may be a cable, and
            // the desktop at the other end is already trying to come back.
            if (!gone->saidGoodbye() && config.linger.count() > 0 && instrument.running()) {
                lingerUntil = Clock::now() + config.linger;
                lingerClientId = gone->clientId();
                if (!lingerClientId.empty()) {
                    backlog.begin(config.backlog, config.backlogBytes);
                }
                logInfo("remote", "{} dropped; the radio keeps running for {:.1f} s", gone->name(),
                        std::chrono::duration<double>(config.linger).count());
            } else {
                logInfo("remote", "{} gone", gone->name());
            }
            if (config.shared) {
                notifyAll(InstrumentNotice::Kind::Info,
                          std::format("{} left; nobody has control", gone->name()));
            }
        }

        if (currentSessions().empty() && !lingerUntil) {
            logInfo("remote", "nobody connected; acquisition stopped");
            idle();
        }
    }

    /// Nobody is being waited for any more, and nothing kept for them.
    void endLinger() {
        lingerUntil.reset();
        lingerClientId.clear();
        backlog.drop();
    }

    /// The radio stopped with nobody to watch it.
    void idle() {
        endLinger();
        instrument.cancelLearning();
        instrument.stop();
        running = false;
    }

    void setController(std::uint64_t id) {
        controllerId.store(id);
        stateDue = true;
        if (advertiser) {
            advertiser->setBusy(id != 0);
        }
    }

    void notifyAll(InstrumentNotice::Kind kind, std::string text) {
        const std::vector<std::byte> bytes = messageBytes(
            msg::kNotice, encodeNotice(InstrumentNotice{.kind = kind, .text = std::move(text)}));
        for (const SessionPtr& session : currentSessions()) {
            session->pushControl(bytes);
        }
    }

    Status takeControl(const Session& from) {
        const std::uint64_t previous = controllerId.load();
        if (previous == from.id()) {
            return ok();
        }
        setController(from.id());
        endLinger();
        logInfo("remote", "{} took control", from.name());
        notifyAll(InstrumentNotice::Kind::Warning,
                  std::format("{} ({}) took control", from.name(), from.kind()));
        return ok();
    }

    Status releaseControl(const Session& from) {
        if (controllerId.load() != from.id()) {
            return ok();
        }
        setController(0);
        notifyAll(InstrumentNotice::Kind::Info, std::format("{} released control", from.name()));
        return ok();
    }

    [[nodiscard]] std::string controllerName() const {
        const std::uint64_t id = controllerId.load();
        for (const SessionPtr& session : currentSessions()) {
            if (session->id() == id) {
                return session->name();
            }
        }
        return {};
    }

    // ---- commands --------------------------------------------------------------------

    /// Runs a batch in order, skipping any edit a later one from the same
    /// client in the same batch replaces. True when anything ran.
    bool execute(std::vector<Queued>& batch) {
        if (batch.empty()) {
            return false;
        }
        for (std::size_t i = 0; i < batch.size(); ++i) {
            Session& from = *batch[i].from;
            const Command& command = batch[i].command;
            const bool superseded = std::any_of(
                batch.begin() + static_cast<std::ptrdiff_t>(i) + 1, batch.end(),
                [&](const Queued& later) {
                    if (later.from.get() != &from || later.command.op != command.op) {
                        return false;
                    }
                    if (command.op == op::kSetParameter) {
                        return later.command.args.getString("key") == command.args.getString("key");
                    }
                    return supersedes(command.op);
                });

            Status status = ok();
            if (from.id() != controllerId.load() && !viewerMay(command.op)) {
                const std::string holder = controllerName();
                status = fail(ErrorCode::PermissionDenied, "{}",
                              holder.empty() ? std::string("Nobody has control; take it first")
                                             : std::format("{} has control", holder));
            } else if (!superseded) {
                status = run(command, from);
            }

            Session::Ledger& ledger = from.ledger();
            ledger.lastAck = std::max(ledger.lastAck, command.seq);
            // Sent again with the acknowledgement even if unchanged: the
            // client put its own edit there, and a refused one must be undone.
            for (const std::string_view touched : sectionsTouchedBy(command.op)) {
                if (const auto found = ledger.sent.find(touched); found != ledger.sent.end()) {
                    ledger.sent.erase(found);
                }
            }
            Reply reply{.seq = command.seq, .ok = status.has_value()};
            if (!status) {
                reply.code = status.error().code();
                reply.message = status.error().message();
            }
            from.pushControl(messageBytes(msg::kReply, reply.toMetadata()));
        }
        return true;
    }

    Status run(const Command& command, Session& from) {
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
            return fetchRecording(from, args.getString("name"),
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
            from.setMaxBins(static_cast<std::uint32_t>(bins));
            return ok();
        }
        if (name == op::kTakeControl) {
            return takeControl(from);
        }
        if (name == op::kHistoryOpen) {
            return historyOpen(from, args.getString("name"));
        }
        if (name == op::kHistoryQuery) {
            return historyQuery(from, args, command.seq);
        }
        if (name == op::kHistoryClose) {
            from.ledger().readers.erase(args.getInt("handle"));
            return ok();
        }
        if (name == op::kReleaseControl) {
            return releaseControl(from);
        }
        if (name.starts_with("setContributor") || name == op::kSelectContributorDataset ||
            name == op::kToggleContributorRow || name == op::kHideContribution) {
            return runContributor(name, args);
        }
        return fail(ErrorCode::Unsupported, "this server does not know '{}'", name);
    }

    /// The server's own plugins, which label what every client is shown.
    static Status runContributor(std::string_view name, const Metadata& args) {
        PluginManager& plugins = PluginManager::instance();
        const std::string id = args.getString("id");
        if (name == op::kSetContributorShown) {
            return plugins.setContributorShown(id, args.getBool("shown"));
        }
        if (name == op::kSetContributorOrder) {
            std::vector<std::string> ids;
            const sweeps::Value* list = args.find("ids");
            if (const std::vector<sweeps::Value>* items =
                    list != nullptr ? list->asArray() : nullptr) {
                for (const sweeps::Value& item : *items) {
                    if (const std::string* text = item.asStringRef()) {
                        ids.push_back(*text);
                    }
                }
            }
            return plugins.setContributorOrder(ids);
        }
        if (name == op::kSelectContributorDataset) {
            const std::int64_t index = args.getInt("index", -1);
            if (index < 0) {
                return fail(ErrorCode::OutOfRange, "no dataset {}", index);
            }
            return plugins.selectDataset(id, static_cast<std::uint32_t>(index));
        }
        if (name == op::kToggleContributorRow) {
            return plugins.toggleContributorRow(id, args.getString("key"));
        }
        if (name == op::kHideContribution) {
            const Contribution entry{.pluginId = id,
                                     .name = args.getString("name"),
                                     .category = args.getString("category"),
                                     .startHz = args.getFloat("startHz"),
                                     .stopHz = args.getFloat("stopHz")};
            if (!plugins.hideContribution(entry)) {
                return fail(ErrorCode::Unavailable, "'{}' cannot hide '{}'", id, entry.name);
            }
            return ok();
        }
        return fail(ErrorCode::Unsupported, "this server does not know '{}'", name);
    }

    // ---- history of recordings ------------------------------------------------------

    /// Opens a recording for this client to read, and tells it what is in it:
    /// the time span, the frequencies, and each segment's grid.
    Status historyOpen(Session& from, const std::string& name) {
        Session::Ledger& ledger = from.ledger();
        if (ledger.readers.size() >= kMaxHistoryReaders) {
            return fail(ErrorCode::Unavailable, "{} recordings are open already; close one first",
                        kMaxHistoryReaders);
        }
        auto path = recordingPath(name);
        if (!path) {
            return std::unexpected(std::move(path).error());
        }
        auto reader = sweeppp::adopt(sweeps::SessionReader::open(*path));
        if (!reader) {
            return std::unexpected(std::move(reader).error());
        }
        const sweeps::SessionSummary& summary = (*reader)->summary();
        std::vector<sweeps::Value> segments;
        for (const sweeps::SegmentInfo& segment : (*reader)->segments()) {
            Metadata row;
            row.setInt("id", segment.id);
            row.setFloat("startHz", segment.grid.startHz);
            row.setFloat("binWidthHz", segment.grid.binWidthHz);
            row.setInt("binCount", segment.grid.binCount);
            row.setInt("startMonotonicNs", static_cast<std::int64_t>(segment.startMonotonicNs));
            row.setInt("startWallNs", static_cast<std::int64_t>(segment.startWallNs));
            row.setString("reason", segment.reason);
            segments.push_back(sweeps::Value::ofHash(std::move(row)));
        }
        const std::int64_t handle = ++ledger.nextReader;
        Metadata body;
        body.setInt("handle", handle);
        body.setString("kind", "opened");
        body.setString("name", name);
        body.setInt("firstLineNs", static_cast<std::int64_t>(summary.firstLineNs));
        body.setInt("lastLineNs", static_cast<std::int64_t>(summary.lastLineNs));
        body.setInt("createdWallNs", static_cast<std::int64_t>(summary.createdWallNs));
        body.setInt("totalLines", static_cast<std::int64_t>(summary.totalLines));
        body.setFloat("lowestHz", summary.lowestHz);
        body.setFloat("highestHz", summary.highestHz);
        body.set("segments",
                 sweeps::Value::ofArray(sweeps::Value::Type::Hash, std::move(segments)));
        from.pushControl(messageBytes(msg::kHistory, body));
        ledger.readers.emplace(handle, std::move(*reader));
        return ok();
    }

    /// A range of an open recording, at the level of detail that fits the
    /// lines and bins asked for. Sent after the live frames, in pieces of a
    /// couple of megabytes; the last says so.
    Status historyQuery(Session& from, const Metadata& args, std::uint64_t seq) {
        const auto found = from.ledger().readers.find(args.getInt("handle"));
        if (found == from.ledger().readers.end()) {
            return fail(ErrorCode::NotFound, "no recording is open as {}", args.getInt("handle"));
        }
        const sweeps::SessionReader& reader = *found->second;
        sweeps::HistoryQuery query;
        query.fromNs = static_cast<std::uint64_t>(args.getInt("fromNs"));
        if (const std::int64_t to = args.getInt("toNs"); to > 0) {
            query.toNs = static_cast<std::uint64_t>(to);
        }
        query.fromHz = args.getFloat("fromHz");
        if (const double to = args.getFloat("toHz"); to > 0.0) {
            query.toHz = to;
        }
        query.maxLines = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(args.getInt("lines", 1024), 1, kMaxHistoryLines));
        query.maxBins = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(args.getInt("bins", 2048), 1, kMaxHistoryBins));
        if (const std::int64_t segment = args.getInt("segmentId", -1); segment >= 0) {
            query.segmentId = static_cast<std::uint32_t>(segment);
        }
        auto tiles = sweeppp::adopt(reader.query(query));
        if (!tiles) {
            return std::unexpected(std::move(tiles).error());
        }

        constexpr std::size_t kPieceBytes = std::size_t{2} * 1024 * 1024;
        std::vector<std::byte> out;
        std::vector<sweeps::Value> piece;
        std::size_t pieceBytes = 0;
        const auto flush = [&](bool last) {
            Metadata body;
            body.setInt("handle", found->first);
            body.setString("kind", "tiles");
            body.setInt("query", static_cast<std::int64_t>(seq));
            body.setBool("last", last);
            body.set("tiles", sweeps::Value::ofArray(sweeps::Value::Type::Hash, std::move(piece)));
            appendMessage(out, msg::kHistory, body);
            piece.clear();
            pieceBytes = 0;
        };
        for (sweeps::HistoryTile& tile : *tiles) {
            Metadata row;
            row.setInt("segmentId", tile.segmentId);
            row.setInt("lod", tile.lod);
            row.setInt("timeBlock", tile.timeBlock);
            row.setInt("freqBlock", tile.freqBlock);
            row.setInt("lines", tile.lines);
            row.setInt("bins", tile.bins);
            row.setInt("firstLineNs", static_cast<std::int64_t>(tile.firstLineNs));
            row.setInt("lastLineNs", static_cast<std::int64_t>(tile.lastLineNs));
            row.setFloat("startHz", tile.startHz);
            row.setFloat("binWidthHz", tile.binWidthHz);
            row.setFloat("originDb", static_cast<double>(tile.originDb));
            pieceBytes += tile.data.size();
            std::vector<std::byte> data(tile.data.size());
            std::memcpy(data.data(), tile.data.data(), tile.data.size());
            row.setBytes("data", std::move(data));
            piece.push_back(sweeps::Value::ofHash(std::move(row)));
            if (pieceBytes >= kPieceBytes) {
                flush(false);
            }
        }
        flush(true);
        if (!from.pushBulk(std::move(out))) {
            return fail(ErrorCode::Unavailable, "too much is waiting to be sent; ask again");
        }
        return ok();
    }

    // ---- state and telemetry -----------------------------------------------------------

    /// The sections every client is sent alike. The bench sections are
    /// rebuilt only when the instrument's revision moved or a command ran; the
    /// rest are cheap enough to build every time.
    void refreshSections(bool afterCommand) {
        const auto put = [this](std::string_view name, Metadata body) {
            Built& built = sectionCache[std::string(name)];
            built.bytes.clear();
            body.encode(built.bytes);
            built.body = std::move(body);
        };

        const DeviceDescriptor* device = instrument.device();
        const bool benchChanged =
            afterCommand || !haveBench || instrument.revision() != lastRevision;
        if (benchChanged) {
            haveBench = true;
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
            put(section::kDevice, std::move(deviceSection));

            Metadata backends = encodeBackends(instrument.fftBackends());
            backends.setString("current", instrument.fftBackendName());
            put(section::kBackends, std::move(backends));

            put(section::kAntennas, encodeAntennas(instrument.antennas().entries()));
            put(section::kAssignments, encodeAssignments(instrument.antennaAssignments()));

            Metadata switchers;
            switchers.setHash("open", encodeSwitcherViews(instrument.openSwitchers()));
            switchers.setHash("available", encodeSwitcherInfos(instrument.availableSwitchers()));
            put(section::kSwitchers, std::move(switchers));
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
        put(section::kValues, std::move(values));

        Metadata run;
        run.setBool("running", instrument.running());
        run.setBool("sweeping", instrument.sweeping());
        run.setInt("startGeneration", static_cast<std::int64_t>(instrument.startGeneration()));
        run.setHash("engine", encodeEngineStats(instrument.engineStats()));
        put(section::kRun, std::move(run));

        put(section::kPlan, encodePlan(instrument.sweepPlan()));
        put(section::kSchedule, encodeSchedule(instrument.schedule()));
        put(section::kPipeline, encodePipeline(instrument.pipelineConfig()));

        Metadata corrections;
        corrections.setHash("settings", encodeCorrectionSettings(instrument.correctionSettings()));
        corrections.setHash("summary", encodeCorrectionSummary(instrument.correctionSummary()));
        put(section::kCorrections, std::move(corrections));

        Metadata learning;
        learning.setBool("active", instrument.learning());
        learning.setString("label", instrument.learningLabel());
        put(section::kLearning, std::move(learning));

        Metadata rfPath;
        rfPath.setHash("legs", encodeRfLegs(instrument.rfPath()));
        rfPath.setHash("coverage", encodeRanges(instrument.antennaCoverage()));
        put(section::kRfPath, std::move(rfPath));

        put(section::kBenchmark, encodeBenchmarkStatus(instrument.benchmark()));
        put(section::kRecordings, recordingsState().toMetadata());

        Metadata overlays;
        overlays.setInt("generation", static_cast<std::int64_t>(
                                          PluginManager::instance().contributionsGeneration()));
        put(section::kOverlays, std::move(overlays));
    }

    /// Every section that differs from what this client last received: the
    /// shared ones as last built, then its own.
    void sendState(Session& target, const std::vector<SessionPtr>& everyone) {
        Session::Ledger& ledger = target.ledger();
        Metadata sections;
        const auto offer = [&](std::string_view name, const Metadata& body,
                               const std::vector<std::byte>& bytes) {
            const auto last = ledger.sent.find(name);
            if (last != ledger.sent.end() && last->second == bytes) {
                return;
            }
            ledger.sent.insert_or_assign(std::string(name), bytes);
            sections.setHash(std::string(name), body);
        };
        for (const auto& [name, built] : sectionCache) {
            offer(name, built.body, built.bytes);
        }

        const auto own = [&](std::string_view name, const Metadata& body) {
            std::vector<std::byte> bytes;
            body.encode(bytes);
            offer(name, body, bytes);
        };

        Metadata link;
        link.setInt("maxBins", target.maxBins());
        own(section::kLink, link);

        const std::uint64_t controller = controllerId.load();
        ControlState control{
            .shared = config.shared, .you = target.id() == controller, .held = controller != 0};
        std::vector<ConnectedClient> clients;
        clients.reserve(everyone.size());
        for (const SessionPtr& session : everyone) {
            const bool controls = session->id() == controller;
            if (controls) {
                control.controller = session->name();
                control.controllerKind = session->kind();
            }
            clients.push_back(ConnectedClient{.id = session->id(),
                                              .name = session->name(),
                                              .kind = session->kind(),
                                              .address = session->peer(),
                                              .controls = controls,
                                              .you = session.get() == &target});
        }
        own(section::kControl, control.toMetadata());
        own(section::kClients, encodeClients(clients));

        // An acknowledgement goes out even when nothing changed: the client
        // is holding its own copy of what it edited until it arrives.
        if (sections.empty() && ledger.lastAck == ledger.lastSentAck) {
            return;
        }
        ledger.lastSentAck = ledger.lastAck;
        target.pushControl(messageBytes(
            msg::kState,
            State{.ackSeq = ledger.lastAck, .sections = std::move(sections)}.toMetadata()));
    }

    [[nodiscard]] TelemetryReport telemetryReport() {
        const TelemetrySnapshot& snapshot = telemetry.sample();
        TelemetryReport report;
        report.stream = snapshot.stream;
        report.process = snapshot.process;
        report.health = instrument.health();

        // Once a second: a CPU share over a quarter second is mostly noise, and
        // the disk and the sensors do not move faster than that.
        if (const auto now = Clock::now(); !host || now - lastHostSample >= 1s) {
            host = hostSampler.sample(config.sessionsDir);
            lastHostSample = now;
        }
        report.host = host;
        return report;
    }

    static void sendTelemetry(Session& target, TelemetryReport report) {
        report.link = target.link();
        std::vector<std::byte> bytes;
        appendTelemetry(bytes, report, monotonicNs());
        target.pushControl(std::move(bytes));
    }

    // ---- members ------------------------------------------------------------------

    HostSampler hostSampler;
    std::optional<HostStats> host;
    Clock::time_point lastHostSample{};

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
    std::deque<Queued> commands;
    bool sessionEnded = false;

    mutable std::mutex sessionMutex;
    std::vector<SessionPtr> sessions;
    /// The session that controls the radio; zero for none. Written by the
    /// control thread, read by `clients()`.
    std::atomic<std::uint64_t> controllerId{0};

    // The control thread's alone.
    std::uint64_t lastSessionId = 0;
    std::map<std::string, Built, std::less<>> sectionCache;
    bool haveBench = false;
    std::uint64_t lastRevision = 0;
    bool stateDue = false;
    bool running = false;
    std::optional<Clock::time_point> lingerUntil;
    /// Whose radio is being kept running: they get control back.
    std::string lingerClientId;
    Backlog backlog;

    std::unique_ptr<mdns::Advertiser> advertiser;
    std::unique_ptr<session::SessionRecorder> recorder;
    FrameBus::SubscriptionId recorderSubscription = 0;
    std::string recordingName;
    /// The same, for `recordingFile()` on any thread.
    mutable std::mutex recordingMutex;
    std::string activeRecording;

    std::mutex adoptMutex;
    std::vector<Handshake> greetings;
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
                                                 .authRequired = !impl.config.token.empty(),
                                                 .shared = impl.config.shared});
        if (advertiser) {
            impl.advertiser = std::move(*advertiser);
        } else {
            logWarn("remote", "not advertised on the LAN: {}", advertiser.error().describe());
        }
    }

    impl.controlThread = std::thread([&impl] { impl.controlLoop(); });
    impl.listenThread = std::thread([&impl] { impl.listenLoop(); });
    impl.started = true;
    logInfo("remote", "serving on {}:{}{}{}", impl.config.listenAddress, impl.listener.port(),
            impl.config.token.empty() ? " (no token)" : "",
            impl.config.shared ? std::format(", shared by up to {}", impl.clientLimit()) : "");
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
    {
        const std::lock_guard lock(impl.adoptMutex);
        for (Impl::Handshake& pending : impl.greetings) {
            pending.thread.join();
        }
        impl.greetings.clear();
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
    return std::ranges::any_of(m_impl->currentSessions(),
                               [](const Impl::SessionPtr& session) { return session->alive(); });
}

void RemoteServer::adopt(std::unique_ptr<net::ByteStream> stream) {
    if (!m_impl->started) {
        stream->close();
        return;
    }
    m_impl->adopt(std::move(stream));
}

Result<std::filesystem::path> RemoteServer::recordingFile(const std::string& name) const {
    using Path = std::filesystem::path;
    const Impl& impl = *m_impl;
    if (impl.config.sessionsDir.empty()) {
        return fail<Path>(ErrorCode::NotFound, "this server keeps no recordings");
    }
    if (name.empty() || name.starts_with('.') || name.find_first_of("/\\") != std::string::npos ||
        name.find("..") != std::string::npos || !name.ends_with(".sweeps")) {
        return fail<Path>(ErrorCode::InvalidArgument, "'{}' is not a recording's name", name);
    }
    const Path path = impl.config.sessionsDir / name;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return fail<Path>(ErrorCode::NotFound, "no recording called '{}'", name);
    }
    {
        const std::lock_guard lock(impl.recordingMutex);
        if (name == impl.activeRecording) {
            return fail<Path>(ErrorCode::Unavailable, "'{}' is still being recorded", name);
        }
    }
    return path;
}

std::vector<ConnectedClient> RemoteServer::clients() const {
    const std::uint64_t controller = m_impl->controllerId.load();
    std::vector<ConnectedClient> clients;
    for (const Impl::SessionPtr& session : m_impl->currentSessions()) {
        clients.push_back(ConnectedClient{.id = session->id(),
                                          .name = session->name(),
                                          .kind = session->kind(),
                                          .address = session->peer(),
                                          .controls = session->id() == controller});
    }
    std::ranges::stable_partition(clients, &ConnectedClient::controls);
    return clients;
}

} // namespace sweeppp::remote
