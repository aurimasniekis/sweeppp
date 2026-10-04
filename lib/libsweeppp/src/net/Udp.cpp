// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/net/Udp.hpp"

#include "net/Native.hpp"

#if defined(_WIN32)
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif

#include <cstring>
#include <memory>
#include <utility>

namespace sweeppp::net {
namespace {

using namespace detail;

constexpr std::size_t kMaxDatagram = 9000;

int familyOf(IpFamily family) noexcept {
    return family == IpFamily::V4 ? AF_INET : AF_INET6;
}

/// A numeric address, scope and all, as the sockaddr it names.
Result<sockaddr_storage> numericAddress(const std::string& address, std::uint16_t port,
                                        IpFamily family, SockLen& length) {
    addrinfo hints{};
    hints.ai_family = familyOf(family);
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo* found = nullptr;
    const std::string portText = std::to_string(port);
    if (getaddrinfo(address.c_str(), portText.c_str(), &hints, &found) != 0 || found == nullptr) {
        return fail<sockaddr_storage>(ErrorCode::InvalidArgument, "'{}' is not an address",
                                      address);
    }
    sockaddr_storage storage{};
    std::memcpy(&storage, found->ai_addr, found->ai_addrlen);
    length = static_cast<SockLen>(found->ai_addrlen);
    freeaddrinfo(found);
    return storage;
}

#if defined(_WIN32)
std::string narrow(const wchar_t* wide) {
    if (wide == nullptr) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(bytes - 1, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}
#endif

std::string numericHost(const sockaddr* address, SockLen length) {
    std::array<char, NI_MAXHOST> host{};
    if (getnameinfo(address, length, host.data(), static_cast<int>(host.size()), nullptr, 0,
                    NI_NUMERICHOST) != 0) {
        return {};
    }
    return host.data();
}

} // namespace

bool parseIpv4(const std::string& address, std::array<std::uint8_t, 4>& out) {
    return inet_pton(AF_INET, address.c_str(), out.data()) == 1;
}

bool parseIpv6(const std::string& address, std::array<std::uint8_t, 16>& out) {
    // A scope says which interface, not what the address is.
    const std::string bare = address.substr(0, address.find('%'));
    return inet_pton(AF_INET6, bare.c_str(), out.data()) == 1;
}

std::vector<NetworkInterface> networkInterfaces() {
    std::vector<NetworkInterface> found;
    const auto entry = [&found](const std::string& name, std::uint32_t index) -> NetworkInterface& {
        const auto match = std::ranges::find(found, name, &NetworkInterface::name);
        if (match != found.end()) {
            return *match;
        }
        return found.emplace_back(NetworkInterface{.name = name, .index = index});
    };

#if defined(_WIN32)
    if (!ensureStarted()) {
        return found;
    }
    ULONG size = 16 * 1024;
    std::vector<std::byte> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        result = GetAdaptersAddresses(
            AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result != NO_ERROR) {
        return found;
    }
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp ||
            (adapter->Flags & IP_ADAPTER_NO_MULTICAST) != 0) {
            continue;
        }
        NetworkInterface& iface =
            entry(narrow(adapter->FriendlyName),
                  adapter->Ipv6IfIndex != 0 ? adapter->Ipv6IfIndex : adapter->IfIndex);
        iface.loopback = adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            const sockaddr* address = unicast->Address.lpSockaddr;
            std::string text = numericHost(address, unicast->Address.iSockaddrLength);
            if (const std::size_t percent = text.find('%'); percent != std::string::npos) {
                text.resize(percent);
            }
            (address->sa_family == AF_INET ? iface.ipv4 : iface.ipv6).push_back(std::move(text));
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) {
        return found;
    }
    const std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> guard(list, &freeifaddrs);
    for (const ifaddrs* item = list; item != nullptr; item = item->ifa_next) {
        if (item->ifa_addr == nullptr || (item->ifa_flags & IFF_UP) == 0) {
            continue;
        }
        const bool loopback = (item->ifa_flags & IFF_LOOPBACK) != 0;
        // Loopback is kept whatever it claims: Linux does not mark it
        // multicast-capable, and a server and a client on one machine still
        // find each other through it.
        if (!loopback && (item->ifa_flags & IFF_MULTICAST) == 0) {
            continue;
        }
        const int family = item->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) {
            continue;
        }
        NetworkInterface& iface = entry(item->ifa_name, if_nametoindex(item->ifa_name));
        iface.loopback = loopback;
        const SockLen length = family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
        std::string text = numericHost(item->ifa_addr, length);
        if (const std::size_t percent = text.find('%'); percent != std::string::npos) {
            text.resize(percent);
        }
        (family == AF_INET ? iface.ipv4 : iface.ipv6).push_back(std::move(text));
    }
#endif

    std::ranges::stable_partition(found,
                                  [](const NetworkInterface& iface) { return !iface.loopback; });
    return found;
}

// ------------------------------------------------------------------ UdpSocket

UdpSocket::~UdpSocket() {
    close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : m_handle(std::exchange(other.m_handle, kInvalidSocket)), m_family(other.m_family) {
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        m_handle = std::exchange(other.m_handle, kInvalidSocket);
        m_family = other.m_family;
    }
    return *this;
}

Result<UdpSocket> UdpSocket::open(IpFamily family, std::uint16_t port,
                                  std::optional<std::string> group) {
    if (auto started = ensureStarted(); !started) {
        return std::unexpected(std::move(started).error());
    }
    UdpSocket socket;
    socket.m_family = family;
    socket.m_handle =
        static_cast<NativeSocket>(::socket(familyOf(family), SOCK_DGRAM, IPPROTO_UDP));
    if (!socket.valid()) {
        return fail<UdpSocket>(ErrorCode::IoError, "cannot create a socket: {}",
                               describe(lastError()));
    }

    // Shared: the system's own responder is usually on the mDNS port already.
    (void)setFlag(socket.m_handle, SOL_SOCKET, SO_REUSEADDR, true, "SO_REUSEADDR");
#if defined(SO_REUSEPORT)
    (void)setFlag(socket.m_handle, SOL_SOCKET, SO_REUSEPORT, true, "SO_REUSEPORT");
#endif
    if (family == IpFamily::V6) {
        (void)setFlag(socket.m_handle, IPPROTO_IPV6, IPV6_V6ONLY, true, "IPV6_V6ONLY");
    }

    sockaddr_storage bound{};
    SockLen length = 0;
    if (family == IpFamily::V4) {
        auto& v4 = reinterpret_cast<sockaddr_in&>(bound);
        v4.sin_family = AF_INET;
        v4.sin_port = htons(port);
        v4.sin_addr.s_addr = htonl(INADDR_ANY);
        length = sizeof(sockaddr_in);
    } else {
        auto& v6 = reinterpret_cast<sockaddr_in6&>(bound);
        v6.sin6_family = AF_INET6;
        v6.sin6_port = htons(port);
        v6.sin6_addr = in6addr_any;
        length = sizeof(sockaddr_in6);
    }
    if (::bind(native(socket.m_handle), reinterpret_cast<const sockaddr*>(&bound), length) != 0) {
        return fail<UdpSocket>(ErrorCode::Unavailable, "cannot bind UDP port {}: {}", port,
                               describe(lastError()));
    }

    // mDNS asks for a hop limit of 255, and its own packets looped back so a
    // server and a client on one machine hear each other.
    const int hops = 255;
    const int loop = 1;
    if (family == IpFamily::V4) {
        const auto ttl = static_cast<unsigned char>(hops);
        setsockopt(native(socket.m_handle), IPPROTO_IP, IP_MULTICAST_TTL,
                   reinterpret_cast<const char*>(&ttl), sizeof ttl);
        const auto loopByte = static_cast<unsigned char>(loop);
        setsockopt(native(socket.m_handle), IPPROTO_IP, IP_MULTICAST_LOOP,
                   reinterpret_cast<const char*>(&loopByte), sizeof loopByte);
    } else {
        setsockopt(native(socket.m_handle), IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
                   reinterpret_cast<const char*>(&hops), sizeof hops);
        const auto loopFlag = static_cast<unsigned int>(loop);
        setsockopt(native(socket.m_handle), IPPROTO_IPV6, IPV6_MULTICAST_LOOP,
                   reinterpret_cast<const char*>(&loopFlag), sizeof loopFlag);
    }

    if (!group) {
        return socket;
    }

    // Every interface, so a cable to a laptop is heard as well as the LAN.
    // One that refuses -- already joined, or no address of this family -- is
    // simply left out.
    bool joined = false;
    for (const NetworkInterface& iface : networkInterfaces()) {
        if (family == IpFamily::V4) {
            for (const std::string& address : iface.ipv4) {
                ip_mreq request{};
                inet_pton(AF_INET, group->c_str(), &request.imr_multiaddr);
                inet_pton(AF_INET, address.c_str(), &request.imr_interface);
                joined |= setsockopt(native(socket.m_handle), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                                     reinterpret_cast<const char*>(&request), sizeof request) == 0;
            }
        } else if (!iface.ipv6.empty()) {
            ipv6_mreq request{};
            inet_pton(AF_INET6, group->c_str(), &request.ipv6mr_multiaddr);
            request.ipv6mr_interface = iface.index;
            joined |= setsockopt(native(socket.m_handle), IPPROTO_IPV6, IPV6_JOIN_GROUP,
                                 reinterpret_cast<const char*>(&request), sizeof request) == 0;
        }
    }
    if (!joined) {
        return fail<UdpSocket>(ErrorCode::Unavailable, "no interface could join {}", *group);
    }
    return socket;
}

Status UdpSocket::sendTo(std::span<const std::uint8_t> data, const std::string& address,
                         std::uint16_t port) {
    SockLen length = 0;
    auto target = numericAddress(address, port, m_family, length);
    if (!target) {
        return std::unexpected(std::move(target).error());
    }
    const auto sent = ::sendto(native(m_handle), reinterpret_cast<const char*>(data.data()),
#if defined(_WIN32)
                               static_cast<int>(data.size()),
#else
                               data.size(),
#endif
                               0, reinterpret_cast<const sockaddr*>(&*target), length);
    if (sent < 0) {
        return fail(ErrorCode::IoError, "cannot send to {}: {}", address, describe(lastError()));
    }
    return ok();
}

Status UdpSocket::sendToGroup(std::span<const std::uint8_t> data, const std::string& group,
                              std::uint16_t port, const NetworkInterface& through) {
    if (m_family == IpFamily::V4) {
        if (through.ipv4.empty()) {
            return fail(ErrorCode::Unavailable, "{} has no IPv4 address", through.name);
        }
        in_addr address{};
        inet_pton(AF_INET, through.ipv4.front().c_str(), &address);
        setsockopt(native(m_handle), IPPROTO_IP, IP_MULTICAST_IF,
                   reinterpret_cast<const char*>(&address), sizeof address);
    } else {
        const auto index = static_cast<unsigned int>(through.index);
        setsockopt(native(m_handle), IPPROTO_IPV6, IPV6_MULTICAST_IF,
                   reinterpret_cast<const char*>(&index), sizeof index);
    }
    return sendTo(data, group, port);
}

Result<std::optional<Datagram>> UdpSocket::receive(std::chrono::milliseconds timeout) {
    using Received = std::optional<Datagram>;
    auto ready = waitFor(m_handle, false, timeout);
    if (!ready) {
        return std::unexpected(std::move(ready).error());
    }
    if (!*ready) {
        return Received{};
    }

    Datagram datagram;
    datagram.bytes.resize(kMaxDatagram);
    sockaddr_storage source{};
    SockLen length = sizeof source;
    const auto got = ::recvfrom(native(m_handle), reinterpret_cast<char*>(datagram.bytes.data()),
#if defined(_WIN32)
                                static_cast<int>(datagram.bytes.size()),
#else
                                datagram.bytes.size(),
#endif
                                0, reinterpret_cast<sockaddr*>(&source), &length);
    if (got < 0) {
        const int code = lastError();
        if (interrupted(code)) {
            return Received{};
        }
        return fail<Received>(ErrorCode::IoError, "cannot receive: {}", describe(code));
    }
    datagram.bytes.resize(static_cast<std::size_t>(got));
    datagram.sourceAddress = numericHost(reinterpret_cast<const sockaddr*>(&source), length);
    datagram.sourcePort = ntohs(source.ss_family == AF_INET
                                    ? reinterpret_cast<const sockaddr_in&>(source).sin_port
                                    : reinterpret_cast<const sockaddr_in6&>(source).sin6_port);
    return Received{std::move(datagram)};
}

void UdpSocket::close() noexcept {
    if (valid()) {
        closeHandle(std::exchange(m_handle, kInvalidSocket));
    }
}

} // namespace sweeppp::net
