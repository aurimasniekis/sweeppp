// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/EventBus.hpp"
#include "sweeppp/core/Result.hpp"
#include "sweeppp/core/Telemetry.hpp"
#include "sweeppp/instrument/LocalInstrument.hpp"
#include "sweeppp/pipeline/FrameBus.hpp"
#include "sweeppp/remote/Protocol.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace sweeppp::remote {

struct ServerConfig {
    std::string listenAddress = "127.0.0.1";
    std::uint16_t port = kDefaultPort; ///< 0 binds an ephemeral port
    /// Empty means no authentication, which `start()` allows on a loopback
    /// address only.
    std::string token;
    /// What clients call this machine; the host name when empty.
    std::string serverName;

    std::chrono::milliseconds handshakeTimeout = kHandshakeTimeout;
    std::chrono::milliseconds silenceTimeout = kSilenceTimeout;
    std::chrono::milliseconds refusalDelay = kRefusalDelay;

    /// After a client drops without saying goodbye, how long the radio keeps
    /// running for it to come back. Zero stops it at once, as a goodbye does.
    std::chrono::milliseconds linger = kDefaultLinger;

    /// Where recordings made on the server go; empty means it makes none.
    std::filesystem::path sessionsDir;
    /// Start recording as soon as the server starts, at `recordBins`.
    bool recordAtStart = false;
    std::uint32_t recordBins = 65'536;

    /// Answer desktops looking for servers on the LAN (mDNS).
    bool advertise = false;
};

/// Serves one `LocalInstrument` to one remote client at a time.
///
/// Everything the instrument does happens on the server's control thread:
/// commands, `tick()`, state. Frames leave from the output bus through a
/// two-slot mailbox, so a slow link merges frames rather than slowing the
/// engine down. A client that says goodbye leaves the radio stopped, its plan
/// and parameters as they were; one that drops leaves it running for
/// `linger`, for the next client -- the same desktop, reconnecting -- to take
/// over.
class RemoteServer {
public:
    /// `output`, `events` and `telemetry` are the ones `instrument` was built
    /// with.
    RemoteServer(LocalInstrument& instrument, FrameBus& output, EventBus& events,
                 Telemetry& telemetry, ServerConfig config);
    ~RemoteServer();

    RemoteServer(const RemoteServer&) = delete;
    RemoteServer& operator=(const RemoteServer&) = delete;
    RemoteServer(RemoteServer&&) = delete;
    RemoteServer& operator=(RemoteServer&&) = delete;

    /// Binds and starts serving. From here until `stop()` returns, the
    /// instrument belongs to the server: the caller must not touch it.
    Status start();

    /// Says goodbye to the client, stops acquisition and joins every thread.
    void stop();

    /// The bound port, once started.
    [[nodiscard]] std::uint16_t port() const noexcept;

    [[nodiscard]] bool clientConnected() const;

    /// The connected client's address; empty with none.
    [[nodiscard]] std::string clientAddress() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sweeppp::remote
