// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/EventBus.hpp"
#include "sweeppp/core/Result.hpp"
#include "sweeppp/core/Telemetry.hpp"
#include "sweeppp/instrument/LocalInstrument.hpp"
#include "sweeppp/net/ByteStream.hpp"
#include "sweeppp/pipeline/FrameBus.hpp"
#include "sweeppp/remote/Messages.hpp"
#include "sweeppp/remote/Protocol.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

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

    /// While a dropped controller's radio lingers, its completed passes are
    /// kept, up to this long and this many bytes, and sent first when that
    /// client comes back. Zero keeps none.
    std::chrono::milliseconds backlog = kDefaultBacklog;
    std::size_t backlogBytes = kDefaultBacklogBytes;

    /// Where recordings made on the server go; empty means it makes none.
    std::filesystem::path sessionsDir;
    /// Start recording as soon as the server starts, at `recordBins`.
    bool recordAtStart = false;
    std::uint32_t recordBins = 65'536;

    /// Answer desktops looking for servers on the LAN (mDNS).
    bool advertise = false;

    /// Let several clients connect at once: one controls, the rest watch.
    /// Without it the server takes one client and turns the next away busy.
    bool shared = false;
    /// How many a shared server takes at once.
    std::size_t maxClients = kDefaultMaxClients;
};

/// Serves one `LocalInstrument` to its clients.
///
/// Everything the instrument does happens on the server's control thread:
/// commands, `tick()`, state. Frames leave from the output bus through a
/// two-slot mailbox per client, so a slow link merges frames rather than
/// slowing the engine or the other clients down.
///
/// One client at a time controls the radio; on a shared server the others
/// watch, and any of them may take control. When the last client says
/// goodbye the radio stops, its plan and parameters as they were. A
/// controller that drops leaves it running for `linger`, for the same
/// desktop, reconnecting, to take control again.
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

    /// Says goodbye to every client, stops acquisition and joins every
    /// thread.
    void stop();

    /// The bound port, once started.
    [[nodiscard]] std::uint16_t port() const noexcept;

    /// A client on a stream already authenticated elsewhere -- a browser's
    /// WebSocket. The client opens with its stream header and a hello, as a
    /// desktop does inside the encrypted channel. Any thread, once started.
    void adopt(std::unique_ptr<net::ByteStream> stream);

    /// A recording to hand out by name, checked as a client's fetch is: only
    /// a finished `.sweeps` file in the recordings folder. Any thread.
    [[nodiscard]] Result<std::filesystem::path> recordingFile(const std::string& name) const;

    [[nodiscard]] bool clientConnected() const;

    /// Everyone connected, controller first.
    [[nodiscard]] std::vector<ConnectedClient> clients() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sweeppp::remote
