// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/net/Socket.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <sweeps/Stream.hpp>
#include <vector>

/// Blocking reads of a record stream, for the handshakes on either end.
namespace sweeppp::remote::io {

using Clock = std::chrono::steady_clock;

inline constexpr std::size_t kReceiveChunk = std::size_t{64} * 1024;

/// How long one wait lasts before the stop flag is looked at again.
inline constexpr std::chrono::milliseconds kReadSlice{200};

[[nodiscard]] inline std::span<const std::uint8_t> asBytes(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

/// The next record, reading as needed until `deadline`. Gives up early when
/// `stopping` is raised.
[[nodiscard]] Result<sweeps::StreamRecord> readRecord(net::TcpSocket& socket,
                                                      sweeps::RecordFramer& framer,
                                                      Clock::time_point deadline,
                                                      const std::atomic<bool>& stopping);

/// Exactly a stream header's worth, so nothing after it is consumed here.
[[nodiscard]] Result<sweeps::StreamHeader> readStreamHeader(net::TcpSocket& socket,
                                                            Clock::time_point deadline,
                                                            const std::atomic<bool>& stopping);

[[nodiscard]] Status sendStreamHeader(net::TcpSocket& socket);

} // namespace sweeppp::remote::io
