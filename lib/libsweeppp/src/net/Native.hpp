// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// winsock2.h before anything that reaches windows.h, which would otherwise
// pull in the original winsock.h and collide with it.
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "sweeppp/core/Result.hpp"
#include "sweeppp/net/Socket.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

/// What the TCP and UDP sockets share: the platform's types and calls, each
/// spelled once for POSIX and once for Winsock.
namespace sweeppp::net::detail {

#if defined(_WIN32)
using SockLen = int;
using Native = SOCKET;
constexpr int kShutdownBoth = SD_BOTH;
#else
using SockLen = socklen_t;
using Native = int;
constexpr int kShutdownBoth = SHUT_RDWR;
#endif

inline Native native(NativeSocket handle) noexcept {
    return static_cast<Native>(handle);
}

inline int lastError() noexcept {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

inline bool interrupted(int code) noexcept {
#if defined(_WIN32)
    return code == WSAEINTR;
#else
    return code == EINTR;
#endif
}

inline std::string describe(int code) {
    return std::system_category().message(code);
}

inline Status ensureStarted() {
#if defined(_WIN32)
    static const int started = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data);
    }();
    if (started != 0) {
        return fail(ErrorCode::IoError, "Winsock did not start: {}", describe(started));
    }
#endif
    return ok();
}

inline void closeHandle(NativeSocket handle) noexcept {
#if defined(_WIN32)
    closesocket(native(handle));
#else
    ::close(native(handle));
#endif
}

inline Status setFlag(NativeSocket handle, int level, int option, bool enabled,
                      std::string_view what) {
    const int value = enabled ? 1 : 0;
    if (setsockopt(native(handle), level, option, reinterpret_cast<const char*>(&value),
                   sizeof value) != 0) {
        return fail(ErrorCode::IoError, "cannot set {}: {}", what, describe(lastError()));
    }
    return ok();
}

/// Waits for `handle` to become readable or, with `forWrite`, writable.
///
/// select on Windows, where WSAPoll does not report a connect that failed;
/// poll elsewhere, where select cannot see a descriptor past FD_SETSIZE.
inline Result<bool> waitFor(NativeSocket handle, bool forWrite, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const auto remaining = std::max(std::chrono::milliseconds(0),
                                        std::chrono::duration_cast<std::chrono::milliseconds>(
                                            deadline - std::chrono::steady_clock::now()));
#if defined(_WIN32)
        fd_set ready;
        fd_set failed;
        FD_ZERO(&ready);
        FD_ZERO(&failed);
        FD_SET(native(handle), &ready);
        FD_SET(native(handle), &failed);
        timeval wait{};
        wait.tv_sec = static_cast<long>(remaining.count() / 1000);
        wait.tv_usec = static_cast<long>((remaining.count() % 1000) * 1000);
        const int result =
            select(0, forWrite ? nullptr : &ready, forWrite ? &ready : nullptr, &failed, &wait);
#else
        pollfd entry{};
        entry.fd = native(handle);
        entry.events = forWrite ? POLLOUT : POLLIN;
        const int result = ::poll(&entry, 1, static_cast<int>(remaining.count()));
#endif
        if (result > 0) {
            return true;
        }
        if (result == 0) {
            return false;
        }
        const int code = lastError();
        if (!interrupted(code)) {
            return fail<bool>(ErrorCode::IoError, "wait on socket failed: {}", describe(code));
        }
    }
}

inline std::string formatAddress(const sockaddr_storage& address) {
    std::array<char, INET6_ADDRSTRLEN> text{};
    if (address.ss_family == AF_INET) {
        const auto& v4 = reinterpret_cast<const sockaddr_in&>(address);
        if (inet_ntop(AF_INET, &v4.sin_addr, text.data(), text.size()) == nullptr) {
            return {};
        }
        return std::format("{}:{}", text.data(), ntohs(v4.sin_port));
    }
    if (address.ss_family == AF_INET6) {
        const auto& v6 = reinterpret_cast<const sockaddr_in6&>(address);
        if (inet_ntop(AF_INET6, &v6.sin6_addr, text.data(), text.size()) == nullptr) {
            return {};
        }
        return std::format("[{}]:{}", text.data(), ntohs(v6.sin6_port));
    }
    return {};
}

} // namespace sweeppp::net::detail
