// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace sweeppp::net {

/// A socket handle on either platform: a file descriptor on POSIX, a `SOCKET`
/// on Windows, whose invalid value is all ones -- -1 here on both.
using NativeSocket = std::intptr_t;
inline constexpr NativeSocket kInvalidSocket = -1;

/// A connected TCP stream, blocking.
///
/// One thread may `receive()` while another `sendAll()`s, and any thread may
/// `shutdown()` to wake both. Moving, closing and destroying belong to the
/// owner alone, once the others have been joined.
class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket();

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    /// Tries each address `host` resolves to, giving each the whole
    /// `timeout`. TimedOut when none answered in time, Unavailable when one
    /// refused.
    [[nodiscard]] static Result<TcpSocket> connect(std::string_view host, std::uint16_t port,
                                                   std::chrono::milliseconds timeout);

    [[nodiscard]] bool valid() const noexcept { return m_handle != kInvalidSocket; }

    /// Sends every byte, or fails having sent some of them.
    Status sendAll(std::span<const std::uint8_t> data);

    /// Whatever has arrived, at most `buffer.size()` bytes; zero once the peer
    /// has closed its side or this one was shut down.
    [[nodiscard]] Result<std::size_t> receive(std::span<std::uint8_t> buffer);

    /// Whether something can be read within `timeout`: data, or the close.
    [[nodiscard]] Result<bool> waitReadable(std::chrono::milliseconds timeout);

    /// Ends both directions, waking a blocked `receive()` or `sendAll()` on
    /// another thread. The handle stays open until `close()`.
    void shutdown() noexcept;
    void close() noexcept;

    /// Sends small records as soon as they are written rather than holding
    /// them back to fill a segment.
    Status setNoDelay(bool enabled);
    Status setKeepAlive(bool enabled);

    /// "192.168.1.20:7332", "[::1]:7332"; empty when unknown.
    [[nodiscard]] std::string peerAddress() const;

private:
    explicit TcpSocket(NativeSocket handle) noexcept : m_handle(handle) {}
    friend class TcpListener;

    NativeSocket m_handle = kInvalidSocket;
};

/// A bound, listening TCP socket.
class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;

    /// Port 0 binds an ephemeral one; `port()` says which. "::" listens on
    /// IPv4 too where the system allows it.
    [[nodiscard]] static Result<TcpListener> listen(std::string_view address, std::uint16_t port,
                                                    int backlog = 4);

    [[nodiscard]] bool valid() const noexcept { return m_handle != kInvalidSocket; }
    [[nodiscard]] std::uint16_t port() const noexcept { return m_port; }

    /// The next connection, or nullopt when none arrived within `timeout`.
    [[nodiscard]] Result<std::optional<TcpSocket>> accept(std::chrono::milliseconds timeout);

    void close() noexcept;

private:
    NativeSocket m_handle = kInvalidSocket;
    std::uint16_t m_port = 0;
};

/// This machine's name, as `gethostname` gives it; empty when it cannot.
[[nodiscard]] std::string hostName();

/// Whether `address` names this machine only: 127.0.0.0/8, ::1, or
/// "localhost". A wildcard or a hostname is not.
[[nodiscard]] bool isLoopbackAddress(std::string_view address);

} // namespace sweeppp::net
