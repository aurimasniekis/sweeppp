// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/net/SecureChannel.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <sweeps/Stream.hpp>
#include <vector>

/// Blocking reads with a deadline, for the handshakes on either end: over the
/// bare socket before the channel exists, and over the channel after.
namespace sweeppp::remote::io {

using Clock = std::chrono::steady_clock;

inline constexpr std::size_t kReceiveChunk = std::size_t{64} * 1024;

/// How long one wait lasts before the stop flag is looked at again.
inline constexpr std::chrono::milliseconds kReadSlice{200};

[[nodiscard]] inline std::span<const std::uint8_t> asBytes(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

/// Exactly `out.size()` bytes from `stream` -- a `TcpSocket` or a
/// `SecureChannel` -- by `deadline`. IoError when the peer closes first.
template <typename Stream>
[[nodiscard]] Status readExactly(Stream& stream, std::span<std::uint8_t> out,
                                 Clock::time_point deadline, const std::atomic<bool>& stopping) {
    std::size_t have = 0;
    while (have < out.size()) {
        if (stopping.load()) {
            return fail(ErrorCode::Cancelled, "cancelled");
        }
        const auto now = Clock::now();
        if (now >= deadline) {
            return fail(ErrorCode::TimedOut, "nothing arrived in time");
        }
        const auto wait = std::min<std::chrono::milliseconds>(
            kReadSlice, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        auto readable = stream.waitReadable(wait);
        if (!readable) {
            return std::unexpected(std::move(readable).error());
        }
        if (!*readable) {
            continue;
        }
        auto got = stream.receive(out.subspan(have));
        if (!got) {
            return std::unexpected(std::move(got).error());
        }
        if (*got == 0) {
            return fail(ErrorCode::IoError, "the peer closed the connection");
        }
        have += *got;
    }
    return ok();
}

/// The next record off the channel, reading as needed until `deadline`.
[[nodiscard]] Result<sweeps::StreamRecord> readRecord(net::SecureChannel& channel,
                                                      sweeps::RecordFramer& framer,
                                                      Clock::time_point deadline,
                                                      const std::atomic<bool>& stopping);

[[nodiscard]] Result<sweeps::StreamHeader> readStreamHeader(net::SecureChannel& channel,
                                                            Clock::time_point deadline,
                                                            const std::atomic<bool>& stopping);

[[nodiscard]] Status sendStreamHeader(net::SecureChannel& channel);

} // namespace sweeppp::remote::io
