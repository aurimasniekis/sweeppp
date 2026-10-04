// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/remote/Handshake.hpp"

#include "remote/StreamIo.hpp"
#include "sweeppp/crypto/Noise.hpp"
#include "sweeppp/remote/Protocol.hpp"

#include <algorithm>
#include <cstring>
#include <sweeps/FileFormat.hpp>
#include <vector>

namespace sweeppp::remote {
namespace {

constexpr std::string_view kPrologueLabel = "sweeppp-remote";

/// What both sides bind into the handshake: the label and the preamble, so a
/// handshake made for another protocol cannot complete as this one.
std::vector<std::uint8_t> prologue() {
    std::vector<std::uint8_t> bytes(kPrologueLabel.begin(), kPrologueLabel.end());
    bytes.insert(bytes.end(), kPreamble.begin(), kPreamble.end());
    return bytes;
}

std::vector<std::uint8_t> encodeBody(const sweeps::Metadata& body) {
    std::vector<std::byte> encoded;
    body.encode(encoded);
    const auto* begin = reinterpret_cast<const std::uint8_t*>(encoded.data());
    return {begin, begin + encoded.size()};
}

Result<sweeps::Metadata> decodeBody(const std::vector<std::uint8_t>& bytes) {
    sweeps::ByteReader reader(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
    return adopt(sweeps::Metadata::decode(reader, kMaxMetadataDepth));
}

/// The preamble, then a handshake message behind its u16 length.
Status sendFramed(net::TcpSocket& socket, bool withPreamble,
                  const std::vector<std::uint8_t>& message) {
    std::vector<std::uint8_t> out;
    if (withPreamble) {
        out.insert(out.end(), kPreamble.begin(), kPreamble.end());
    }
    out.push_back(static_cast<std::uint8_t>(message.size() & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(message.size() >> 8U));
    out.insert(out.end(), message.begin(), message.end());
    return socket.sendAll(out);
}

Status sendPreamble(net::TcpSocket& socket) {
    return socket.sendAll(
        {reinterpret_cast<const std::uint8_t*>(kPreamble.data()), kPreamble.size()});
}

/// The other side's preamble: ours, an older Sweep++ -- whose streams open
/// with the same four letters -- or something else entirely.
Status readPreamble(net::TcpSocket& socket, Deadline deadline, const std::atomic<bool>& stopping,
                    std::string_view peer) {
    std::array<std::uint8_t, kPreamble.size()> theirs{};
    if (auto read = io::readExactly(socket, theirs, deadline, stopping); !read) {
        return read;
    }
    if (std::memcmp(theirs.data(), kPreamble.data(), kPreamble.size()) == 0) {
        return ok();
    }
    if (std::memcmp(theirs.data(), kPreamble.data(), 4) == 0) {
        return fail(ErrorCode::Unsupported, "the {} runs another version of Sweep++", peer);
    }
    return fail(ErrorCode::ProtocolError, "the {} is not speaking the Sweep++ protocol", peer);
}

Result<std::vector<std::uint8_t>> readFramed(net::TcpSocket& socket, std::size_t limit,
                                             Deadline deadline, const std::atomic<bool>& stopping) {
    std::array<std::uint8_t, 2> length{};
    if (auto read = io::readExactly(socket, length, deadline, stopping); !read) {
        return std::unexpected(std::move(read).error());
    }
    const std::size_t bytes = std::size_t{length[0]} | (std::size_t{length[1]} << 8U);
    if (bytes > limit) {
        return fail<std::vector<std::uint8_t>>(ErrorCode::ProtocolError,
                                               "a handshake message of {} bytes", bytes);
    }
    std::vector<std::uint8_t> message(bytes);
    if (auto read = io::readExactly(socket, message, deadline, stopping); !read) {
        return std::unexpected(std::move(read).error());
    }
    return message;
}

/// Stream headers both ways, inside the channel, as Appendix C opens a stream.
Status exchangeStreamHeaders(net::SecureChannel& channel, Deadline deadline,
                             const std::atomic<bool>& stopping) {
    if (auto sent = io::sendStreamHeader(channel); !sent) {
        return sent;
    }
    if (auto header = io::readStreamHeader(channel, deadline, stopping); !header) {
        return std::unexpected(std::move(header).error());
    }
    return ok();
}

} // namespace

Result<net::SecureChannel> openClientChannel(net::TcpSocket socket, std::string_view token,
                                             const Hello& hello, Deadline deadline) {
    using Channel = net::SecureChannel;
    const std::atomic<bool> never{false};

    crypto::NoiseHandshake handshake(crypto::NoiseHandshake::Role::Initiator,
                                     crypto::pskFromToken(token), prologue());
    auto first = handshake.writeMessage(encodeBody(hello.toMetadata()));
    if (!first) {
        return std::unexpected(std::move(first).error());
    }
    if (auto sent = sendFramed(socket, true, *first); !sent) {
        return std::unexpected(std::move(sent).error());
    }

    if (auto preamble = readPreamble(socket, deadline, never, "server"); !preamble) {
        return std::unexpected(std::move(preamble).error());
    }
    // A server that does not share the token cannot answer, and says nothing:
    // anything it said would help someone guessing.
    auto second = readFramed(socket, crypto::kNoiseMaxMessageBytes, deadline, never);
    if (!second) {
        if (second.error().code() == ErrorCode::IoError) {
            return fail<Channel>(ErrorCode::PermissionDenied,
                                 "the server did not accept the token");
        }
        return std::unexpected(std::move(second).error());
    }
    if (auto payload = handshake.readMessage(*second); !payload) {
        return fail<Channel>(ErrorCode::PermissionDenied, "the server's answer did not "
                                                          "authenticate");
    }

    auto [send, receive] = handshake.split();
    net::SecureChannel channel(std::move(socket), std::move(send), std::move(receive));
    if (auto headers = exchangeStreamHeaders(channel, deadline, never); !headers) {
        return std::unexpected(std::move(headers).error());
    }
    return channel;
}

Result<AcceptedChannel> acceptChannel(net::TcpSocket socket, std::string_view token,
                                      std::string_view software, Deadline deadline,
                                      const std::atomic<bool>& stopping) {
    using Accepted = AcceptedChannel;
    if (auto sent = sendPreamble(socket); !sent) {
        return std::unexpected(std::move(sent).error());
    }
    if (auto preamble = readPreamble(socket, deadline, stopping, "client"); !preamble) {
        return std::unexpected(std::move(preamble).error());
    }

    // Before the token is known to match, a hello's worth and no more.
    auto first = readFramed(socket, kMaxPreAuthRecordBytes, deadline, stopping);
    if (!first) {
        return std::unexpected(std::move(first).error());
    }

    crypto::NoiseHandshake handshake(crypto::NoiseHandshake::Role::Responder,
                                     crypto::pskFromToken(token), prologue());
    auto payload = handshake.readMessage(*first);
    if (!payload) {
        return std::unexpected(std::move(payload).error());
    }
    auto body = decodeBody(*payload);
    if (!body) {
        return fail<Accepted>(ErrorCode::ProtocolError, "an unreadable hello: {}",
                              body.error().message());
    }
    const Hello hello = Hello::from(*body);

    sweeps::Metadata answer;
    answer.setString("software", std::string(software));
    auto second = handshake.writeMessage(encodeBody(answer));
    if (!second) {
        return std::unexpected(std::move(second).error());
    }
    if (auto sent = sendFramed(socket, false, *second); !sent) {
        return std::unexpected(std::move(sent).error());
    }

    auto [receive, send] = handshake.split();
    net::SecureChannel channel(std::move(socket), std::move(send), std::move(receive));
    if (auto headers = exchangeStreamHeaders(channel, deadline, stopping); !headers) {
        return std::unexpected(std::move(headers).error());
    }
    return AcceptedChannel{.channel = std::move(channel), .hello = hello};
}

} // namespace sweeppp::remote
