// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/remote/RemoteServer.hpp"

#include "remote/StreamIo.hpp"
#include "sweeppp/core/Clock.hpp"
#include "sweeppp/core/Log.hpp"
#include "sweeppp/crypto/Sha256.hpp"
#include "sweeppp/history/EventMapping.hpp"
#include "sweeppp/net/Socket.hpp"
#include "sweeppp/remote/FrameCodec.hpp"
#include "sweeppp/remote/Messages.hpp"
#include "sweeppp/remote/WireCodec.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <format>
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
    Session(net::TcpSocket socket, sweeps::RecordFramer framer,
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
    void offerFrame(const SpectrumFramePtr& frame) {
        {
            const std::lock_guard lock(m_mutex);
            if (frame->passComplete) {
                if (m_pass) {
                    m_passesCoalesced.fetch_add(1);
                }
                if (m_partial) {
                    m_partialsCoalesced.fetch_add(1);
                    m_partial.reset();
                }
                m_pass = frame;
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

    [[nodiscard]] LinkStats link() const {
        LinkStats link;
        link.framesSent = m_framesSent.load();
        link.passesCoalesced = m_passesCoalesced.load();
        link.partialsCoalesced = m_partialsCoalesced.load();
        link.eventsDropped = m_eventsDropped.load();
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
            return false;
        }
        // Any other name is from a newer client, and ignored.
        return true;
    }

    void sendLoop() {
        FrameEncoder encoder;
        std::vector<std::byte> buffer;
        std::vector<std::vector<std::byte>> control;
        std::deque<OutgoingEvent> events;
        while (true) {
            SpectrumFramePtr pass;
            SpectrumFramePtr partial;
            bool closeSegment = false;
            bool finishing = false;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait(lock, [this] {
                    return !m_alive.load() || m_finishing || !m_control.empty() ||
                           !m_events.empty() || m_pass || m_partial || m_closeSegment;
                });
                if (!m_alive.load() && !m_finishing) {
                    return;
                }
                control.swap(m_control);
                events.swap(m_events);
                pass = std::exchange(m_pass, nullptr);
                partial = std::exchange(m_partial, nullptr);
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
            if (pass) {
                encoder.encode(*pass, buffer);
                m_framesSent.fetch_add(1);
            }
            if (partial) {
                encoder.encode(*partial, buffer);
                m_framesSent.fetch_add(1);
            }
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

    net::TcpSocket m_socket;
    sweeps::RecordFramer m_framer;
    std::string m_peer;
    std::chrono::milliseconds m_silenceTimeout;
    std::atomic<bool> m_alive{true};

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::vector<std::vector<std::byte>> m_control;
    std::deque<OutgoingEvent> m_events;
    SpectrumFramePtr m_pass;
    SpectrumFramePtr m_partial;
    bool m_closeSegment = false;
    bool m_finishing = false;

    std::atomic<std::uint64_t> m_framesSent{0};
    std::atomic<std::uint64_t> m_passesCoalesced{0};
    std::atomic<std::uint64_t> m_partialsCoalesced{0};
    std::atomic<std::uint64_t> m_eventsDropped{0};

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
        net::TcpSocket socket;
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

        if (!io::sendStreamHeader(socket)) {
            return;
        }
        if (auto theirs = io::readStreamHeader(socket, deadline, stopping); !theirs) {
            logInfo("remote", "{}: {}", peer, theirs.error().describe());
            return;
        }

        const auto refuse = [&](std::string_view reason, std::string text) {
            (void)socket.sendAll(asBytes(messageBytes(
                msg::kRefused,
                Refused{.reason = std::string(reason), .message = std::move(text)}.toMetadata())));
            socket.shutdown();
        };

        sweeps::RecordFramer framer(kMaxPreAuthRecordBytes);
        const auto nextMessage = [&](std::string_view expected) -> Result<Message> {
            auto record = io::readRecord(socket, framer, deadline, stopping);
            if (!record) {
                return std::unexpected(std::move(record).error());
            }
            if (!isControl(*record)) {
                return fail<Message>(ErrorCode::ProtocolError,
                                     "expected '{}', got a record of type {}", expected,
                                     record->header.type);
            }
            auto message = decodeMessage(*record);
            if (message && message->name != expected) {
                return fail<Message>(ErrorCode::ProtocolError, "expected '{}', got '{}'", expected,
                                     message->name);
            }
            return message;
        };

        auto hello = nextMessage(msg::kHello);
        if (!hello) {
            logInfo("remote", "{}: {}", peer, hello.error().describe());
            if (hello.error().code() == ErrorCode::Unsupported) {
                refuse(refusal::kVersion, hello.error().message());
            } else if (hello.error().code() == ErrorCode::ProtocolError) {
                refuse(refusal::kProtocol, hello.error().message());
            }
            return;
        }
        const Hello greeting = Hello::from(hello->body);
        if (greeting.protocolVersion != kProtocolVersion) {
            refuse(refusal::kVersion, std::format("this server speaks protocol {}, the client {}",
                                                  kProtocolVersion, greeting.protocolVersion));
            return;
        }

        Challenge challenge{.protocolVersion = kProtocolVersion,
                            .authRequired = !config.token.empty(),
                            .software = std::format("{} {}", productName(), versionString())};
        if (!crypto::fillRandom(challenge.serverNonce)) {
            refuse(refusal::kProtocol, "the server could not make a nonce");
            return;
        }
        if (!socket.sendAll(asBytes(messageBytes(msg::kChallenge, challenge.toMetadata())))) {
            return;
        }

        auto auth = nextMessage(msg::kAuth);
        if (!auth) {
            logInfo("remote", "{}: {}", peer, auth.error().describe());
            if (auth.error().code() == ErrorCode::ProtocolError) {
                refuse(refusal::kProtocol, auth.error().message());
            }
            return;
        }
        if (challenge.authRequired) {
            const Auth answer = Auth::from(auth->body);
            const crypto::Sha256Digest expected =
                authMac(config.token, challenge.serverNonce, answer.clientNonce);
            if (!crypto::constantTimeEqual(expected, answer.mac)) {
                logWarn("remote", "{}: wrong token", peer);
                // Slows guessing to one try a second per connection.
                std::unique_lock lock(controlMutex);
                controlWake.wait_for(lock, config.refusalDelay, [this] { return stopping.load(); });
                lock.unlock();
                refuse(refusal::kAuth, "the token was not accepted");
                return;
            }
        }

        framer.setMaxPayloadBytes(kMaxClientRecordBytes);
        {
            const std::lock_guard lock(controlMutex);
            arrivals.push_back(Arrival{.socket = std::move(socket), .framer = std::move(framer)});
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

            const bool executed = execute(batch);
            instrument.tick(monotonicNs());
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
        {
            const std::lock_guard lock(controlMutex);
            arrivals.clear();
        }
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
        logInfo("remote", "{} gone; acquisition stopped", ended->peer());
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
