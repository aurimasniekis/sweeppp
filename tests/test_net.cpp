// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <cstdint>
#include <doctest/doctest.h>
#include <future>
#include <optional>
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
