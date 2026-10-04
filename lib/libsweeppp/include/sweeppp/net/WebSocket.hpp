// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/net/ByteStream.hpp"
#include "sweeppp/net/Http.hpp"
#include "sweeppp/net/Socket.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// WebSocket (RFC 6455), the server's side: what a browser needs to carry the
/// record stream, and no extensions.
namespace sweeppp::net::ws {

/// A whole message, however many frames it came in.
inline constexpr std::size_t kMaxMessageBytes = std::size_t{4} * 1024 * 1024;

enum class Opcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

/// What the server answers a `Sec-WebSocket-Key` with.
[[nodiscard]] std::string acceptKey(std::string_view clientKey);

/// The accept key for an upgrade request, or why it is not one: GET,
/// `Upgrade: websocket`, `Connection: upgrade`, version 13 and a key of 16
/// bytes.
[[nodiscard]] Result<std::string> checkUpgrade(const http::Request& request);

/// Whether the request came from a page this server served: no Origin, as
/// from a program, or one naming the same host and port as `Host`.
[[nodiscard]] bool sameOrigin(const http::Request& request);

/// One frame. A server's go out unmasked; a client's carry `mask`.
[[nodiscard]] std::vector<std::uint8_t>
encodeFrame(Opcode opcode, std::span<const std::uint8_t> payload, bool fin = true,
            std::optional<std::array<std::uint8_t, 4>> mask = std::nullopt);

/// A message: a data message whole, or a control frame as it came.
struct Message {
    Opcode opcode = Opcode::Binary;
    std::vector<std::uint8_t> payload;
};

/// Frames off a byte stream, put back together into messages.
///
/// ProtocolError for anything RFC 6455 says to fail the connection over: an
/// unmasked frame from a client, reserved bits, an unknown opcode, a control
/// frame fragmented or over 125 bytes, a continuation with nothing to
/// continue, or a message over `maxMessage`.
class MessageReader {
public:
    explicit MessageReader(bool requireMask = true, std::size_t maxMessage = kMaxMessageBytes)
        : m_requireMask(requireMask), m_maxMessage(maxMessage) {}

    void feed(std::span<const std::uint8_t> bytes);

    /// The next message; nothing until one is complete.
    [[nodiscard]] Result<std::optional<Message>> next();

    /// Bytes fed and not yet part of a returned message.
    [[nodiscard]] std::size_t buffered() const noexcept { return m_buffer.size() - m_read; }

private:
    bool m_requireMask;
    std::size_t m_maxMessage;
    std::vector<std::uint8_t> m_buffer;
    std::size_t m_read = 0;
    std::optional<Opcode> m_fragmentOf;
    std::vector<std::uint8_t> m_fragments;
    bool m_failed = false;
};

/// The record stream over an upgraded connection: each `sendAll` is one
/// binary message, and binary messages arrive as one byte stream.
///
/// Pings are answered and a close is echoed; text is a protocol error.
class WebSocketChannel final : public ByteStream {
public:
    explicit WebSocketChannel(TcpSocket socket);
    ~WebSocketChannel() override;

    WebSocketChannel(const WebSocketChannel&) = delete;
    WebSocketChannel& operator=(const WebSocketChannel&) = delete;
    WebSocketChannel(WebSocketChannel&&) = delete;
    WebSocketChannel& operator=(WebSocketChannel&&) = delete;

    Status sendAll(std::span<const std::uint8_t> data) override;
    [[nodiscard]] Result<std::size_t> receive(std::span<std::uint8_t> buffer) override;
    [[nodiscard]] Result<bool> waitReadable(std::chrono::milliseconds timeout) override;
    void shutdown() noexcept override;
    void close() noexcept override;
    [[nodiscard]] std::string peerAddress() const override { return m_peer; }

private:
    Status sendFrame(Opcode opcode, std::span<const std::uint8_t> payload);
    /// Takes whatever whole messages are buffered: data into the plain
    /// buffer, control answered. Stops at the first data message.
    Status pump();

    TcpSocket m_socket;
    std::string m_peer;
    std::mutex m_sendMutex;
    bool m_closeSent = false;

    // The receiving thread's alone.
    MessageReader m_reader;
    std::vector<std::uint8_t> m_plain;
    std::size_t m_plainRead = 0;
    bool m_ended = false;
};

} // namespace sweeppp::net::ws
