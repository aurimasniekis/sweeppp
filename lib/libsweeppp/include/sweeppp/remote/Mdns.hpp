// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/remote/RemoteInstrument.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Finding servers on the LAN with multicast DNS service discovery (RFC 6762,
/// RFC 6763): a server answers for `_sweeppp._tcp.local`, a desktop asks.
///
/// Only as much of DNS as that takes. The desktop asks for unicast answers
/// from a port of its own, and takes the address an answer came from as the
/// server's -- which, on a link-local network such as a cable between two
/// machines, carries the interface it must be reached through.
namespace sweeppp::remote::mdns {

inline constexpr std::uint16_t kPort = 5353;
inline constexpr std::string_view kGroupV4 = "224.0.0.251";
inline constexpr std::string_view kGroupV6 = "ff02::fb";

/// What a server says about itself.
struct Advert {
    std::string instance; ///< "Sweep++ on pi"
    std::string host;     ///< "pi", answered as "pi.local"
    std::uint16_t port = 0;
    std::string device; ///< "HackRF One"
    bool authRequired = false;
    bool busy = false;   ///< Somebody controls it
    bool shared = false; ///< Others may watch while somebody does
    std::vector<std::string> ipv4;
    std::vector<std::string> ipv6;
};

/// A server an answer described.
struct Found {
    std::string instance;
    std::string host;
    std::uint16_t port = 0;
    std::string device;
    bool authRequired = false;
    bool busy = false;
    bool shared = false;
    std::uint32_t protocolVersion = 0;
};

/// A question for every Sweep++ server, asking for answers by unicast.
[[nodiscard]] std::vector<std::uint8_t> encodeQuery(std::uint16_t id);

struct Question {
    std::uint16_t id = 0;
    bool forUs = false;
};

/// Whether a packet asks after Sweep++ servers. ProtocolError for one that
/// is not a well-formed DNS question.
[[nodiscard]] Result<Question> parseQuery(std::span<const std::uint8_t> packet);

/// The answer: PTR, SRV and TXT for the instance, A and AAAA for the host.
/// `question` repeats the question for a legacy unicast answer (RFC 6762
/// section 6.7); `ttl` zero is a goodbye.
[[nodiscard]] std::vector<std::uint8_t> encodeAnswer(const Advert& advert, std::uint16_t id,
                                                     bool question, std::uint32_t ttl);

/// The servers an answer describes; nothing when it is about other services.
/// ProtocolError for a malformed packet.
[[nodiscard]] Result<std::vector<Found>> parseAnswer(std::span<const std::uint8_t> packet);

/// Answers questions about one server for as long as it lives, on a thread
/// of its own.
class Advertiser {
public:
    [[nodiscard]] static Result<std::unique_ptr<Advertiser>> start(Advert advert);
    ~Advertiser();

    Advertiser(const Advertiser&) = delete;
    Advertiser& operator=(const Advertiser&) = delete;
    Advertiser(Advertiser&&) = delete;
    Advertiser& operator=(Advertiser&&) = delete;

    /// Whether a client controls the server, as the next answer says.
    void setBusy(bool busy) noexcept { m_busy.store(busy); }

private:
    struct Impl;
    explicit Advertiser(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
    std::atomic<bool> m_busy{false};
};

/// A server on the LAN, as the desktop lists it.
struct DiscoveredServer {
    Found found;
    RemoteEndpoint endpoint; ///< Where the answer came from, and the server's port
    std::uint64_t lastSeenNs = 0;
};

/// Asks for servers every few seconds while it lives, on a thread of its own.
class Browser {
public:
    [[nodiscard]] static Result<std::unique_ptr<Browser>> start();
    ~Browser();

    Browser(const Browser&) = delete;
    Browser& operator=(const Browser&) = delete;
    Browser(Browser&&) = delete;
    Browser& operator=(Browser&&) = delete;

    /// Asks again now rather than at the next interval.
    void refresh() noexcept;

    /// Those heard from recently, by name.
    [[nodiscard]] std::vector<DiscoveredServer> servers() const;

private:
    struct Impl;
    explicit Browser(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace sweeppp::remote::mdns
