// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <limits>
#include <random>
#include <string>
#include <sweeppp/core/EventBus.hpp>
#include <sweeppp/core/HostStats.hpp>
#include <sweeppp/history/EventMapping.hpp>
#include <sweeppp/remote/ClockMap.hpp>
#include <sweeppp/remote/FrameCodec.hpp>
#include <sweeppp/remote/Messages.hpp>
#include <sweeppp/remote/Protocol.hpp>
#include <sweeppp/remote/WireCodec.hpp>
#include <sweeps/FileFormat.hpp>
#include <sweeps/Records.hpp>
#include <sweeps/Stream.hpp>
#include <thread>
#include <vector>

using namespace sweeppp;
using namespace sweeppp::remote;

namespace {

/// Through the bytes and back, so a round trip tests the wire and not only
/// the in-memory hash.
Metadata wire(const Metadata& in) {
    std::vector<std::byte> bytes;
    in.encode(bytes);
    sweeps::ByteReader reader(bytes.data(), bytes.size());
    auto decoded = Metadata::decode(reader, kMaxMetadataDepth);
    REQUIRE(decoded.has_value());
    CHECK(reader.exhausted());
    return std::move(*decoded);
}

/// Every record in `bytes`, framed as a receiver would.
std::vector<sweeps::StreamRecord> recordsOf(const std::vector<std::byte>& bytes) {
    sweeps::RecordFramer framer;
    framer.feed(bytes.data(), bytes.size());
    std::vector<sweeps::StreamRecord> records;
    sweeps::StreamRecord record;
    while (true) {
        auto next = framer.next(record);
        REQUIRE(next.has_value());
        if (!*next) {
            break;
        }
        records.push_back(std::move(record));
    }
    CHECK(framer.buffered() == 0);
    return records;
}

std::size_t countOf(const std::vector<sweeps::StreamRecord>& records, sweeps::RecordType type) {
    return static_cast<std::size_t>(
        std::ranges::count_if(records, [type](const sweeps::StreamRecord& record) {
            return record.header.type == static_cast<std::uint16_t>(type);
        }));
}

/// Applies a stream to a mirror and returns the frame the last commit made.
std::shared_ptr<SpectrumFrame> mirrorStream(FrameMirror& mirror,
                                            const std::vector<std::byte>& bytes) {
    std::shared_ptr<SpectrumFrame> last;
    for (const sweeps::StreamRecord& record : recordsOf(bytes)) {
        if (record.header.type == static_cast<std::uint16_t>(sweeps::RecordType::PluginData)) {
            auto message = decodeMessage(record);
            REQUIRE(message.has_value());
            REQUIRE(message->name == msg::kFrame);
            auto frame = mirror.commit(FrameCommit::from(message->body));
            REQUIRE(frame.has_value());
            last = *frame;
        } else {
            REQUIRE(mirror.apply(record).has_value());
        }
    }
    return last;
}

SpectrumFrame makeFrame(std::size_t bins, std::uint64_t sequence) {
    SpectrumFrame frame;
    frame.sequence = sequence;
    frame.hostTimeNs = 1'000'000 + sequence;
    frame.wallTimeNs = 2'000'000 + sequence;
    frame.sweepPass = sequence / 4;
    frame.sweepStep = static_cast<std::uint32_t>(sequence % 4);
    frame.passComplete = sequence % 4 == 3;
    frame.startHz = 100e6;
    frame.binWidthHz = 9765.625;
    frame.averageCount = 2;
    frame.clippedFraction = 0.125F;
    frame.config.fftSize = 1024;
    frame.config.sampleRate = 10e6;
    frame.config.deviceId = "synthetic";
    frame.binsDbfs.resize(bins);
    for (std::size_t i = 0; i < bins; ++i) {
        frame.binsDbfs[i] = -90.0F + (20.0F * std::sin(static_cast<float>(i) * 0.01F));
    }
    return frame;
}

std::vector<std::byte> tileRecord(const sweeps::TileHeader& header, std::size_t dataBytes) {
    std::vector<std::uint8_t> data(dataBytes, 100);
    std::vector<std::byte> payload;
    sweeps::encodeTile(payload, header, data.data(), data.size());
    std::vector<std::byte> out;
    sweeps::appendRecord(out, static_cast<std::uint16_t>(sweeps::RecordType::Tile), payload.data(),
                         payload.size());
    return out;
}

} // namespace

// ---------------------------------------------------------------- the codec

TEST_CASE("a plan crosses the wire bit for bit") {
    SweepPlan plan;
    plan.name = "ISM";
    plan.segments = {SweepSegment{.startHz = 433.05e6, .stopHz = 434.79e6, .dwellSeconds = 0.1},
                     SweepSegment{.startHz = 2.4e9, .stopHz = 2.4835e9}};
    plan.mode = SweepMode::Detail;
    plan.rbwHz = 1.0 / 3.0;
    plan.sampleRate = 61.44e6;
    plan.usableBandwidthFraction = 0.8;
    plan.stepOverlap = 1e-300;
    plan.dwellSeconds = 0.25;
    plan.averageCount = 7;
    plan.window = WindowType::Kaiser;
    plan.windowBeta = 12.5;
    plan.fftOverlap = 0.5;
    plan.overlapResolution = SweepPlan::OverlapResolution::Mean;
    plan.continuous = false;
    plan.antennaRouting = true;
    plan.portStrategy = SweepPlan::PortStrategy::FewestSwitches;

    const SweepPlan back = decodePlan(wire(encodePlan(plan)));
    CHECK(back.name == plan.name);
    REQUIRE(back.segments.size() == 2);
    CHECK(back.segments[0].startHz == plan.segments[0].startHz);
    CHECK(back.segments[0].dwellSeconds == plan.segments[0].dwellSeconds);
    CHECK(back.segments[1].stopHz == plan.segments[1].stopHz);
    CHECK(back.mode == plan.mode);
    CHECK(back.rbwHz == plan.rbwHz);
    CHECK(back.sampleRate == plan.sampleRate);
    CHECK(back.usableBandwidthFraction == plan.usableBandwidthFraction);
    CHECK(back.stepOverlap == plan.stepOverlap);
    CHECK(back.dwellSeconds == plan.dwellSeconds);
    CHECK(back.averageCount == plan.averageCount);
    CHECK(back.window == plan.window);
    CHECK(back.windowBeta == plan.windowBeta);
    CHECK(back.fftOverlap == plan.fftOverlap);
    CHECK(back.overlapResolution == plan.overlapResolution);
    CHECK(back.continuous == plan.continuous);
    CHECK(back.antennaRouting == plan.antennaRouting);
    CHECK(back.portStrategy == plan.portStrategy);
}

TEST_CASE("missing keys and enums out of range decode to the defaults") {
    const SweepPlan defaults;
    const SweepPlan plan = decodePlan(Metadata{});
    CHECK(plan.segments.empty());
    CHECK(plan.rbwHz == defaults.rbwHz);
    CHECK(plan.continuous == defaults.continuous);

    Metadata hostile;
    hostile.setInt("mode", 99);
    hostile.setInt("window", -1);
    hostile.setString("rbwHz", "not a number");
    hostile.setInt("averageCount", -5);
    hostile.setString("segments", "not an array");
    const SweepPlan odd = decodePlan(wire(hostile));
    CHECK(odd.mode == defaults.mode);
    CHECK(odd.window == defaults.window);
    CHECK(odd.rbwHz == defaults.rbwHz);
    CHECK(odd.averageCount == defaults.averageCount);
    CHECK(odd.segments.empty());

    const PipelineConfig pipeline = decodePipeline(Metadata{});
    CHECK(pipeline.fftSize == PipelineConfig{}.fftSize);
    const CorrectionSettings corrections = decodeCorrectionSettings(Metadata{});
    CHECK(corrections.flatten == CorrectionSettings{}.flatten);
}

TEST_CASE("a radio's descriptor round-trips, parameters and all") {
    DeviceDescriptor device;
    device.info.driver = "bladerf";
    device.info.id = "abc123";
    device.info.label = "bladeRF 2.0 micro xA9";
    device.info.firmware = VersionReport{.version = "2.4.0", .knownLatest = "2.5.0"};
    device.info.fpga.aheadOfDriver = true;
    device.info.minFrequencyHz = 47e6;
    device.info.maxFrequencyHz = 6e9;
    device.info.maxSampleRate = 61.44e6;
    device.info.linkCapacityBytesPerSec = 400'000'000;
    device.info.linkDescription = "USB 3.0 SuperSpeed";

    SdrParameter gain;
    gain.key = "gain";
    gain.label = "Gain";
    gain.type = SdrParameterType::Int;
    gain.unit = "dB";
    gain.min = -15;
    gain.max = 60;
    gain.step = 1;
    gain.calibrationAffecting = true;
    gain.defaultValue = std::int64_t{30};
    gain.appliesWhenKey = "agc";
    gain.appliesWhenValues = {"off", "manual"};

    SdrParameter port;
    port.key = "rx_port";
    port.type = SdrParameterType::Enum;
    port.enumValues = {SdrEnumValue{.value = "rx1", .label = "RX1", .description = "SMA"},
                       SdrEnumValue{.value = "rx2", .label = "RX2"}};
    port.defaultValue = std::string("rx1");
    port.requiresStop = true;
    device.parameters = {gain, port};
    device.rxPorts = {SdrRxPort{.id = "rx1", .label = "RX1", .minHz = 47e6, .maxHz = 6e9}};
    device.supportedSampleRates = {2e6, 61.44e6};

    const DeviceDescriptor back = decodeDevice(wire(encodeDevice(device)));
    CHECK(back.info.driver == "bladerf");
    CHECK(back.info.firmware.knownLatest == "2.5.0");
    CHECK(back.info.fpga.aheadOfDriver);
    CHECK(back.info.maxFrequencyHz == 6e9);
    CHECK(back.info.linkCapacityBytesPerSec == 400'000'000);
    REQUIRE(back.parameters.size() == 2);
    CHECK(back.parameters[0].type == SdrParameterType::Int);
    CHECK(back.parameters[0].calibrationAffecting);
    CHECK(asInt(back.parameters[0].defaultValue) == 30);
    CHECK(back.parameters[0].appliesWhenValues == gain.appliesWhenValues);
    REQUIRE(back.parameters[1].enumValues.size() == 2);
    CHECK(back.parameters[1].enumValues[0].description == "SMA");
    CHECK(asString(back.parameters[1].defaultValue) == "rx1");
    CHECK(back.parameters[1].requiresStop);
    REQUIRE(back.rxPorts.size() == 1);
    CHECK(back.rxPorts[0].maxHz == 6e9);
    CHECK(back.supportedSampleRates == device.supportedSampleRates);
}

TEST_CASE("parameter values keep their type") {
    for (const SdrValue& value : {SdrValue{true}, SdrValue{std::int64_t{-7}}, SdrValue{2.5e6},
                                  SdrValue{std::string("rx2")}}) {
        Metadata holder;
        holder.set("v", encodeValue(value));
        const Metadata back = wire(holder);
        REQUIRE(back.find("v") != nullptr);
        CHECK(equals(decodeValue(*back.find("v")), value));
        CHECK(decodeValue(*back.find("v")).index() == value.index());
    }
}

TEST_CASE("bench state round-trips: antennas, assignments, switchers and legs") {
    const Antenna discone{.id = "discone",
                          .name = "Discone",
                          .category = "Wideband",
                          .type = "discone",
                          .startHz = 25e6,
                          .stopHz = 1.3e9,
                          .gainDbi = 2.15,
                          .needsBiasT = true,
                          .notes = "roof",
                          .builtin = true};
    const std::vector<Antenna> antennas = decodeAntennas(wire(encodeAntennas({&discone, 1})));
    REQUIRE(antennas.size() == 1);
    CHECK(antennas[0].gainDbi == 2.15);
    CHECK(antennas[0].needsBiasT);
    CHECK(antennas[0].builtin);
    CHECK(antennas[0].notes == "roof");

    AntennaAssignments assignments;
    assignments.assign("bladerf:abc", "rx1", "discone");
    assignments.assignSwitcher("bladerf:abc", "rx2", "relay:1");
    assignments.assignInput("relay:1", "in3", "yagi");
    assignments.setFallbackPort("bladerf:abc", "rx2");
    const AntennaAssignments back = decodeAssignments(wire(encodeAssignments(assignments)));
    CHECK(back.antennaFor("bladerf:abc", "rx1") == "discone");
    CHECK(back.switcherFor("bladerf:abc", "rx2") == "relay:1");
    CHECK(back.antennaOnInput("relay:1", "in3") == "yagi");
    CHECK(back.fallbackPort("bladerf:abc") == "rx2");
    CHECK(back.entries().size() == assignments.entries().size());

    const SwitcherView view{
        .key = "relay:1",
        .info = RfPathInfo{.driver = "relay", .id = "1", .label = "Relay", .inputCount = 4},
        .inputs = {RfPathInput{.id = "in1", .label = "J1", .minHz = 0, .maxHz = 6e9}},
        .selectedInput = 2};
    const std::vector<SwitcherView> views =
        decodeSwitcherViews(wire(encodeSwitcherViews({&view, 1})));
    REQUIRE(views.size() == 1);
    CHECK(views[0].info.inputCount == 4);
    CHECK(views[0].inputs.at(0).maxHz == 6e9);
    CHECK(views[0].selectedInput == 2);

    RfLegView leg;
    leg.route.portIndex = 1;
    leg.route.id = "rx2";
    leg.route.inputIndex = kNoInput;
    leg.route.gainDbi = 3;
    leg.switcherKey = "relay:1";
    leg.antenna = discone;
    leg.portLabel = "RX2 via J3";
    leg.live = true;
    const std::vector<RfLegView> legs = decodeRfLegs(wire(encodeRfLegs({&leg, 1})));
    REQUIRE(legs.size() == 1);
    CHECK(legs[0].route.inputIndex == kNoInput);
    CHECK(legs[0].route.portIndex == 1);
    CHECK(legs[0].antenna.id == "discone");
    CHECK(legs[0].portLabel == "RX2 via J3");
    CHECK(legs[0].live);

    const std::vector<std::pair<double, double>> ranges{{1e6, 2e6}, {3e9, 4e9}};
    CHECK(decodeRanges(wire(encodeRanges(ranges))) == ranges);
}

TEST_CASE("run, correction and telemetry figures round-trip") {
    ScheduleSummary schedule;
    schedule.stepCount = 271;
    schedule.fftSize = 16384;
    schedule.actualRbwHz = 5625.0;
    schedule.gridBinCount = 1'581'334;
    schedule.unroutedHz = {{585.5e6, 613.8e6}};
    schedule.portSwitches = 1;
    const ScheduleSummary scheduleBack = decodeSchedule(wire(encodeSchedule(schedule)));
    CHECK(scheduleBack.stepCount == 271);
    CHECK(scheduleBack.gridBinCount == 1'581'334);
    CHECK(scheduleBack.unroutedHz == schedule.unroutedHz);

    const EngineStats stats{.stitched = 5, .tooShort = 1, .lastPassCoverage = 0.99, .passCount = 9};
    const EngineStats statsBack = decodeEngineStats(wire(encodeEngineStats(stats)));
    CHECK(statsBack.stitched == 5);
    CHECK(statsBack.lastPassCoverage == 0.99);
    CHECK(statsBack.passCount == 9);

    const CorrectionSummary summary{
        .present = true, .floorPoints = 512, .automaticSpurs = 3, .floorStaleReason = "gain"};
    const CorrectionSummary summaryBack =
        decodeCorrectionSummary(wire(encodeCorrectionSummary(summary)));
    CHECK(summaryBack.present);
    CHECK(summaryBack.floorPoints == 512);
    CHECK(summaryBack.automaticSpurs == 3);
    CHECK(summaryBack.floorStaleReason == "gain");

    const CorrectionSettings settings{
        .dcRemoval = false, .flatten = true, .spurMask = false, .autoSpurs = true};
    const CorrectionSettings settingsBack =
        decodeCorrectionSettings(wire(encodeCorrectionSettings(settings)));
    CHECK(settingsBack.toBits() == settings.toBits());

    PipelineConfig pipeline;
    pipeline.fftSize = 16384;
    pipeline.throttleMode = ThrottleMode::EveryNth;
    pipeline.everyNth = 3;
    pipeline.dbfsToDbmOffset = -12.75;
    const PipelineConfig pipelineBack = decodePipeline(wire(encodePipeline(pipeline)));
    CHECK(pipelineBack.fftSize == 16384);
    CHECK(pipelineBack.throttleMode == ThrottleMode::EveryNth);
    CHECK(pipelineBack.everyNth == 3);
    CHECK(pipelineBack.dbfsToDbmOffset == -12.75);

    FftBackendInfo backend;
    backend.name = "fftw";
    backend.capabilities.type = FftBackendType::Gpu;
    backend.capabilities.maxSize = 1U << 20U;
    backend.capabilities.sizeConstraint = FftSizeConstraint::PowerOfTwo;
    backend.available = true;
    const std::vector<FftBackendInfo> backends =
        decodeBackends(wire(encodeBackends({&backend, 1})));
    REQUIRE(backends.size() == 1);
    CHECK(backends[0].capabilities.type == FftBackendType::Gpu);
    CHECK(backends[0].capabilities.maxSize == (1U << 20U));
    CHECK(backends[0].capabilities.sizeConstraint == FftSizeConstraint::PowerOfTwo);

    StreamStats stream;
    stream.samplesDelivered = std::numeric_limits<std::uint64_t>::max();
    stream.ringFillFraction = 0.5F;
    const StreamStats streamBack = decodeStreamStats(wire(encodeStreamStats(stream)));
    CHECK(streamBack.samplesDelivered == stream.samplesDelivered);
    CHECK(streamBack.ringFillFraction == 0.5F);

    ProcessStats process;
    process.throttleReason = ThrottleReason::CpuLimited;
    process.fftLatencyP99Us = 42.5F;
    process.sweepSpeedHzPerSec = 6e10;
    const ProcessStats processBack = decodeProcessStats(wire(encodeProcessStats(process)));
    CHECK(processBack.throttleReason == ThrottleReason::CpuLimited);
    CHECK(processBack.fftLatencyP99Us == 42.5F);
    CHECK(processBack.sweepSpeedHzPerSec == 6e10);

    const SdrHealthReading reading{
        .label = "Temp", .value = "41 C", .numeric = 41, .maximum = 85, .alarm = true};
    const std::vector<SdrHealthReading> health = decodeHealth(wire(encodeHealth({&reading, 1})));
    REQUIRE(health.size() == 1);
    CHECK(health[0].alarm);
    CHECK(health[0].maximum == 85.0F);

    const InstrumentNotice notice{.kind = InstrumentNotice::Kind::Condition, .text = "drift"};
    const InstrumentNotice noticeBack = decodeNotice(wire(encodeNotice(notice)));
    CHECK(noticeBack.kind == InstrumentNotice::Kind::Condition);
    CHECK(noticeBack.text == "drift");
}

// ------------------------------------------------------------- the messages

TEST_CASE("the server's host figures ride the telemetry record, and are optional") {
    TelemetryReport report;
    report.link.framesSent = 9;
    report.host = HostStats{.system = "Linux 6.6 aarch64",
                            .cores = 4,
                            .cpuPercent = 37.5,
                            .processCpuPercent = 112.0,
                            .memoryTotalBytes = 8ULL << 30U,
                            .memoryUsedBytes = 3ULL << 30U,
                            .processMemoryBytes = 250ULL << 20U,
                            .load1 = 1.25,
                            .uptimeSeconds = 86400.5,
                            .temperatureC = 61.2,
                            .diskTotalBytes = 64ULL << 30U,
                            .diskFreeBytes = 40ULL << 30U};

    const auto roundTrip = [](const TelemetryReport& in) {
        std::vector<std::byte> bytes;
        appendTelemetry(bytes, in, 123);
        sweeps::RecordFramer framer(kMaxServerRecordBytes);
        framer.feed(bytes.data(), bytes.size());
        sweeps::StreamRecord record;
        REQUIRE(framer.next(record).value_or(false));
        auto decoded = decodeTelemetry(record);
        REQUIRE(decoded.has_value());
        return *decoded;
    };

    const TelemetryReport back = roundTrip(report);
    REQUIRE(back.host.has_value());
    CHECK(back.host->system == "Linux 6.6 aarch64");
    CHECK(back.host->cores == 4);
    CHECK(back.host->cpuPercent == doctest::Approx(37.5));
    CHECK(back.host->processCpuPercent == doctest::Approx(112.0));
    CHECK(back.host->memoryTotalBytes == 8ULL << 30U);
    CHECK(back.host->memoryUsedBytes == 3ULL << 30U);
    CHECK(back.host->processMemoryBytes == 250ULL << 20U);
    CHECK(back.host->load1 == doctest::Approx(1.25));
    CHECK(back.host->uptimeSeconds == doctest::Approx(86400.5));
    REQUIRE(back.host->temperatureC.has_value());
    CHECK(*back.host->temperatureC == doctest::Approx(61.2));
    CHECK(back.host->diskFreeBytes == 40ULL << 30U);
    CHECK(back.link.framesSent == 9);

    report.host->temperatureC.reset();
    CHECK_FALSE(roundTrip(report).host->temperatureC.has_value());
    report.host.reset();
    CHECK_FALSE(roundTrip(report).host.has_value());
}

TEST_CASE("this machine's own figures are sampled, and rates appear on the second call") {
    HostSampler sampler;
    const HostStats first = sampler.sample({});
    CHECK(first.cores > 0);
    CHECK(first.memoryTotalBytes > 0);
    CHECK(first.cpuPercent < 0.0);
#if defined(__APPLE__) || defined(__linux__)
    CHECK_FALSE(first.system.empty());
    CHECK(first.system.find('\0') == std::string::npos);
    CHECK(first.processMemoryBytes > 0);
    CHECK(first.memoryUsedBytes > 0);
    CHECK(first.memoryUsedBytes <= first.memoryTotalBytes);
    CHECK(first.uptimeSeconds > 0.0);
    CHECK(first.diskTotalBytes > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const HostStats second = sampler.sample({});
    CHECK(second.processCpuPercent >= 0.0);
#endif
}

TEST_CASE("control messages frame, decode and keep their fields") {
    std::vector<std::byte> bytes;
    Command command{.seq = 41, .op = std::string(op::kSetParameter)};
    command.args.setString("key", "gain");
    command.args.set("value", encodeValue(SdrValue{std::int64_t{20}}));
    appendMessage(bytes, msg::kCommand, command.toMetadata(), 123);

    const Reply reply{
        .seq = 41, .ok = false, .code = ErrorCode::OutOfRange, .message = "gain out of range"};
    appendMessage(bytes, msg::kReply, reply.toMetadata());

    const Refused refused{.reason = std::string(refusal::kBusy), .message = "in use"};
    appendMessage(bytes, msg::kRefused, refused.toMetadata());

    const std::vector<sweeps::StreamRecord> records = recordsOf(bytes);
    REQUIRE(records.size() == 3);

    auto first = decodeMessage(records[0]);
    REQUIRE(first.has_value());
    CHECK(first->name == msg::kCommand);
    CHECK(first->monotonicNs == 123);
    const Command commandBack = Command::from(first->body);
    CHECK(commandBack.seq == 41);
    CHECK(commandBack.op == op::kSetParameter);
    CHECK(commandBack.args.getString("key") == "gain");

    auto second = decodeMessage(records[1]);
    REQUIRE(second.has_value());
    const Reply replyBack = Reply::from(second->body);
    CHECK_FALSE(replyBack.ok);
    CHECK(replyBack.code == ErrorCode::OutOfRange);
    CHECK(replyBack.message == "gain out of range");

    auto third = decodeMessage(records[2]);
    REQUIRE(third.has_value());
    const Refused refusedBack = Refused::from(third->body);
    CHECK(refusedBack.reason == refusal::kBusy);
    CHECK(refusedBack.message == "in use");
}

TEST_CASE("a record for another plugin or another protocol version is refused") {
    std::vector<std::byte> payload;
    REQUIRE(sweeps::encodePluginData(payload, "org.example.other", "hello", kProtocolVersion, 0,
                                     nullptr, 0)
                .has_value());
    sweeps::StreamRecord foreign;
    foreign.header.type = static_cast<std::uint16_t>(sweeps::RecordType::PluginData);
    foreign.payload = payload;
    auto decoded = decodeMessage(foreign);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code() == ErrorCode::ProtocolError);

    payload.clear();
    REQUIRE(
        sweeps::encodePluginData(payload, kPluginId, "hello", kProtocolVersion + 1, 0, nullptr, 0)
            .has_value());
    foreign.payload = payload;
    decoded = decodeMessage(foreign);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code() == ErrorCode::Unsupported);
}

TEST_CASE("a hello says which protocol and which software") {
    const Hello hello{.protocolVersion = kProtocolVersion, .software = "Sweep++ 0.2.0"};
    const Hello back = Hello::from(wire(hello.toMetadata()));
    CHECK(back.protocolVersion == kProtocolVersion);
    CHECK(back.software == "Sweep++ 0.2.0");
    CHECK(Hello::from(Metadata{}).protocolVersion == 0);
}

// ------------------------------------------------------------------ frames

TEST_CASE("an encoded frame is mirrored to within half a quantisation step") {
    SpectrumFrame frame = makeFrame(2500, 7);
    frame.binsDbfs[10] = kUnmeasuredDbfs;
    frame.binsDbfs[2499] = kUnmeasuredDbfs;

    FrameEncoder encoder;
    std::vector<std::byte> bytes;
    encoder.encode(frame, bytes);
    const std::vector<sweeps::StreamRecord> records = recordsOf(bytes);
    CHECK(countOf(records, sweeps::RecordType::SegmentOpen) == 1);
    CHECK(countOf(records, sweeps::RecordType::Tile) == 3);

    FrameMirror mirror;
    const std::shared_ptr<SpectrumFrame> back = mirrorStream(mirror, bytes);
    REQUIRE(back != nullptr);
    REQUIRE(back->binCount() == frame.binCount());
    for (std::size_t i = 0; i < frame.binCount(); ++i) {
        if (!measured(frame.binsDbfs[i])) {
            CHECK_FALSE(measured(back->binsDbfs[i]));
            continue;
        }
        CHECK(std::abs(back->binsDbfs[i] - frame.binsDbfs[i]) <= 0.25F + 1e-4F);
    }
    CHECK(back->sequence == 7);
    CHECK(back->hostTimeNs == frame.hostTimeNs);
    CHECK(back->wallTimeNs == frame.wallTimeNs);
    CHECK(back->sweepPass == frame.sweepPass);
    CHECK(back->sweepStep == frame.sweepStep);
    CHECK(back->passComplete == frame.passComplete);
    CHECK(back->startHz == frame.startHz);
    CHECK(back->binWidthHz == frame.binWidthHz);
    CHECK(back->averageCount == 2);
    CHECK(back->clippedFraction == 0.125F);
    CHECK(back->config.fftSize == 1024);
    CHECK(back->config.deviceId == "synthetic");
}

TEST_CASE("only the blocks that changed are sent again") {
    FrameEncoder encoder;
    FrameMirror mirror;
    SpectrumFrame frame = makeFrame(4096, 0);

    std::vector<std::byte> bytes;
    encoder.encode(frame, bytes);
    (void)mirrorStream(mirror, bytes);

    SUBCASE("nothing changed: a commit and nothing else") {
        bytes.clear();
        frame.sequence = 1;
        encoder.encode(frame, bytes);
        const std::vector<sweeps::StreamRecord> records = recordsOf(bytes);
        CHECK(records.size() == 1);
        CHECK(mirrorStream(mirror, bytes)->sequence == 1);
    }

    SUBCASE("one step's worth of a sweep: its block alone") {
        bytes.clear();
        for (std::size_t i = 1100; i < 1200; ++i) {
            frame.binsDbfs[i] = -30.0F;
        }
        encoder.encode(frame, bytes);
        const std::vector<sweeps::StreamRecord> records = recordsOf(bytes);
        CHECK(countOf(records, sweeps::RecordType::Tile) == 1);
        CHECK(countOf(records, sweeps::RecordType::SegmentOpen) == 0);
        const auto back = mirrorStream(mirror, bytes);
        CHECK(back->binsDbfs[1150] == doctest::Approx(-30.0F).epsilon(0.01));
        CHECK(std::abs(back->binsDbfs[3000] - frame.binsDbfs[3000]) <= 0.26F);
    }

    SUBCASE("a change too small to show is not sent") {
        bytes.clear();
        frame.binsDbfs[5] += 1e-5F;
        encoder.encode(frame, bytes);
        CHECK(countOf(recordsOf(bytes), sweeps::RecordType::Tile) == 0);
        CHECK(encoder.stats().tilesUnchanged == 1);
    }

    SUBCASE("a new config opens a segment and resends everything") {
        bytes.clear();
        frame.config.gains = {{"lna", 16.0}};
        encoder.encode(frame, bytes);
        const std::vector<sweeps::StreamRecord> records = recordsOf(bytes);
        CHECK(countOf(records, sweeps::RecordType::SegmentClose) == 1);
        CHECK(countOf(records, sweeps::RecordType::SegmentOpen) == 1);
        CHECK(countOf(records, sweeps::RecordType::Tile) == 4);
        const auto back = mirrorStream(mirror, bytes);
        REQUIRE(back->config.gains.size() == 1);
        CHECK(back->config.gains[0].second == 16.0);
        CHECK(mirror.segment()->id == 1);
    }

    SUBCASE("a new grid opens a segment the mirror resizes to") {
        bytes.clear();
        SpectrumFrame wider = makeFrame(5000, 2);
        encoder.encode(wider, bytes);
        const auto back = mirrorStream(mirror, bytes);
        CHECK(back->binCount() == 5000);
    }

    SUBCASE("after a reset the next frame is complete on its own") {
        encoder.reset();
        bytes.clear();
        encoder.encode(frame, bytes);
        FrameMirror fresh;
        const auto back = mirrorStream(fresh, bytes);
        REQUIRE(back != nullptr);
        CHECK(back->binCount() == 4096);
    }
}

TEST_CASE("a mirror refuses tiles and segments that do not fit") {
    FrameEncoder encoder;
    FrameMirror mirror;
    std::vector<std::byte> bytes;
    encoder.encode(makeFrame(1500, 0), bytes);
    (void)mirrorStream(mirror, bytes);

    const auto applyOne = [&](const std::vector<std::byte>& record) {
        sweeps::RecordFramer framer;
        framer.feed(record.data(), record.size());
        sweeps::StreamRecord out;
        auto next = framer.next(out);
        REQUIRE(next.has_value());
        REQUIRE(*next);
        return mirror.apply(out);
    };

    const sweeps::TileHeader good{
        .segmentId = 0, .lod = 0, .freqBlock = 1, .lines = 1, .bins = 476};
    CHECK(applyOne(tileRecord(good, 476)).has_value());

    sweeps::TileHeader header = good;
    header.freqBlock = 2;
    CHECK_FALSE(applyOne(tileRecord(header, 476)).has_value());

    header = good;
    header.freqBlock = 0xFFFFFFFFU;
    CHECK_FALSE(applyOne(tileRecord(header, 476)).has_value());

    header = good;
    header.bins = 400;
    CHECK_FALSE(applyOne(tileRecord(header, 400)).has_value());

    header = good;
    header.lod = 1;
    CHECK_FALSE(applyOne(tileRecord(header, 476)).has_value());

    header = good;
    header.lines = 2;
    CHECK_FALSE(applyOne(tileRecord(header, 952)).has_value());

    header = good;
    header.originDb = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(applyOne(tileRecord(header, 476)).has_value());

    header = good;
    header.segmentId = 9;
    header.freqBlock = 77;
    CHECK(applyOne(tileRecord(header, 476)).has_value());

    sweeps::SegmentInfo huge;
    huge.id = 5;
    huge.grid = sweeps::SegmentGrid{.startHz = 0, .binWidthHz = 1, .binCount = kMaxGridBins + 1};
    std::vector<std::byte> payload;
    sweeps::encodeSegmentOpen(payload, huge);
    std::vector<std::byte> record;
    sweeps::appendRecord(record, static_cast<std::uint16_t>(sweeps::RecordType::SegmentOpen),
                         payload.data(), payload.size());
    CHECK_FALSE(applyOne(record).has_value());

    huge.grid = sweeps::SegmentGrid{.startHz = 0, .binWidthHz = 0, .binCount = 10};
    payload.clear();
    record.clear();
    sweeps::encodeSegmentOpen(payload, huge);
    sweeps::appendRecord(record, static_cast<std::uint16_t>(sweeps::RecordType::SegmentOpen),
                         payload.data(), payload.size());
    CHECK_FALSE(applyOne(record).has_value());

    CHECK_FALSE(mirror.commit(FrameCommit{.segmentId = 3}).has_value());
    CHECK(mirror.segment()->id == 0);
}

TEST_CASE("a damaged stream is refused, never crashed on") {
    // A realistic stream: two segments, partial updates, messages.
    FrameEncoder encoder;
    std::vector<std::byte> stream;
    for (std::uint64_t i = 0; i < 6; ++i) {
        SpectrumFrame frame = makeFrame(i < 4 ? 3000 : 2000, i);
        frame.binsDbfs[(i * 500) % frame.binCount()] = -10.0F;
        encoder.encode(frame, stream);
    }
    appendMessage(stream, msg::kState, State{.ackSeq = 3}.toMetadata());
    encoder.close(99, stream);

    std::mt19937 random(20261003);
    for (int trial = 0; trial < 10000; ++trial) {
        std::vector<std::byte> damaged = stream;
        if (trial % 2 == 0) {
            const std::size_t flips = 1 + (random() % 8);
            for (std::size_t f = 0; f < flips; ++f) {
                damaged[random() % damaged.size()] ^=
                    std::byte{static_cast<std::uint8_t>(1U << (random() % 8))};
            }
        } else {
            damaged.resize(random() % damaged.size());
        }

        sweeps::RecordFramer framer(kMaxServerRecordBytes);
        FrameMirror mirror;
        std::size_t offset = 0;
        while (offset < damaged.size()) {
            const std::size_t chunk =
                std::min<std::size_t>(1 + (random() % 4096), damaged.size() - offset);
            framer.feed(damaged.data() + offset, chunk);
            offset += chunk;
            sweeps::StreamRecord record;
            bool broken = false;
            while (true) {
                auto next = framer.next(record);
                if (!next) {
                    broken = true;
                    break;
                }
                if (!*next) {
                    break;
                }
                if (record.header.type ==
                    static_cast<std::uint16_t>(sweeps::RecordType::PluginData)) {
                    if (auto message = decodeMessage(record);
                        message && message->name == msg::kFrame) {
                        (void)mirror.commit(FrameCommit::from(message->body));
                    }
                } else {
                    (void)mirror.apply(record);
                }
            }
            if (broken) {
                break;
            }
        }
    }
    CHECK(true);
}

TEST_CASE("payloads with valid checksums but hostile contents are refused") {
    // The framer trusts a matching CRC, so each decoder has to stand on its
    // own: here every record is rebuilt with its checksum recomputed.
    FrameEncoder encoder;
    std::vector<std::byte> stream;
    encoder.encode(makeFrame(3000, 0), stream);
    appendMessage(stream, msg::kCommand,
                  Command{.seq = 1, .op = std::string(op::kStart)}.toMetadata());
    const std::vector<sweeps::StreamRecord> records = recordsOf(stream);

    std::mt19937 random(7);
    for (int trial = 0; trial < 5000; ++trial) {
        sweeps::StreamRecord record = records[random() % records.size()];
        if (record.payload.empty()) {
            continue;
        }
        const std::size_t edits = 1 + (random() % 6);
        for (std::size_t e = 0; e < edits; ++e) {
            record.payload[random() % record.payload.size()] =
                std::byte{static_cast<std::uint8_t>(random())};
        }
        if (random() % 4 == 0) {
            record.payload.resize(random() % record.payload.size());
        }

        FrameMirror mirror;
        if (record.header.type == static_cast<std::uint16_t>(sweeps::RecordType::PluginData)) {
            if (auto message = decodeMessage(record)) {
                (void)Command::from(message->body);
                (void)FrameCommit::from(message->body);
                (void)decodePlan(message->body);
            }
        } else {
            // The intact SegmentOpen first, so a damaged tile meets a grid.
            (void)mirror.apply(records[0]);
            (void)mirror.apply(record);
        }
    }
    CHECK(true);
}

// ------------------------------------------------------------------ clocks

TEST_CASE("the clock map takes its offset from the fastest round trip") {
    ClockMap clocks;
    CHECK_FALSE(clocks.calibrated());
    CHECK(clocks.toClient(5, 1000) == 1000);

    // Server clock runs 1 s behind the client's. A slow ping first, with its
    // reply delayed on the way back: an estimate skewed by 40 ms.
    clocks.observe(10'000'000'000, 9'000'000'000 + 10'000'000, 10'100'000'000);
    // Then a fast, symmetric one.
    clocks.observe(11'000'000'000, 10'000'000'000 + 1'000'000, 11'002'000'000);
    CHECK(clocks.calibrated());
    CHECK(clocks.bestRoundTripNs() == 2'000'000);
    CHECK(clocks.lastRoundTripNs() == 2'000'000);

    // An event the server stamped at 10.5 s happened at the client's 11.5 s.
    CHECK(clocks.toClient(10'500'000'000, 12'000'000'000) == 11'500'000'000);
}

TEST_CASE("mapped times never run backwards and never reach the future") {
    ClockMap clocks;
    clocks.observe(2'000'000'000, 1'000'000'000, 2'000'000'000);

    CHECK(clocks.toClient(1'500'000'000, 3'000'000'000) == 2'500'000'000);
    // Out of order on the server's side: held at what was already returned.
    CHECK(clocks.toClient(1'400'000'000, 3'000'000'000) == 2'500'000'000);
    // Ahead of the client's now: held at now.
    CHECK(clocks.toClient(9'000'000'000, 3'100'000'000) == 3'100'000'000);

    clocks.reset();
    CHECK_FALSE(clocks.calibrated());
    CHECK(clocks.toClient(1, 7) == 7);
}

// ------------------------------------------------------------------ events

TEST_CASE("bus events survive a trip through Event records") {
    using namespace sweeppp::session;
    EventBus bus;
    std::vector<ParameterChangedEvent> parameters;
    std::vector<SweepPassEvent> passes;
    std::vector<DeviceErrorEvent> errors;
    const auto a = bus.subscribe<ParameterChangedEvent>(
        [&](const ParameterChangedEvent& e) { parameters.push_back(e); });
    const auto b =
        bus.subscribe<SweepPassEvent>([&](const SweepPassEvent& e) { passes.push_back(e); });
    const auto c =
        bus.subscribe<DeviceErrorEvent>([&](const DeviceErrorEvent& e) { errors.push_back(e); });

    const auto through = [](const SessionEvent& event) {
        std::vector<std::byte> payload;
        encodeEvent(payload, event);
        sweeps::ByteReader reader(payload.data(), payload.size());
        auto decoded = decodeEvent(reader);
        REQUIRE(decoded.has_value());
        return std::move(*decoded);
    };

    CHECK(publishSessionEvent(
        bus,
        through(toSessionEvent(
            ParameterChangedEvent{
                .monotonicNs = 5, .key = "gain", .value = "20", .calibrationAffecting = true},
            77)),
        900));
    CHECK(publishSessionEvent(
        bus,
        through(toSessionEvent(
            SweepPassEvent{.passId = 3, .startHz = 1e6, .stopHz = 6e9, .durationSeconds = 0.086},
            0)),
        901));
    CHECK(publishSessionEvent(
        bus, through(toSessionEvent(DeviceErrorEvent{.deviceId = "x", .message = "lost"}, 0)),
        902));
    CHECK_FALSE(publishSessionEvent(
        bus, SessionEvent::of(SessionEvent::Kind::SegmentBoundary, 0, 0, SegmentBoundaryData{}),
        0));

    REQUIRE(parameters.size() == 1);
    CHECK(parameters[0].monotonicNs == 900);
    CHECK(parameters[0].key == "gain");
    CHECK(parameters[0].calibrationAffecting);
    REQUIRE(passes.size() == 1);
    CHECK(passes[0].passId == 3);
    CHECK(passes[0].stopHz == 6e9);
    CHECK(passes[0].durationSeconds == 0.086);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].message == "lost");

    bus.unsubscribe(a);
    bus.unsubscribe(b);
    bus.unsubscribe(c);
}

TEST_CASE("an encoder told what changed sends exactly what one comparing everything does") {
    FrameEncoder told;
    FrameEncoder blind;
    std::vector<std::byte> withRange;
    std::vector<std::byte> without;

    SpectrumFrame frame = makeFrame(10'000, 0);
    told.encode(frame, withRange, ChangedBins::unknown());
    blind.encode(frame, without);

    for (std::uint64_t i = 1; i < 40; ++i) {
        frame.sequence = i;
        frame.hostTimeNs += 1000;
        const std::size_t first = (i * 731) % 9000;
        const std::size_t end = first + 600;
        for (std::size_t bin = first; bin < end; ++bin) {
            frame.binsDbfs[bin] = -50.0F - static_cast<float>(i % 7);
        }
        told.encode(frame, withRange, ChangedBins{.first = first, .end = end});
        blind.encode(frame, without);
    }
    CHECK(withRange == without);

    ChangedBins merged;
    CHECK(merged.empty());
    merged.add(ChangedBins{.first = 10, .end = 20});
    merged.add(ChangedBins{.first = 50, .end = 60});
    CHECK(merged.first == 10);
    CHECK(merged.end == 60);
    merged.add(ChangedBins::unknown());
    CHECK_FALSE(merged.known);
    merged.add(ChangedBins{.first = 1, .end = 2});
    CHECK_FALSE(merged.known);
}

TEST_CASE("a frame reduced for a slow link keeps its peaks and its gaps") {
    SpectrumFrame frame = makeFrame(10, 3);
    frame.binWidthHz = 1000.0;
    frame.binsDbfs = {
        -90, -20, -90, kUnmeasuredDbfs, kUnmeasuredDbfs, kUnmeasuredDbfs, -80, kUnmeasuredDbfs,
        -70, -60};

    ChangedBins changed{.first = 4, .end = 7};
    const SpectrumFrame reduced = reduceFrame(frame, 4, changed);

    // ceil(10 / 4) = 3 bins to a group, four groups, the last one short.
    REQUIRE(reduced.binCount() == 4);
    CHECK(reduced.binWidthHz == 3000.0);
    CHECK(reduced.startHz == frame.startHz);
    CHECK(reduced.binsDbfs[0] == -20.0F);
    CHECK_FALSE(measured(reduced.binsDbfs[1]));
    CHECK(reduced.binsDbfs[2] == -70.0F);
    CHECK(reduced.binsDbfs[3] == -60.0F);
    CHECK(reduced.sequence == frame.sequence);
    CHECK(reduced.passComplete == frame.passComplete);
    CHECK(changed.first == 1);
    CHECK(changed.end == 3);

    ChangedBins whole = ChangedBins::unknown();
    CHECK(reduceFrame(frame, 100, whole).binCount() == 10);
    CHECK_FALSE(whole.known);
}
