// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Version.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

/// The remote instrument protocol: `.sweeps` records over TCP (Appendix C of
/// the format specification), with the control messages in PluginData records
/// under one plugin id.
namespace sweeppp::remote {

inline constexpr std::string_view kPluginId = "org.sweeppp.remote";
inline constexpr std::uint32_t kProtocolVersion = kRemoteProtocolVersion;
inline constexpr std::uint16_t kDefaultPort = 7332;

// ---- limits -----------------------------------------------------------------

/// Largest handshake message a server accepts before the client has shown it
/// holds the token: a hello, never more.
inline constexpr std::uint32_t kMaxPreAuthRecordBytes = 4U * 1024U;
inline constexpr std::uint32_t kMaxClientRecordBytes = 4U * 1024U * 1024U;
inline constexpr std::uint32_t kMaxServerRecordBytes = 16U * 1024U * 1024U;

/// Metadata nesting a message may use; the protocol itself needs four.
inline constexpr unsigned kMaxMetadataDepth = 8;

/// A grid wider than this is refused by the receiver: 64 MiB of levels.
inline constexpr std::uint32_t kMaxGridBins = 16U * 1024U * 1024U;

/// The fewest bins a link may reduce frames to.
inline constexpr std::uint32_t kMinLinkBins = 1024;

inline constexpr std::size_t kMaxQueuedCommands = 256;

/// How many clients a shared server takes at once unless told otherwise.
inline constexpr std::size_t kDefaultMaxClients = 8;

/// One piece of a recording being downloaded, and how many a client may have
/// asked for and not yet received.
inline constexpr std::uint32_t kChunkBytes = 256U * 1024U;
inline constexpr std::size_t kMaxChunksInFlight = 4;

inline constexpr std::chrono::seconds kHandshakeTimeout{5};
inline constexpr std::chrono::seconds kSilenceTimeout{10};
inline constexpr std::chrono::milliseconds kRefusalDelay{1000};
inline constexpr std::chrono::milliseconds kPingInterval{1000};

/// How long a server keeps a radio running for a client that dropped without
/// saying goodbye, so a reconnect finds it still sweeping.
inline constexpr std::chrono::seconds kDefaultLinger{30};

// ---- message names -------------------------------------------------------------

namespace msg {
inline constexpr std::string_view kWelcome = "welcome";
inline constexpr std::string_view kRefused = "refused";
inline constexpr std::string_view kCommand = "command";
inline constexpr std::string_view kReply = "reply";
inline constexpr std::string_view kState = "state";
inline constexpr std::string_view kNotice = "notice";
inline constexpr std::string_view kFrame = "frame";
inline constexpr std::string_view kPing = "ping";
inline constexpr std::string_view kPong = "pong";
inline constexpr std::string_view kBye = "bye";
inline constexpr std::string_view kChunk = "chunk";
} // namespace msg

/// Why a server turned a connection away.
namespace refusal {
inline constexpr std::string_view kBusy = "busy";
inline constexpr std::string_view kVersion = "version";
inline constexpr std::string_view kLimit = "limit";
inline constexpr std::string_view kProtocol = "protocol";
inline constexpr std::string_view kShutdown = "shutdown";
} // namespace refusal

/// What kind of program a client is, as its hello says.
namespace client {
inline constexpr std::string_view kDesktop = "desktop";
inline constexpr std::string_view kWeb = "web";
} // namespace client

/// The commands a client may send, by `op`.
namespace op {
inline constexpr std::string_view kStart = "start";
inline constexpr std::string_view kStop = "stop";
inline constexpr std::string_view kRestart = "restart";
inline constexpr std::string_view kSetSweeping = "setSweeping";
inline constexpr std::string_view kApplySweepPlan = "applySweepPlan";
inline constexpr std::string_view kSweepRange = "sweepRange";
inline constexpr std::string_view kApplyPipelineConfig = "applyPipelineConfig";
inline constexpr std::string_view kSetFftBackend = "setFftBackend";
inline constexpr std::string_view kSetParameter = "setParameter";
inline constexpr std::string_view kResetTelemetry = "resetTelemetry";
inline constexpr std::string_view kSetCorrectionSettings = "setCorrectionSettings";
inline constexpr std::string_view kStartLearning = "startLearning";
inline constexpr std::string_view kCancelLearning = "cancelLearning";
inline constexpr std::string_view kClearAutoSpurs = "clearAutoSpurs";
inline constexpr std::string_view kClearCorrections = "clearCorrections";
inline constexpr std::string_view kSetUserAntennas = "setUserAntennas";
inline constexpr std::string_view kSetAssignments = "setAssignments";
inline constexpr std::string_view kRescanSwitchers = "rescanSwitchers";
/// The connection's, not the instrument's: how many bins frames are reduced
/// to before they are sent. Zero sends them whole.
inline constexpr std::string_view kSetLinkResolution = "setLinkResolution";
inline constexpr std::string_view kStartBenchmark = "startBenchmark";
inline constexpr std::string_view kCancelBenchmark = "cancelBenchmark";
inline constexpr std::string_view kStartRecording = "startRecording";
inline constexpr std::string_view kStopRecording = "stopRecording";
inline constexpr std::string_view kDeleteRecording = "deleteRecording";
/// Answered with a `chunk` of the file rather than in a state section.
inline constexpr std::string_view kFetchRecording = "fetchRecording";
/// On a shared server: become the one client whose commands change anything,
/// from whoever has it now. Immediate; the other is told.
inline constexpr std::string_view kTakeControl = "takeControl";
inline constexpr std::string_view kReleaseControl = "releaseControl";
} // namespace op

/// The parts of an instrument's state a `state` message carries, each a hash
/// under its own key, sent when it changes.
namespace section {
inline constexpr std::string_view kDevice = "device";
inline constexpr std::string_view kValues = "values";
inline constexpr std::string_view kRun = "run";
inline constexpr std::string_view kPlan = "plan";
inline constexpr std::string_view kSchedule = "schedule";
inline constexpr std::string_view kPipeline = "pipeline";
inline constexpr std::string_view kBackends = "backends";
inline constexpr std::string_view kCorrections = "corrections";
inline constexpr std::string_view kLearning = "learning";
inline constexpr std::string_view kAntennas = "antennas";
inline constexpr std::string_view kAssignments = "assignments";
inline constexpr std::string_view kSwitchers = "switchers";
inline constexpr std::string_view kRfPath = "rfPath";
inline constexpr std::string_view kLink = "link";
inline constexpr std::string_view kBenchmark = "benchmark";
inline constexpr std::string_view kRecordings = "recordings";
/// Who controls the radio, and whether it is this client.
inline constexpr std::string_view kControl = "control";
/// Everyone connected, controller and viewers.
inline constexpr std::string_view kClients = "clients";
} // namespace section

} // namespace sweeppp::remote
