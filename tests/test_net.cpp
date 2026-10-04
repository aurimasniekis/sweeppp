// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <cstdint>
#include <doctest/doctest.h>
#include <future>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sweeppp/crypto/Noise.hpp>
#include <sweeppp/net/SecureChannel.hpp>
#include <sweeppp/net/Socket.hpp>
#include <thread>
#include <vector>

using namespace sweeppp;
using namespace sweeppp::net;
using namespace std::chrono_literals;

namespace {

/// Both ends of one loopback connection.
struct Pair {
    TcpSocket client;
    TcpSocket server;
};

Pair connectedPair() {
    auto listener = TcpListener::listen("127.0.0.1", 0);
    REQUIRE(listener.has_value());
    REQUIRE(listener->port() != 0);

    auto client = TcpSocket::connect("127.0.0.1", listener->port(), 2000ms);
    REQUIRE(client.has_value());
    auto accepted = listener->accept(2000ms);
    REQUIRE(accepted.has_value());
    REQUIRE(accepted->has_value());
    return Pair{std::move(*client), std::move(**accepted)};
}

std::vector<std::uint8_t> receiveExactly(TcpSocket& socket, std::size_t count) {
    std::vector<std::uint8_t> received(count);
    std::size_t filled = 0;
    while (filled < count) {
        auto got = socket.receive(std::span<std::uint8_t>(received).subspan(filled));
        REQUIRE(got.has_value());
        if (*got == 0) {
            break;
        }
        filled += *got;
    }
    received.resize(filled);
    return received;
}

} // namespace

TEST_CASE("bytes cross a loopback connection both ways") {
    Pair pair = connectedPair();
    REQUIRE(pair.client.setNoDelay(true).has_value());
    REQUIRE(pair.client.setKeepAlive(true).has_value());

    const std::array<std::uint8_t, 5> hello{'h', 'e', 'l', 'l', 'o'};
    REQUIRE(pair.client.sendAll(hello).has_value());
    CHECK(receiveExactly(pair.server, hello.size()) ==
          std::vector<std::uint8_t>(hello.begin(), hello.end()));

    const std::array<std::uint8_t, 3> back{1, 2, 3};
    REQUIRE(pair.server.sendAll(back).has_value());
    CHECK(receiveExactly(pair.client, back.size()) ==
          std::vector<std::uint8_t>(back.begin(), back.end()));

    CHECK(pair.server.peerAddress().starts_with("127.0.0.1:"));
}

TEST_CASE("a send larger than the socket buffers arrives whole") {
    Pair pair = connectedPair();
    std::vector<std::uint8_t> payload(4 << 20);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(i ^ (i >> 8));
    }

    auto reader =
        std::async(std::launch::async, [&] { return receiveExactly(pair.server, payload.size()); });
    REQUIRE(pair.client.sendAll(payload).has_value());
    CHECK(reader.get() == payload);
}

TEST_CASE("readability is reported only once something arrives") {
    Pair pair = connectedPair();
    auto idle = pair.server.waitReadable(20ms);
    REQUIRE(idle.has_value());
    CHECK_FALSE(*idle);

    const std::array<std::uint8_t, 1> one{42};
    REQUIRE(pair.client.sendAll(one).has_value());
    auto ready = pair.server.waitReadable(2000ms);
    REQUIRE(ready.has_value());
    CHECK(*ready);
}

TEST_CASE("a closed peer reads as the end of the stream") {
    Pair pair = connectedPair();
    pair.client.close();
    std::array<std::uint8_t, 16> buffer{};
    auto got = pair.server.receive(buffer);
    // A reset is as good as an end here: either way nothing more is coming.
    CHECK((!got.has_value() || *got == 0));
}

TEST_CASE("shutdown wakes a receive blocked on another thread") {
    Pair pair = connectedPair();
    std::promise<void> entered;
    auto reader = std::async(std::launch::async, [&] {
        entered.set_value();
        std::array<std::uint8_t, 16> buffer{};
        return pair.server.receive(buffer);
    });
    entered.get_future().wait();
    std::this_thread::sleep_for(50ms);

    pair.server.shutdown();
    REQUIRE(reader.wait_for(5s) == std::future_status::ready);
    const auto got = reader.get();
    CHECK((!got.has_value() || *got == 0));
}

TEST_CASE("connecting to a port nobody listens on fails promptly") {
    std::uint16_t port = 0;
    {
        auto listener = TcpListener::listen("127.0.0.1", 0);
        REQUIRE(listener.has_value());
        port = listener->port();
    }

    const auto started = std::chrono::steady_clock::now();
    auto client = TcpSocket::connect("127.0.0.1", port, 5000ms);
    REQUIRE_FALSE(client.has_value());
    CHECK(client.error().code() == ErrorCode::Unavailable);
    CHECK(std::chrono::steady_clock::now() - started < 2s);
}

TEST_CASE("an accept with nobody connecting times out empty") {
    auto listener = TcpListener::listen("127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto accepted = listener->accept(20ms);
    REQUIRE(accepted.has_value());
    CHECK_FALSE(accepted->has_value());
}

TEST_CASE("an unresolvable host is reported as such") {
    auto client = TcpSocket::connect("no-such-host.invalid", 7332, 500ms);
    REQUIRE_FALSE(client.has_value());
    CHECK(client.error().code() == ErrorCode::NotFound);
}

TEST_CASE("loopback addresses are told from the rest") {
    CHECK(isLoopbackAddress("127.0.0.1"));
    CHECK(isLoopbackAddress("127.1.2.3"));
    CHECK(isLoopbackAddress("::1"));
    CHECK(isLoopbackAddress("[::1]"));
    CHECK(isLoopbackAddress("::ffff:127.0.0.1"));
    CHECK(isLoopbackAddress("localhost"));
    CHECK(isLoopbackAddress("LocalHost"));

    CHECK_FALSE(isLoopbackAddress("0.0.0.0"));
    CHECK_FALSE(isLoopbackAddress("::"));
    CHECK_FALSE(isLoopbackAddress("192.168.1.20"));
    CHECK_FALSE(isLoopbackAddress("128.0.0.1"));
    CHECK_FALSE(isLoopbackAddress("pi.local"));
    CHECK_FALSE(isLoopbackAddress(""));
}

namespace {

crypto::Key fixedKey(std::uint8_t fill) {
    crypto::Key key{};
    key.fill(fill);
    return key;
}

/// Frames as a SecureChannel would write them, for a peer that writes by hand.
std::vector<std::uint8_t> frameOf(crypto::CipherState& cipher, std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> frame(2);
    REQUIRE(cipher.encrypt({}, data, frame).has_value());
    const std::size_t length = frame.size() - 2;
    frame[0] = static_cast<std::uint8_t>(length & 0xFFU);
    frame[1] = static_cast<std::uint8_t>(length >> 8U);
    return frame;
}

std::vector<std::uint8_t> receiveAll(SecureChannel& channel, std::size_t count) {
    std::vector<std::uint8_t> out(count);
    std::size_t have = 0;
    while (have < count) {
        auto got = channel.receive(std::span<std::uint8_t>(out).subspan(have));
        REQUIRE(got.has_value());
        REQUIRE(*got > 0);
        have += *got;
    }
    return out;
}

} // namespace

TEST_CASE("an encrypted channel carries a stream however it is cut") {
    Pair pair = connectedPair();
    SecureChannel sender(std::move(pair.client), crypto::CipherState(fixedKey(1)),
                         crypto::CipherState(fixedKey(2)));
    SecureChannel receiver(std::move(pair.server), crypto::CipherState(fixedKey(2)),
                           crypto::CipherState(fixedKey(1)));

    // Several frames' worth, so the cut between frames is crossed too.
    std::vector<std::uint8_t> payload(SecureChannel::kMaxFramePlaintext * 3 + 123);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>((i * 7) ^ (i >> 9));
    }
    auto reader =
        std::async(std::launch::async, [&] { return receiveAll(receiver, payload.size()); });
    REQUIRE(sender.sendAll(payload).has_value());
    CHECK(reader.get() == payload);

    const std::array<std::uint8_t, 3> back{9, 8, 7};
    REQUIRE(receiver.sendAll(back).has_value());
    CHECK(receiveAll(sender, back.size()) == std::vector<std::uint8_t>(back.begin(), back.end()));
}

TEST_CASE("an encrypted channel reassembles frames that arrive a byte at a time") {
    Pair pair = connectedPair();
    TcpSocket writer = std::move(pair.client);
    SecureChannel receiver(std::move(pair.server), crypto::CipherState(fixedKey(2)),
                           crypto::CipherState(fixedKey(1)));
    crypto::CipherState cipher(fixedKey(1));

    std::vector<std::uint8_t> wire;
    for (const std::string_view part : {"first ", "second ", "third"}) {
        const auto frame =
            frameOf(cipher, {reinterpret_cast<const std::uint8_t*>(part.data()), part.size()});
        wire.insert(wire.end(), frame.begin(), frame.end());
    }
    auto reader = std::async(std::launch::async, [&] { return receiveAll(receiver, 18); });
    for (const std::uint8_t byte : wire) {
        REQUIRE(writer.sendAll(std::span<const std::uint8_t>(&byte, 1)).has_value());
    }
    const std::vector<std::uint8_t> got = reader.get();
    CHECK(std::string(got.begin(), got.end()) == "first second third");
}

TEST_CASE("a tampered frame ends an encrypted channel") {
    Pair pair = connectedPair();
    TcpSocket writer = std::move(pair.client);
    SecureChannel receiver(std::move(pair.server), crypto::CipherState(fixedKey(2)),
                           crypto::CipherState(fixedKey(1)));
    crypto::CipherState cipher(fixedKey(1));

    const std::array<std::uint8_t, 4> data{1, 2, 3, 4};
    std::vector<std::uint8_t> frame = frameOf(cipher, data);
    frame[4] ^= 0x40;
    std::vector<std::uint8_t> good = frameOf(cipher, data);
    frame.insert(frame.end(), good.begin(), good.end());
    REQUIRE(writer.sendAll(frame).has_value());

    std::array<std::uint8_t, 16> buffer{};
    auto first = receiver.receive(buffer);
    REQUIRE_FALSE(first.has_value());
    CHECK(first.error().code() == ErrorCode::Corrupt);
    CHECK_FALSE(receiver.receive(buffer).has_value());
}
