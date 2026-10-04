// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/crypto/Noise.hpp"
#include "sweeppp/net/ByteStream.hpp"
#include "sweeppp/net/Socket.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace sweeppp::net {

/// A byte stream encrypted with the two ciphers a Noise handshake produced.
///
/// On the wire, a frame is a u16 little-endian length and that many bytes of
/// ciphertext, tag included. What a caller sends is cut into frames and what
/// arrives is put back together, so to either side it is the same byte stream
/// a bare socket carries.
///
/// Like `TcpSocket`: one thread may receive while another sends, and any may
/// `shutdown()`. A frame that fails authentication ends the stream: nothing
/// after it can be trusted to be where it seems.
class SecureChannel final : public ByteStream {
public:
    /// The most plaintext one frame carries.
    static constexpr std::size_t kMaxFramePlaintext =
        crypto::kNoiseMaxMessageBytes - crypto::kNoiseTagBytes;

    SecureChannel();
    SecureChannel(TcpSocket socket, crypto::CipherState send, crypto::CipherState receive);
    ~SecureChannel() override;

    SecureChannel(const SecureChannel&) = delete;
    SecureChannel& operator=(const SecureChannel&) = delete;
    SecureChannel(SecureChannel&& other) noexcept;
    SecureChannel& operator=(SecureChannel&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept;

    Status sendAll(std::span<const std::uint8_t> data) override;

    /// Plaintext, at most `buffer.size()` bytes, waiting for a whole frame if
    /// need be; zero once the stream has ended.
    [[nodiscard]] Result<std::size_t> receive(std::span<std::uint8_t> buffer) override;

    /// Whether `receive()` has something: plaintext already decrypted, or
    /// bytes on the socket.
    [[nodiscard]] Result<bool> waitReadable(std::chrono::milliseconds timeout) override;

    void shutdown() noexcept override;
    void close() noexcept override;
    [[nodiscard]] std::string peerAddress() const override;

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace sweeppp::net
