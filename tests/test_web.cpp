// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ReferenceFft.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <sweeppp/backends/sdr/SyntheticDevice.hpp>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/crypto/Sha1.hpp>
#include <sweeppp/crypto/Sha256.hpp>
#include <sweeppp/fft/FftBackendManager.hpp>
#include <sweeppp/instrument/LocalInstrument.hpp>
#include <sweeppp/net/Base64.hpp>
#include <sweeppp/net/Http.hpp>
#include <sweeppp/net/Socket.hpp>
#include <sweeppp/net/WebSocket.hpp>
#include <sweeppp/remote/Handshake.hpp>
#include <sweeppp/remote/Messages.hpp>
#include <sweeppp/remote/RemoteServer.hpp>
#include <sweeppp/remote/WireCodec.hpp>
#include <sweeppp/sdr/ISdrDevice.hpp>
#include <sweeppp/web/WebServer.hpp>
#include <sweeps/FileFormat.hpp>
#include <sweeps/Stream.hpp>
#include <thread>
#include <vector>

using namespace sweeppp;
using namespace std::chrono_literals;
namespace http = sweeppp::net::http;
namespace ws = sweeppp::net::ws;

namespace {

using Clock = std::chrono::steady_clock;

std::vector<std::uint8_t> bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

std::string hex(std::span<const std::uint8_t> digest) {
    return crypto::toHex(digest);
}

} // namespace

// ---- primitives ---------------------------------------------------------------

TEST_CASE("SHA-1 matches the RFC 3174 test vectors") {
    CHECK(hex(crypto::sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex(crypto::sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK(hex(crypto::sha1(std::string(1'000'000, 'a'))) ==
          "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    std::string repeated;
    for (int i = 0; i < 80; ++i) {
        repeated += "01234567";
    }
    CHECK(hex(crypto::sha1(repeated)) == "dea356a2cddd90c7a7ecedc5ebb563934f460452");
    CHECK(hex(crypto::sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST_CASE("base64 matches RFC 4648 and refuses what is not base64") {
    const std::array<std::pair<std::string_view, std::string_view>, 7> vectors{{
        {"", ""},
        {"f", "Zg=="},
        {"fo", "Zm8="},
        {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="},
        {"fooba", "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"},
    }};
    for (const auto& [plain, encoded] : vectors) {
        CHECK(net::base64Encode(bytes(plain)) == encoded);
        const auto decoded = net::base64Decode(encoded);
        REQUIRE(decoded.has_value());
        CHECK(*decoded == bytes(plain));
    }
    CHECK_FALSE(net::base64Decode("Zm9").has_value());
    CHECK_FALSE(net::base64Decode("Zm9v!A==").has_value());
    CHECK_FALSE(net::base64Decode("Z===").has_value());
    CHECK_FALSE(net::base64Decode("Zg==Zg==").has_value());
}

TEST_CASE("the WebSocket accept key is the RFC 6455 example's") {
    CHECK(ws::acceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST_CASE("a request head is parsed, and a hostile one refused") {
    auto request = http::parseHead("GET /api/overlays?from=1e8&to=2e8 HTTP/1.1\r\n"
                                   "Host: pi:8080\r\n"
                                   "Cookie: a=1; sweeppp=abc\r\n"
                                   "X-Repeat: one\r\n"
                                   "x-repeat: two\r\n");
    REQUIRE(request.has_value());
    CHECK(request->method == "GET");
    CHECK(request->path == "/api/overlays");
    CHECK(request->queryValue("from") == "1e8");
    CHECK(request->header("host") == "pi:8080");
    CHECK(request->header("x-repeat") == "one, two");
    CHECK(request->cookie("sweeppp") == "abc");
    CHECK(request->cookie("missing").empty());

    auto decoded = http::parseHead("GET /assets/a%20b.js HTTP/1.0\r\n");
    REQUIRE(decoded.has_value());
    CHECK(decoded->path == "/assets/a b.js");

    for (const std::string_view hostile : {
             "GET  / HTTP/1.1\r\n",
             "GET / HTTP/1.1 extra\r\n",
             "GET relative HTTP/1.1\r\n",
             "GET / HTTP/2.0\r\n",
             "G\"T / HTTP/1.1\r\n",
             "GET /%00 HTTP/1.1\r\n",
             "GET /%zz HTTP/1.1\r\n",
             "GET /%4 HTTP/1.1\r\n",
             "GET / HTTP/1.1\r\n folded: header\r\n",
             "GET / HTTP/1.1\r\nno colon\r\n",
             "GET / HTTP/1.1\r\n: empty name\r\n",
             "GET / HTTP/1.1\r\nBad\x01: value\r\n",
             "GET / HTTP/1.1\r\nName: va\x7Flue\r\n",
         }) {
        CAPTURE(hostile);
        CHECK_FALSE(http::parseHead(hostile).has_value());
    }

    std::string many = "GET / HTTP/1.1\r\n";
    for (std::size_t i = 0; i <= http::kMaxHeaders; ++i) {
        many += std::format("H{}: v\r\n", i);
    }
    CHECK_FALSE(http::parseHead(many).has_value());
    CHECK_FALSE(
        http::parseHead("GET /" + std::string(http::kMaxHeadBytes, 'a') + " HTTP/1.1").has_value());
}

TEST_CASE("a form body gives up its fields, decoded") {
    CHECK(http::formValue("token=a%2Bb+c&x=1", "token") == "a+b c");
    CHECK(http::formValue("x=1", "token") == std::nullopt);
    CHECK(http::formValue("token=", "token") == "");
    CHECK(http::formValue("token=%zz", "token") == std::nullopt);
}

TEST_CASE("frames are read back into messages, masked, fragmented or not") {
    const std::array<std::uint8_t, 4> mask{0x11, 0x22, 0x33, 0x44};
    ws::MessageReader reader;

    SUBCASE("one masked binary frame, fed a byte at a time") {
        const std::vector<std::uint8_t> payload(300, 0xAB);
        const std::vector<std::uint8_t> frame =
            ws::encodeFrame(ws::Opcode::Binary, payload, true, mask);
        std::optional<ws::Message> got;
        for (const std::uint8_t byte : frame) {
            reader.feed(std::span(&byte, 1));
            auto next = reader.next();
            REQUIRE(next.has_value());
            if (*next) {
                got = std::move(**next);
            }
        }
        REQUIRE(got.has_value());
        CHECK(got->opcode == ws::Opcode::Binary);
        CHECK(got->payload == payload);
    }

    SUBCASE("fragments, with a ping between them") {
        const auto part1 = ws::encodeFrame(ws::Opcode::Binary, bytes("hel"), false, mask);
        const auto ping = ws::encodeFrame(ws::Opcode::Ping, bytes("p"), true, mask);
        const auto part2 = ws::encodeFrame(ws::Opcode::Continuation, bytes("lo"), true, mask);
        reader.feed(part1);
        reader.feed(ping);
        reader.feed(part2);
        auto first = reader.next();
        REQUIRE(first.has_value());
        REQUIRE(first->has_value());
        CHECK((*first)->opcode == ws::Opcode::Ping);
        auto second = reader.next();
        REQUIRE(second.has_value());
        REQUIRE(second->has_value());
        CHECK((*second)->opcode == ws::Opcode::Binary);
        CHECK((*second)->payload == bytes("hello"));
    }

    SUBCASE("a 64 KiB frame uses the long length") {
        const std::vector<std::uint8_t> payload(70'000, 7);
        reader.feed(ws::encodeFrame(ws::Opcode::Binary, payload, true, mask));
        auto next = reader.next();
        REQUIRE(next.has_value());
        REQUIRE(next->has_value());
        CHECK((*next)->payload.size() == payload.size());
    }
}

TEST_CASE("frames RFC 6455 says to fail the connection over are refused") {
    const std::array<std::uint8_t, 4> mask{1, 2, 3, 4};
    const auto refused = [](const std::vector<std::uint8_t>& stream,
                            std::size_t maxMessage = ws::kMaxMessageBytes) {
        ws::MessageReader reader(true, maxMessage);
        reader.feed(stream);
        while (true) {
            auto next = reader.next();
            if (!next) {
                // And it stays failed.
                return !reader.next().has_value();
            }
            if (!*next) {
                return false;
            }
        }
    };

    CHECK(refused(ws::encodeFrame(ws::Opcode::Binary, bytes("x"))));
    std::vector<std::uint8_t> reserved =
        ws::encodeFrame(ws::Opcode::Binary, bytes("x"), true, mask);
    reserved[0] |= 0x40;
    CHECK(refused(reserved));
    std::vector<std::uint8_t> unknown = ws::encodeFrame(ws::Opcode::Binary, bytes("x"), true, mask);
    unknown[0] = static_cast<std::uint8_t>((unknown[0] & 0xF0U) | 0x3U);
    CHECK(refused(unknown));
    CHECK(
        refused(ws::encodeFrame(ws::Opcode::Ping, std::vector<std::uint8_t>(126, 0), true, mask)));
    CHECK(refused(ws::encodeFrame(ws::Opcode::Ping, bytes("x"), false, mask)));
    CHECK(refused(ws::encodeFrame(ws::Opcode::Continuation, bytes("x"), true, mask)));
    std::vector<std::uint8_t> nested = ws::encodeFrame(ws::Opcode::Binary, bytes("a"), false, mask);
    const auto second = ws::encodeFrame(ws::Opcode::Binary, bytes("b"), true, mask);
    nested.insert(nested.end(), second.begin(), second.end());
    CHECK(refused(nested));
    CHECK(refused(
        ws::encodeFrame(ws::Opcode::Binary, std::vector<std::uint8_t>(2000, 0), true, mask), 1000));
    // Fragments that are each small but add up to too much.
    std::vector<std::uint8_t> pieces;
    for (int i = 0; i < 3; ++i) {
        const auto piece = ws::encodeFrame(i == 0 ? ws::Opcode::Binary : ws::Opcode::Continuation,
                                           std::vector<std::uint8_t>(400, 0), i == 2, mask);
        pieces.insert(pieces.end(), piece.begin(), piece.end());
    }
    CHECK(refused(pieces, 1000));
    // A declared length of 2^63 and up.
    std::vector<std::uint8_t> huge{0x82, 0xFF, 0x80, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4};
    CHECK(refused(huge));
}

TEST_CASE("an upgrade is checked, and only this site may make one") {
    const auto upgrade = [](std::string extra) {
        return http::parseHead("GET /ws HTTP/1.1\r\n"
                               "Host: pi.local:8080\r\n"
                               "Upgrade: websocket\r\n"
                               "Connection: keep-alive, Upgrade\r\n"
                               "Sec-WebSocket-Version: 13\r\n"
                               "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" +
                               extra)
            .value();
    };
    auto accepted = ws::checkUpgrade(upgrade(""));
    REQUIRE(accepted.has_value());
    CHECK(*accepted == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    CHECK(ws::sameOrigin(upgrade("")));
    CHECK(ws::sameOrigin(upgrade("Origin: http://pi.local:8080\r\n")));
    CHECK(ws::sameOrigin(upgrade("Origin: https://PI.local:8080\r\n")));
    CHECK_FALSE(ws::sameOrigin(upgrade("Origin: http://evil.example\r\n")));
    CHECK_FALSE(ws::sameOrigin(upgrade("Origin: http://pi.local:8081\r\n")));
    CHECK_FALSE(ws::sameOrigin(upgrade("Origin: null\r\n")));

    auto version = http::parseHead("GET /ws HTTP/1.1\r\nUpgrade: websocket\r\n"
                                   "Connection: Upgrade\r\nSec-WebSocket-Version: 8\r\n"
                                   "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n");
    CHECK_FALSE(ws::checkUpgrade(*version).has_value());
    auto shortKey = http::parseHead("GET /ws HTTP/1.1\r\nUpgrade: websocket\r\n"
                                    "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                                    "Sec-WebSocket-Key: c2hvcnQ=\r\n");
    CHECK_FALSE(ws::checkUpgrade(*shortKey).has_value());
}

// ---- the server ------------------------------------------------------------------

namespace {

constexpr std::array<std::uint8_t, 5> kIndex{'h', 'e', 'l', 'l', 'o'};
constexpr std::array<std::uint8_t, 3> kScript{'j', 's', '!'};
constexpr std::array<web::EmbeddedFile, 2> kAssets{{
    {"index.html", kIndex, "text/html; charset=utf-8", "index1"},
    {"assets/app.js", kScript, "text/javascript; charset=utf-8", "app1"},
}};

/// A synthetic radio behind a radio server and a web server, both on
/// ephemeral loopback ports.
class WebFixture {
public:
    explicit WebFixture(std::string token = "secret", bool shared = false,
                        std::chrono::seconds idle = std::chrono::hours(12))
        : m_root(std::filesystem::temp_directory_path() /
                 std::format("sweeppp-web-{}", monotonicNs())) {
        registerReferenceFftBackend();
        registerBuiltinSdrDevices();
        auto backend = FftBackendManager::instance().acquire("reference");
        REQUIRE(backend.has_value());
        instrument = std::make_unique<LocalInstrument>(output, events, telemetry,
                                                       InstrumentPaths::under(m_root), **backend);
        auto device = SdrDeviceManager::instance().open("synthetic", "");
        REQUIRE(device.has_value());
        instrument->adoptDevice(std::move(*device));
        SweepPlan plan;
        plan.segments = {SweepSegment{.startHz = 100e6, .stopHz = 140e6}};
        plan.sampleRate = 8e6;
        plan.rbwHz = 100e3;
        plan.applyMode(SweepMode::Fast);
        REQUIRE(instrument->applySweepPlan(plan).has_value());

        std::filesystem::create_directories(m_root / "sessions");
        server = std::make_unique<remote::RemoteServer>(
            *instrument, output, events, telemetry,
            remote::ServerConfig{.port = 0,
                                 .token = token,
                                 .serverName = "bench",
                                 .handshakeTimeout = 1500ms,
                                 .refusalDelay = 50ms,
                                 .sessionsDir = m_root / "sessions",
                                 .shared = shared});
        REQUIRE(server->start().has_value());
        web =
            std::make_unique<web::WebServer>(*server, web::WebServerConfig{.port = 0,
                                                                           .token = token,
                                                                           .assets = kAssets,
                                                                           .sessionIdle = idle,
                                                                           .loginInterval = 50ms});
        REQUIRE(web->start().has_value());
    }

    ~WebFixture() {
        web.reset();
        server.reset();
        instrument.reset();
        std::error_code ec;
        std::filesystem::remove_all(m_root, ec);
    }

    WebFixture(const WebFixture&) = delete;
    WebFixture& operator=(const WebFixture&) = delete;
    WebFixture(WebFixture&&) = delete;
    WebFixture& operator=(WebFixture&&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return m_root; }

    FrameBus output;
    EventBus events;
    Telemetry telemetry;
    std::unique_ptr<LocalInstrument> instrument;
    std::unique_ptr<remote::RemoteServer> server;
    std::unique_ptr<web::WebServer> web;

private:
    std::filesystem::path m_root;
};

struct Reply {
    int status = 0;
    std::string head;
    std::string body;

    [[nodiscard]] std::string header(std::string_view name) const {
        std::string lowerHead = head;
        std::ranges::transform(lowerHead, lowerHead.begin(), [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        const std::string key = std::format("\r\n{}: ", name);
        const std::size_t at = lowerHead.find(key);
        if (at == std::string::npos) {
            return {};
        }
        const std::size_t start = at + key.size();
        return head.substr(start, head.find("\r\n", start) - start);
    }
};

/// Sends raw bytes as a request and reads the answer until the server closes.
Reply roundTrip(std::uint16_t port, std::string_view request) {
    auto socket = net::TcpSocket::connect("127.0.0.1", port, 2000ms);
    REQUIRE(socket.has_value());
    REQUIRE(socket->sendAll(bytes(request)).has_value());
    std::string all;
    std::array<std::uint8_t, 4096> chunk{};
    const auto deadline = Clock::now() + 5s;
    while (Clock::now() < deadline) {
        auto readable = socket->waitReadable(100ms);
        if (!readable || !*readable) {
            continue;
        }
        auto got = socket->receive(chunk);
        if (!got || *got == 0) {
            break;
        }
        all.append(reinterpret_cast<const char*>(chunk.data()), *got);
    }
    Reply reply;
    const std::size_t end = all.find("\r\n\r\n");
    reply.head = all.substr(0, end);
    reply.body = end == std::string::npos ? std::string{} : all.substr(end + 4);
    if (all.size() > 12) {
        reply.status = std::stoi(all.substr(9, 3));
    }
    return reply;
}

Reply get(std::uint16_t port, std::string_view path, std::string_view headers = {}) {
    return roundTrip(
        port, std::format("GET {} HTTP/1.1\r\nHost: 127.0.0.1:{}\r\n{}\r\n", path, port, headers));
}

Reply login(std::uint16_t port, std::string_view token) {
    const std::string body = std::format("token={}", token);
    return roundTrip(port, std::format("POST /login HTTP/1.1\r\nHost: 127.0.0.1:{}\r\n"
                                       "Content-Type: application/x-www-form-urlencoded\r\n"
                                       "Content-Length: {}\r\n\r\n{}",
                                       port, body.size(), body));
}

std::string cookieOf(const Reply& reply) {
    const std::string set = reply.header("set-cookie");
    return set.substr(0, set.find(';'));
}

/// A browser's side of /ws, by hand: masked frames out, the record stream in.
class BrowserClient {
public:
    /// Nothing, with the HTTP answer kept, when the upgrade is refused.
    static std::optional<BrowserClient> open(std::uint16_t port, std::string_view cookie,
                                             std::string_view origin, Reply* refusal = nullptr) {
        auto socket = net::TcpSocket::connect("127.0.0.1", port, 2000ms);
        REQUIRE(socket.has_value());
        const std::string request = std::format(
            "GET /ws HTTP/1.1\r\nHost: 127.0.0.1:{}\r\nOrigin: {}\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nCookie: {}\r\n\r\n",
            port, origin, cookie);
        REQUIRE(socket->sendAll(bytes(request)).has_value());

        std::string head;
        std::array<std::uint8_t, 1> one{};
        const auto deadline = Clock::now() + 3s;
        while (!head.ends_with("\r\n\r\n") && Clock::now() < deadline) {
            auto readable = socket->waitReadable(50ms);
            if (!readable || !*readable) {
                continue;
            }
            auto got = socket->receive(one);
            if (!got || *got == 0) {
                break;
            }
            head += static_cast<char>(one[0]);
        }
        if (!head.starts_with("HTTP/1.1 101")) {
            if (refusal != nullptr) {
                refusal->head = head;
                refusal->status = head.size() > 12 ? std::stoi(head.substr(9, 3)) : 0;
            }
            return std::nullopt;
        }
        CHECK(head.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
        BrowserClient client;
        client.m_socket = std::move(*socket);
        return client;
    }

    /// The stream header and a hello, as the page opens with.
    void hello(std::string name) {
        std::vector<std::byte> out;
        sweeps::encodeStreamHeader(out, sweeps::StreamHeader{});
        remote::appendMessage(out, remote::msg::kHello,
                              remote::Hello{.protocolVersion = remote::kProtocolVersion,
                                            .software = "test page",
                                            .kind = std::string(remote::client::kWeb),
                                            .name = std::move(name)}
                                  .toMetadata());
        send(out);
    }

    std::uint64_t command(std::string_view op, sweeps::Metadata args = {}) {
        std::vector<std::byte> out;
        const std::uint64_t seq = ++m_seq;
        remote::appendMessage(
            out, remote::msg::kCommand,
            remote::Command{.seq = seq, .op = std::string(op), .args = std::move(args)}
                .toMetadata());
        send(out);
        return seq;
    }

    void send(const std::vector<std::byte>& data) {
        const std::span<const std::uint8_t> view(reinterpret_cast<const std::uint8_t*>(data.data()),
                                                 data.size());
        REQUIRE(m_socket
                    .sendAll(ws::encodeFrame(ws::Opcode::Binary, view, true,
                                             std::array<std::uint8_t, 4>{9, 8, 7, 6}))
                    .has_value());
    }

    /// The next control message called `name`, keeping score on the way.
    std::optional<remote::Message> expect(std::string_view name,
                                          std::chrono::milliseconds timeout = 5000ms) {
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            sweeps::StreamRecord record;
            auto got = m_framer.next(record);
            REQUIRE(got.has_value());
            if (*got) {
                if (record.header.type == static_cast<std::uint16_t>(sweeps::RecordType::Tile)) {
                    ++m_tiles;
                }
                if (record.header.type !=
                    static_cast<std::uint16_t>(sweeps::RecordType::PluginData)) {
                    continue;
                }
                auto message = remote::decodeMessage(record);
                REQUIRE(message.has_value());
                remember(*message);
                if (message->name == name) {
                    return std::move(*message);
                }
                continue;
            }
            if (!pumpSocket()) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    void pump(std::chrono::milliseconds duration) {
        (void)expect("nothing will be called this", duration);
    }

    [[nodiscard]] const std::vector<remote::Reply>& replies() const noexcept { return m_replies; }
    [[nodiscard]] const sweeps::Metadata& sections() const noexcept { return m_sections; }
    [[nodiscard]] std::size_t tiles() const noexcept { return m_tiles; }

private:
    /// One more piece off the socket; false once it has closed.
    bool pumpSocket() {
        auto readable = m_socket.waitReadable(50ms);
        if (!readable) {
            return false;
        }
        if (*readable) {
            std::array<std::uint8_t, 65536> chunk{};
            auto got = m_socket.receive(chunk);
            if (!got || *got == 0) {
                return false;
            }
            m_reader.feed(std::span(chunk).first(*got));
        }
        while (true) {
            auto message = m_reader.next();
            REQUIRE(message.has_value());
            if (!*message) {
                return true;
            }
            if ((*message)->opcode == ws::Opcode::Close) {
                return false;
            }
            if ((*message)->opcode != ws::Opcode::Binary) {
                continue;
            }
            const std::vector<std::uint8_t>& payload = (*message)->payload;
            std::size_t offset = 0;
            if (!m_sawHeader) {
                REQUIRE(payload.size() >= sweeps::StreamHeader::kBytes);
                REQUIRE(
                    sweeps::decodeStreamHeader(reinterpret_cast<const std::byte*>(payload.data()),
                                               sweeps::StreamHeader::kBytes)
                        .has_value());
                m_sawHeader = true;
                offset = sweeps::StreamHeader::kBytes;
            }
            m_framer.feed(reinterpret_cast<const std::byte*>(payload.data()) + offset,
                          payload.size() - offset);
        }
    }

    void remember(const remote::Message& message) {
        if (message.name == remote::msg::kState) {
            const remote::State state = remote::State::from(message.body);
            for (const auto& [name, value] : state.sections) {
                m_sections.set(name, value);
            }
        } else if (message.name == remote::msg::kReply) {
            m_replies.push_back(remote::Reply::from(message.body));
        }
    }

    BrowserClient() = default;

    net::TcpSocket m_socket;
    ws::MessageReader m_reader{false};
    sweeps::RecordFramer m_framer{remote::kMaxServerRecordBytes};
    bool m_sawHeader = false;
    std::uint64_t m_seq = 0;
    std::vector<remote::Reply> m_replies;
    sweeps::Metadata m_sections;
    std::size_t m_tiles = 0;
};

bool eventually(const std::function<bool()>& done, std::chrono::milliseconds timeout = 5000ms) {
    const auto deadline = Clock::now() + timeout;
    while (!done()) {
        if (Clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

std::string originOf(std::uint16_t port) {
    return std::format("http://127.0.0.1:{}", port);
}

} // namespace

TEST_CASE("the page and its files are served from the table, and nothing else") {
    WebFixture fixture;
    const std::uint16_t port = fixture.web->port();

    const Reply index = get(port, "/");
    CHECK(index.status == 200);
    CHECK(index.body == "hello");
    CHECK(index.header("content-type") == "text/html; charset=utf-8");
    CHECK_FALSE(index.header("content-security-policy").empty());
    CHECK(index.header("etag") == "\"index1\"");
    CHECK(get(port, "/", "If-None-Match: \"index1\"\r\n").status == 304);
    // Anything the page routes itself gets the page.
    CHECK(get(port, "/history").body == "hello");

    const Reply script = get(port, "/assets/app.js");
    CHECK(script.status == 200);
    CHECK(script.body == "js!");
    CHECK(get(port, "/assets/missing.js").status == 404);
    CHECK(get(port, "/assets/../index.html").status == 404);
    CHECK(get(port, "/assets/%2e%2e/index.html").status == 404);
    CHECK(get(port, "/assets//app.js").status == 404);
    CHECK(get(port, "/api/nothing").status == 404);

    CHECK(roundTrip(port, "DELETE / HTTP/1.1\r\nHost: x\r\n\r\n").status == 405);
    CHECK(roundTrip(port, "GARBAGE\r\n\r\n").status == 400);
    CHECK(roundTrip(port, "GET / HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc").status == 413);
}

TEST_CASE("the browser logs in with the token and gets a cookie that lasts") {
    WebFixture fixture;
    const std::uint16_t port = fixture.web->port();

    CHECK(get(port, "/api/auth").body == R"({"authenticated":false,"required":true})");
    CHECK(get(port, "/api/overlays?from=1e8&to=2e8").status == 401);
    CHECK(get(port, "/api/recordings/x.sweeps").status == 401);
    CHECK(login(port, "wrong").status == 401);

    // Too soon after the last try from this address.
    CHECK(login(port, "secret").status == 429);
    std::this_thread::sleep_for(60ms);
    const Reply good = login(port, "secret");
    CHECK(good.status == 204);
    const std::string setCookie = good.header("set-cookie");
    CHECK(setCookie.find("HttpOnly") != std::string::npos);
    CHECK(setCookie.find("SameSite=Strict") != std::string::npos);
    const std::string cookie = cookieOf(good);
    REQUIRE(cookie.size() == std::string("sweeppp=").size() + 64);

    const std::string header = std::format("Cookie: {}\r\n", cookie);
    CHECK(get(port, "/api/auth", header).body == R"({"authenticated":true,"required":true})");
    CHECK(get(port, "/api/overlays?from=1e8&to=2e8", header).status == 200);
    CHECK(get(port, "/api/overlays?from=x&to=2e8", header).status == 400);
    CHECK(get(port, "/api/auth", "Cookie: sweeppp=forged\r\n").body ==
          R"({"authenticated":false,"required":true})");

    CHECK(
        roundTrip(port, std::format("POST /logout HTTP/1.1\r\nHost: x\r\n{}\r\n", header)).status ==
        204);
    CHECK(get(port, "/api/auth", header).body == R"({"authenticated":false,"required":true})");
}

TEST_CASE("a session idle past its limit has to log in again") {
    WebFixture fixture("secret", false, 1s);
    const std::uint16_t port = fixture.web->port();
    const std::string header = std::format("Cookie: {}\r\n", cookieOf(login(port, "secret")));
    CHECK(get(port, "/api/auth", header).body.find("\"authenticated\":true") != std::string::npos);
    std::this_thread::sleep_for(1200ms);
    CHECK(get(port, "/api/auth", header).body.find("\"authenticated\":false") != std::string::npos);
}

TEST_CASE("themes and colour maps are served without logging in") {
    WebFixture fixture;
    const Reply themes = get(fixture.web->port(), "/api/themes");
    CHECK(themes.status == 200);
    CHECK(themes.header("content-type") == "application/json");
    CHECK(themes.body.find("\"chrome\"") != std::string::npos);
    const Reply maps = get(fixture.web->port(), "/api/colormaps");
    CHECK(maps.status == 200);
    CHECK(maps.body.find("\"stops\"") != std::string::npos);
}

TEST_CASE("presets and the data contributors are served once logged in") {
    WebFixture fixture;
    const std::uint16_t port = fixture.web->port();
    CHECK(get(port, "/api/presets").status == 401);
    CHECK(get(port, "/api/contributors").status == 401);

    const std::string header = std::format("Cookie: {}\r\n", cookieOf(login(port, "secret")));
    const Reply presets = get(port, "/api/presets", header);
    CHECK(presets.status == 200);
    CHECK(presets.body.find("\"FM broadcast\"") != std::string::npos);
    CHECK(presets.body.find("\"segments\"") != std::string::npos);

    const Reply contributors = get(port, "/api/contributors", header);
    CHECK(contributors.status == 200);
    CHECK(contributors.body.find("\"generation\"") != std::string::npos);
    CHECK(contributors.body.find("\"contributors\"") != std::string::npos);
}

TEST_CASE("only a page from this server can open the WebSocket, and only logged in") {
    WebFixture fixture;
    const std::uint16_t port = fixture.web->port();
    const std::string cookie = cookieOf(login(port, "secret"));

    Reply refusal;
    CHECK_FALSE(BrowserClient::open(port, "", originOf(port), &refusal).has_value());
    CHECK(refusal.status == 401);
    CHECK_FALSE(BrowserClient::open(port, cookie, "http://evil.example", &refusal).has_value());
    CHECK(refusal.status == 403);
    CHECK(BrowserClient::open(port, cookie, originOf(port)).has_value());
}

TEST_CASE("a browser on the WebSocket is welcomed with the state and gets frames") {
    WebFixture fixture;
    const std::uint16_t port = fixture.web->port();
    auto browser = BrowserClient::open(port, cookieOf(login(port, "secret")), originOf(port));
    REQUIRE(browser.has_value());
    browser->hello("Firefox on test");

    auto welcome = browser->expect(remote::msg::kWelcome);
    REQUIRE(welcome.has_value());
    CHECK(remote::Welcome::from(welcome->body).serverName == "bench");
    REQUIRE(browser->expect(remote::msg::kState).has_value());
    CHECK(remote::hashAt(browser->sections(), remote::section::kDevice).getBool("present"));
    const auto clients =
        remote::decodeClients(remote::hashAt(browser->sections(), remote::section::kClients));
    REQUIRE(clients.size() == 1);
    CHECK(clients.front().name == "Firefox on test");
    CHECK(clients.front().kind == remote::client::kWeb);

    const std::uint64_t seq = browser->command(remote::op::kStart);
    REQUIRE(browser->expect(remote::msg::kFrame).has_value());
    CHECK(browser->tiles() > 0);
    browser->pump(200ms);
    CHECK(std::ranges::any_of(browser->replies(),
                              [seq](const remote::Reply& r) { return r.seq == seq && r.ok; }));
}

TEST_CASE("a WebSocket that does not open with a hello is turned away") {
    WebFixture fixture;
    const std::uint16_t port = fixture.web->port();
    auto browser = BrowserClient::open(port, cookieOf(login(port, "secret")), originOf(port));
    REQUIRE(browser.has_value());
    std::vector<std::byte> out;
    sweeps::encodeStreamHeader(out, sweeps::StreamHeader{});
    remote::appendMessage(out, remote::msg::kPing, remote::Ping{.id = 1}.toMetadata());
    browser->send(out);
    auto refused = browser->expect(remote::msg::kRefused);
    REQUIRE(refused.has_value());
    CHECK(remote::Refused::from(refused->body).reason == remote::refusal::kProtocol);
    CHECK_FALSE(fixture.server->clientConnected());
}

TEST_CASE("recordings download over HTTP, by a name the server knows") {
    WebFixture fixture;
    const std::uint16_t port = fixture.web->port();
    const std::string header = std::format("Cookie: {}\r\n", cookieOf(login(port, "secret")));
    {
        std::ofstream file(fixture.root() / "sessions" / "server-1.sweeps", std::ios::binary);
        file << "not really a session";
        std::ofstream secret(fixture.root() / "secret.sweeps");
        secret << "outside";
    }
    const Reply got = get(port, "/api/recordings/server-1.sweeps", header);
    CHECK(got.status == 200);
    CHECK(got.body == "not really a session");
    CHECK(got.header("content-disposition") == "attachment; filename=\"server-1.sweeps\"");
    CHECK(get(port, "/api/recordings/missing.sweeps", header).status == 404);
    CHECK(get(port, "/api/recordings/..%2fsecret.sweeps", header).status == 404);
    CHECK(get(port, "/api/recordings/.hidden.sweeps", header).status == 404);
}

TEST_CASE("a browser and a desktop share a server, and either can take control") {
    WebFixture fixture("secret", true);
    const std::uint16_t port = fixture.web->port();

    // The desktop first, over the Noise channel.
    auto socket = net::TcpSocket::connect("127.0.0.1", fixture.server->port(), 2000ms);
    REQUIRE(socket.has_value());
    auto channel =
        remote::openClientChannel(std::move(*socket), "secret",
                                  remote::Hello{.protocolVersion = remote::kProtocolVersion,
                                                .software = "test",
                                                .kind = std::string(remote::client::kDesktop),
                                                .name = "desk"},
                                  Clock::now() + 3s);
    REQUIRE(channel.has_value());
    REQUIRE(eventually([&] { return fixture.server->clients().size() == 1; }));

    auto browser = BrowserClient::open(port, cookieOf(login(port, "secret")), originOf(port));
    REQUIRE(browser.has_value());
    browser->hello("phone");
    REQUIRE(browser->expect(remote::msg::kWelcome).has_value());
    REQUIRE(browser->expect(remote::msg::kState).has_value());
    const auto control = [&] {
        return remote::ControlState::from(
            remote::hashAt(browser->sections(), remote::section::kControl));
    };
    CHECK_FALSE(control().you);
    CHECK(control().controller == "desk");

    const std::uint64_t refused = browser->command(remote::op::kStart);
    browser->pump(300ms);
    const auto reply = std::ranges::find(browser->replies(), refused, &remote::Reply::seq);
    REQUIRE(reply != browser->replies().end());
    CHECK_FALSE(reply->ok);
    CHECK_FALSE(fixture.instrument->running());

    browser->command(remote::op::kTakeControl);
    browser->pump(300ms);
    CHECK(control().you);
    const auto clients = fixture.server->clients();
    REQUIRE(clients.size() == 2);
    CHECK(clients.front().name == "phone");
    CHECK(clients.front().kind == remote::client::kWeb);

    browser->command(remote::op::kStart);
    CHECK(browser->expect(remote::msg::kFrame).has_value());
    CHECK(fixture.instrument->running());
}
