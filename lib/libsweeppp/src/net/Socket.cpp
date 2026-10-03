// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/net/Socket.hpp"

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

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <format>
#include <memory>
#include <system_error>
#include <utility>

namespace sweeppp::net {
namespace {

#if defined(_WIN32)
using SockLen = int;
using Native = SOCKET;
constexpr int kShutdownBoth = SD_BOTH;
#else
using SockLen = socklen_t;
using Native = int;
constexpr int kShutdownBoth = SHUT_RDWR;
#endif

// Linux raises SIGPIPE on a send to a closed peer unless told otherwise per
// call; macOS is told once per socket, in `configure()`.
#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

Native native(NativeSocket handle) noexcept {
    return static_cast<Native>(handle);
}

int lastError() noexcept {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool interrupted(int code) noexcept {
#if defined(_WIN32)
    return code == WSAEINTR;
#else
    return code == EINTR;
#endif
}

std::string describe(int code) {
    return std::system_category().message(code);
}

/// The code for what a failed connect or send means to the caller: a refusal
/// is worth retrying later, a timeout is its own thing, the rest is I/O.
ErrorCode classify(int code) noexcept {
#if defined(_WIN32)
    if (code == WSAECONNREFUSED) {
        return ErrorCode::Unavailable;
    }
    if (code == WSAETIMEDOUT) {
        return ErrorCode::TimedOut;
    }
#else
    if (code == ECONNREFUSED) {
        return ErrorCode::Unavailable;
    }
    if (code == ETIMEDOUT) {
        return ErrorCode::TimedOut;
    }
#endif
    return ErrorCode::IoError;
}

Status ensureStarted() {
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

void closeHandle(NativeSocket handle) noexcept {
#if defined(_WIN32)
    closesocket(native(handle));
#else
    ::close(native(handle));
#endif
}

bool setBlocking(NativeSocket handle, bool blocking) noexcept {
#if defined(_WIN32)
    u_long mode = blocking ? 0 : 1;
    return ioctlsocket(native(handle), FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(native(handle), F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(native(handle), F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK) == 0;
#endif
}

void configure(NativeSocket handle) noexcept {
#if defined(SO_NOSIGPIPE)
    const int on = 1;
    setsockopt(native(handle), SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#else
    (void)handle;
#endif
}

Status setFlag(NativeSocket handle, int level, int option, bool enabled, std::string_view what) {
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
Result<bool> waitFor(NativeSocket handle, bool forWrite, std::chrono::milliseconds timeout) {
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

std::string formatAddress(const sockaddr_storage& address) {
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

using AddressList = std::unique_ptr<addrinfo, decltype(&freeaddrinfo)>;

Result<AddressList> resolve(std::string_view host, std::uint16_t port, bool passive) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICSERV | (passive ? AI_PASSIVE : 0);

    const std::string hostText(host);
    const std::string portText = std::to_string(port);
    addrinfo* found = nullptr;
    const int result = getaddrinfo(hostText.empty() ? nullptr : hostText.c_str(), portText.c_str(),
                                   &hints, &found);
    if (result != 0 || found == nullptr) {
        return fail<AddressList>(ErrorCode::NotFound, "cannot resolve '{}': {}", host,
                                 gai_strerror(result));
    }
    return AddressList(found, &freeaddrinfo);
}

} // namespace

// ---------------------------------------------------------------- TcpSocket

TcpSocket::~TcpSocket() {
    close();
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : m_handle(std::exchange(other.m_handle, kInvalidSocket)) {
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        m_handle = std::exchange(other.m_handle, kInvalidSocket);
    }
    return *this;
}

Result<TcpSocket> TcpSocket::connect(std::string_view host, std::uint16_t port,
                                     std::chrono::milliseconds timeout) {
    if (Status started = ensureStarted(); !started) {
        return std::unexpected(std::move(started).error());
    }
    Result<AddressList> addresses = resolve(host, port, false);
    if (!addresses) {
        return std::unexpected(std::move(addresses).error());
    }

    Error last{ErrorCode::Unavailable,
               std::format("{}:{} has no address to connect to", host, port)};
    for (const addrinfo* entry = addresses->get(); entry != nullptr; entry = entry->ai_next) {
        TcpSocket socket(static_cast<NativeSocket>(
            ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol)));
        if (!socket.valid()) {
            last = Error{ErrorCode::IoError,
                         std::format("cannot create a socket: {}", describe(lastError()))};
            continue;
        }
        configure(socket.m_handle);
        setBlocking(socket.m_handle, false);

        if (::connect(native(socket.m_handle), entry->ai_addr,
                      static_cast<SockLen>(entry->ai_addrlen)) != 0) {
            const int code = lastError();
#if defined(_WIN32)
            const bool pending = code == WSAEWOULDBLOCK;
#else
            const bool pending = code == EINPROGRESS;
#endif
            if (!pending) {
                last = Error{classify(code), std::format("{}:{}: {}", host, port, describe(code))};
                continue;
            }

            const Result<bool> ready = waitFor(socket.m_handle, true, timeout);
            if (!ready) {
                last = ready.error();
                continue;
            }
            if (!*ready) {
                last = Error{ErrorCode::TimedOut, std::format("{}:{} did not answer within {} ms",
                                                              host, port, timeout.count())};
                continue;
            }

            int pendingError = 0;
            SockLen length = sizeof pendingError;
            getsockopt(native(socket.m_handle), SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&pendingError), &length);
            if (pendingError != 0) {
                last = Error{classify(pendingError),
                             std::format("{}:{}: {}", host, port, describe(pendingError))};
                continue;
            }
        }

        setBlocking(socket.m_handle, true);
        return socket;
    }
    return std::unexpected(std::move(last));
}

Status TcpSocket::sendAll(std::span<const std::uint8_t> data) {
    if (!valid()) {
        return fail(ErrorCode::IoError, "send on a closed socket");
    }
    // Winsock takes an int length; a chunk well inside it serves both.
    constexpr std::size_t kChunk = std::size_t{1} << 30;
    std::size_t sent = 0;
    while (sent < data.size()) {
        const std::size_t chunk = std::min(data.size() - sent, kChunk);
#if defined(_WIN32)
        const auto result =
            ::send(native(m_handle), reinterpret_cast<const char*>(data.data() + sent),
                   static_cast<int>(chunk), kSendFlags);
#else
        const auto result = ::send(native(m_handle), data.data() + sent, chunk, kSendFlags);
#endif
        if (result < 0) {
            const int code = lastError();
            if (interrupted(code)) {
                continue;
            }
            return fail(classify(code), "send failed: {}", describe(code));
        }
        if (result == 0) {
            return fail(ErrorCode::IoError, "send made no progress");
        }
        sent += static_cast<std::size_t>(result);
    }
    return ok();
}

Result<std::size_t> TcpSocket::receive(std::span<std::uint8_t> buffer) {
    if (!valid()) {
        return fail<std::size_t>(ErrorCode::IoError, "receive on a closed socket");
    }
    if (buffer.empty()) {
        return std::size_t{0};
    }
    const std::size_t want = std::min(buffer.size(), std::size_t{1} << 30);
    while (true) {
#if defined(_WIN32)
        const auto result = ::recv(native(m_handle), reinterpret_cast<char*>(buffer.data()),
                                   static_cast<int>(want), 0);
#else
        const auto result = ::recv(native(m_handle), buffer.data(), want, 0);
#endif
        if (result >= 0) {
            return static_cast<std::size_t>(result);
        }
        const int code = lastError();
        if (interrupted(code)) {
            continue;
        }
#if defined(_WIN32)
        // Winsock reports a receive after our own shutdown as an error where
        // POSIX reports the end of the stream.
        if (code == WSAESHUTDOWN) {
            return std::size_t{0};
        }
#endif
        return fail<std::size_t>(classify(code), "receive failed: {}", describe(code));
    }
}

Result<bool> TcpSocket::waitReadable(std::chrono::milliseconds timeout) {
    if (!valid()) {
        return fail<bool>(ErrorCode::IoError, "wait on a closed socket");
    }
    return waitFor(m_handle, false, timeout);
}

void TcpSocket::shutdown() noexcept {
    if (valid()) {
        ::shutdown(native(m_handle), kShutdownBoth);
    }
}

void TcpSocket::close() noexcept {
    if (valid()) {
        closeHandle(std::exchange(m_handle, kInvalidSocket));
    }
}

Status TcpSocket::setNoDelay(bool enabled) {
    return setFlag(m_handle, IPPROTO_TCP, TCP_NODELAY, enabled, "TCP_NODELAY");
}

Status TcpSocket::setKeepAlive(bool enabled) {
    return setFlag(m_handle, SOL_SOCKET, SO_KEEPALIVE, enabled, "SO_KEEPALIVE");
}

std::string TcpSocket::peerAddress() const {
    if (!valid()) {
        return {};
    }
    sockaddr_storage address{};
    SockLen length = sizeof address;
    if (getpeername(native(m_handle), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return {};
    }
    return formatAddress(address);
}

// -------------------------------------------------------------- TcpListener

TcpListener::~TcpListener() {
    close();
}

TcpListener::TcpListener(TcpListener&& other) noexcept
    : m_handle(std::exchange(other.m_handle, kInvalidSocket)),
      m_port(std::exchange(other.m_port, std::uint16_t{0})) {
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        close();
        m_handle = std::exchange(other.m_handle, kInvalidSocket);
        m_port = std::exchange(other.m_port, std::uint16_t{0});
    }
    return *this;
}

Result<TcpListener> TcpListener::listen(std::string_view address, std::uint16_t port, int backlog) {
    if (Status started = ensureStarted(); !started) {
        return std::unexpected(std::move(started).error());
    }
    Result<AddressList> addresses = resolve(address, port, true);
    if (!addresses) {
        return std::unexpected(std::move(addresses).error());
    }

    Error last{ErrorCode::Unavailable, std::format("'{}' has no address to listen on", address)};
    for (const addrinfo* entry = addresses->get(); entry != nullptr; entry = entry->ai_next) {
        TcpListener listener;
        listener.m_handle = static_cast<NativeSocket>(
            ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol));
        if (!listener.valid()) {
            last = Error{ErrorCode::IoError,
                         std::format("cannot create a socket: {}", describe(lastError()))};
            continue;
        }

#if defined(_WIN32)
        // SO_REUSEADDR on Windows lets a second process steal the port; this
        // is the option that means what SO_REUSEADDR means elsewhere.
        (void)setFlag(listener.m_handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, true, "");
#else
        // A restarted server otherwise waits out TIME_WAIT on its own port.
        (void)setFlag(listener.m_handle, SOL_SOCKET, SO_REUSEADDR, true, "");
#endif
        if (entry->ai_family == AF_INET6) {
            (void)setFlag(listener.m_handle, IPPROTO_IPV6, IPV6_V6ONLY, false, "");
        }

        if (::bind(native(listener.m_handle), entry->ai_addr,
                   static_cast<SockLen>(entry->ai_addrlen)) != 0) {
            const int code = lastError();
            last = Error{ErrorCode::Unavailable,
                         std::format("cannot listen on {}:{}: {}", address, port, describe(code))};
            continue;
        }
        if (::listen(native(listener.m_handle), backlog) != 0) {
            const int code = lastError();
            last = Error{ErrorCode::IoError,
                         std::format("cannot listen on {}:{}: {}", address, port, describe(code))};
            continue;
        }

        sockaddr_storage bound{};
        SockLen length = sizeof bound;
        if (getsockname(native(listener.m_handle), reinterpret_cast<sockaddr*>(&bound), &length) ==
            0) {
            if (bound.ss_family == AF_INET) {
                listener.m_port = ntohs(reinterpret_cast<const sockaddr_in&>(bound).sin_port);
            } else if (bound.ss_family == AF_INET6) {
                listener.m_port = ntohs(reinterpret_cast<const sockaddr_in6&>(bound).sin6_port);
            }
        }
        return listener;
    }
    return std::unexpected(std::move(last));
}

Result<std::optional<TcpSocket>> TcpListener::accept(std::chrono::milliseconds timeout) {
    using Accepted = std::optional<TcpSocket>;
    if (!valid()) {
        return fail<Accepted>(ErrorCode::IoError, "accept on a closed listener");
    }
    Result<bool> ready = waitFor(m_handle, false, timeout);
    if (!ready) {
        return std::unexpected(std::move(ready).error());
    }
    if (!*ready) {
        return Accepted{};
    }

    const auto handle = static_cast<NativeSocket>(::accept(native(m_handle), nullptr, nullptr));
    if (handle == kInvalidSocket) {
        const int code = lastError();
        // The peer gave up between the wake and the accept: nothing to hand
        // over, and nothing wrong with the listener.
#if defined(_WIN32)
        if (code == WSAECONNRESET || code == WSAEWOULDBLOCK || interrupted(code)) {
#else
        if (code == ECONNABORTED || code == EAGAIN || code == EWOULDBLOCK || interrupted(code)) {
#endif
            return Accepted{};
        }
        return fail<Accepted>(ErrorCode::IoError, "accept failed: {}", describe(code));
    }
    configure(handle);
    return Accepted{TcpSocket(handle)};
}

void TcpListener::close() noexcept {
    if (valid()) {
        closeHandle(std::exchange(m_handle, kInvalidSocket));
    }
    m_port = 0;
}

// ---------------------------------------------------------------- addresses

std::string hostName() {
    if (!ensureStarted()) {
        return {};
    }
    std::array<char, 256> name{};
    if (gethostname(name.data(), static_cast<int>(name.size() - 1)) != 0) {
        return {};
    }
    return std::string(name.data());
}

bool isLoopbackAddress(std::string_view address) {
    if (address.size() >= 2 && address.front() == '[' && address.back() == ']') {
        address = address.substr(1, address.size() - 2);
    }
    std::string lowered(address);
    std::ranges::transform(lowered, lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowered == "localhost") {
        return true;
    }

    in_addr v4{};
    if (inet_pton(AF_INET, lowered.c_str(), &v4) == 1) {
        return (ntohl(v4.s_addr) >> 24) == 127;
    }
    in6_addr v6{};
    if (inet_pton(AF_INET6, lowered.c_str(), &v6) == 1) {
        static constexpr std::array<std::uint8_t, 16> kLoopback{0, 0, 0, 0, 0, 0, 0, 0,
                                                                0, 0, 0, 0, 0, 0, 0, 1};
        static constexpr std::array<std::uint8_t, 12> kMappedPrefix{0, 0, 0, 0, 0,    0,
                                                                    0, 0, 0, 0, 0xff, 0xff};
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&v6);
        if (std::memcmp(bytes, kLoopback.data(), kLoopback.size()) == 0) {
            return true;
        }
        return std::memcmp(bytes, kMappedPrefix.data(), kMappedPrefix.size()) == 0 &&
               bytes[12] == 127;
    }
    return false;
}

} // namespace sweeppp::net
