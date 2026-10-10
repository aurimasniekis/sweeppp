// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/remote/RemoteInstrument.hpp"

#include "remote/StreamIo.hpp"
#include "sweeppp/core/Clock.hpp"
#include "sweeppp/core/Log.hpp"
#include "sweeppp/core/Version.hpp"
#include "sweeppp/history/EventMapping.hpp"
#include "sweeppp/net/Socket.hpp"
#include "sweeppp/remote/ClockMap.hpp"
#include "sweeppp/remote/FrameCodec.hpp"
#include "sweeppp/remote/Handshake.hpp"
#include "sweeppp/remote/Messages.hpp"
#include "sweeppp/remote/WireCodec.hpp"

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <format>
#include <fstream>
#include <mutex>
#include <sweeps/Records.hpp>
#include <thread>

namespace sweeppp::remote {
namespace {

using io::asBytes;
using io::Clock;
using sweeps::Metadata;

constexpr std::uint64_t kRateWindowNs = 1'000'000'000;

bool isType(const sweeps::StreamRecord& record, sweeps::RecordType type) {
    return record.header.type == static_cast<std::uint16_t>(type);
}

std::vector<std::byte> messageBytes(std::string_view name, const Metadata& body) {
    std::vector<std::byte> out;
    appendMessage(out, name, body, monotonicNs());
    return out;
}

/// A refusal, as the error a caller can show.
Error refusalError(const Refused& refused, const std::string& server) {
    if (refused.reason == refusal::kBusy) {
        return Error{ErrorCode::Unavailable,
                     std::format("{} is in use by another desktop", server)};
    }
    if (refused.reason == refusal::kLimit) {
        return Error{ErrorCode::Unavailable,
                     std::format("{} has as many clients as it takes", server)};
    }
    if (refused.reason == refusal::kVersion) {
        return Error{ErrorCode::Unsupported, std::format("{}: {}", server, refused.message)};
    }
    return Error{ErrorCode::ProtocolError,
                 std::format("{} refused the connection: {}", server,
                             refused.message.empty() ? refused.reason : refused.message)};
}

} // namespace

// --------------------------------------------------------------- the endpoint

std::string RemoteEndpoint::address() const {
    if (host.find(':') != std::string::npos) {
        return std::format("[{}]:{}", host, port);
    }
    return std::format("{}:{}", host, port);
}

Result<RemoteEndpoint> RemoteEndpoint::parse(std::string_view address) {
    RemoteEndpoint endpoint;
    std::string_view portText;
    if (address.starts_with('[')) {
        const std::size_t close = address.find(']');
        if (close == std::string_view::npos) {
            return fail<RemoteEndpoint>(ErrorCode::InvalidArgument, "'{}' has no closing ]",
                                        address);
        }
        endpoint.host = std::string(address.substr(1, close - 1));
        const std::string_view rest = address.substr(close + 1);
        if (!rest.empty()) {
            if (!rest.starts_with(':')) {
                return fail<RemoteEndpoint>(ErrorCode::InvalidArgument, "'{}' is not host:port",
                                            address);
            }
            portText = rest.substr(1);
        }
    } else if (const std::size_t colon = address.rfind(':');
               colon != std::string_view::npos && address.find(':') == colon) {
        endpoint.host = std::string(address.substr(0, colon));
        portText = address.substr(colon + 1);
    } else {
        // No colon, or several: a bare IPv6 address with no port.
        endpoint.host = std::string(address);
    }

    if (endpoint.host.empty()) {
        return fail<RemoteEndpoint>(ErrorCode::InvalidArgument, "'{}' names no host", address);
    }
    if (!portText.empty()) {
        unsigned value = 0;
        const auto [end, error] =
            std::from_chars(portText.data(), portText.data() + portText.size(), value);
        if (error != std::errc{} || end != portText.data() + portText.size() || value == 0 ||
            value > 65535) {
            return fail<RemoteEndpoint>(ErrorCode::InvalidArgument, "'{}' is not a port", portText);
        }
        endpoint.port = static_cast<std::uint16_t>(value);
    }
    return endpoint;
}

// -------------------------------------------------------------------- the link

/// The connection and the two threads on it. Everything the reader hands the
/// owner goes through the inbox; everything the owner sends goes through the
/// outbox.
struct RemoteInstrument::Link {
    Link(net::SecureChannel socket, sweeps::RecordFramer framer, FrameBus& output, EventBus& events,
         std::int64_t welcomeOffsetNs, std::uint64_t resumeAfterNs)
        : socket(std::move(socket)), framer(std::move(framer)), output(output), events(events),
          welcomeOffsetNs(welcomeOffsetNs), lastReplayNs(resumeAfterNs) {}

    void start() {
        reader = std::thread([this] { readLoop(); });
        writer = std::thread([this] { writeLoop(); });
    }

    /// Says goodbye if it can, then closes and joins.
    void close() {
        {
            const std::lock_guard lock(outMutex);
            closing = true;
        }
        outWake.notify_all();
        if (writer.joinable()) {
            writer.join();
        }
        stopping.store(true);
        socket.shutdown();
        if (reader.joinable()) {
            reader.join();
        }
    }

    void push(std::vector<std::byte> bytes) {
        {
            const std::lock_guard lock(outMutex);
            outbox.push_back(std::move(bytes));
        }
        outWake.notify_all();
    }

    // ---- the reader -------------------------------------------------------------

    void readLoop() {
        std::vector<std::byte> buffer(io::kReceiveChunk);
        auto lastHeard = Clock::now();
        std::string reason;
        while (!stopping.load() && reason.empty()) {
            sweeps::StreamRecord record;
            while (reason.empty()) {
                auto next = framer.next(record);
                if (!next) {
                    reason = next.error().message();
                    break;
                }
                if (!*next) {
                    break;
                }
                reason = handle(record);
            }
            if (!reason.empty()) {
                break;
            }

            auto readable = socket.waitReadable(io::kReadSlice);
            if (!readable) {
                reason = readable.error().message();
                break;
            }
            if (!*readable) {
                if (Clock::now() - lastHeard > kSilenceTimeout) {
                    reason = std::format("nothing heard for {} s", kSilenceTimeout.count());
                }
                continue;
            }
            auto got =
                socket.receive({reinterpret_cast<std::uint8_t*>(buffer.data()), buffer.size()});
            if (!got) {
                reason = got.error().message();
                break;
            }
            if (*got == 0) {
                reason = "the server closed the connection";
                break;
            }
            framer.feed(buffer.data(), *got);
            bytesReceived.fetch_add(*got);
            lastHeard = Clock::now();
        }

        {
            const std::lock_guard lock(inMutex);
            if (lostReason.empty()) {
                lostReason = std::move(reason);
            }
        }
        alive.store(false);
        outWake.notify_all();
    }

    /// Empty to carry on, or why the link ends here.
    std::string handle(const sweeps::StreamRecord& record) {
        using sweeps::RecordType;
        const std::uint64_t now = monotonicNs();

        if (isType(record, RecordType::SegmentOpen) || isType(record, RecordType::Tile) ||
            isType(record, RecordType::SegmentClose)) {
            if (auto applied = mirror.apply(record); !applied) {
                return applied.error().message();
            }
            return {};
        }

        if (isType(record, RecordType::Event)) {
            sweeps::ByteReader in(record.payload.data(), record.payload.size());
            if (auto event = sweeps::decodeEvent(in)) {
                session::publishSessionEvent(events, *event,
                                             clocks.toClient(event->monotonicNs, now));
            }
            return {};
        }

        if (isType(record, RecordType::Telemetry)) {
            if (auto report = decodeTelemetry(record)) {
                const std::lock_guard lock(inMutex);
                telemetry = std::move(*report);
            }
            return {};
        }

        if (isType(record, RecordType::EndOfStream)) {
            return "the server ended the stream";
        }

        if (!isType(record, RecordType::PluginData)) {
            return {};
        }
        auto message = decodeMessage(record);
        if (!message) {
            return message.error().message();
        }

        if (message->name == msg::kFrame) {
            const FrameCommit commit = FrameCommit::from(message->body);
            auto frame = mirror.commit(commit);
            if (!frame) {
                return frame.error().message();
            }
            const std::uint64_t hostNs = commit.replayed ? replayedTime(commit.hostTimeNs, now)
                                                         : clocks.toClient(commit.hostTimeNs, now);
            (*frame)->hostTimeNs = hostNs;
            (*frame)->replayed = commit.replayed;
            (*frame)->wallTimeNs = wallClockNs() - (now - hostNs);
            output.publish(std::move(*frame));
        } else if (message->name == msg::kPong) {
            const Pong pong = Pong::from(message->body);
            clocks.observe(pong.clientNs, pong.serverNs, now);
            roundTripNs.store(clocks.lastRoundTripNs());
        } else if (message->name == msg::kBye) {
            const Bye bye = Bye::from(message->body);
            return bye.reason == refusal::kShutdown
                       ? "the server shut down"
                       : std::format("the server said goodbye: {}", bye.reason);
        } else {
            const std::lock_guard lock(inMutex);
            inbox.push_back(std::move(*message));
        }
        return {};
    }

    /// A frame from before this link existed, on the offset the welcome gave
    /// -- no ping has answered yet -- no later than now, but always after the
    /// frame before it: order matters more than a nanosecond either way.
    std::uint64_t replayedTime(std::uint64_t serverNs, std::uint64_t now) {
        const auto guess =
            static_cast<std::uint64_t>(static_cast<std::int64_t>(serverNs) + welcomeOffsetNs);
        const std::uint64_t mapped = std::max(std::min(guess, now), lastReplayNs + 1);
        lastReplayNs = mapped;
        clocks.raiseFloor(mapped + 1);
        return mapped;
    }

    // ---- the writer -------------------------------------------------------------

    void writeLoop() {
        std::uint64_t pingId = 0;
        auto nextPing = Clock::now();
        std::vector<std::vector<std::byte>> batch;
        std::vector<std::byte> buffer;
        while (true) {
            bool closingNow = false;
            {
                std::unique_lock lock(outMutex);
                outWake.wait_until(lock, nextPing,
                                   [this] { return closing || !outbox.empty() || !alive.load(); });
                // A link already gone says no goodbye: to the server, that is
                // what tells a dropped client from one that left.
                if (!alive.load()) {
                    return;
                }
                batch.swap(outbox);
                closingNow = closing;
            }

            buffer.clear();
            for (const std::vector<std::byte>& message : batch) {
                buffer.insert(buffer.end(), message.begin(), message.end());
            }
            batch.clear();
            if (Clock::now() >= nextPing) {
                appendMessage(buffer, msg::kPing,
                              Ping{.id = ++pingId, .clientNs = monotonicNs()}.toMetadata());
                nextPing = Clock::now() + kPingInterval;
            }
            if (closingNow) {
                appendMessage(buffer, msg::kBye, Bye{.reason = "closed"}.toMetadata());
            }
            if (!buffer.empty() && !socket.sendAll(asBytes(buffer))) {
                return;
            }
            if (closingNow) {
                return;
            }
        }
    }

    net::SecureChannel socket;
    sweeps::RecordFramer framer;
    FrameBus& output;
    EventBus& events;

    std::atomic<bool> stopping{false};
    std::atomic<bool> alive{true};
    std::atomic<std::uint64_t> bytesReceived{0};
    std::atomic<std::uint64_t> roundTripNs{0};

    std::mutex outMutex;
    std::condition_variable outWake;
    std::vector<std::vector<std::byte>> outbox;
    bool closing = false;

    std::mutex inMutex;
    std::vector<Message> inbox;
    std::optional<TelemetryReport> telemetry;
    std::string lostReason;

    // The reader's alone.
    ClockMap clocks;
    FrameMirror mirror;
    std::int64_t welcomeOffsetNs = 0;
    std::uint64_t lastReplayNs = 0;

    std::thread reader;
    std::thread writer;
};

/// A download under way: what the operator sees, and the file it is going to.
struct RemoteInstrument::DownloadState {
    Download view;
    std::filesystem::path part;
    std::ofstream stream;
    std::uint64_t requested = 0;
    std::size_t inFlight = 0;
};

// ---------------------------------------------------------------- connecting

RemoteInstrument::RemoteInstrument(RemoteEndpoint endpoint, EventBus& events)
    : m_endpoint(std::move(endpoint)), m_events(events) {
}

RemoteInstrument::~RemoteInstrument() {
    disconnect();
}

Result<std::unique_ptr<RemoteInstrument>>
RemoteInstrument::connect(const RemoteEndpoint& endpoint, FrameBus& output, EventBus& events,
                          std::chrono::milliseconds timeout, const ClientIdentity& identity) {
    using Instance = std::unique_ptr<RemoteInstrument>;
    const std::string where = endpoint.address();
    const std::atomic<bool> never{false};
    const auto deadline = Clock::now() + timeout;

    auto socket = net::TcpSocket::connect(endpoint.host, endpoint.port, timeout);
    if (!socket) {
        return std::unexpected(std::move(socket).error());
    }
    (void)socket->setNoDelay(true);
    (void)socket->setKeepAlive(true);

    auto opened =
        openClientChannel(std::move(*socket), endpoint.token,
                          Hello{.protocolVersion = kProtocolVersion,
                                .software = std::format("{} {}", productName(), versionString()),
                                .kind = std::string(client::kDesktop),
                                .name = identity.name.empty() ? net::hostName() : identity.name,
                                .clientId = identity.clientId},
                          deadline);
    if (!opened) {
        return fail<Instance>(opened.error().code(), "{}: {}", where, opened.error().message());
    }
    net::SecureChannel& channel = *opened;

    sweeps::RecordFramer framer(kMaxServerRecordBytes);
    std::optional<TelemetryReport> telemetry;
    const auto nextMessage = [&](std::initializer_list<std::string_view> names) -> Result<Message> {
        while (true) {
            auto record = io::readRecord(channel, framer, deadline, never);
            if (!record) {
                return std::unexpected(std::move(record).error());
            }
            if (isType(*record, sweeps::RecordType::Telemetry)) {
                if (auto report = decodeTelemetry(*record)) {
                    telemetry = std::move(*report);
                }
                continue;
            }
            if (!isType(*record, sweeps::RecordType::PluginData)) {
                continue;
            }
            auto message = decodeMessage(*record);
            if (!message) {
                return std::unexpected(std::move(message).error());
            }
            if (std::ranges::find(names, message->name) != names.end()) {
                return message;
            }
        }
    };

    auto answer = nextMessage({msg::kWelcome, msg::kRefused});
    if (!answer) {
        return std::unexpected(std::move(answer).error());
    }
    if (answer->name == msg::kRefused) {
        return std::unexpected(refusalError(Refused::from(answer->body), where));
    }

    Instance instrument(new RemoteInstrument(endpoint, events));
    const Welcome welcome = Welcome::from(answer->body);
    const std::int64_t welcomeOffsetNs = welcome.serverNs == 0
                                             ? 0
                                             : static_cast<std::int64_t>(monotonicNs()) -
                                                   static_cast<std::int64_t>(welcome.serverNs);
    instrument->m_serverName = welcome.serverName.empty() ? endpoint.host : welcome.serverName;
    instrument->m_controlState.shared = welcome.shared;

    auto state = nextMessage({msg::kState});
    if (!state) {
        return std::unexpected(std::move(state).error());
    }
    const State first = State::from(state->body);
    instrument->applyState(first.ackSeq, first.sections);

    instrument->m_linkThreads =
        std::make_unique<Link>(std::move(channel), std::move(framer), output, events,
                               welcomeOffsetNs, identity.resumeAfterNs);
    if (telemetry) {
        const std::lock_guard lock(instrument->m_linkThreads->inMutex);
        instrument->m_linkThreads->telemetry = std::move(telemetry);
    }
    return instrument;
}

void RemoteInstrument::begin() {
    if (!m_linkThreads || m_closed) {
        return;
    }
    m_linkThreads->start();
    if (m_device) {
        m_events.publish(DeviceOpenedEvent{.monotonicNs = monotonicNs(),
                                           .deviceId = m_device->info.id,
                                           .label = displayLabel(),
                                           .serial = m_device->info.serial});
    }
}

void RemoteInstrument::disconnect() {
    if (m_closed) {
        return;
    }
    m_closed = true;
    if (m_linkThreads) {
        m_linkThreads->close();
    }
    if (m_device) {
        m_events.publish(
            DeviceClosedEvent{.monotonicNs = monotonicNs(),
                              .deviceId = m_device->info.id,
                              .reason = m_linkError.empty() ? "disconnected" : m_linkError});
    }
    for (const auto& download : m_downloads) {
        if (!download->view.done && download->view.error.empty()) {
            failDownload(*download, "the link went; download it again to carry on");
        }
    }
    m_device.reset();
    m_values.clear();
    m_running = false;
    m_learning = false;
    m_health.clear();
    m_haveTelemetry = false;
}

bool RemoteInstrument::linkUp() const noexcept {
    return !m_closed && m_linkThreads != nullptr;
}

void RemoteInstrument::abandon() {
    if (m_linkThreads && !m_closed) {
        m_linkThreads->stopping.store(true);
        m_linkThreads->socket.shutdown();
    }
}

void RemoteInstrument::linkLost(std::string reason) {
    m_linkError = std::move(reason);
    m_lost = LostState{.running = m_running};
    if (m_device) {
        for (const SdrParameter& parameter : m_device->parameters) {
            if (const auto value = m_values.find(parameter.key);
                !parameter.readOnly && value != m_values.end()) {
                m_lost.parameters.emplace_back(parameter.key, value->second);
            }
        }
    }
    logWarn("remote", "lost {}: {}", m_endpoint.address(), m_linkError);
    m_notices.push_back(InstrumentNotice{
        .kind = InstrumentNotice::Kind::Condition,
        .text = std::format("Lost the link to {}: {}", m_serverName, m_linkError)});
    disconnect();
}

// ------------------------------------------------------------------ the state

Status RemoteInstrument::mayChange() const {
    if (canControl()) {
        return ok();
    }
    return fail(ErrorCode::PermissionDenied, "{}; take control to change this",
                m_controlState.controller.empty()
                    ? std::string("Nobody has control")
                    : std::format("{} has control", m_controlState.controller));
}

std::uint64_t RemoteInstrument::send(std::string_view op, Metadata args) {
    if (!linkUp()) {
        return 0;
    }
    const std::uint64_t seq = ++m_seq;
    for (const std::string_view name : sectionsTouchedBy(op)) {
        m_pending.insert_or_assign(std::string(name), seq);
    }
    m_linkThreads->push(messageBytes(
        msg::kCommand,
        Command{.seq = seq, .op = std::string(op), .args = std::move(args)}.toMetadata()));
    return seq;
}

void RemoteInstrument::applyState(std::uint64_t ackSeq, const Metadata& sections) {
    for (const auto& [name, value] : sections) {
        const Metadata* body = value.asHash();
        if (body == nullptr) {
            continue;
        }
        if (const auto pending = m_pending.find(name); pending != m_pending.end()) {
            if (ackSeq < pending->second) {
                continue;
            }
            m_pending.erase(pending);
        }
        applySection(name, *body);
    }
}

void RemoteInstrument::applySection(std::string_view name, const Metadata& body) {
    if (name == section::kDevice) {
        if (body.getBool("present")) {
            m_device = decodeDevice(hashAt(body, "descriptor"));
        } else {
            m_device.reset();
        }
        m_antennaKey = body.getString("antennaKey");
        m_deviceLabel = body.getString("displayLabel");
    } else if (name == section::kValues) {
        m_values.clear();
        for (const auto& [key, value] : hashAt(body, "parameters")) {
            m_values.insert_or_assign(key, decodeValue(value));
        }
        m_selectedRxPort = body.getString("selectedRxPort");
    } else if (name == section::kRun) {
        m_running = body.getBool("running");
        m_sweeping = body.getBool("sweeping");
        m_startGeneration = static_cast<std::uint64_t>(body.getInt("startGeneration"));
        m_engine = decodeEngineStats(hashAt(body, "engine"));
    } else if (name == section::kPlan) {
        m_plan = decodePlan(body);
    } else if (name == section::kSchedule) {
        m_schedule = decodeSchedule(body);
    } else if (name == section::kPipeline) {
        m_pipeline = decodePipeline(body);
    } else if (name == section::kBackends) {
        m_backends = decodeBackends(body);
        m_backendName = body.getString("current");
    } else if (name == section::kCorrections) {
        m_correctionSettings = decodeCorrectionSettings(hashAt(body, "settings"));
        m_correctionSummary = decodeCorrectionSummary(hashAt(body, "summary"));
    } else if (name == section::kLearning) {
        m_learning = body.getBool("active");
        m_learningLabel = body.getString("label");
    } else if (name == section::kAntennas) {
        m_antennas = AntennaLibrary::of(decodeAntennas(body));
    } else if (name == section::kAssignments) {
        m_assignments = decodeAssignments(body);
    } else if (name == section::kSwitchers) {
        m_switchers = decodeSwitcherViews(hashAt(body, "open"));
        m_availableSwitchers = decodeSwitcherInfos(hashAt(body, "available"));
    } else if (name == section::kRecordings) {
        m_recordings = ServerRecordings::from(body);
    } else if (name == section::kBenchmark) {
        m_benchmark = decodeBenchmarkStatus(body);
    } else if (name == section::kLink) {
        m_linkMaxBins = static_cast<std::uint32_t>(body.getInt("maxBins"));
    } else if (name == section::kRfPath) {
        m_rfLegs = decodeRfLegs(hashAt(body, "legs"));
        m_coverage = decodeRanges(hashAt(body, "coverage"));
    } else if (name == section::kControl) {
        m_controlState = ControlState::from(body);
    } else if (name == section::kClients) {
        m_clients = decodeClients(body);
    }
}

void RemoteInstrument::tick(std::uint64_t nowNs) {
    if (!m_linkThreads || m_closed) {
        return;
    }

    std::vector<Message> inbox;
    std::optional<TelemetryReport> telemetry;
    {
        const std::lock_guard lock(m_linkThreads->inMutex);
        inbox.swap(m_linkThreads->inbox);
        telemetry = std::exchange(m_linkThreads->telemetry, std::nullopt);
    }

    for (const Message& message : inbox) {
        if (message.name == msg::kState) {
            const State state = State::from(message.body);
            applyState(state.ackSeq, state.sections);
        } else if (message.name == msg::kReply) {
            const Reply reply = Reply::from(message.body);
            const auto fetch = m_fetches.find(reply.seq);
            if (fetch != m_fetches.end()) {
                const std::string name = fetch->second;
                m_fetches.erase(fetch);
                if (!reply.ok) {
                    for (const auto& download : m_downloads) {
                        if (download->view.name == name && !download->view.done &&
                            download->view.error.empty()) {
                            failDownload(*download, reply.message);
                        }
                    }
                }
            } else if (!reply.ok) {
                m_notices.push_back(
                    InstrumentNotice{.kind = InstrumentNotice::Kind::Error, .text = reply.message});
            }
        } else if (message.name == msg::kChunk) {
            receiveChunk(Chunk::from(message.body));
        } else if (message.name == msg::kNotice) {
            m_notices.push_back(decodeNotice(message.body));
        }
    }

    if (telemetry) {
        m_engineTelemetry.stream = telemetry->stream;
        m_engineTelemetry.process = telemetry->process;
        m_health = std::move(telemetry->health);
        m_serverHost = std::move(telemetry->host);
        m_link.framesSent = telemetry->link.framesSent;
        m_link.passesCoalesced = telemetry->link.passesCoalesced;
        m_link.partialsCoalesced = telemetry->link.partialsCoalesced;
        m_link.eventsDropped = telemetry->link.eventsDropped;
        m_link.encodeNs = telemetry->link.encodeNs;
        m_haveTelemetry = true;
    }

    const std::uint64_t received = m_linkThreads->bytesReceived.load();
    m_link.bytesReceived = received;
    m_link.roundTripMs = static_cast<double>(m_linkThreads->roundTripNs.load()) / 1e6;
    if (m_rateWindowNs == 0) {
        m_rateWindowNs = nowNs;
        m_rateWindowBytes = received;
    } else if (nowNs - m_rateWindowNs >= kRateWindowNs) {
        m_link.bytesPerSec =
            static_cast<double>(received - m_rateWindowBytes) / nsToSeconds(nowNs - m_rateWindowNs);
        m_rateWindowNs = nowNs;
        m_rateWindowBytes = received;
    }

    if (!m_linkThreads->alive.load()) {
        std::string reason;
        {
            const std::lock_guard lock(m_linkThreads->inMutex);
            reason = m_linkThreads->lostReason;
        }
        linkLost(reason.empty() ? std::string("the connection ended") : std::move(reason));
    }
}

std::vector<InstrumentNotice> RemoteInstrument::takeNotices() {
    return std::exchange(m_notices, {});
}

const TelemetrySnapshot* RemoteInstrument::engineTelemetry() const noexcept {
    return m_haveTelemetry ? &m_engineTelemetry : nullptr;
}

void RemoteInstrument::resetTelemetry() {
    if (!canControl()) {
        return;
    }
    send(op::kResetTelemetry, {});
}

// ------------------------------------------------------------------ the radio

std::string RemoteInstrument::displayLabel() const {
    if (!m_device) {
        return m_serverName;
    }
    const std::string& label = m_deviceLabel.empty() ? m_device->info.label : m_deviceLabel;
    return std::format("{} on {}", label, m_serverName);
}

const DeviceDescriptor* RemoteInstrument::device() const noexcept {
    return m_device ? &*m_device : nullptr;
}

std::optional<SdrValue> RemoteInstrument::parameter(std::string_view key) const {
    const auto found = m_values.find(key);
    return found != m_values.end() ? std::optional<SdrValue>(found->second) : std::nullopt;
}

Status RemoteInstrument::setDeviceParameter(const std::string& key, const SdrValue& value) {
    if (auto allowed = mayChange(); !allowed) {
        return allowed;
    }
    if (!m_device) {
        return fail(ErrorCode::NotFound, "no device is open");
    }
    m_values.insert_or_assign(key, value);
    if (key == "sample_rate" && m_sweeping) {
        m_plan.sampleRate = asDouble(value);
    }
    Metadata args;
    args.setString("key", key);
    args.set("value", encodeValue(value));
    send(op::kSetParameter, std::move(args));
    return ok();
}

bool RemoteInstrument::parameterNeedsStop(const SdrParameter& parameter) const noexcept {
    if (!parameter.requiresStop) {
        return false;
    }
    return !(m_sweeping && parameter.key == "sample_rate");
}

// ------------------------------------------------------------------ running

Status RemoteInstrument::start() {
    if (auto allowed = mayChange(); !allowed) {
        return allowed;
    }
    if (!m_device) {
        return fail(ErrorCode::NotFound, "no device is open");
    }
    m_running = true;
    send(op::kStart, {});
    return ok();
}

void RemoteInstrument::stop() {
    if (!canControl()) {
        return;
    }
    m_running = false;
    send(op::kStop, {});
}

Status RemoteInstrument::restart() {
    if (auto allowed = mayChange(); !allowed) {
        return allowed;
    }
    send(op::kRestart, {});
    return ok();
}

Status RemoteInstrument::setSweeping(bool enabled) {
    if (auto allowed = mayChange(); !allowed) {
        return allowed;
    }
    m_sweeping = enabled;
    Metadata args;
    args.setBool("enabled", enabled);
    send(op::kSetSweeping, std::move(args));
    return ok();
}

Status RemoteInstrument::applySweepPlan(const SweepPlan& plan) {
    if (auto allowed = mayChange(); !allowed) {
        return allowed;
    }
    m_plan = plan;
    Metadata args;
    args.setHash("plan", encodePlan(plan));
    send(op::kApplySweepPlan, std::move(args));
    return plan.validate();
}

Status RemoteInstrument::sweepRange(const SweepPlan& plan) {
    if (auto allowed = mayChange(); !allowed) {
        return allowed;
    }
    m_plan = plan;
    m_sweeping = true;
    Metadata args;
    args.setHash("plan", encodePlan(plan));
    send(op::kSweepRange, std::move(args));
    return plan.validate();
}

Status RemoteInstrument::applyPipelineConfig(const PipelineConfig& config) {
    if (auto allowed = mayChange(); !allowed) {
        return allowed;
    }
    m_pipeline = config;
    Metadata args;
    args.setHash("config", encodePipeline(config));
    send(op::kApplyPipelineConfig, std::move(args));
    return ok();
}

Status RemoteInstrument::setFftBackend(std::string_view name) {
    if (auto allowed = mayChange(); !allowed) {
        return allowed;
    }
    const bool known = std::ranges::any_of(m_backends, [name](const FftBackendInfo& backend) {
        return backend.name == name && backend.available;
    });
    if (!known) {
        return fail(ErrorCode::NotFound, "{} has no FFT backend '{}'", m_serverName, name);
    }
    m_backendName = std::string(name);
    Metadata args;
    args.setString("name", std::string(name));
    send(op::kSetFftBackend, std::move(args));
    return ok();
}

// -------------------------------------------------------------- corrections

void RemoteInstrument::setCorrectionSettings(const CorrectionSettings& settings) {
    if (!canControl()) {
        return;
    }
    m_correctionSettings = settings;
    Metadata args;
    args.setHash("settings", encodeCorrectionSettings(settings));
    send(op::kSetCorrectionSettings, std::move(args));
}

Status RemoteInstrument::startLearning() {
    if (auto allowed = mayChange(); !allowed) {
        return allowed;
    }
    if (!m_running) {
        return fail(ErrorCode::Unavailable, "start acquisition before learning corrections");
    }
    m_learning = true;
    send(op::kStartLearning, {});
    return ok();
}

void RemoteInstrument::cancelLearning() {
    if (!canControl()) {
        return;
    }
    m_learning = false;
    send(op::kCancelLearning, {});
}

void RemoteInstrument::clearAutoSpurs() {
    if (!canControl()) {
        return;
    }
    send(op::kClearAutoSpurs, {});
}

void RemoteInstrument::clearCorrections() {
    if (!canControl()) {
        return;
    }
    send(op::kClearCorrections, {});
}

// ----------------------------------------------------------------- antennas

Status RemoteInstrument::setUserAntennas(std::vector<Antenna> entries) {
    if (auto allowed = mayChange(); !allowed) {
        return allowed;
    }
    std::vector<Antenna> library;
    for (const Antenna& existing : m_antennas.entries()) {
        const bool shadowed = std::ranges::any_of(
            entries, [&existing](const Antenna& entry) { return entry.id == existing.id; });
        if (existing.builtin && !shadowed) {
            library.push_back(existing);
        }
    }
    for (Antenna& entry : entries) {
        entry.builtin = false;
        library.push_back(entry);
    }
    m_antennas = AntennaLibrary::of(std::move(library));

    Metadata args;
    args.setHash("antennas", encodeAntennas(entries));
    send(op::kSetUserAntennas, std::move(args));
    return ok();
}

Status RemoteInstrument::setAntennaAssignments(AntennaAssignments assignments) {
    if (auto allowed = mayChange(); !allowed) {
        return allowed;
    }
    Metadata args;
    args.setHash("assignments", encodeAssignments(assignments));
    m_assignments = std::move(assignments);
    send(op::kSetAssignments, std::move(args));
    return ok();
}

const Antenna* RemoteInstrument::antennaOnPort(std::string_view portId) const {
    if (!m_device) {
        return nullptr;
    }
    const std::string_view id = m_assignments.antennaFor(m_antennaKey, portId);
    return id.empty() ? nullptr : m_antennas.find(id);
}

const SwitcherView* RemoteInstrument::switcher(std::string_view key) const {
    const auto match = std::ranges::find(m_switchers, key, &SwitcherView::key);
    return match != m_switchers.end() ? &*match : nullptr;
}

// ------------------------------------------------------------- recordings

void RemoteInstrument::startRecording(std::uint32_t maxBins) {
    if (!canControl()) {
        return;
    }
    Metadata args;
    args.setInt("maxBins", maxBins);
    send(op::kStartRecording, std::move(args));
}

void RemoteInstrument::stopRecording() {
    if (!canControl()) {
        return;
    }
    send(op::kStopRecording, {});
}

void RemoteInstrument::deleteRecording(const std::string& name) {
    if (!canControl()) {
        return;
    }
    Metadata args;
    args.setString("name", name);
    send(op::kDeleteRecording, std::move(args));
}

Status RemoteInstrument::beginDownload(const std::string& name,
                                       const std::filesystem::path& directory) {
    const auto file = std::ranges::find(m_recordings.files, name, &RecordingFile::name);
    if (file == m_recordings.files.end()) {
        return fail(ErrorCode::NotFound, "{} has no recording called '{}'", m_serverName, name);
    }
    if (m_recordings.active && m_recordings.current == name) {
        return fail(ErrorCode::Unavailable, "'{}' is still being recorded; stop it first", name);
    }
    std::erase_if(m_downloads, [&name](const std::unique_ptr<DownloadState>& download) {
        return download->view.name == name;
    });

    auto download = std::make_unique<DownloadState>();
    download->view.name = name;
    download->view.path = directory / name;
    download->view.totalBytes = file->bytes;
    download->part = directory / (name + ".part");

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    // What a dropped link left behind is the start of the file: carry on
    // from it, unless it is somehow longer than the file now is.
    std::uint64_t resumeAt = std::filesystem::exists(download->part, ec)
                                 ? std::filesystem::file_size(download->part, ec)
                                 : 0;
    if (ec || resumeAt > file->bytes) {
        resumeAt = 0;
    }
    download->stream.open(download->part,
                          std::ios::binary | (resumeAt > 0 ? std::ios::app : std::ios::trunc));
    if (!download->stream) {
        return fail(ErrorCode::IoError, "cannot write {}", download->part.string());
    }
    download->view.received = resumeAt;
    download->requested = resumeAt;

    DownloadState& added = *m_downloads.emplace_back(std::move(download));
    if (added.view.received >= added.view.totalBytes) {
        receiveChunk(Chunk{
            .name = name, .offset = added.view.received, .totalBytes = added.view.totalBytes});
    } else {
        requestMoreOf(added);
    }
    return ok();
}

void RemoteInstrument::requestMoreOf(DownloadState& download) {
    while (download.inFlight < kMaxChunksInFlight &&
           download.requested < download.view.totalBytes) {
        Metadata args;
        args.setString("name", download.view.name);
        args.setInt("offset", static_cast<std::int64_t>(download.requested));
        const std::uint64_t seq = send(op::kFetchRecording, std::move(args));
        if (seq == 0) {
            failDownload(download, "the link went");
            return;
        }
        m_fetches.emplace(seq, download.view.name);
        download.requested += kChunkBytes;
        ++download.inFlight;
    }
}

void RemoteInstrument::receiveChunk(const Chunk& chunk) {
    const auto found = std::ranges::find_if(m_downloads, [&chunk](const auto& download) {
        return download->view.name == chunk.name && !download->view.done &&
               download->view.error.empty();
    });
    if (found == m_downloads.end()) {
        return;
    }
    DownloadState& download = **found;
    if (chunk.offset != download.view.received) {
        failDownload(download, "pieces arrived out of order");
        return;
    }

    download.stream.write(reinterpret_cast<const char*>(chunk.data.data()),
                          static_cast<std::streamsize>(chunk.data.size()));
    if (!download.stream) {
        failDownload(download, std::format("cannot write {}", download.part.string()));
        return;
    }
    download.view.received += chunk.data.size();
    download.view.totalBytes = chunk.totalBytes;
    download.inFlight = download.inFlight > 0 ? download.inFlight - 1 : 0;

    if (download.view.received < download.view.totalBytes) {
        if (chunk.data.empty()) {
            failDownload(download, "the server sent nothing");
            return;
        }
        requestMoreOf(download);
        return;
    }

    download.stream.close();
    // Never over a file already there: a second download of the same name
    // lands beside the first.
    std::filesystem::path target = download.view.path;
    std::error_code ec;
    for (int copy = 1; std::filesystem::exists(target, ec); ++copy) {
        target = download.view.path.parent_path() /
                 std::format("{}-{}{}", download.view.path.stem().string(), copy,
                             download.view.path.extension().string());
    }
    std::filesystem::rename(download.part, target, ec);
    if (ec) {
        failDownload(download, std::format("cannot keep {}: {}", target.string(), ec.message()));
        return;
    }
    download.view.path = target;
    download.view.done = true;
    m_notices.push_back(InstrumentNotice{
        .kind = InstrumentNotice::Kind::Success,
        .text = std::format("{} downloaded to {}", download.view.name, target.string())});
}

void RemoteInstrument::failDownload(DownloadState& download, std::string why) {
    download.stream.close();
    download.view.error = std::move(why);
    m_notices.push_back(
        InstrumentNotice{.kind = InstrumentNotice::Kind::Error,
                         .text = std::format("Download of {} stopped: {}", download.view.name,
                                             download.view.error)});
}

void RemoteInstrument::cancelDownload(const std::string& name) {
    std::erase_if(m_downloads, [&name](const std::unique_ptr<DownloadState>& download) {
        if (download->view.name != name) {
            return false;
        }
        download->stream.close();
        std::error_code ec;
        if (!download->view.done) {
            std::filesystem::remove(download->part, ec);
        }
        return true;
    });
}

std::vector<RemoteInstrument::Download> RemoteInstrument::downloads() const {
    std::vector<Download> views;
    views.reserve(m_downloads.size());
    for (const auto& download : m_downloads) {
        views.push_back(download->view);
    }
    return views;
}

Status RemoteInstrument::startBenchmark(const FftBenchmarkConfig& config) {
    if (auto allowed = mayChange(); !allowed) {
        return allowed;
    }
    if (m_benchmark.running) {
        return fail(ErrorCode::Unavailable, "a benchmark is already running on {}", m_serverName);
    }
    m_benchmark = BenchmarkStatus{.running = true};
    Metadata args;
    args.setHash("config", encodeBenchmarkConfig(config));
    send(op::kStartBenchmark, std::move(args));
    return ok();
}

void RemoteInstrument::cancelBenchmark() {
    if (!canControl()) {
        return;
    }
    send(op::kCancelBenchmark, {});
}

void RemoteInstrument::setLinkResolution(std::uint32_t maxBins) {
    m_linkMaxBins = maxBins;
    Metadata args;
    args.setInt("maxBins", maxBins);
    send(op::kSetLinkResolution, std::move(args));
}

void RemoteInstrument::rescanSwitchers() {
    if (!canControl()) {
        return;
    }
    send(op::kRescanSwitchers, {});
}

Status RemoteInstrument::takeControl() {
    if (!linkUp()) {
        return fail(ErrorCode::Unavailable, "not connected to {}", m_serverName);
    }
    m_controlState.you = true;
    m_controlState.held = true;
    send(op::kTakeControl, {});
    return ok();
}

void RemoteInstrument::releaseControl() {
    m_controlState.you = false;
    send(op::kReleaseControl, {});
}

} // namespace sweeppp::remote
