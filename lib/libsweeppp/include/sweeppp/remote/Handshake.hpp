// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/net/SecureChannel.hpp"
#include "sweeppp/net/Socket.hpp"
#include "sweeppp/remote/Messages.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <string_view>

/// Opening a connection: an eight-byte preamble each way, the two messages of
/// Noise_NNpsk0 with the token as the key, then the `.sweeps` stream header
/// each way inside the encrypted channel. What follows is the record stream.
namespace sweeppp::remote {

/// Sent first by both sides, so a peer of another protocol version is told
/// apart from one that has the wrong token.
inline constexpr std::array<char, 8> kPreamble{'S', 'W', 'P', 'P', 'N', 'Z', '0', '2'};

using Deadline = std::chrono::steady_clock::time_point;

/// The client's side. PermissionDenied when the server ends the handshake --
/// what it does with a token it does not share; Unsupported for a server of
/// another protocol version.
[[nodiscard]] Result<net::SecureChannel> openClientChannel(net::TcpSocket socket,
                                                           std::string_view token,
                                                           const Hello& hello, Deadline deadline);

struct AcceptedChannel {
    net::SecureChannel channel;
    Hello hello; ///< What the client said about itself
};

/// The server's side. PermissionDenied for a client with another token,
/// ProtocolError or Unsupported for one not speaking this protocol. Gives up
/// early when `stopping` is raised.
[[nodiscard]] Result<AcceptedChannel> acceptChannel(net::TcpSocket socket, std::string_view token,
                                                    std::string_view software, Deadline deadline,
                                                    const std::atomic<bool>& stopping);

} // namespace sweeppp::remote
