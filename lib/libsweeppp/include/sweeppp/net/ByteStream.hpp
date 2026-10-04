// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace sweeppp::net {

/// A connected, ordered byte stream, whatever carries it: the Noise channel a
/// desktop speaks, or the WebSocket a browser does.
///
/// One thread may `receive()` while another `sendAll()`s, and any may
/// `shutdown()` to wake both.
class ByteStream {
public:
    virtual ~ByteStream() = default;

    ByteStream(const ByteStream&) = delete;
    ByteStream& operator=(const ByteStream&) = delete;

    virtual Status sendAll(std::span<const std::uint8_t> data) = 0;

    /// At most `buffer.size()` bytes; zero once the stream has ended.
    [[nodiscard]] virtual Result<std::size_t> receive(std::span<std::uint8_t> buffer) = 0;

    /// Whether `receive()` has something within `timeout`.
    [[nodiscard]] virtual Result<bool> waitReadable(std::chrono::milliseconds timeout) = 0;

    virtual void shutdown() noexcept = 0;
    virtual void close() noexcept = 0;
    [[nodiscard]] virtual std::string peerAddress() const = 0;

protected:
    ByteStream() = default;
    ByteStream(ByteStream&&) = default;
    ByteStream& operator=(ByteStream&&) = default;
};

} // namespace sweeppp::net
