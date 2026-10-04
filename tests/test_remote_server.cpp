// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ReferenceFft.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <sweeppp/backends/sdr/SyntheticDevice.hpp>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/crypto/Sha256.hpp>
#include <sweeppp/fft/FftBackendManager.hpp>
#include <sweeppp/instrument/LocalInstrument.hpp>
#include <sweeppp/net/SecureChannel.hpp>
#include <sweeppp/net/Socket.hpp>
#include <sweeppp/remote/Handshake.hpp>
#include <sweeppp/remote/Messages.hpp>
#include <sweeppp/remote/Protocol.hpp>
#include <sweeppp/remote/RemoteServer.hpp>
#include <sweeppp/remote/WireCodec.hpp>
#include <sweeppp/sdr/ISdrDevice.hpp>
#include <sweeps/FileFormat.hpp>
#include <sweeps/Records.hpp>
#include <sweeps/SessionReader.hpp>
#include <sweeps/Stream.hpp>
#include <thread>
#include <vector>

using namespace sweeppp;
using namespace sweeppp::remote;
using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

std::span<const std::uint8_t> asBytes(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

/// The protocol by hand above the channel, with nothing of the real client in
/// it: what a server must cope with is whatever arrives, not what our client
/// sends. Before `handshake()` it writes to the bare socket, which is how the
/// tests poke the handshake itself.
class RawClient {
public:
    explicit RawClient(std::uint16_t port) {
        auto socket = net::TcpSocket::connect("127.0.0.1", port, 2000ms);
        REQUIRE(socket.has_value());
        m_raw = std::move(*socket);
    }

    /// Bytes on the bare socket, before any handshake.
    void sendRaw(const std::vector<std::uint8_t>& bytes) { (void)m_raw.sendAll(bytes); }

    void send(std::string_view name, const sweeps::Metadata& body) {
        std::vector<std::byte> bytes;
        appendMessage(bytes, name, body, monotonicNs());
        (void)m_channel.sendAll(asBytes(bytes));
    }

    std::uint64_t command(std::string_view op, sweeps::Metadata args = {}) {
        const std::uint64_t seq = ++m_seq;
        send(msg::kCommand,
             Command{.seq = seq, .op = std::string(op), .args = std::move(args)}.toMetadata());
        return seq;
    }

    /// The channel, then the server's welcome or refusal. Nothing, with the
    /// error kept, when the handshake itself fails.
    std::optional<Message> handshake(std::string_view token = {},
                                     std::uint32_t version = kProtocolVersion,
                                     std::string name = "test", std::string clientId = {}) {
        auto opened = openClientChannel(std::move(m_raw), token,
                                        Hello{.protocolVersion = version,
                                              .software = "test",
                                              .kind = std::string(client::kDesktop),
                                              .name = std::move(name),
                                              .clientId = std::move(clientId)},
                                        Clock::now() + 3s);
        if (!opened) {
            m_handshakeError = opened.error();
            return std::nullopt;
        }
        m_channel = std::move(*opened);
        m_open = true;
        return expectAny({msg::kWelcome, msg::kRefused});
    }

    [[nodiscard]] const Error& handshakeError() const noexcept { return m_handshakeError; }

    /// The next record, or nullopt when the connection ends or `timeout`
    /// passes.
    std::optional<sweeps::StreamRecord> next(std::chrono::milliseconds timeout = 5000ms) {
        const auto deadline = Clock::now() + timeout;
        std::vector<std::byte> buffer(std::size_t{64} * 1024);
        while (true) {
            if (m_open) {
                sweeps::StreamRecord record;
                auto got = m_framer.next(record);
                REQUIRE(got.has_value());
                if (*got) {
                    return record;
                }
            }
            if (m_closed || Clock::now() >= deadline) {
                return std::nullopt;
            }
            const std::span<std::uint8_t> into{reinterpret_cast<std::uint8_t*>(buffer.data()),
                                               buffer.size()};
            auto readable = m_open ? m_channel.waitReadable(50ms) : m_raw.waitReadable(50ms);
            if (!readable || !*readable) {
                continue;
            }
            auto received = m_open ? m_channel.receive(into) : m_raw.receive(into);
            if (!received || *received == 0) {
                m_closed = true;
                continue;
            }
            if (m_open) {
                m_framer.feed(buffer.data(), *received);
            }
        }
    }

    /// The next control message called `name`, skipping everything else.
    std::optional<Message> expect(std::string_view name,
                                  std::chrono::milliseconds timeout = 5000ms) {
        return expectAny({name}, timeout);
    }

    std::optional<Message> expectAny(std::initializer_list<std::string_view> names,
                                     std::chrono::milliseconds timeout = 5000ms) {
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            auto record = next(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()));
            if (!record) {
                return std::nullopt;
            }
            observe(*record);
            if (record->header.type != static_cast<std::uint16_t>(sweeps::RecordType::PluginData)) {
                continue;
            }
            auto message = decodeMessage(*record);
            REQUIRE(message.has_value());
            remember(*message);
            if (std::ranges::find(names, message->name) != names.end()) {
                return std::move(*message);
            }
        }
        return std::nullopt;
    }

    /// The next record of `type`, skipping everything else.
    std::optional<sweeps::StreamRecord> expectRecord(sweeps::RecordType type,
                                                     std::chrono::milliseconds timeout = 5000ms) {
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            auto record = next(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()));
            if (!record) {
                return std::nullopt;
            }
            observe(*record);
            if (record->header.type == static_cast<std::uint16_t>(type)) {
                return record;
            }
            if (record->header.type == static_cast<std::uint16_t>(sweeps::RecordType::PluginData)) {
                if (auto message = decodeMessage(*record)) {
                    remember(*message);
                }
            }
        }
        return std::nullopt;
    }

    /// Reads until the server closes the connection, or `timeout`.
    bool closedWithin(std::chrono::milliseconds timeout) {
        const auto deadline = Clock::now() + timeout;
        while (!m_closed && Clock::now() < deadline) {
            auto record = next(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()));
            if (record) {
                observe(*record);
            }
        }
        return m_closed;
    }

    void close() {
        m_channel.close();
        m_raw.close();
    }

    [[nodiscard]] const sweeps::Metadata& sections() const noexcept { return m_sections; }
    [[nodiscard]] std::uint64_t ackSeq() const noexcept { return m_ackSeq; }
    [[nodiscard]] const std::vector<Reply>& replies() const noexcept { return m_replies; }
    [[nodiscard]] const std::vector<sweeps::SessionEvent>& events() const noexcept {
        return m_events;
    }
    [[nodiscard]] const std::vector<InstrumentNotice>& notices() const noexcept {
        return m_notices;
    }
    [[nodiscard]] ControlState control() const {
        return ControlState::from(hashAt(m_sections, section::kControl));
    }
    [[nodiscard]] std::vector<ConnectedClient> clients() const {
        return decodeClients(hashAt(m_sections, section::kClients));
    }
    [[nodiscard]] std::size_t tiles() const noexcept { return m_tiles; }
    [[nodiscard]] std::size_t telemetry() const noexcept { return m_telemetry; }

    /// Drains whatever arrives for `duration`, keeping score.
    void pump(std::chrono::milliseconds duration) {
        const auto deadline = Clock::now() + duration;
        while (Clock::now() < deadline) {
            auto record = next(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()));
            if (!record) {
                return;
            }
            observe(*record);
            if (record->header.type == static_cast<std::uint16_t>(sweeps::RecordType::PluginData)) {
                if (auto message = decodeMessage(*record)) {
                    remember(*message);
                }
            }
        }
    }

private:
    void observe(const sweeps::StreamRecord& record) {
        switch (static_cast<sweeps::RecordType>(record.header.type)) {
        case sweeps::RecordType::Tile:
            ++m_tiles;
            break;
        case sweeps::RecordType::Telemetry:
            ++m_telemetry;
            break;
        case sweeps::RecordType::Event: {
            sweeps::ByteReader reader(record.payload.data(), record.payload.size());
            if (auto event = sweeps::decodeEvent(reader)) {
                m_events.push_back(std::move(*event));
            }
            break;
        }
        default:
            break;
        }
    }

    void remember(const Message& message) {
        if (message.name == msg::kState) {
            const State state = State::from(message.body);
            m_ackSeq = state.ackSeq;
            for (const auto& [name, value] : state.sections) {
                m_sections.set(name, value);
            }
        } else if (message.name == msg::kReply) {
            m_replies.push_back(Reply::from(message.body));
        } else if (message.name == msg::kNotice) {
            m_notices.push_back(decodeNotice(message.body));
        }
    }

    net::TcpSocket m_raw;
    net::SecureChannel m_channel;
    bool m_open = false;
    Error m_handshakeError;
    sweeps::RecordFramer m_framer{kMaxServerRecordBytes};
    bool m_closed = false;
    std::uint64_t m_seq = 0;

    sweeps::Metadata m_sections;
    std::uint64_t m_ackSeq = 0;
    std::vector<Reply> m_replies;
    std::vector<InstrumentNotice> m_notices;
    std::vector<sweeps::SessionEvent> m_events;
    std::size_t m_tiles = 0;
    std::size_t m_telemetry = 0;
};

/// A synthetic radio served on an ephemeral loopback port, with its bench in a
/// directory of its own.
class ServerFixture {
public:
    explicit ServerFixture(ServerConfig config = {})
        : m_root(std::filesystem::temp_directory_path() /
                 std::format("sweeppp-server-{}", monotonicNs())) {
        registerReferenceFftBackend();
        registerBuiltinSdrDevices();
        auto backend = FftBackendManager::instance().acquire("reference");
        REQUIRE(backend.has_value());
        instrument = std::make_unique<LocalInstrument>(output, events, telemetry,
                                                       InstrumentPaths::under(m_root), **backend);

        auto device = SdrDeviceManager::instance().open("synthetic", "");
        REQUIRE(device.has_value());
        instrument->adoptDevice(std::move(*device));
        REQUIRE(instrument->applySweepPlan(quickPlan()).has_value());

        config.port = 0;
        config.serverName = "bench";
        config.handshakeTimeout = 1500ms;
        config.refusalDelay = 50ms;
        server = std::make_unique<RemoteServer>(*instrument, output, events, telemetry,
                                                std::move(config));
        REQUIRE(server->start().has_value());
    }

    ~ServerFixture() {
        server.reset();
        instrument.reset();
        std::error_code ec;
        std::filesystem::remove_all(m_root, ec);
    }

    ServerFixture(const ServerFixture&) = delete;
    ServerFixture& operator=(const ServerFixture&) = delete;
    ServerFixture(ServerFixture&&) = delete;
    ServerFixture& operator=(ServerFixture&&) = delete;

    static SweepPlan quickPlan() {
        SweepPlan plan;
        plan.segments = {SweepSegment{.startHz = 100e6, .stopHz = 140e6}};
        plan.sampleRate = 8e6;
        plan.rbwHz = 100e3;
        plan.applyMode(SweepMode::Fast);
        return plan;
    }

    [[nodiscard]] std::uint16_t port() const { return server->port(); }
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return m_root; }

    /// Polls until `done`, for a server-side condition.
    static bool eventually(const std::function<bool()>& done,
                           std::chrono::milliseconds timeout = 5000ms) {
        const auto deadline = Clock::now() + timeout;
        while (!done()) {
            if (Clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(10ms);
        }
        return true;
    }

    FrameBus output;
    EventBus events;
    Telemetry telemetry;
    std::unique_ptr<LocalInstrument> instrument;
    std::unique_ptr<RemoteServer> server;

private:
    std::filesystem::path m_root;
};

} // namespace

TEST_CASE("a client is welcomed with the whole state and can start the radio") {
    ServerFixture fixture;
    RawClient client(fixture.port());

    auto welcome = client.handshake();
    REQUIRE(welcome.has_value());
    REQUIRE(welcome->name == msg::kWelcome);
    CHECK(Welcome::from(welcome->body).serverName == "bench");

    REQUIRE(client.expect(msg::kState).has_value());
    const sweeps::Metadata& device = hashAt(client.sections(), section::kDevice);
    CHECK(device.getBool("present"));
    const DeviceDescriptor descriptor = decodeDevice(hashAt(device, "descriptor"));
    CHECK(descriptor.info.driver == "synthetic");
    CHECK_FALSE(descriptor.parameters.empty());
    CHECK(decodePlan(hashAt(client.sections(), section::kPlan)).lowestHz() ==
          doctest::Approx(100e6));
    CHECK_FALSE(hashAt(client.sections(), section::kRun).getBool("running"));
    CHECK(client.sections().contains(section::kAntennas));
    CHECK(client.sections().contains(section::kBackends));
    CHECK(ServerFixture::eventually([&] { return fixture.server->clientConnected(); }));

    const std::uint64_t seq = client.command(op::kStart);
    REQUIRE(client.expectRecord(sweeps::RecordType::SegmentOpen).has_value());
    auto frame = client.expect(msg::kFrame);
    REQUIRE(frame.has_value());
    CHECK(FrameCommit::from(frame->body).segmentId == 0);
    client.pump(300ms);
    CHECK(client.tiles() > 0);
    CHECK(client.telemetry() > 0);
    CHECK(client.ackSeq() >= seq);
    REQUIRE_FALSE(client.replies().empty());
    CHECK(client.replies().front().seq == seq);
    CHECK(client.replies().front().ok);
    CHECK(hashAt(client.sections(), section::kRun).getBool("running"));
    CHECK(std::ranges::any_of(client.events(), [](const sweeps::SessionEvent& event) {
        return event.kindEnum() == sweeps::SessionEvent::Kind::SweepPass;
    }));
}

TEST_CASE("a ping is answered with the server's clocks") {
    ServerFixture fixture;
    RawClient client(fixture.port());
    REQUIRE(client.handshake().has_value());
    client.send(msg::kPing, Ping{.id = 9, .clientNs = 1234}.toMetadata());
    auto pong = client.expect(msg::kPong);
    REQUIRE(pong.has_value());
    const Pong answer = Pong::from(pong->body);
    CHECK(answer.id == 9);
    CHECK(answer.clientNs == 1234);
    CHECK(answer.serverNs > 0);
    CHECK(answer.serverWallNs > 0);
}

TEST_CASE("a token is checked, and a wrong one refused") {
    ServerFixture fixture(ServerConfig{.token = "correct horse"});

    SUBCASE("the right token") {
        RawClient client(fixture.port());
        auto answer = client.handshake("correct horse");
        REQUIRE(answer.has_value());
        CHECK(answer->name == msg::kWelcome);
    }

    SUBCASE("a wrong one ends the handshake, saying nothing") {
        RawClient client(fixture.port());
        CHECK_FALSE(client.handshake("battery staple").has_value());
        CHECK(client.handshakeError().code() == ErrorCode::PermissionDenied);
        CHECK_FALSE(fixture.server->clientConnected());
    }

    SUBCASE("none at all") {
        RawClient client(fixture.port());
        CHECK_FALSE(client.handshake().has_value());
        CHECK(client.handshakeError().code() == ErrorCode::PermissionDenied);
    }
}

TEST_CASE("a server without a token refuses to listen beyond loopback") {
    registerReferenceFftBackend();
    auto backend = FftBackendManager::instance().acquire("reference");
    REQUIRE(backend.has_value());
    FrameBus output;
    EventBus events;
    Telemetry telemetry;
    LocalInstrument instrument(
        output, events, telemetry,
        InstrumentPaths::under(std::filesystem::temp_directory_path() / "sweeppp-unused"),
        **backend);
    RemoteServer server(instrument, output, events, telemetry,
                        ServerConfig{.listenAddress = "0.0.0.0", .port = 0});
    auto started = server.start();
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().code() == ErrorCode::PermissionDenied);
}

TEST_CASE("a client speaking another protocol version is told so") {
    ServerFixture fixture;
    RawClient client(fixture.port());
    auto answer = client.handshake({}, kProtocolVersion + 1);
    REQUIRE(answer.has_value());
    CHECK(answer->name == msg::kRefused);
    CHECK(Refused::from(answer->body).reason == refusal::kVersion);
}

TEST_CASE("a second client is turned away, but only once it has authenticated") {
    ServerFixture fixture(ServerConfig{.token = "t"});
    RawClient first(fixture.port());
    REQUIRE(first.handshake("t")->name == msg::kWelcome);
    REQUIRE(ServerFixture::eventually([&] { return fixture.server->clientConnected(); }));

    SUBCASE("unauthenticated, it learns nothing of the first") {
        RawClient second(fixture.port());
        CHECK_FALSE(second.handshake("guess").has_value());
        CHECK(second.handshakeError().code() == ErrorCode::PermissionDenied);
    }

    SUBCASE("authenticated, it is told the radio is busy") {
        RawClient second(fixture.port());
        auto answer = second.handshake("t");
        REQUIRE(answer.has_value());
        CHECK(answer->name == msg::kRefused);
        CHECK(Refused::from(answer->body).reason == refusal::kBusy);
        CHECK(fixture.server->clientConnected());
    }
}

TEST_CASE("what arrives before authentication is held to a few bytes and seconds") {
    ServerFixture fixture;

    const std::vector<std::uint8_t> preamble(kPreamble.begin(), kPreamble.end());

    SUBCASE("a handshake message too large for a hello") {
        RawClient client(fixture.port());
        std::vector<std::uint8_t> bytes = preamble;
        bytes.push_back(0xFF);
        bytes.push_back(0xFF);
        bytes.resize(bytes.size() + 0xFFFF, 0x41);
        client.sendRaw(bytes);
        CHECK(client.closedWithin(2000ms));
    }

    SUBCASE("a preamble that is not ours") {
        RawClient client(fixture.port());
        client.sendRaw(std::vector<std::uint8_t>(32, 'G'));
        CHECK(client.closedWithin(2000ms));
    }

    SUBCASE("a client of the first protocol, which opened with a stream header") {
        RawClient client(fixture.port());
        std::vector<std::byte> header;
        sweeps::encodeStreamHeader(header, sweeps::StreamHeader{});
        const auto* begin = reinterpret_cast<const std::uint8_t*>(header.data());
        client.sendRaw({begin, begin + header.size()});
        CHECK(client.closedWithin(2000ms));
        CHECK_FALSE(fixture.instrument->running());
    }

    SUBCASE("a handshake message that is noise") {
        RawClient client(fixture.port());
        std::vector<std::uint8_t> bytes = preamble;
        bytes.push_back(64);
        bytes.push_back(0);
        bytes.resize(bytes.size() + 64, 0x5A);
        client.sendRaw(bytes);
        CHECK(client.closedWithin(3000ms));
    }

    SUBCASE("nothing at all") {
        RawClient client(fixture.port());
        CHECK(client.closedWithin(4000ms));
    }

    // Whatever the last one did, the next is still served.
    RawClient after(fixture.port());
    auto welcome = after.handshake();
    REQUIRE(welcome.has_value());
    CHECK(welcome->name == msg::kWelcome);
}

TEST_CASE("a client that says goodbye leaves the radio stopped and the server free") {
    ServerFixture fixture;
    {
        RawClient client(fixture.port());
        REQUIRE(client.handshake().has_value());
        client.command(op::kStart);
        REQUIRE(client.expect(msg::kFrame).has_value());
        client.send(msg::kBye, Bye{.reason = "closed"}.toMetadata());
        CHECK(client.closedWithin(2000ms));
    }
    CHECK(ServerFixture::eventually([&] { return !fixture.server->clientConnected(); }));
    CHECK(ServerFixture::eventually([&] { return !fixture.instrument->running(); }));

    RawClient next(fixture.port());
    auto welcome = next.handshake();
    REQUIRE(welcome.has_value());
    CHECK(welcome->name == msg::kWelcome);
    REQUIRE(next.expect(msg::kState).has_value());
    CHECK_FALSE(hashAt(next.sections(), section::kRun).getBool("running"));
    // The plan it was given survives the client.
    CHECK(decodePlan(hashAt(next.sections(), section::kPlan)).lowestHz() == doctest::Approx(100e6));
}

TEST_CASE("a client that drops leaves the radio running for a while, then stopped") {
    ServerFixture fixture(ServerConfig{.linger = 500ms});
    {
        RawClient client(fixture.port());
        REQUIRE(client.handshake().has_value());
        client.command(op::kStart);
        REQUIRE(client.expect(msg::kFrame).has_value());
        client.close();
    }
    CHECK(ServerFixture::eventually([&] { return !fixture.server->clientConnected(); }));
    CHECK(fixture.instrument->running());
    CHECK(ServerFixture::eventually([&] { return !fixture.instrument->running(); }, 3000ms));
}

TEST_CASE("a client back within the linger takes over the running radio") {
    ServerFixture fixture(ServerConfig{.linger = 10s});
    {
        RawClient client(fixture.port());
        REQUIRE(client.handshake().has_value());
        client.command(op::kStart);
        REQUIRE(client.expect(msg::kFrame).has_value());
        client.close();
    }
    CHECK(ServerFixture::eventually([&] { return !fixture.server->clientConnected(); }));

    RawClient back(fixture.port());
    REQUIRE(back.handshake().has_value());
    REQUIRE(back.expect(msg::kState).has_value());
    CHECK(hashAt(back.sections(), section::kRun).getBool("running"));
    // Frames carry on with no start from the new client, from a fresh segment.
    REQUIRE(back.expectRecord(sweeps::RecordType::SegmentOpen).has_value());
    CHECK(back.expect(msg::kFrame).has_value());
}

TEST_CASE("a client that falls silent is dropped") {
    ServerFixture fixture(ServerConfig{.silenceTimeout = 300ms});
    RawClient client(fixture.port());
    REQUIRE(client.handshake().has_value());
    CHECK(client.closedWithin(3000ms));
    CHECK(ServerFixture::eventually([&] { return !fixture.server->clientConnected(); }));
}

TEST_CASE("stopping the server says goodbye and ends the stream") {
    ServerFixture fixture;
    RawClient client(fixture.port());
    REQUIRE(client.handshake().has_value());
    REQUIRE(ServerFixture::eventually([&] { return fixture.server->clientConnected(); }));

    std::thread stopper([&] { fixture.server->stop(); });
    auto bye = client.expect(msg::kBye);
    REQUIRE(bye.has_value());
    CHECK(Bye::from(bye->body).reason == refusal::kShutdown);
    CHECK(client.expectRecord(sweeps::RecordType::EndOfStream).has_value());
    CHECK(client.closedWithin(2000ms));
    stopper.join();
}

TEST_CASE("a parameter set remotely is applied once and reported once") {
    ServerFixture fixture;
    RawClient client(fixture.port());
    REQUIRE(client.handshake().has_value());

    sweeps::Metadata args;
    args.setString("key", "gain");
    args.set("value", encodeValue(SdrValue{std::int64_t{24}}));
    const std::uint64_t seq = client.command(op::kSetParameter, args);
    client.pump(400ms);

    REQUIRE(std::ranges::any_of(
        client.replies(), [seq](const Reply& reply) { return reply.seq == seq && reply.ok; }));
    const auto changes = std::ranges::count_if(client.events(), [](const sweeps::SessionEvent& e) {
        const auto* body = e.as<sweeps::ParameterChangedData>();
        return body != nullptr && body->key == "gain";
    });
    CHECK(changes == 1);
    const sweeps::Metadata& values =
        hashAt(hashAt(client.sections(), section::kValues), "parameters");
    REQUIRE(values.find("gain") != nullptr);
    CHECK(asInt(decodeValue(*values.find("gain"))) == 24);

    sweeps::Metadata bad;
    bad.setString("key", "no_such_parameter");
    bad.set("value", encodeValue(SdrValue{true}));
    const std::uint64_t badSeq = client.command(op::kSetParameter, bad);
    const std::uint64_t unknownSeq = client.command("teleport");
    client.pump(300ms);
    CHECK(std::ranges::any_of(client.replies(), [badSeq](const Reply& reply) {
        return reply.seq == badSeq && !reply.ok;
    }));
    CHECK(std::ranges::any_of(client.replies(), [unknownSeq](const Reply& reply) {
        return reply.seq == unknownSeq && !reply.ok && reply.code == ErrorCode::Unsupported;
    }));
}

TEST_CASE("a burst of plans settles on the last one, every command answered") {
    ServerFixture fixture;
    RawClient client(fixture.port());
    REQUIRE(client.handshake().has_value());

    std::uint64_t last = 0;
    for (int i = 0; i < 20; ++i) {
        SweepPlan plan = ServerFixture::quickPlan();
        plan.segments = {SweepSegment{.startHz = 200e6 + (i * 1e6), .stopHz = 240e6 + (i * 1e6)}};
        sweeps::Metadata args;
        args.setHash("plan", encodePlan(plan));
        last = client.command(op::kApplySweepPlan, args);
    }
    client.pump(600ms);

    CHECK(client.replies().size() == 20);
    CHECK(std::ranges::all_of(client.replies(), [](const Reply& reply) { return reply.ok; }));
    CHECK(client.ackSeq() == last);
    CHECK(decodePlan(hashAt(client.sections(), section::kPlan)).lowestHz() ==
          doctest::Approx(219e6));
}

TEST_CASE("bench edits made remotely land in the server's own directory") {
    ServerFixture fixture;
    RawClient client(fixture.port());
    REQUIRE(client.handshake().has_value());

    const Antenna yagi{.id = "yagi", .name = "Yagi", .startHz = 400e6, .stopHz = 470e6};
    sweeps::Metadata args;
    args.setHash("antennas", encodeAntennas({&yagi, 1}));
    const std::uint64_t seq = client.command(op::kSetUserAntennas, args);
    client.pump(400ms);

    CHECK(std::ranges::any_of(client.replies(),
                              [seq](const Reply& reply) { return reply.seq == seq && reply.ok; }));
    CHECK(std::filesystem::exists(fixture.root() / "antennas" / "custom.toml"));
    const std::vector<Antenna> library =
        decodeAntennas(hashAt(client.sections(), section::kAntennas));
    CHECK(std::ranges::any_of(library,
                              [](const Antenna& a) { return a.id == "yagi" && !a.builtin; }));
}

// ---- shared servers -------------------------------------------------------------

namespace {

std::optional<Reply> replyTo(const RawClient& client, std::uint64_t seq) {
    const auto found = std::ranges::find(client.replies(), seq, &Reply::seq);
    return found != client.replies().end() ? std::optional<Reply>(*found) : std::nullopt;
}

} // namespace

TEST_CASE("a shared server lets a second client watch, but not change anything") {
    ServerFixture fixture(ServerConfig{.shared = true});
    RawClient desk(fixture.port());
    auto welcome = desk.handshake({}, kProtocolVersion, "desk");
    REQUIRE(welcome.has_value());
    CHECK(Welcome::from(welcome->body).shared);
    REQUIRE(desk.expect(msg::kState).has_value());
    CHECK(desk.control().you);
    CHECK(desk.control().shared);

    RawClient watcher(fixture.port());
    auto second = watcher.handshake({}, kProtocolVersion, "watcher");
    REQUIRE(second.has_value());
    REQUIRE(second->name == msg::kWelcome);
    REQUIRE(watcher.expect(msg::kState).has_value());
    CHECK_FALSE(watcher.control().you);
    CHECK(watcher.control().held);
    CHECK(watcher.control().controller == "desk");
    CHECK(watcher.control().controllerKind == client::kDesktop);

    // Its edit is refused, and what it touched is sent back as it was.
    const SweepPlan before = decodePlan(hashAt(watcher.sections(), section::kPlan));
    SweepPlan wanted = ServerFixture::quickPlan();
    wanted.segments = {SweepSegment{.startHz = 400e6, .stopHz = 440e6}};
    sweeps::Metadata args;
    args.setHash("plan", encodePlan(wanted));
    const std::uint64_t refused = watcher.command(op::kApplySweepPlan, args);
    // What only changes its own connection is still its to ask for.
    sweeps::Metadata link;
    link.setInt("maxBins", 4096);
    const std::uint64_t linkSeq = watcher.command(op::kSetLinkResolution, link);
    watcher.pump(400ms);
    REQUIRE(replyTo(watcher, refused).has_value());
    CHECK_FALSE(replyTo(watcher, refused)->ok);
    CHECK(replyTo(watcher, refused)->code == ErrorCode::PermissionDenied);
    CHECK(replyTo(watcher, refused)->message.find("desk") != std::string::npos);
    CHECK(watcher.ackSeq() >= refused);
    CHECK(fixture.instrument->sweepPlan().lowestHz() == doctest::Approx(before.lowestHz()));
    REQUIRE(replyTo(watcher, linkSeq).has_value());
    CHECK(replyTo(watcher, linkSeq)->ok);
    CHECK(hashAt(watcher.sections(), section::kLink).getInt("maxBins") == 4096);

    // Both see each other, and both get the radio's frames.
    const std::vector<ConnectedClient> seen = watcher.clients();
    REQUIRE(seen.size() == 2);
    CHECK(std::ranges::any_of(
        seen, [](const ConnectedClient& c) { return c.name == "desk" && c.controls && !c.you; }));
    CHECK(std::ranges::any_of(seen, [](const ConnectedClient& c) {
        return c.name == "watcher" && !c.controls && c.you;
    }));
    CHECK(fixture.server->clients().size() == 2);
    CHECK(fixture.server->clients().front().name == "desk");

    desk.command(op::kStart);
    CHECK(desk.expect(msg::kFrame).has_value());
    CHECK(watcher.expect(msg::kFrame).has_value());
}

TEST_CASE("any client of a shared server can take control from the one that has it") {
    ServerFixture fixture(ServerConfig{.shared = true});
    RawClient desk(fixture.port());
    REQUIRE(desk.handshake({}, kProtocolVersion, "desk").has_value());
    REQUIRE(ServerFixture::eventually([&] { return fixture.server->clients().size() == 1; }));
    RawClient phone(fixture.port());
    REQUIRE(phone.handshake({}, kProtocolVersion, "phone").has_value());

    const std::uint64_t take = phone.command(op::kTakeControl);
    phone.pump(300ms);
    desk.pump(300ms);
    REQUIRE(replyTo(phone, take).has_value());
    CHECK(replyTo(phone, take)->ok);
    CHECK(phone.control().you);
    CHECK_FALSE(desk.control().you);
    CHECK(desk.control().controller == "phone");
    const auto told = [](const RawClient& client) {
        return std::ranges::any_of(client.notices(), [](const InstrumentNotice& notice) {
            return notice.text.find("phone (desktop) took control") != std::string::npos;
        });
    };
    CHECK(told(desk));
    CHECK(told(phone));

    // The new controller's edits run; the old one's are refused.
    sweeps::Metadata gain;
    gain.setString("key", "gain");
    gain.set("value", encodeValue(SdrValue{std::int64_t{30}}));
    const std::uint64_t mine = phone.command(op::kSetParameter, gain);
    const std::uint64_t theirs = desk.command(op::kStart);
    phone.pump(300ms);
    desk.pump(300ms);
    CHECK(replyTo(phone, mine)->ok);
    CHECK_FALSE(replyTo(desk, theirs)->ok);
    CHECK_FALSE(fixture.instrument->running());

    // And back again.
    desk.command(op::kTakeControl);
    desk.pump(300ms);
    phone.pump(300ms);
    CHECK(desk.control().you);
    CHECK_FALSE(phone.control().you);

    // Released, nobody has it until somebody takes it.
    desk.command(op::kReleaseControl);
    desk.pump(300ms);
    phone.pump(100ms);
    CHECK_FALSE(desk.control().held);
    CHECK_FALSE(phone.control().held);
    const std::uint64_t orphan = phone.command(op::kStart);
    phone.pump(300ms);
    CHECK_FALSE(replyTo(phone, orphan)->ok);
}

TEST_CASE("when the controller drops, viewers keep watching and control is free") {
    ServerFixture fixture(ServerConfig{.linger = 300ms, .shared = true});
    RawClient desk(fixture.port());
    REQUIRE(desk.handshake({}, kProtocolVersion, "desk").has_value());
    REQUIRE(ServerFixture::eventually([&] { return fixture.server->clients().size() == 1; }));
    RawClient watcher(fixture.port());
    REQUIRE(watcher.handshake({}, kProtocolVersion, "watcher").has_value());
    desk.command(op::kStart);
    REQUIRE(desk.expect(msg::kFrame).has_value());

    desk.close();
    REQUIRE(ServerFixture::eventually([&] { return fixture.server->clients().size() == 1; }));
    watcher.pump(800ms);
    CHECK_FALSE(watcher.control().held);
    CHECK(watcher.clients().size() == 1);
    // Well past the linger, the radio still runs for the one watching.
    CHECK(fixture.instrument->running());
    CHECK(watcher.expect(msg::kFrame).has_value());

    // Once the last one leaves, it stops.
    watcher.send(msg::kBye, Bye{.reason = "closed"}.toMetadata());
    CHECK(watcher.closedWithin(5000ms));
    CHECK(ServerFixture::eventually([&] { return !fixture.instrument->running(); }));
}

TEST_CASE("a dropped controller's radio waits for that client, not the next") {
    ServerFixture fixture(ServerConfig{.linger = 10s, .shared = true});
    {
        RawClient desk(fixture.port());
        REQUIRE(desk.handshake({}, kProtocolVersion, "desk", "desk-id").has_value());
        desk.command(op::kStart);
        REQUIRE(desk.expect(msg::kFrame).has_value());
        desk.close();
    }
    REQUIRE(ServerFixture::eventually([&] { return !fixture.server->clientConnected(); }));
    CHECK(fixture.instrument->running());

    RawClient other(fixture.port());
    REQUIRE(other.handshake({}, kProtocolVersion, "other", "other-id").has_value());
    REQUIRE(other.expect(msg::kState).has_value());
    CHECK_FALSE(other.control().you);
    CHECK_FALSE(other.control().held);

    RawClient back(fixture.port());
    REQUIRE(back.handshake({}, kProtocolVersion, "desk", "desk-id").has_value());
    REQUIRE(back.expect(msg::kState).has_value());
    CHECK(back.control().you);
    CHECK(hashAt(back.sections(), section::kRun).getBool("running"));
}

TEST_CASE("a shared server takes as many clients as it is told, and no more") {
    ServerFixture fixture(ServerConfig{.shared = true, .maxClients = 2});
    RawClient first(fixture.port());
    REQUIRE(first.handshake({}, kProtocolVersion, "one")->name == msg::kWelcome);
    RawClient second(fixture.port());
    REQUIRE(second.handshake({}, kProtocolVersion, "two")->name == msg::kWelcome);
    REQUIRE(ServerFixture::eventually([&] { return fixture.server->clients().size() == 2; }));

    RawClient third(fixture.port());
    auto answer = third.handshake({}, kProtocolVersion, "three");
    REQUIRE(answer.has_value());
    CHECK(answer->name == msg::kRefused);
    CHECK(Refused::from(answer->body).reason == refusal::kLimit);
    CHECK(fixture.server->clients().size() == 2);
}

// ---- history of server recordings ----------------------------------------------

TEST_CASE("a recording on the server is read in tiles without downloading it") {
    const std::filesystem::path sessions =
        std::filesystem::temp_directory_path() / std::format("sweeppp-history-{}", monotonicNs());
    ServerFixture fixture(ServerConfig{.sessionsDir = sessions});
    RawClient client(fixture.port());
    REQUIRE(client.handshake().has_value());

    client.command(op::kStart);
    sweeps::Metadata bins;
    bins.setInt("maxBins", 4096);
    client.command(op::kStartRecording, bins);
    // Lines, not a time: under a sanitiser a second may hold none.
    const auto deadline = Clock::now() + 20s;
    while (ServerRecordings::from(hashAt(client.sections(), section::kRecordings)).lines < 20 &&
           Clock::now() < deadline) {
        client.pump(200ms);
    }
    client.command(op::kStopRecording);
    // Nothing more to record: frames would only crowd out the answers below.
    client.command(op::kStop);
    client.pump(500ms);
    const ServerRecordings recordings =
        ServerRecordings::from(hashAt(client.sections(), section::kRecordings));
    REQUIRE_FALSE(recordings.files.empty());
    const std::string name = recordings.files.front().name;

    sweeps::Metadata open;
    open.setString("name", name);
    const std::uint64_t openSeq = client.command(op::kHistoryOpen, open);
    auto opened = client.expect(msg::kHistory, 15000ms);
    REQUIRE(opened.has_value());
    CHECK(opened->body.getString("kind") == "opened");
    const std::int64_t handle = opened->body.getInt("handle");
    CHECK(handle > 0);
    CHECK(opened->body.getInt("lastLineNs") > opened->body.getInt("firstLineNs"));
    const sweeps::Value* segments = opened->body.find("segments");
    REQUIRE(segments != nullptr);
    REQUIRE(segments->asArray() != nullptr);
    CHECK_FALSE(segments->asArray()->empty());
    client.pump(100ms);
    CHECK(std::ranges::any_of(client.replies(),
                              [openSeq](const Reply& r) { return r.seq == openSeq && r.ok; }));

    sweeps::Metadata query;
    query.setInt("handle", handle);
    query.setInt("lines", 64);
    query.setInt("bins", 512);
    client.command(op::kHistoryQuery, query);
    std::vector<sweeps::Metadata> tiles;
    while (true) {
        auto piece = client.expect(msg::kHistory, 15000ms);
        REQUIRE(piece.has_value());
        REQUIRE(piece->body.getString("kind") == "tiles");
        if (const std::vector<sweeps::Value>* rows = piece->body.find("tiles")->asArray()) {
            for (const sweeps::Value& row : *rows) {
                tiles.push_back(*row.asHash());
            }
        }
        if (piece->body.getBool("last")) {
            break;
        }
    }

    // What the server sent is what reading the file here gives.
    auto reader = sweeps::SessionReader::open(sessions / name);
    REQUIRE(reader.has_value());
    auto direct = (*reader)->query(sweeps::HistoryQuery{.maxLines = 64, .maxBins = 512});
    REQUIRE(direct.has_value());
    REQUIRE_FALSE(direct->empty());
    REQUIRE(tiles.size() == direct->size());
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        const sweeps::HistoryTile& want = (*direct)[i];
        const sweeps::Metadata& got = tiles[i];
        CHECK(got.getInt("freqBlock") == want.freqBlock);
        CHECK(got.getInt("lines") == want.lines);
        CHECK(got.getInt("lod") == want.lod);
        CHECK(static_cast<float>(got.getFloat("originDb")) == want.originDb);
        const std::vector<std::byte>* data = got.find("data")->asBytes();
        REQUIRE(data != nullptr);
        REQUIRE(data->size() == want.data.size());
        CHECK(std::memcmp(data->data(), want.data.data(), data->size()) == 0);
    }

    // A name the server did not list is refused, as is a third reader.
    sweeps::Metadata bad;
    bad.setString("name", "../etc/passwd");
    const std::uint64_t badSeq = client.command(op::kHistoryOpen, bad);
    const std::uint64_t second = client.command(op::kHistoryOpen, open);
    const std::uint64_t third = client.command(op::kHistoryOpen, open);
    client.pump(400ms);
    const auto replied = [&](std::uint64_t seq) {
        return *std::ranges::find(client.replies(), seq, &Reply::seq);
    };
    CHECK_FALSE(replied(badSeq).ok);
    CHECK(replied(second).ok);
    CHECK_FALSE(replied(third).ok);

    sweeps::Metadata close;
    close.setInt("handle", handle);
    const std::uint64_t closeSeq = client.command(op::kHistoryClose, close);
    const std::uint64_t after = client.command(op::kHistoryQuery, query);
    client.pump(300ms);
    CHECK(replied(closeSeq).ok);
    CHECK_FALSE(replied(after).ok);

    std::error_code ec;
    std::filesystem::remove_all(sessions, ec);
}
