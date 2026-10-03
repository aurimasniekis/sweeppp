// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ReferenceFft.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
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
#include <sweeppp/net/Socket.hpp>
#include <sweeppp/remote/Messages.hpp>
#include <sweeppp/remote/Protocol.hpp>
#include <sweeppp/remote/RemoteServer.hpp>
#include <sweeppp/remote/WireCodec.hpp>
#include <sweeppp/sdr/ISdrDevice.hpp>
#include <sweeps/FileFormat.hpp>
#include <sweeps/Records.hpp>
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

/// The protocol by hand, with nothing of the real client in it: what a
/// server must cope with is whatever arrives, not what our client sends.
class RawClient {
public:
    explicit RawClient(std::uint16_t port) {
        auto socket = net::TcpSocket::connect("127.0.0.1", port, 2000ms);
        REQUIRE(socket.has_value());
        m_socket = std::move(*socket);
    }

    void sendRaw(const std::vector<std::byte>& bytes) { (void)m_socket.sendAll(asBytes(bytes)); }

    void sendHeader(sweeps::StreamHeader header = {}) {
        std::vector<std::byte> bytes;
        sweeps::encodeStreamHeader(bytes, header);
        sendRaw(bytes);
    }

    void send(std::string_view name, const sweeps::Metadata& body) {
        std::vector<std::byte> bytes;
        appendMessage(bytes, name, body, monotonicNs());
        sendRaw(bytes);
    }

    std::uint64_t command(std::string_view op, sweeps::Metadata args = {}) {
        const std::uint64_t seq = ++m_seq;
        send(msg::kCommand,
             Command{.seq = seq, .op = std::string(op), .args = std::move(args)}.toMetadata());
        return seq;
    }

    /// The next record, or nullopt when the connection ends or `timeout`
    /// passes.
    std::optional<sweeps::StreamRecord> next(std::chrono::milliseconds timeout = 5000ms) {
        const auto deadline = Clock::now() + timeout;
        std::vector<std::byte> buffer(std::size_t{64} * 1024);
        while (true) {
            if (!m_headerRead) {
                if (m_pending.size() >= sweeps::StreamHeader::kBytes) {
                    auto header =
                        sweeps::decodeStreamHeader(m_pending.data(), sweeps::StreamHeader::kBytes);
                    REQUIRE(header.has_value());
                    m_framer.feed(m_pending.data() + sweeps::StreamHeader::kBytes,
                                  m_pending.size() - sweeps::StreamHeader::kBytes);
                    m_pending.clear();
                    m_headerRead = true;
                }
            }
            if (m_headerRead) {
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
            auto readable = m_socket.waitReadable(50ms);
            if (!readable || !*readable) {
                continue;
            }
            auto received =
                m_socket.receive({reinterpret_cast<std::uint8_t*>(buffer.data()), buffer.size()});
            if (!received || *received == 0) {
                m_closed = true;
                continue;
            }
            if (m_headerRead) {
                m_framer.feed(buffer.data(), *received);
            } else {
                m_pending.insert(m_pending.end(), buffer.begin(),
                                 buffer.begin() + static_cast<std::ptrdiff_t>(*received));
            }
        }
    }

    /// The next control message called `name`, skipping everything else.
    std::optional<Message> expect(std::string_view name,
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
            if (message->name == name) {
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

    /// Header, hello, auth. The welcome's or refusal's message.
    std::optional<Message> handshake(std::string_view token = {},
                                     std::uint32_t version = kProtocolVersion) {
        sendHeader();
        send(msg::kHello, Hello{.protocolVersion = version, .software = "test"}.toMetadata());
        auto challenge = expectAny({msg::kChallenge, msg::kRefused});
        if (!challenge || challenge->name == msg::kRefused) {
            return challenge;
        }
        const Challenge offered = Challenge::from(challenge->body);
        Auth auth;
        auth.clientNonce[0] = 42;
        auth.mac = authMac(token, offered.serverNonce, auth.clientNonce);
        send(msg::kAuth, auth.toMetadata());
        return expectAny({msg::kWelcome, msg::kRefused});
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

    void close() { m_socket.close(); }

    /// Every state section received so far, latest first-come merged.
    const sweeps::Metadata& sections() const noexcept { return m_sections; }
    std::uint64_t ackSeq() const noexcept { return m_ackSeq; }
    const std::vector<Reply>& replies() const noexcept { return m_replies; }
    const std::vector<sweeps::SessionEvent>& events() const noexcept { return m_events; }
    std::size_t tiles() const noexcept { return m_tiles; }
    std::size_t telemetry() const noexcept { return m_telemetry; }

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
        }
    }

    net::TcpSocket m_socket;
    sweeps::RecordFramer m_framer{kMaxServerRecordBytes};
    std::vector<std::byte> m_pending;
    bool m_headerRead = false;
    bool m_closed = false;
    std::uint64_t m_seq = 0;

    sweeps::Metadata m_sections;
    std::uint64_t m_ackSeq = 0;
    std::vector<Reply> m_replies;
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

    SUBCASE("a wrong one") {
        RawClient client(fixture.port());
        auto answer = client.handshake("battery staple");
        REQUIRE(answer.has_value());
        CHECK(answer->name == msg::kRefused);
        CHECK(Refused::from(answer->body).reason == refusal::kAuth);
        CHECK(client.closedWithin(2000ms));
        CHECK_FALSE(fixture.server->clientConnected());
    }

    SUBCASE("none at all") {
        RawClient client(fixture.port());
        auto answer = client.handshake();
        REQUIRE(answer.has_value());
        CHECK(answer->name == msg::kRefused);
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
        second.sendHeader();
        second.send(msg::kHello, Hello{.protocolVersion = kProtocolVersion}.toMetadata());
        auto reply = second.expectAny({msg::kChallenge, msg::kRefused});
        REQUIRE(reply.has_value());
        CHECK(reply->name == msg::kChallenge);

        Auth wrong;
        second.send(msg::kAuth, wrong.toMetadata());
        auto refused = second.expect(msg::kRefused);
        REQUIRE(refused.has_value());
        CHECK(Refused::from(refused->body).reason == refusal::kAuth);
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

    SUBCASE("a record too large for a hello") {
        RawClient client(fixture.port());
        client.sendHeader();
        sweeps::Metadata body;
        body.setString("padding", std::string(std::size_t{kMaxPreAuthRecordBytes} * 2, 'x'));
        client.send(msg::kHello, body);
        CHECK(client.closedWithin(2000ms));
    }

    SUBCASE("a preamble that is not a stream header") {
        RawClient client(fixture.port());
        client.sendRaw(std::vector<std::byte>(32, std::byte{'G'}));
        CHECK(client.closedWithin(2000ms));
    }

    SUBCASE("a command before the handshake") {
        RawClient client(fixture.port());
        client.sendHeader();
        client.command(op::kStart);
        auto refused = client.expect(msg::kRefused);
        REQUIRE(refused.has_value());
        CHECK(Refused::from(refused->body).reason == refusal::kProtocol);
        CHECK_FALSE(fixture.instrument->running());
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

TEST_CASE("a client that goes away leaves the radio stopped and the server free") {
    ServerFixture fixture;
    {
        RawClient client(fixture.port());
        REQUIRE(client.handshake().has_value());
        client.command(op::kStart);
        REQUIRE(client.expect(msg::kFrame).has_value());
        client.close();
    }
    CHECK(ServerFixture::eventually([&] { return !fixture.server->clientConnected(); }));

    RawClient next(fixture.port());
    auto welcome = next.handshake();
    REQUIRE(welcome.has_value());
    CHECK(welcome->name == msg::kWelcome);
    REQUIRE(next.expect(msg::kState).has_value());
    CHECK_FALSE(hashAt(next.sections(), section::kRun).getBool("running"));
    // The plan it was given survives the client.
    CHECK(decodePlan(hashAt(next.sections(), section::kPlan)).lowestHz() == doctest::Approx(100e6));
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
