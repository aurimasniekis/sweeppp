// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/net/Socket.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sweeppp::net {

enum class IpFamily : std::uint8_t { V4, V6 };

/// An interface that is up and can carry multicast, with its addresses.
struct NetworkInterface {
    std::string name;
    std::uint32_t index = 0;
    bool loopback = false;
    std::vector<std::string> ipv4; ///< "192.168.1.20", "169.254.3.7"
    std::vector<std::string> ipv6; ///< "fe80::1", without a scope
};

/// A numeric address as its bytes, in network order. False for anything else.
bool parseIpv4(const std::string& address, std::array<std::uint8_t, 4>& out);
bool parseIpv6(const std::string& address, std::array<std::uint8_t, 16>& out);

/// Every interface that is up and multicast-capable, loopback last.
[[nodiscard]] std::vector<NetworkInterface> networkInterfaces();

/// One datagram, with where it came from: an address a reply can go back to,
/// scoped to its interface when it is link-local ("fe80::1%en5").
struct Datagram {
    std::vector<std::uint8_t> bytes;
    std::string sourceAddress;
    std::uint16_t sourcePort = 0;
};

/// A UDP socket of one family, for multicast DNS: on the mDNS port, joined to
/// its group on every interface, or on an ephemeral port to ask from.
class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    /// Bound to `port` -- shared with whatever else listens there, as the
    /// system's own mDNS responder does -- and, when `group` is given, joined
    /// to it on every interface. Port 0 binds an ephemeral one.
    [[nodiscard]] static Result<UdpSocket> open(IpFamily family, std::uint16_t port,
                                                std::optional<std::string> group = std::nullopt);

    [[nodiscard]] bool valid() const noexcept { return m_handle != kInvalidSocket; }
    [[nodiscard]] IpFamily family() const noexcept { return m_family; }

    /// To `address:port`; a link-local address carries its scope.
    Status sendTo(std::span<const std::uint8_t> data, const std::string& address,
                  std::uint16_t port);

    /// To a multicast `group:port` out of one interface: by one of its IPv4
    /// addresses, or by its index for IPv6.
    Status sendToGroup(std::span<const std::uint8_t> data, const std::string& group,
                       std::uint16_t port, const NetworkInterface& through);

    /// The next datagram, or nullopt after `timeout`.
    [[nodiscard]] Result<std::optional<Datagram>> receive(std::chrono::milliseconds timeout);

    void close() noexcept;

private:
    NativeSocket m_handle = kInvalidSocket;
    IpFamily m_family = IpFamily::V4;
};

} // namespace sweeppp::net
