// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <cstdint>
#include <doctest/doctest.h>
#include <random>
#include <sweeppp/net/Udp.hpp>
#include <sweeppp/remote/Mdns.hpp>
#include <sweeppp/remote/RemoteInstrument.hpp>
#include <thread>
#include <vector>

using namespace sweeppp;
using namespace sweeppp::remote;
using namespace std::chrono_literals;

namespace {

mdns::Advert sampleAdvert() {
    return mdns::Advert{.instance = "Sweep++ on pi",
                        .host = "pi",
                        .port = 7400,
                        .device = "HackRF One",
                        .authRequired = true,
                        .busy = false,
                        .shared = true,
                        .ipv4 = {"192.168.1.20", "169.254.3.7"},
                        .ipv6 = {"fe80::1"}};
}

} // namespace

TEST_CASE("an answer describes the server it was made for") {
    const std::vector<std::uint8_t> packet = mdns::encodeAnswer(sampleAdvert(), 0x1234, true, 10);
    auto found = mdns::parseAnswer(packet);
    REQUIRE(found.has_value());
    REQUIRE(found->size() == 1);
    const mdns::Found& server = found->front();
    CHECK(server.instance == "Sweep++ on pi");
    CHECK(server.host == "pi");
    CHECK(server.port == 7400);
    CHECK(server.device == "HackRF One");
    CHECK(server.authRequired);
    CHECK_FALSE(server.busy);
    CHECK(server.shared);
    CHECK(server.protocolVersion == kProtocolVersion);
}

TEST_CASE("a query is recognised as ours, and an answer is not taken for one") {
    const std::vector<std::uint8_t> query = mdns::encodeQuery(77);
    auto question = mdns::parseQuery(query);
    REQUIRE(question.has_value());
    CHECK(question->forUs);
    CHECK(question->id == 77);

    auto answer = mdns::parseQuery(mdns::encodeAnswer(sampleAdvert(), 0, false, 120));
    REQUIRE(answer.has_value());
    CHECK_FALSE(answer->forUs);

    // A question for someone else's service.
    std::vector<std::uint8_t> other{0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    for (const std::string_view label : {"_http", "_tcp", "local"}) {
        other.push_back(static_cast<std::uint8_t>(label.size()));
        other.insert(other.end(), label.begin(), label.end());
    }
    other.insert(other.end(), {0, 0, 12, 0, 1});
    auto foreign = mdns::parseQuery(other);
    REQUIRE(foreign.has_value());
    CHECK_FALSE(foreign->forUs);
}

TEST_CASE("hostile DNS packets are refused, never crashed on") {
    SUBCASE("a compression pointer pointing at itself") {
        // Header with one answer, then a name that is only a pointer to
        // offset 12 -- itself.
        const std::vector<std::uint8_t> loop{0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0xC0, 12};
        CHECK_FALSE(mdns::parseAnswer(loop).has_value());
        // The same name as a question.
        const std::vector<std::uint8_t> asked{0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 12};
        CHECK_FALSE(mdns::parseQuery(asked).has_value());
    }

    SUBCASE("counts far beyond the packet") {
        const std::vector<std::uint8_t> counts{0, 0, 0x84, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};
        CHECK_FALSE(mdns::parseAnswer(counts).has_value());
        const std::vector<std::uint8_t> questions{0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
        CHECK_FALSE(mdns::parseQuery(questions).has_value());
    }

    SUBCASE("every truncation and many corruptions of a real answer") {
        const std::vector<std::uint8_t> packet = mdns::encodeAnswer(sampleAdvert(), 1, true, 10);
        for (std::size_t length = 0; length < packet.size(); ++length) {
            (void)mdns::parseAnswer({packet.data(), length});
            (void)mdns::parseQuery({packet.data(), length});
        }
        std::mt19937 random(5353);
        for (int trial = 0; trial < 20000; ++trial) {
            std::vector<std::uint8_t> damaged = packet;
            for (int edit = 0; edit < 4; ++edit) {
                damaged[random() % damaged.size()] = static_cast<std::uint8_t>(random());
            }
            (void)mdns::parseAnswer(damaged);
            (void)mdns::parseQuery(damaged);
        }
    }
    CHECK(true);
}

TEST_CASE("addresses are read with or without a scope") {
    std::array<std::uint8_t, 4> v4{};
    CHECK(net::parseIpv4("169.254.3.7", v4));
    CHECK(v4[0] == 169);
    CHECK(v4[3] == 7);
    CHECK_FALSE(net::parseIpv4("pi.local", v4));

    std::array<std::uint8_t, 16> v6{};
    CHECK(net::parseIpv6("fe80::1%en5", v6));
    CHECK(v6[0] == 0xFE);
    CHECK(v6[15] == 1);

    auto scoped = RemoteEndpoint::parse("[fe80::1%en5]:7400");
    REQUIRE(scoped.has_value());
    CHECK(scoped->host == "fe80::1%en5");
    CHECK(scoped->port == 7400);
    CHECK(scoped->address() == "[fe80::1%en5]:7400");
}

TEST_CASE("a server advertising on this machine is found by a browser") {
    auto advertiser = mdns::Advertiser::start(sampleAdvert());
    if (!advertiser) {
        MESSAGE("skipped: ", advertiser.error().describe());
        return;
    }
    auto browser = mdns::Browser::start();
    REQUIRE(browser.has_value());

    const auto deadline = std::chrono::steady_clock::now() + 4s;
    std::vector<mdns::DiscoveredServer> found;
    while (std::chrono::steady_clock::now() < deadline) {
        found = (*browser)->servers();
        if (std::ranges::any_of(found, [](const mdns::DiscoveredServer& server) {
                return server.found.instance == "Sweep++ on pi";
            })) {
            break;
        }
        std::this_thread::sleep_for(100ms);
    }
    const auto ours = std::ranges::find_if(found, [](const mdns::DiscoveredServer& server) {
        return server.found.instance == "Sweep++ on pi";
    });
    if (ours == found.end()) {
        MESSAGE("skipped: no multicast route on this machine");
        return;
    }
    CHECK(ours->endpoint.port == 7400);
    CHECK_FALSE(ours->endpoint.host.empty());
    CHECK(ours->found.authRequired);

    (*advertiser)->setBusy(true);
    (*browser)->refresh();
    const auto busyDeadline = std::chrono::steady_clock::now() + 3s;
    bool busy = false;
    while (!busy && std::chrono::steady_clock::now() < busyDeadline) {
        for (const mdns::DiscoveredServer& server : (*browser)->servers()) {
            busy |= server.found.instance == "Sweep++ on pi" && server.found.busy;
        }
        std::this_thread::sleep_for(100ms);
    }
    CHECK(busy);
}
