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
    static constexpr std::array kLinkChange{kLink};
    static constexpr std::array kBenchmarkChange{kBenchmark};
    static constexpr std::array kRecordingChange{kRecordings};
    static constexpr std::array kControlChange{kControl, kClients};

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
    if (name == op::kSetLinkResolution) {
        return kLinkChange;
    }
    if (name == op::kStartBenchmark || name == op::kCancelBenchmark) {
        return kBenchmarkChange;
    }
    if (name == op::kStartRecording || name == op::kStopRecording || name == op::kDeleteRecording) {
        return kRecordingChange;
    }
    if (name == op::kTakeControl || name == op::kReleaseControl) {
        return kControlChange;
    }
    return {};
}

bool viewerMay(std::string_view name) noexcept {
    return name == op::kSetLinkResolution || name == op::kFetchRecording ||
           name == op::kTakeControl || name == op::kReleaseControl || name == op::kHistoryOpen ||
           name == op::kHistoryQuery || name == op::kHistoryClose;
}

// ---- the handshake -------------------------------------------------------------

Metadata Hello::toMetadata() const {
    Metadata out;
    out.setInt("protocolVersion", protocolVersion);
    out.setString("software", software);
    out.setString("kind", kind);
    out.setString("name", name);
    out.setString("clientId", clientId);
    return out;
}

Hello Hello::from(const Metadata& in) {
    return Hello{.protocolVersion = getU32(in, "protocolVersion"),
                 .software = in.getString("software"),
                 .kind = in.getString("kind"),
                 .name = in.getString("name"),
                 .clientId = in.getString("clientId")};
}

Metadata Welcome::toMetadata() const {
    Metadata out;
    out.setString("serverName", serverName);
    out.setBool("shared", shared);
    setU64(out, "serverNs", serverNs);
    return out;
}

Welcome Welcome::from(const Metadata& in) {
    return Welcome{.serverName = in.getString("serverName"),
                   .shared = in.getBool("shared"),
                   .serverNs = getU64(in, "serverNs")};
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

Metadata ControlState::toMetadata() const {
    Metadata out;
    out.setBool("shared", shared);
    out.setBool("you", you);
    out.setBool("held", held);
    out.setString("controller", controller);
    out.setString("controllerKind", controllerKind);
    return out;
}

ControlState ControlState::from(const Metadata& in) {
    return ControlState{.shared = in.getBool("shared"),
                        .you = in.getBool("you"),
                        .held = in.getBool("held"),
                        .controller = in.getString("controller"),
                        .controllerKind = in.getString("controllerKind")};
}

Metadata encodeClients(std::span<const ConnectedClient> clients) {
    std::vector<Value> rows;
    rows.reserve(clients.size());
    for (const ConnectedClient& client : clients) {
        Metadata row;
        setU64(row, "id", client.id);
        row.setString("name", client.name);
        row.setString("kind", client.kind);
        row.setString("address", client.address);
        row.setBool("controls", client.controls);
        row.setBool("you", client.you);
        rows.push_back(Value::ofHash(std::move(row)));
    }
    Metadata out;
    out.set("list", Value::ofArray(Value::Type::Hash, std::move(rows)));
    return out;
}

std::vector<ConnectedClient> decodeClients(const Metadata& in) {
    std::vector<ConnectedClient> clients;
    const Value* list = in.find("list");
    const std::vector<Value>* rows = list != nullptr ? list->asArray() : nullptr;
    if (rows == nullptr) {
        return clients;
    }
    for (const Value& row : *rows) {
        if (const Metadata* client = row.asHash()) {
            clients.push_back(ConnectedClient{.id = getU64(*client, "id"),
                                              .name = client->getString("name"),
                                              .kind = client->getString("kind"),
                                              .address = client->getString("address"),
                                              .controls = client->getBool("controls"),
                                              .you = client->getBool("you")});
        }
    }
    return clients;
}

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
    if (replayed) {
        out.setBool("replayed", true);
    }
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
                       .clippedFraction = static_cast<float>(in.getFloat("clippedFraction")),
                       .replayed = in.getBool("replayed")};
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

// ---- recordings on the server -------------------------------------------------------

Metadata ServerRecordings::toMetadata() const {
    std::vector<Value> rows;
    rows.reserve(files.size());
    for (const RecordingFile& file : files) {
        Metadata row;
        row.setString("name", file.name);
        setU64(row, "bytes", file.bytes);
        setU64(row, "modifiedWallNs", file.modifiedWallNs);
        rows.push_back(Value::ofHash(std::move(row)));
    }
    Metadata out;
    out.setBool("available", available);
    out.setBool("active", active);
    out.setString("current", current);
    setU64(out, "lines", lines);
    setU64(out, "bytes", bytes);
    out.set("files", Value::ofArray(Value::Type::Hash, std::move(rows)));
    return out;
}

ServerRecordings ServerRecordings::from(const Metadata& in) {
    ServerRecordings recordings{.available = in.getBool("available"),
                                .active = in.getBool("active"),
                                .current = in.getString("current"),
                                .lines = getU64(in, "lines"),
                                .bytes = getU64(in, "bytes")};
    const Value* files = in.find("files");
    const std::vector<Value>* rows = files != nullptr ? files->asArray() : nullptr;
    if (rows != nullptr) {
        for (const Value& row : *rows) {
            if (const Metadata* file = row.asHash()) {
                recordings.files.push_back(
                    RecordingFile{.name = file->getString("name"),
                                  .bytes = getU64(*file, "bytes"),
                                  .modifiedWallNs = getU64(*file, "modifiedWallNs")});
            }
        }
    }
    return recordings;
}

Metadata Chunk::toMetadata() const {
    Metadata out;
    out.setString("name", name);
    setU64(out, "offset", offset);
    setU64(out, "totalBytes", totalBytes);
    out.setBytes("data", data);
    return out;
}

Chunk Chunk::from(const Metadata& in) {
    Chunk chunk{.name = in.getString("name"),
                .offset = getU64(in, "offset"),
                .totalBytes = getU64(in, "totalBytes")};
    if (const Value* data = in.find("data")) {
        if (const std::vector<std::byte>* bytes = data->asBytes()) {
            chunk.data = *bytes;
        }
    }
    return chunk;
}

// ---- telemetry --------------------------------------------------------------------

namespace {

Metadata encodeHost(const HostStats& host) {
    Metadata out;
    out.setString("system", host.system);
    out.setInt("cores", host.cores);
    out.setFloat("cpuPercent", host.cpuPercent);
    out.setFloat("processCpuPercent", host.processCpuPercent);
    setU64(out, "memoryTotalBytes", host.memoryTotalBytes);
    setU64(out, "memoryUsedBytes", host.memoryUsedBytes);
    setU64(out, "processMemoryBytes", host.processMemoryBytes);
    out.setFloat("load1", host.load1);
    out.setFloat("uptimeSeconds", host.uptimeSeconds);
    if (host.temperatureC) {
        out.setFloat("temperatureC", *host.temperatureC);
    }
    setU64(out, "diskTotalBytes", host.diskTotalBytes);
    setU64(out, "diskFreeBytes", host.diskFreeBytes);
    return out;
}

HostStats decodeHost(const Metadata& in) {
    HostStats host;
    host.system = in.getString("system");
    host.cores = static_cast<std::uint32_t>(std::max<std::int64_t>(in.getInt("cores"), 0));
    host.cpuPercent = in.getFloat("cpuPercent", -1.0);
    host.processCpuPercent = in.getFloat("processCpuPercent", -1.0);
    host.memoryTotalBytes = getU64(in, "memoryTotalBytes");
    host.memoryUsedBytes = getU64(in, "memoryUsedBytes");
    host.processMemoryBytes = getU64(in, "processMemoryBytes");
    host.load1 = in.getFloat("load1", -1.0);
    host.uptimeSeconds = in.getFloat("uptimeSeconds", -1.0);
    if (in.find("temperatureC") != nullptr) {
        host.temperatureC = in.getFloat("temperatureC");
    }
    host.diskTotalBytes = getU64(in, "diskTotalBytes");
    host.diskFreeBytes = getU64(in, "diskFreeBytes");
    return host;
}

} // namespace

void appendTelemetry(std::vector<std::byte>& out, const TelemetryReport& report,
                     std::uint64_t monotonicNs) {
    Metadata link;
    setU64(link, "framesSent", report.link.framesSent);
    setU64(link, "passesCoalesced", report.link.passesCoalesced);
    setU64(link, "partialsCoalesced", report.link.partialsCoalesced);
    setU64(link, "eventsDropped", report.link.eventsDropped);
    setU64(link, "encodeNs", report.link.encodeNs);

    Metadata body;
    setU64(body, "monotonicNs", monotonicNs);
    body.setHash("stream", encodeStreamStats(report.stream));
    body.setHash("process", encodeProcessStats(report.process));
    body.setHash("health", encodeHealth(report.health));
    body.setHash("link", std::move(link));
    if (report.host) {
        body.setHash("host", encodeHost(*report.host));
    }

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
    report.link.encodeNs = getU64(link, "encodeNs");
    if (const Value* host = body->find("host"); host != nullptr && host->asHash() != nullptr) {
        report.host = decodeHost(*host->asHash());
    }
    return report;
}

} // namespace sweeppp::remote
