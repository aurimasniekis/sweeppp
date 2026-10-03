// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/remote/Messages.hpp"

#include "sweeppp/remote/Protocol.hpp"
#include "sweeppp/remote/WireCodec.hpp"

#include <algorithm>
#include <array>
#include <sweeps/FileFormat.hpp>
#include <sweeps/Records.hpp>

namespace sweeppp::remote {
namespace {

using sweeps::Metadata;
using sweeps::Value;

template <std::size_t N>
std::vector<std::byte> bytesOf(const std::array<std::uint8_t, N>& in) {
    std::vector<std::byte> out(N);
    std::ranges::transform(in, out.begin(), [](std::uint8_t b) { return std::byte{b}; });
    return out;
}

/// Exactly N bytes under `key`, or zeros.
template <std::size_t N>
std::array<std::uint8_t, N> fixedBytes(const Metadata& in, std::string_view key) {
    std::array<std::uint8_t, N> out{};
    const Value* value = in.find(key);
    const std::vector<std::byte>* bytes = value != nullptr ? value->asBytes() : nullptr;
    if (bytes != nullptr && bytes->size() == N) {
        std::ranges::transform(*bytes, out.begin(),
                               [](std::byte b) { return std::to_integer<std::uint8_t>(b); });
    }
    return out;
}

void setU64(Metadata& out, std::string key, std::uint64_t value) {
    out.setInt(std::move(key), static_cast<std::int64_t>(value));
}

std::uint64_t getU64(const Metadata& in, std::string_view key) {
    return static_cast<std::uint64_t>(in.getInt(key));
}

std::uint32_t getU32(const Metadata& in, std::string_view key, std::uint32_t fallback = 0) {
    const std::int64_t value = in.getInt(key, fallback);
    return value < 0 || value > 0xFFFFFFFFLL ? fallback : static_cast<std::uint32_t>(value);
}

} // namespace

void appendMessage(std::vector<std::byte>& out, std::string_view name, const Metadata& body,
                   std::uint64_t monotonicNs) {
    std::vector<std::byte> encodedBody;
    body.encode(encodedBody);
    std::vector<std::byte> payload;
    // The plugin id is a constant and every body far below the u32 limit, so
    // the only ways this can fail are not reachable from here.
    (void)sweeps::encodePluginData(payload, kPluginId, name, kProtocolVersion, monotonicNs,
                                   encodedBody.data(), encodedBody.size());
    sweeps::appendRecord(out, static_cast<std::uint16_t>(sweeps::RecordType::PluginData),
                         payload.data(), payload.size());
}

Result<Message> decodeMessage(const sweeps::StreamRecord& record) {
    sweeps::ByteReader reader(record.payload.data(), record.payload.size());
    auto view = adopt(sweeps::decodePluginData(reader));
    if (!view) {
        return std::unexpected(std::move(view).error());
    }
    if (view->pluginId != kPluginId) {
        return fail<Message>(ErrorCode::ProtocolError, "a record for '{}' on the control stream",
                             view->pluginId);
    }
    if (view->schemaVersion != kProtocolVersion) {
        return fail<Message>(ErrorCode::Unsupported, "protocol version {}, this end speaks {}",
                             view->schemaVersion, kProtocolVersion);
    }

    sweeps::ByteReader body(view->body, view->bodyBytes);
    auto decoded = adopt(Metadata::decode(body, kMaxMetadataDepth));
    if (!decoded) {
        return fail<Message>(ErrorCode::ProtocolError, "'{}': {}", view->recordName,
                             decoded.error().message());
    }
    return Message{.name = std::move(view->recordName),
                   .monotonicNs = view->monotonicNs,
                   .body = std::move(*decoded)};
}

std::span<const std::string_view> sectionsTouchedBy(std::string_view name) noexcept {
    using namespace section;
    static constexpr std::array kRunChange{kRun, kPlan, kSchedule};
    static constexpr std::array kPlanChange{kPlan, kSchedule, kRun};
    static constexpr std::array kPipelineChange{kPipeline};
    static constexpr std::array kBackendChange{kBackends};
    static constexpr std::array kParameterChange{kValues, kPlan, kSchedule, kRun};
    static constexpr std::array kCorrectionChange{kCorrections, kLearning};
    static constexpr std::array kAntennaChange{kAntennas, kRfPath};
    static constexpr std::array kAssignmentChange{kAssignments, kRfPath, kPlan, kSchedule};
    static constexpr std::array kSwitcherChange{kSwitchers, kRfPath};

    if (name == op::kStart || name == op::kStop || name == op::kRestart ||
        name == op::kSetSweeping) {
        return kRunChange;
    }
    if (name == op::kApplySweepPlan || name == op::kSweepRange) {
        return kPlanChange;
    }
    if (name == op::kApplyPipelineConfig) {
        return kPipelineChange;
    }
    if (name == op::kSetFftBackend) {
        return kBackendChange;
    }
    if (name == op::kSetParameter) {
        return kParameterChange;
    }
    if (name == op::kSetCorrectionSettings || name == op::kStartLearning ||
        name == op::kCancelLearning || name == op::kClearAutoSpurs ||
        name == op::kClearCorrections) {
        return kCorrectionChange;
    }
    if (name == op::kSetUserAntennas) {
        return kAntennaChange;
    }
    if (name == op::kSetAssignments) {
        return kAssignmentChange;
    }
    if (name == op::kRescanSwitchers) {
        return kSwitcherChange;
    }
    return {};
}

crypto::Sha256Digest authMac(std::string_view token, const Nonce& serverNonce,
                             const Nonce& clientNonce) {
    std::vector<std::uint8_t> message;
    message.reserve(kAuthContext.size() + serverNonce.size() + clientNonce.size());
    message.insert(message.end(), kAuthContext.begin(), kAuthContext.end());
    message.insert(message.end(), serverNonce.begin(), serverNonce.end());
    message.insert(message.end(), clientNonce.begin(), clientNonce.end());
    return crypto::hmacSha256(crypto::bytesOf(token), message);
}

// ---- the handshake -------------------------------------------------------------

Metadata Hello::toMetadata() const {
    Metadata out;
    out.setInt("protocolVersion", protocolVersion);
    out.setString("software", software);
    return out;
}

Hello Hello::from(const Metadata& in) {
    return Hello{.protocolVersion = getU32(in, "protocolVersion"),
                 .software = in.getString("software")};
}

Metadata Challenge::toMetadata() const {
    Metadata out;
    out.setInt("protocolVersion", protocolVersion);
    out.setBytes("serverNonce", bytesOf(serverNonce));
    out.setBool("authRequired", authRequired);
    out.setString("software", software);
    return out;
}

Challenge Challenge::from(const Metadata& in) {
    return Challenge{.protocolVersion = getU32(in, "protocolVersion"),
                     .serverNonce = fixedBytes<kNonceBytes>(in, "serverNonce"),
                     .authRequired = in.getBool("authRequired", true),
                     .software = in.getString("software")};
}

Metadata Auth::toMetadata() const {
    Metadata out;
    out.setBytes("clientNonce", bytesOf(clientNonce));
    out.setBytes("mac", bytesOf(mac));
    return out;
}

Auth Auth::from(const Metadata& in) {
    return Auth{.clientNonce = fixedBytes<kNonceBytes>(in, "clientNonce"),
                .mac = fixedBytes<32>(in, "mac")};
}

Metadata Welcome::toMetadata() const {
    Metadata out;
    out.setString("serverName", serverName);
    return out;
}

Welcome Welcome::from(const Metadata& in) {
    return Welcome{.serverName = in.getString("serverName")};
}

Metadata Refused::toMetadata() const {
    Metadata out;
    out.setString("reason", reason);
    out.setString("message", message);
    return out;
}

Refused Refused::from(const Metadata& in) {
    return Refused{.reason = in.getString("reason"), .message = in.getString("message")};
}

// ---- control ---------------------------------------------------------------------

Metadata Command::toMetadata() const {
    Metadata out;
    setU64(out, "seq", seq);
    out.setString("op", op);
    out.setHash("args", args);
    return out;
}

Command Command::from(const Metadata& in) {
    return Command{.seq = getU64(in, "seq"), .op = in.getString("op"), .args = hashAt(in, "args")};
}

Metadata Reply::toMetadata() const {
    Metadata out;
    setU64(out, "seq", seq);
    out.setBool("ok", ok);
    out.setInt("code", static_cast<std::int64_t>(code));
    out.setString("message", message);
    return out;
}

Reply Reply::from(const Metadata& in) {
    const std::int64_t code = in.getInt("code");
    const bool known = code >= 0 && code <= static_cast<std::int64_t>(ErrorCode::Corrupt);
    return Reply{.seq = getU64(in, "seq"),
                 .ok = in.getBool("ok"),
                 .code = known ? static_cast<ErrorCode>(code) : ErrorCode::Unknown,
                 .message = in.getString("message")};
}

Metadata State::toMetadata() const {
    Metadata out;
    setU64(out, "ackSeq", ackSeq);
    out.setHash("sections", sections);
    return out;
}

State State::from(const Metadata& in) {
    return State{.ackSeq = getU64(in, "ackSeq"), .sections = hashAt(in, "sections")};
}

Metadata FrameCommit::toMetadata() const {
    Metadata out;
    out.setInt("segmentId", segmentId);
    out.setInt("line", line);
    setU64(out, "sequence", sequence);
    setU64(out, "hostTimeNs", hostTimeNs);
    setU64(out, "wallTimeNs", wallTimeNs);
    setU64(out, "deviceTimeNs", deviceTimeNs);
    setU64(out, "sweepPass", sweepPass);
    out.setInt("sweepStep", sweepStep);
    out.setBool("passComplete", passComplete);
    out.setInt("averageCount", averageCount);
    out.setFloat("clippedFraction", static_cast<double>(clippedFraction));
    return out;
}

FrameCommit FrameCommit::from(const Metadata& in) {
    return FrameCommit{.segmentId = getU32(in, "segmentId"),
                       .line = getU32(in, "line"),
                       .sequence = getU64(in, "sequence"),
                       .hostTimeNs = getU64(in, "hostTimeNs"),
                       .wallTimeNs = getU64(in, "wallTimeNs"),
                       .deviceTimeNs = getU64(in, "deviceTimeNs"),
                       .sweepPass = getU64(in, "sweepPass"),
                       .sweepStep = getU32(in, "sweepStep"),
                       .passComplete = in.getBool("passComplete"),
                       .averageCount = getU32(in, "averageCount", 1),
                       .clippedFraction = static_cast<float>(in.getFloat("clippedFraction"))};
}

Metadata Ping::toMetadata() const {
    Metadata out;
    setU64(out, "id", id);
    setU64(out, "clientNs", clientNs);
    return out;
}

Ping Ping::from(const Metadata& in) {
    return Ping{.id = getU64(in, "id"), .clientNs = getU64(in, "clientNs")};
}

Metadata Pong::toMetadata() const {
    Metadata out;
    setU64(out, "id", id);
    setU64(out, "clientNs", clientNs);
    setU64(out, "serverNs", serverNs);
    setU64(out, "serverWallNs", serverWallNs);
    return out;
}

Pong Pong::from(const Metadata& in) {
    return Pong{.id = getU64(in, "id"),
                .clientNs = getU64(in, "clientNs"),
                .serverNs = getU64(in, "serverNs"),
                .serverWallNs = getU64(in, "serverWallNs")};
}

Metadata Bye::toMetadata() const {
    Metadata out;
    out.setString("reason", reason);
    return out;
}

Bye Bye::from(const Metadata& in) {
    return Bye{.reason = in.getString("reason")};
}

// ---- telemetry --------------------------------------------------------------------

void appendTelemetry(std::vector<std::byte>& out, const TelemetryReport& report,
                     std::uint64_t monotonicNs) {
    Metadata link;
    setU64(link, "framesSent", report.link.framesSent);
    setU64(link, "passesCoalesced", report.link.passesCoalesced);
    setU64(link, "partialsCoalesced", report.link.partialsCoalesced);
    setU64(link, "eventsDropped", report.link.eventsDropped);

    Metadata body;
    setU64(body, "monotonicNs", monotonicNs);
    body.setHash("stream", encodeStreamStats(report.stream));
    body.setHash("process", encodeProcessStats(report.process));
    body.setHash("health", encodeHealth(report.health));
    body.setHash("link", std::move(link));

    std::vector<std::byte> payload;
    body.encode(payload);
    sweeps::appendRecord(out, static_cast<std::uint16_t>(sweeps::RecordType::Telemetry),
                         payload.data(), payload.size());
}

Result<TelemetryReport> decodeTelemetry(const sweeps::StreamRecord& record) {
    sweeps::ByteReader reader(record.payload.data(), record.payload.size());
    auto body = adopt(Metadata::decode(reader, kMaxMetadataDepth));
    if (!body) {
        return std::unexpected(std::move(body).error());
    }
    const Metadata& link = hashAt(*body, "link");
    TelemetryReport report;
    report.stream = decodeStreamStats(hashAt(*body, "stream"));
    report.process = decodeProcessStats(hashAt(*body, "process"));
    report.health = decodeHealth(hashAt(*body, "health"));
    report.link.framesSent = getU64(link, "framesSent");
    report.link.passesCoalesced = getU64(link, "passesCoalesced");
    report.link.partialsCoalesced = getU64(link, "partialsCoalesced");
    report.link.eventsDropped = getU64(link, "eventsDropped");
    return report;
}

} // namespace sweeppp::remote
