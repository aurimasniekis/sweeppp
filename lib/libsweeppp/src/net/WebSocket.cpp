// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/net/WebSocket.hpp"

#include "sweeppp/crypto/Sha1.hpp"
#include "sweeppp/net/Base64.hpp"

#include <algorithm>

namespace sweeppp::net::ws {
namespace {

constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::size_t kMaxControlPayload = 125;
constexpr std::size_t kReceiveChunk = std::size_t{64} * 1024;

bool isControl(Opcode opcode) noexcept {
    return (static_cast<std::uint8_t>(opcode) & 0x8U) != 0;
}

bool knownOpcode(std::uint8_t value) noexcept {
    return value <= 0x2 || (value >= 0x8 && value <= 0xA);
}

/// Whether a comma-separated header names `token`, in any case.
bool listContains(std::string_view list, std::string_view token) {
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        std::string_view item = list.substr(0, comma);
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
        while (!item.empty() && item.front() == ' ') {
            item.remove_prefix(1);
        }
        while (!item.empty() && item.back() == ' ') {
            item.remove_suffix(1);
        }
        if (std::ranges::equal(item, token, [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            })) {
            return true;
        }
    }
    return false;
}

std::string_view withoutScheme(std::string_view origin) {
    for (const std::string_view scheme : {"http://", "https://"}) {
        if (origin.starts_with(scheme)) {
            return origin.substr(scheme.size());
        }
    }
    return {};
}

} // namespace

std::string acceptKey(std::string_view clientKey) {
    std::string joined(clientKey);
    joined += kGuid;
    const crypto::Sha1Digest digest = crypto::sha1(joined);
    return base64Encode(digest);
}

Result<std::string> checkUpgrade(const http::Request& request) {
    if (request.method != "GET") {
        return fail<std::string>(ErrorCode::ProtocolError, "an upgrade must be a GET");
    }
    if (!listContains(request.header("upgrade"), "websocket") ||
        !listContains(request.header("connection"), "upgrade")) {
        return fail<std::string>(ErrorCode::ProtocolError, "not a WebSocket upgrade");
    }
    if (request.header("sec-websocket-version") != "13") {
        return fail<std::string>(ErrorCode::Unsupported, "WebSocket version '{}', not 13",
                                 request.header("sec-websocket-version"));
    }
    const std::string_view key = request.header("sec-websocket-key");
    const auto decoded = base64Decode(key);
    if (!decoded || decoded->size() != 16) {
        return fail<std::string>(ErrorCode::ProtocolError, "a malformed Sec-WebSocket-Key");
    }
    return acceptKey(key);
}

bool sameOrigin(const http::Request& request) {
    const std::string_view origin = request.header("origin");
    if (origin.empty()) {
        return true;
    }
    const std::string_view host = request.header("host");
    const std::string_view authority = withoutScheme(origin);
    return !host.empty() && !authority.empty() &&
           std::ranges::equal(authority, host, [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

std::vector<std::uint8_t> encodeFrame(Opcode opcode, std::span<const std::uint8_t> payload,
                                      bool fin, std::optional<std::array<std::uint8_t, 4>> mask) {
    std::vector<std::uint8_t> out;
    out.reserve(payload.size() + 14);
    out.push_back(
        static_cast<std::uint8_t>((fin ? 0x80U : 0U) | static_cast<std::uint8_t>(opcode)));
    const std::uint8_t maskBit = mask ? 0x80U : 0U;
    if (payload.size() < 126) {
        out.push_back(static_cast<std::uint8_t>(maskBit | payload.size()));
    } else if (payload.size() <= 0xFFFF) {
        out.push_back(maskBit | 126U);
        out.push_back(static_cast<std::uint8_t>(payload.size() >> 8U));
        out.push_back(static_cast<std::uint8_t>(payload.size()));
    } else {
        out.push_back(maskBit | 127U);
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(payload.size()) >>
                                                    static_cast<unsigned>(shift)));
        }
    }
    if (mask) {
        out.insert(out.end(), mask->begin(), mask->end());
        for (std::size_t i = 0; i < payload.size(); ++i) {
            out.push_back(payload[i] ^ (*mask)[i % 4]);
        }
    } else {
        out.insert(out.end(), payload.begin(), payload.end());
    }
    return out;
}

// ---------------------------------------------------------------- the reader

void MessageReader::feed(std::span<const std::uint8_t> bytes) {
    // Compact before growing, so a long stream does not keep everything it
    // ever carried.
    if (m_read > 0 && m_read == m_buffer.size()) {
        m_buffer.clear();
        m_read = 0;
    } else if (m_read > kReceiveChunk) {
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(m_read));
        m_read = 0;
    }
    m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
}

namespace {

/// A frame's header, once enough of it has arrived.
struct FrameHead {
    bool fin = false;
    Opcode opcode = Opcode::Binary;
    bool masked = false;
    std::size_t headerBytes = 0; ///< Mask key included
    std::uint64_t length = 0;
};

/// Nothing until the whole header is there; an error for one that breaks the
/// rules whatever follows it.
Result<std::optional<FrameHead>> parseFrameHead(const std::uint8_t* p, std::size_t available,
                                                bool requireMask) {
    using Head = std::optional<FrameHead>;
    if (available < 2) {
        return Head{};
    }
    FrameHead head;
    head.fin = (p[0] & 0x80U) != 0;
    if ((p[0] & 0x70U) != 0) {
        return fail<Head>(ErrorCode::ProtocolError, "WebSocket: reserved bits set");
    }
    const std::uint8_t opcodeValue = p[0] & 0x0FU;
    if (!knownOpcode(opcodeValue)) {
        return fail<Head>(ErrorCode::ProtocolError, "WebSocket: an unknown opcode");
    }
    head.opcode = static_cast<Opcode>(opcodeValue);
    head.masked = (p[1] & 0x80U) != 0;
    if (requireMask && !head.masked) {
        return fail<Head>(ErrorCode::ProtocolError, "WebSocket: an unmasked frame from a client");
    }

    head.headerBytes = 2;
    head.length = p[1] & 0x7FU;
    if (head.length == 126) {
        if (available < 4) {
            return Head{};
        }
        head.length = (std::uint64_t{p[2]} << 8U) | p[3];
        head.headerBytes = 4;
    } else if (head.length == 127) {
        if (available < 10) {
            return Head{};
        }
        head.length = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            head.length = (head.length << 8U) | p[2 + i];
        }
        if ((head.length >> 63U) != 0) {
            return fail<Head>(ErrorCode::ProtocolError, "WebSocket: a negative length");
        }
        head.headerBytes = 10;
    }
    if (isControl(head.opcode) && (!head.fin || head.length > kMaxControlPayload)) {
        return fail<Head>(ErrorCode::ProtocolError,
                          "WebSocket: a control frame fragmented or too long");
    }
    if (head.masked) {
        head.headerBytes += 4;
    }
    return Head{head};
}

} // namespace

Result<std::optional<Message>> MessageReader::next() {
    using Next = std::optional<Message>;
    const auto broken = [this](std::string_view why) {
        m_failed = true;
        return fail<Next>(ErrorCode::ProtocolError, "WebSocket: {}", why);
    };
    if (m_failed) {
        return broken("the stream already failed");
    }

    while (true) {
        const std::size_t available = m_buffer.size() - m_read;
        const std::uint8_t* p = m_buffer.data() + m_read;
        auto parsed = parseFrameHead(p, available, m_requireMask);
        if (!parsed) {
            m_failed = true;
            return std::unexpected(std::move(parsed).error());
        }
        if (!*parsed) {
            return Next{};
        }
        const FrameHead& head = **parsed;
        if (head.length > m_maxMessage || m_fragments.size() + head.length > m_maxMessage) {
            return broken("a message too large");
        }
        if (available < head.headerBytes + head.length) {
            return Next{};
        }

        std::vector<std::uint8_t> payload(p + head.headerBytes, p + head.headerBytes + head.length);
        if (head.masked) {
            const std::uint8_t* key = p + head.headerBytes - 4;
            for (std::size_t i = 0; i < payload.size(); ++i) {
                payload[i] ^= key[i % 4];
            }
        }
        m_read += head.headerBytes + static_cast<std::size_t>(head.length);

        if (isControl(head.opcode)) {
            return Next{Message{.opcode = head.opcode, .payload = std::move(payload)}};
        }
        if (head.opcode == Opcode::Continuation && !m_fragmentOf) {
            return broken("a continuation of nothing");
        }
        if (head.opcode != Opcode::Continuation) {
            if (m_fragmentOf) {
                return broken("a new message inside a fragmented one");
            }
            m_fragmentOf = head.opcode;
        }
        m_fragments.insert(m_fragments.end(), payload.begin(), payload.end());
        if (head.fin) {
            Message message{.opcode = *m_fragmentOf, .payload = std::move(m_fragments)};
            m_fragments.clear();
            m_fragmentOf.reset();
            return Next{std::move(message)};
        }
    }
}

// --------------------------------------------------------------- the channel

WebSocketChannel::WebSocketChannel(TcpSocket socket)
    : m_socket(std::move(socket)), m_peer(m_socket.peerAddress()) {
}

WebSocketChannel::~WebSocketChannel() {
    close();
}

Status WebSocketChannel::sendFrame(Opcode opcode, std::span<const std::uint8_t> payload) {
    const std::vector<std::uint8_t> frame = encodeFrame(opcode, payload);
    const std::lock_guard lock(m_sendMutex);
    if (m_closeSent) {
        return fail(ErrorCode::IoError, "the WebSocket is closing");
    }
    if (opcode == Opcode::Close) {
        m_closeSent = true;
    }
    return m_socket.sendAll(frame);
}

Status WebSocketChannel::sendAll(std::span<const std::uint8_t> data) {
    if (data.empty()) {
        return ok();
    }
    return sendFrame(Opcode::Binary, data);
}

Status WebSocketChannel::pump() {
    while (m_plainRead == m_plain.size() && !m_ended) {
        auto message = m_reader.next();
        if (!message) {
            m_ended = true;
            // 1002: protocol error.
            (void)sendFrame(Opcode::Close, std::array<std::uint8_t, 2>{0x03, 0xEA});
            return std::unexpected(std::move(message).error());
        }
        if (!*message) {
            return ok();
        }
        Message& got = **message;
        switch (got.opcode) {
        case Opcode::Binary:
            m_plain = std::move(got.payload);
            m_plainRead = 0;
            break;
        case Opcode::Ping:
            (void)sendFrame(Opcode::Pong, got.payload);
            break;
        case Opcode::Close:
            // The status echoed is all a close needs back.
            (void)sendFrame(
                Opcode::Close,
                std::span(got.payload).first(std::min<std::size_t>(got.payload.size(), 2)));
            m_ended = true;
            break;
        case Opcode::Text:
            m_ended = true;
            // 1003: a type this end does not take.
            (void)sendFrame(Opcode::Close, std::array<std::uint8_t, 2>{0x03, 0xEB});
            return fail(ErrorCode::ProtocolError, "a text message on a binary stream");
        default:
            break;
        }
    }
    return ok();
}

Result<std::size_t> WebSocketChannel::receive(std::span<std::uint8_t> buffer) {
    std::vector<std::uint8_t> chunk;
    while (true) {
        if (auto pumped = pump(); !pumped) {
            return std::unexpected(std::move(pumped).error());
        }
        if (m_plainRead < m_plain.size()) {
            break;
        }
        if (m_ended) {
            return std::size_t{0};
        }
        chunk.resize(kReceiveChunk);
        auto got = m_socket.receive(chunk);
        if (!got) {
            return std::unexpected(std::move(got).error());
        }
        if (*got == 0) {
            m_ended = true;
            return std::size_t{0};
        }
        m_reader.feed(std::span(chunk).first(*got));
    }

    const std::size_t take = std::min(buffer.size(), m_plain.size() - m_plainRead);
    std::copy_n(m_plain.begin() + static_cast<std::ptrdiff_t>(m_plainRead), take, buffer.begin());
    m_plainRead += take;
    return take;
}

Result<bool> WebSocketChannel::waitReadable(std::chrono::milliseconds timeout) {
    // Only a whole message counts: a frame half-arrived is the socket's to
    // wait on, so a client that stalls mid-frame still reads as silent.
    if (auto pumped = pump(); !pumped || m_plainRead < m_plain.size() || m_ended) {
        return true;
    }
    return m_socket.waitReadable(timeout);
}

void WebSocketChannel::shutdown() noexcept {
    m_socket.shutdown();
}

void WebSocketChannel::close() noexcept {
    m_socket.close();
}

} // namespace sweeppp::net::ws
