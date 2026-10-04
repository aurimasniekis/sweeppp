// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/net/SecureChannel.hpp"

#include <algorithm>
#include <mutex>
#include <vector>

namespace sweeppp::net {
namespace {

constexpr std::size_t kLengthBytes = 2;
constexpr std::size_t kReceiveChunk = std::size_t{64} * 1024;

} // namespace

struct SecureChannel::State {
    TcpSocket socket;

    std::mutex sendMutex;
    crypto::CipherState send;
    std::vector<std::uint8_t> sendBuffer;

    // The receiving thread's alone.
    crypto::CipherState receive;
    std::vector<std::uint8_t> raw;
    std::size_t rawRead = 0;
    std::vector<std::uint8_t> plain;
    std::size_t plainRead = 0;
    bool broken = false;

    /// A whole frame, if the raw buffer holds one.
    [[nodiscard]] std::size_t frameBytes() const noexcept {
        const std::size_t available = raw.size() - rawRead;
        if (available < kLengthBytes) {
            return 0;
        }
        const std::size_t length =
            std::size_t{raw[rawRead]} | (std::size_t{raw[rawRead + 1]} << 8U);
        return available >= kLengthBytes + length ? kLengthBytes + length : 0;
    }
};

SecureChannel::SecureChannel() = default;

SecureChannel::SecureChannel(TcpSocket socket, crypto::CipherState send,
                             crypto::CipherState receive)
    : m_state(std::make_unique<State>()) {
    m_state->socket = std::move(socket);
    m_state->send = std::move(send);
    m_state->receive = std::move(receive);
}

SecureChannel::~SecureChannel() = default;
SecureChannel::SecureChannel(SecureChannel&& other) noexcept = default;
SecureChannel& SecureChannel::operator=(SecureChannel&& other) noexcept = default;

bool SecureChannel::valid() const noexcept {
    return m_state && m_state->socket.valid();
}

Status SecureChannel::sendAll(std::span<const std::uint8_t> data) {
    if (!m_state) {
        return fail(ErrorCode::IoError, "send on a closed channel");
    }
    if (data.empty()) {
        return ok();
    }
    State& state = *m_state;
    const std::lock_guard lock(state.sendMutex);

    // Every frame of one call goes out in one write, so a burst of small
    // records costs one system call rather than one each.
    state.sendBuffer.clear();
    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t take = std::min(data.size() - offset, kMaxFramePlaintext);
        const std::size_t lengthAt = state.sendBuffer.size();
        state.sendBuffer.resize(lengthAt + kLengthBytes);
        if (auto encrypted = state.send.encrypt({}, data.subspan(offset, take), state.sendBuffer);
            !encrypted) {
            return encrypted;
        }
        const std::size_t length = state.sendBuffer.size() - lengthAt - kLengthBytes;
        state.sendBuffer[lengthAt] = static_cast<std::uint8_t>(length & 0xFFU);
        state.sendBuffer[lengthAt + 1] = static_cast<std::uint8_t>(length >> 8U);
        offset += take;
    }

    return state.socket.sendAll(state.sendBuffer);
}

Result<std::size_t> SecureChannel::receive(std::span<std::uint8_t> buffer) {
    if (!m_state) {
        return fail<std::size_t>(ErrorCode::IoError, "receive on a closed channel");
    }
    State& state = *m_state;
    if (state.broken) {
        return fail<std::size_t>(ErrorCode::Corrupt, "the channel failed authentication");
    }

    while (state.plainRead == state.plain.size()) {
        state.plain.clear();
        state.plainRead = 0;

        if (const std::size_t frame = state.frameBytes(); frame > 0) {
            const std::span<const std::uint8_t> ciphertext(
                state.raw.data() + state.rawRead + kLengthBytes, frame - kLengthBytes);
            if (auto decrypted = state.receive.decrypt({}, ciphertext, state.plain); !decrypted) {
                state.broken = true;
                return std::unexpected(std::move(decrypted).error());
            }
            state.rawRead += frame;
            if (state.rawRead == state.raw.size()) {
                state.raw.clear();
                state.rawRead = 0;
            }
            continue;
        }

        // Compact before reading more, so a long stream does not grow the
        // buffer by everything it ever received.
        if (state.rawRead > 0) {
            state.raw.erase(state.raw.begin(),
                            state.raw.begin() + static_cast<std::ptrdiff_t>(state.rawRead));
            state.rawRead = 0;
        }
        const std::size_t at = state.raw.size();
        state.raw.resize(at + kReceiveChunk);
        auto got = state.socket.receive({state.raw.data() + at, kReceiveChunk});
        if (!got) {
            state.raw.resize(at);
            return std::unexpected(std::move(got).error());
        }
        state.raw.resize(at + *got);
        if (*got == 0) {
            return std::size_t{0};
        }
    }

    const std::size_t take = std::min(buffer.size(), state.plain.size() - state.plainRead);
    std::copy_n(state.plain.begin() + static_cast<std::ptrdiff_t>(state.plainRead), take,
                buffer.begin());
    state.plainRead += take;
    return take;
}

Result<bool> SecureChannel::waitReadable(std::chrono::milliseconds timeout) {
    if (!m_state) {
        return fail<bool>(ErrorCode::IoError, "wait on a closed channel");
    }
    State& state = *m_state;
    if (state.plainRead < state.plain.size() || state.frameBytes() > 0) {
        return true;
    }
    return state.socket.waitReadable(timeout);
}

void SecureChannel::shutdown() noexcept {
    if (m_state) {
        m_state->socket.shutdown();
    }
}

void SecureChannel::close() noexcept {
    if (m_state) {
        m_state->socket.close();
    }
}

std::string SecureChannel::peerAddress() const {
    return m_state ? m_state->socket.peerAddress() : std::string{};
}

} // namespace sweeppp::net
