// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/HostStats.hpp"
#include "sweeppp/core/Result.hpp"
#include "sweeppp/core/Telemetry.hpp"
#include "sweeppp/instrument/Instrument.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sweeps/Metadata.hpp>
#include <sweeps/Stream.hpp>
#include <vector>

/// The control messages: PluginData records under `kPluginId`, each body a
/// typed-metadata hash.
namespace sweeppp::remote {

/// One control message off the stream.
struct Message {
    std::string name;
    std::uint64_t monotonicNs = 0;
    sweeps::Metadata body;
};

/// Appends a whole record carrying `body` as message `name`.
void appendMessage(std::vector<std::byte>& out, std::string_view name, const sweeps::Metadata& body,
                   std::uint64_t monotonicNs = 0);

/// The record as a control message. ProtocolError for a PluginData record of
/// another plugin, or a body that does not decode; Unsupported for a schema
/// version other than this protocol's. The caller checks the record type.
[[nodiscard]] Result<Message> decodeMessage(const sweeps::StreamRecord& record);

/// The state sections a command can change. The server sends them again with
/// its acknowledgement, changed or not, and the client keeps its own copy of
/// them until that arrives -- so an edit the server refused is put back.
[[nodiscard]] std::span<const std::string_view> sectionsTouchedBy(std::string_view op) noexcept;

/// Whether a client that does not control a shared server may still send
/// `op`: what changes only its own connection, and taking control.
[[nodiscard]] bool viewerMay(std::string_view op) noexcept;

// ---- the handshake ---------------------------------------------------------------

/// The client's half of the handshake's payload.
struct Hello {
    std::uint32_t protocolVersion = 0;
    std::string software; ///< "Sweep++ 0.2.0"
    std::string kind;     ///< One of `client::`
    std::string name;     ///< "studio-mac", "Firefox on iPhone"
    /// Random, kept by the client across runs: how a server knows the
    /// desktop that dropped is the one coming back.
    std::string clientId;

    [[nodiscard]] sweeps::Metadata toMetadata() const;
    [[nodiscard]] static Hello from(const sweeps::Metadata& in);
};

struct Welcome {
    std::string serverName; ///< The server's host name, for "HackRF One on pi"
    bool shared = false;    ///< Whether other clients may watch alongside
    /// The server's monotonic clock as it sent this: a first guess at the
    /// offset, before any ping, for frames it replays straight away.
    std::uint64_t serverNs = 0;

    [[nodiscard]] sweeps::Metadata toMetadata() const;
    [[nodiscard]] static Welcome from(const sweeps::Metadata& in);
};

struct Refused {
    std::string reason; ///< One of `refusal::`
    std::string message;

    [[nodiscard]] sweeps::Metadata toMetadata() const;
    [[nodiscard]] static Refused from(const sweeps::Metadata& in);
};

// ---- control -----------------------------------------------------------------

/// The `control` section: who may change things.
struct ControlState {
    bool shared = false;
    bool you = false;  ///< This client controls
    bool held = false; ///< Somebody does
    std::string controller;
    std::string controllerKind;

    [[nodiscard]] sweeps::Metadata toMetadata() const;
    [[nodiscard]] static ControlState from(const sweeps::Metadata& in);
};

/// One row of the `clients` section.
struct ConnectedClient {
    std::uint64_t id = 0; ///< The server's, for this connection
    std::string name;
    std::string kind;
    std::string address;
    bool controls = false;
    bool you = false;
};

[[nodiscard]] sweeps::Metadata encodeClients(std::span<const ConnectedClient> clients);
[[nodiscard]] std::vector<ConnectedClient> decodeClients(const sweeps::Metadata& in);

struct Command {
    std::uint64_t seq = 0;
    std::string op; ///< One of `op::`
    sweeps::Metadata args;

    [[nodiscard]] sweeps::Metadata toMetadata() const;
    [[nodiscard]] static Command from(const sweeps::Metadata& in);
};

struct Reply {
    std::uint64_t seq = 0;
    bool ok = true;
    ErrorCode code = ErrorCode::Unknown;
    std::string message;

    [[nodiscard]] sweeps::Metadata toMetadata() const;
    [[nodiscard]] static Reply from(const sweeps::Metadata& in);
};

/// Sections of the instrument's state, each under its `section::` name.
struct State {
    /// The last command executed before this was taken: a client holding an
    /// edit sent later keeps its own copy of what that edit touched.
    std::uint64_t ackSeq = 0;
    sweeps::Metadata sections;

    [[nodiscard]] sweeps::Metadata toMetadata() const;
    [[nodiscard]] static State from(const sweeps::Metadata& in);
};

/// What completes a line: the tiles before it, applied, are this frame.
struct FrameCommit {
    std::uint32_t segmentId = 0;
    std::uint32_t line = 0;
    std::uint64_t sequence = 0;
    std::uint64_t hostTimeNs = 0;
    std::uint64_t wallTimeNs = 0;
    std::uint64_t deviceTimeNs = 0;
    std::uint64_t sweepPass = 0;
    std::uint32_t sweepStep = 0;
    bool passComplete = false;
    std::uint32_t averageCount = 1;
    float clippedFraction = 0.0F;
    bool replayed = false; ///< Kept while this client was away; see SpectrumFrame

    [[nodiscard]] sweeps::Metadata toMetadata() const;
    [[nodiscard]] static FrameCommit from(const sweeps::Metadata& in);
};

struct Ping {
    std::uint64_t id = 0;
    std::uint64_t clientNs = 0;

    [[nodiscard]] sweeps::Metadata toMetadata() const;
    [[nodiscard]] static Ping from(const sweeps::Metadata& in);
};

struct Pong {
    std::uint64_t id = 0;
    std::uint64_t clientNs = 0; ///< Echoed from the ping
    std::uint64_t serverNs = 0;
    std::uint64_t serverWallNs = 0;

    [[nodiscard]] sweeps::Metadata toMetadata() const;
    [[nodiscard]] static Pong from(const sweeps::Metadata& in);
};

struct Bye {
    std::string reason;

    [[nodiscard]] sweeps::Metadata toMetadata() const;
    [[nodiscard]] static Bye from(const sweeps::Metadata& in);
};

// ---- recordings on the server -------------------------------------------------------

struct RecordingFile {
    std::string name;
    std::uint64_t bytes = 0;
    std::uint64_t modifiedWallNs = 0;
};

/// What the server is recording, and what it has recorded.
struct ServerRecordings {
    bool available = false; ///< Whether the server has somewhere to record to
    bool active = false;
    std::string current;
    std::uint64_t lines = 0;
    std::uint64_t bytes = 0;
    std::vector<RecordingFile> files;

    [[nodiscard]] sweeps::Metadata toMetadata() const;
    [[nodiscard]] static ServerRecordings from(const sweeps::Metadata& in);
};

/// A piece of a recording, in answer to `fetchRecording`.
struct Chunk {
    std::string name;
    std::uint64_t offset = 0;
    std::uint64_t totalBytes = 0;
    std::vector<std::byte> data;

    [[nodiscard]] sweeps::Metadata toMetadata() const;
    [[nodiscard]] static Chunk from(const sweeps::Metadata& in);
};

// ---- telemetry ------------------------------------------------------------------

/// The body of a Telemetry record (§4.9): what the server measured.
struct TelemetryReport {
    StreamStats stream;
    ProcessStats process;
    std::vector<SdrHealthReading> health;
    LinkStats link;                ///< The server's half
    std::optional<HostStats> host; ///< The machine the server runs on
};

void appendTelemetry(std::vector<std::byte>& out, const TelemetryReport& report,
                     std::uint64_t monotonicNs);

/// The caller checks the record type.
[[nodiscard]] Result<TelemetryReport> decodeTelemetry(const sweeps::StreamRecord& record);

} // namespace sweeppp::remote
