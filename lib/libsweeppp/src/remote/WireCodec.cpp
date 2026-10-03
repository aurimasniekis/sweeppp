// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/remote/WireCodec.hpp"

#include <string>
#include <type_traits>

namespace sweeppp::remote {
namespace {

using sweeps::Value;

// ---- helpers ----------------------------------------------------------------

void setU64(Metadata& out, std::string key, std::uint64_t value) {
    out.setInt(std::move(key), static_cast<std::int64_t>(value));
}

std::uint64_t getU64(const Metadata& in, std::string_view key, std::uint64_t fallback = 0) {
    return static_cast<std::uint64_t>(in.getInt(key, static_cast<std::int64_t>(fallback)));
}

std::uint32_t getU32(const Metadata& in, std::string_view key, std::uint32_t fallback = 0) {
    const std::int64_t value = in.getInt(key, fallback);
    return value < 0 || value > 0xFFFFFFFFLL ? fallback : static_cast<std::uint32_t>(value);
}

std::size_t getSize(const Metadata& in, std::string_view key, std::size_t fallback = 0) {
    return static_cast<std::size_t>(getU64(in, key, fallback));
}

float getF32(const Metadata& in, std::string_view key, float fallback = 0.0F) {
    return static_cast<float>(in.getFloat(key, static_cast<double>(fallback)));
}

template <typename E>
void setEnum(Metadata& out, std::string key, E value) {
    out.setInt(std::move(key), static_cast<std::int64_t>(value));
}

/// `count` values from zero; anything else is the fallback.
template <typename E>
E getEnum(const Metadata& in, std::string_view key, E fallback, std::int64_t count) {
    const std::int64_t value = in.getInt(key, static_cast<std::int64_t>(fallback));
    return value >= 0 && value < count ? static_cast<E>(value) : fallback;
}

template <typename T, typename Encode>
Value hashArray(std::span<const T> items, Encode encode) {
    std::vector<Value> elements;
    elements.reserve(items.size());
    for (const T& item : items) {
        elements.push_back(Value::ofHash(encode(item)));
    }
    return Value::ofArray(Value::Type::Hash, std::move(elements));
}

/// The hashes in the array under `key`, decoded; any other element is skipped.
template <typename Decode>
auto hashList(const Metadata& in, std::string_view key, Decode decode) {
    std::vector<std::invoke_result_t<Decode, const Metadata&>> out;
    const Value* value = in.find(key);
    const std::vector<Value>* elements = value != nullptr ? value->asArray() : nullptr;
    if (elements == nullptr) {
        return out;
    }
    out.reserve(elements->size());
    for (const Value& element : *elements) {
        if (const Metadata* hash = element.asHash()) {
            out.push_back(decode(*hash));
        }
    }
    return out;
}

Value stringArray(std::span<const std::string> items) {
    std::vector<Value> elements;
    elements.reserve(items.size());
    for (const std::string& item : items) {
        elements.push_back(Value::ofString(item));
    }
    return Value::ofArray(Value::Type::String, std::move(elements));
}

std::vector<std::string> stringList(const Metadata& in, std::string_view key) {
    std::vector<std::string> out;
    const Value* value = in.find(key);
    const std::vector<Value>* elements = value != nullptr ? value->asArray() : nullptr;
    if (elements == nullptr) {
        return out;
    }
    for (const Value& element : *elements) {
        if (const std::string* text = element.asStringRef()) {
            out.push_back(*text);
        }
    }
    return out;
}

Value floatArray(std::span<const double> items) {
    std::vector<Value> elements;
    elements.reserve(items.size());
    for (const double item : items) {
        elements.push_back(Value::ofFloat(item));
    }
    return Value::ofArray(Value::Type::Float, std::move(elements));
}

std::vector<double> floatList(const Metadata& in, std::string_view key) {
    std::vector<double> out;
    const Value* value = in.find(key);
    const std::vector<Value>* elements = value != nullptr ? value->asArray() : nullptr;
    if (elements == nullptr || value->elementType() != Value::Type::Float) {
        return out;
    }
    out.reserve(elements->size());
    for (const Value& element : *elements) {
        out.push_back(element.asFloat());
    }
    return out;
}

// ---- single entries -------------------------------------------------------------

Metadata encodeVersionReport(const VersionReport& report) {
    Metadata out;
    out.setString("version", report.version);
    out.setString("knownLatest", report.knownLatest);
    out.setBool("aheadOfDriver", report.aheadOfDriver);
    return out;
}

VersionReport decodeVersionReport(const Metadata& in) {
    return VersionReport{.version = in.getString("version"),
                         .knownLatest = in.getString("knownLatest"),
                         .aheadOfDriver = in.getBool("aheadOfDriver")};
}

Metadata encodeInfo(const SdrDeviceInfo& info) {
    Metadata out;
    out.setString("driver", info.driver);
    out.setString("id", info.id);
    out.setString("label", info.label);
    out.setString("serial", info.serial);
    out.setString("hardwareRevision", info.hardwareRevision);
    out.setHash("firmware", encodeVersionReport(info.firmware));
    out.setHash("fpga", encodeVersionReport(info.fpga));
    out.setFloat("minFrequencyHz", info.minFrequencyHz);
    out.setFloat("maxFrequencyHz", info.maxFrequencyHz);
    out.setFloat("minSampleRate", info.minSampleRate);
    out.setFloat("maxSampleRate", info.maxSampleRate);
    setU64(out, "linkCapacityBytesPerSec", info.linkCapacityBytesPerSec);
    out.setString("linkDescription", info.linkDescription);
    return out;
}

SdrDeviceInfo decodeInfo(const Metadata& in) {
    SdrDeviceInfo info;
    info.driver = in.getString("driver");
    info.id = in.getString("id");
    info.label = in.getString("label");
    info.serial = in.getString("serial");
    info.hardwareRevision = in.getString("hardwareRevision");
    info.firmware = decodeVersionReport(hashAt(in, "firmware"));
    info.fpga = decodeVersionReport(hashAt(in, "fpga"));
    info.minFrequencyHz = in.getFloat("minFrequencyHz");
    info.maxFrequencyHz = in.getFloat("maxFrequencyHz");
    info.minSampleRate = in.getFloat("minSampleRate");
    info.maxSampleRate = in.getFloat("maxSampleRate");
    info.linkCapacityBytesPerSec = getU64(in, "linkCapacityBytesPerSec");
    info.linkDescription = in.getString("linkDescription");
    return info;
}

Metadata encodeEnumValue(const SdrEnumValue& value) {
    Metadata out;
    out.setString("value", value.value);
    out.setString("label", value.label);
    out.setString("description", value.description);
    return out;
}

SdrEnumValue decodeEnumValue(const Metadata& in) {
    return SdrEnumValue{.value = in.getString("value"),
                        .label = in.getString("label"),
                        .description = in.getString("description")};
}

Metadata encodeParameter(const SdrParameter& parameter) {
    Metadata out;
    out.setString("key", parameter.key);
    out.setString("label", parameter.label);
    out.setString("group", parameter.group);
    setEnum(out, "type", parameter.type);
    out.setString("unit", parameter.unit);
    out.setFloat("min", parameter.min);
    out.setFloat("max", parameter.max);
    out.setFloat("step", parameter.step);
    out.set("enumValues",
            hashArray(std::span<const SdrEnumValue>(parameter.enumValues), encodeEnumValue));
    out.setBool("readOnly", parameter.readOnly);
    out.setBool("gridAffecting", parameter.gridAffecting);
    out.setBool("calibrationAffecting", parameter.calibrationAffecting);
    out.setBool("requiresStop", parameter.requiresStop);
    out.setString("description", parameter.description);
    out.set("defaultValue", encodeValue(parameter.defaultValue));
    out.setString("appliesWhenKey", parameter.appliesWhenKey);
    out.set("appliesWhenValues", stringArray(parameter.appliesWhenValues));
    return out;
}

SdrParameter decodeParameter(const Metadata& in) {
    SdrParameter parameter;
    parameter.key = in.getString("key");
    parameter.label = in.getString("label");
    parameter.group = in.getString("group");
    parameter.type = getEnum(in, "type", SdrParameterType::Double, 5);
    parameter.unit = in.getString("unit");
    parameter.min = in.getFloat("min");
    parameter.max = in.getFloat("max");
    parameter.step = in.getFloat("step");
    parameter.enumValues = hashList(in, "enumValues", decodeEnumValue);
    parameter.readOnly = in.getBool("readOnly");
    parameter.gridAffecting = in.getBool("gridAffecting");
    parameter.calibrationAffecting = in.getBool("calibrationAffecting");
    parameter.requiresStop = in.getBool("requiresStop");
    parameter.description = in.getString("description");
    if (const Value* value = in.find("defaultValue")) {
        parameter.defaultValue = decodeValue(*value);
    }
    parameter.appliesWhenKey = in.getString("appliesWhenKey");
    parameter.appliesWhenValues = stringList(in, "appliesWhenValues");
    return parameter;
}

Metadata encodeRxPort(const SdrRxPort& port) {
    Metadata out;
    out.setString("id", port.id);
    out.setString("label", port.label);
    out.setString("connector", port.connector);
    out.setFloat("minHz", port.minHz);
    out.setFloat("maxHz", port.maxHz);
    out.setBool("biasTee", port.biasTee);
    out.setBool("requiresStop", port.requiresStop);
    out.setFloat("switchSeconds", port.switchSeconds);
    return out;
}

SdrRxPort decodeRxPort(const Metadata& in) {
    return SdrRxPort{.id = in.getString("id"),
                     .label = in.getString("label"),
                     .connector = in.getString("connector"),
                     .minHz = in.getFloat("minHz"),
                     .maxHz = in.getFloat("maxHz"),
                     .biasTee = in.getBool("biasTee"),
                     .requiresStop = in.getBool("requiresStop"),
                     .switchSeconds = in.getFloat("switchSeconds")};
}

Metadata encodeSegment(const SweepSegment& segment) {
    Metadata out;
    out.setFloat("startHz", segment.startHz);
    out.setFloat("stopHz", segment.stopHz);
    out.setFloat("dwellSeconds", segment.dwellSeconds);
    return out;
}

SweepSegment decodeSegment(const Metadata& in) {
    return SweepSegment{.startHz = in.getFloat("startHz"),
                        .stopHz = in.getFloat("stopHz"),
                        .dwellSeconds = in.getFloat("dwellSeconds")};
}

Metadata encodeRange(const std::pair<double, double>& range) {
    Metadata out;
    out.setFloat("startHz", range.first);
    out.setFloat("stopHz", range.second);
    return out;
}

std::pair<double, double> decodeRange(const Metadata& in) {
    return {in.getFloat("startHz"), in.getFloat("stopHz")};
}

Metadata encodeBackend(const FftBackendInfo& info) {
    Metadata capabilities;
    setEnum(capabilities, "type", info.capabilities.type);
    setU64(capabilities, "minSize", info.capabilities.minSize);
    setU64(capabilities, "maxSize", info.capabilities.maxSize);
    setEnum(capabilities, "sizeConstraint", info.capabilities.sizeConstraint);
    capabilities.setBool("supportsBatch", info.capabilities.supportsBatch);
    capabilities.setBool("supportsInPlace", info.capabilities.supportsInPlace);
    capabilities.setBool("threadSafeExecute", info.capabilities.threadSafeExecute);
    capabilities.setBool("threadSafePlanning", info.capabilities.threadSafePlanning);

    Metadata out;
    out.setString("name", info.name);
    out.setString("displayName", info.displayName);
    out.setString("description", info.description);
    out.setHash("capabilities", std::move(capabilities));
    out.setBool("available", info.available);
    out.setString("unavailableReason", info.unavailableReason);
    out.setBool("isSuggestedDefault", info.isSuggestedDefault);
    return out;
}

FftBackendInfo decodeBackend(const Metadata& in) {
    FftBackendInfo info;
    info.name = in.getString("name");
    info.displayName = in.getString("displayName");
    info.description = in.getString("description");
    const Metadata& capabilities = hashAt(in, "capabilities");
    const FftCapabilities defaults;
    info.capabilities.type = getEnum(capabilities, "type", defaults.type, 2);
    info.capabilities.minSize = getSize(capabilities, "minSize", defaults.minSize);
    info.capabilities.maxSize = getSize(capabilities, "maxSize", defaults.maxSize);
    info.capabilities.sizeConstraint =
        getEnum(capabilities, "sizeConstraint", defaults.sizeConstraint, 2);
    info.capabilities.supportsBatch = capabilities.getBool("supportsBatch", defaults.supportsBatch);
    info.capabilities.supportsInPlace =
        capabilities.getBool("supportsInPlace", defaults.supportsInPlace);
    info.capabilities.threadSafeExecute =
        capabilities.getBool("threadSafeExecute", defaults.threadSafeExecute);
    info.capabilities.threadSafePlanning =
        capabilities.getBool("threadSafePlanning", defaults.threadSafePlanning);
    info.available = in.getBool("available");
    info.unavailableReason = in.getString("unavailableReason");
    info.isSuggestedDefault = in.getBool("isSuggestedDefault");
    return info;
}

Metadata encodeAntenna(const Antenna& antenna) {
    Metadata out;
    out.setString("id", antenna.id);
    out.setString("name", antenna.name);
    out.setString("category", antenna.category);
    out.setString("type", antenna.type);
    out.setFloat("startHz", antenna.startHz);
    out.setFloat("stopHz", antenna.stopHz);
    out.setFloat("gainDbi", antenna.gainDbi);
    out.setBool("needsBiasT", antenna.needsBiasT);
    out.setString("notes", antenna.notes);
    out.setBool("builtin", antenna.builtin);
    return out;
}

Antenna decodeAntenna(const Metadata& in) {
    return Antenna{.id = in.getString("id"),
                   .name = in.getString("name"),
                   .category = in.getString("category"),
                   .type = in.getString("type"),
                   .startHz = in.getFloat("startHz"),
                   .stopHz = in.getFloat("stopHz"),
                   .gainDbi = in.getFloat("gainDbi"),
                   .needsBiasT = in.getBool("needsBiasT"),
                   .notes = in.getString("notes"),
                   .builtin = in.getBool("builtin")};
}

Metadata encodeAssignment(const AntennaAssignment& assignment) {
    Metadata out;
    out.setString("device", assignment.device);
    out.setString("port", assignment.port);
    out.setString("switcher", assignment.switcher);
    out.setString("input", assignment.input);
    out.setString("antenna", assignment.antenna);
    return out;
}

AntennaAssignment decodeAssignment(const Metadata& in) {
    return AntennaAssignment{.device = in.getString("device"),
                             .port = in.getString("port"),
                             .switcher = in.getString("switcher"),
                             .input = in.getString("input"),
                             .antenna = in.getString("antenna")};
}

Metadata encodeFallback(const std::pair<std::string, std::string>& fallback) {
    Metadata out;
    out.setString("device", fallback.first);
    out.setString("port", fallback.second);
    return out;
}

std::pair<std::string, std::string> decodeFallback(const Metadata& in) {
    return {in.getString("device"), in.getString("port")};
}

Metadata encodeSwitcherInfo(const RfPathInfo& info) {
    Metadata out;
    out.setString("driver", info.driver);
    out.setString("id", info.id);
    out.setString("label", info.label);
    out.setString("model", info.model);
    out.setString("serial", info.serial);
    out.setInt("inputCount", info.inputCount);
    out.setBool("requiresStop", info.requiresStop);
    out.setFloat("switchSeconds", info.switchSeconds);
    return out;
}

RfPathInfo decodeSwitcherInfo(const Metadata& in) {
    return RfPathInfo{.driver = in.getString("driver"),
                      .id = in.getString("id"),
                      .label = in.getString("label"),
                      .model = in.getString("model"),
                      .serial = in.getString("serial"),
                      .inputCount = getU32(in, "inputCount"),
                      .requiresStop = in.getBool("requiresStop"),
                      .switchSeconds = in.getFloat("switchSeconds")};
}

Metadata encodeSwitcherInput(const RfPathInput& input) {
    Metadata out;
    out.setString("id", input.id);
    out.setString("label", input.label);
    out.setFloat("minHz", input.minHz);
    out.setFloat("maxHz", input.maxHz);
    return out;
}

RfPathInput decodeSwitcherInput(const Metadata& in) {
    return RfPathInput{.id = in.getString("id"),
                       .label = in.getString("label"),
                       .minHz = in.getFloat("minHz"),
                       .maxHz = in.getFloat("maxHz")};
}

Metadata encodeSwitcherView(const SwitcherView& view) {
    Metadata out;
    out.setString("key", view.key);
    out.setHash("info", encodeSwitcherInfo(view.info));
    out.set("inputs", hashArray(std::span<const RfPathInput>(view.inputs), encodeSwitcherInput));
    out.setInt("selectedInput", view.selectedInput);
    return out;
}

SwitcherView decodeSwitcherView(const Metadata& in) {
    return SwitcherView{.key = in.getString("key"),
                        .info = decodeSwitcherInfo(hashAt(in, "info")),
                        .inputs = hashList(in, "inputs", decodeSwitcherInput),
                        .selectedInput = getU32(in, "selectedInput")};
}

Metadata encodeRoute(const RoutePort& route) {
    Metadata out;
    setU64(out, "portIndex", route.portIndex);
    out.setString("id", route.id);
    setU64(out, "inputIndex", route.inputIndex);
    out.setFloat("startHz", route.startHz);
    out.setFloat("stopHz", route.stopHz);
    out.setFloat("gainDbi", route.gainDbi);
    out.setFloat("switchSeconds", route.switchSeconds);
    out.setFloat("inputSwitchSeconds", route.inputSwitchSeconds);
    return out;
}

RoutePort decodeRoute(const Metadata& in) {
    return RoutePort{.portIndex = getSize(in, "portIndex"),
                     .id = in.getString("id"),
                     .inputIndex = getSize(in, "inputIndex", kNoInput),
                     .startHz = in.getFloat("startHz"),
                     .stopHz = in.getFloat("stopHz"),
                     .gainDbi = in.getFloat("gainDbi"),
                     .switchSeconds = in.getFloat("switchSeconds"),
                     .inputSwitchSeconds = in.getFloat("inputSwitchSeconds")};
}

Metadata encodeRfLeg(const RfLegView& leg) {
    Metadata out;
    out.setHash("route", encodeRoute(leg.route));
    out.setString("switcherKey", leg.switcherKey);
    out.setHash("antenna", encodeAntenna(leg.antenna));
    out.setString("portLabel", leg.portLabel);
    out.setBool("live", leg.live);
    return out;
}

RfLegView decodeRfLeg(const Metadata& in) {
    return RfLegView{.route = decodeRoute(hashAt(in, "route")),
                     .switcherKey = in.getString("switcherKey"),
                     .antenna = decodeAntenna(hashAt(in, "antenna")),
                     .portLabel = in.getString("portLabel"),
                     .live = in.getBool("live")};
}

Metadata encodeReading(const SdrHealthReading& reading) {
    Metadata out;
    out.setString("label", reading.label);
    out.setString("value", reading.value);
    out.setFloat("numeric", static_cast<double>(reading.numeric));
    out.setFloat("minimum", static_cast<double>(reading.minimum));
    out.setFloat("maximum", static_cast<double>(reading.maximum));
    out.setBool("alarm", reading.alarm);
    return out;
}

SdrHealthReading decodeReading(const Metadata& in) {
    return SdrHealthReading{.label = in.getString("label"),
                            .value = in.getString("value"),
                            .numeric = getF32(in, "numeric"),
                            .minimum = getF32(in, "minimum"),
                            .maximum = getF32(in, "maximum"),
                            .alarm = in.getBool("alarm")};
}

} // namespace

const Metadata& hashAt(const Metadata& in, std::string_view key) {
    static const Metadata kEmpty;
    const Value* value = in.find(key);
    const Metadata* hash = value != nullptr ? value->asHash() : nullptr;
    return hash != nullptr ? *hash : kEmpty;
}

// ---- values -------------------------------------------------------------------

Value encodeValue(const SdrValue& value) {
    return std::visit(
        [](const auto& held) -> Value {
            using T = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<T, bool>) {
                return Value::ofBool(held);
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return Value::ofInt(held);
            } else if constexpr (std::is_same_v<T, double>) {
                return Value::ofFloat(held);
            } else {
                return Value::ofString(held);
            }
        },
        value);
}

SdrValue decodeValue(const Value& value) {
    switch (value.type()) {
    case Value::Type::Bool:
        return value.asBool();
    case Value::Type::Int:
        return value.asInt();
    case Value::Type::Float:
        return value.asFloat();
    case Value::Type::String:
        return value.asString();
    default:
        return std::int64_t{0};
    }
}

// ---- the radio ----------------------------------------------------------------

Metadata encodeDevice(const DeviceDescriptor& device) {
    Metadata out;
    out.setHash("info", encodeInfo(device.info));
    out.set("parameters",
            hashArray(std::span<const SdrParameter>(device.parameters), encodeParameter));
    out.set("rxPorts", hashArray(std::span<const SdrRxPort>(device.rxPorts), encodeRxPort));
    out.set("supportedSampleRates", floatArray(device.supportedSampleRates));
    return out;
}

DeviceDescriptor decodeDevice(const Metadata& in) {
    return DeviceDescriptor{.info = decodeInfo(hashAt(in, "info")),
                            .parameters = hashList(in, "parameters", decodeParameter),
                            .rxPorts = hashList(in, "rxPorts", decodeRxPort),
                            .supportedSampleRates = floatList(in, "supportedSampleRates")};
}

// ---- running ------------------------------------------------------------------

Metadata encodePlan(const SweepPlan& plan) {
    Metadata out;
    out.setString("name", plan.name);
    out.set("segments", hashArray(std::span<const SweepSegment>(plan.segments), encodeSegment));
    setEnum(out, "mode", plan.mode);
    out.setFloat("rbwHz", plan.rbwHz);
    out.setFloat("sampleRate", plan.sampleRate);
    out.setFloat("usableBandwidthFraction", plan.usableBandwidthFraction);
    out.setFloat("stepOverlap", plan.stepOverlap);
    out.setFloat("dcGuardFraction", plan.dcGuardFraction);
    out.setFloat("dwellSeconds", plan.dwellSeconds);
    out.setInt("averageCount", plan.averageCount);
    setEnum(out, "window", plan.window);
    out.setFloat("windowBeta", plan.windowBeta);
    out.setFloat("fftOverlap", plan.fftOverlap);
    setEnum(out, "overlapResolution", plan.overlapResolution);
    out.setBool("continuous", plan.continuous);
    out.setBool("antennaRouting", plan.antennaRouting);
    setEnum(out, "portStrategy", plan.portStrategy);
    return out;
}

SweepPlan decodePlan(const Metadata& in) {
    const SweepPlan defaults;
    SweepPlan plan;
    plan.name = in.getString("name");
    plan.segments = hashList(in, "segments", decodeSegment);
    plan.mode = getEnum(in, "mode", defaults.mode, 2);
    plan.rbwHz = in.getFloat("rbwHz", defaults.rbwHz);
    plan.sampleRate = in.getFloat("sampleRate", defaults.sampleRate);
    plan.usableBandwidthFraction =
        in.getFloat("usableBandwidthFraction", defaults.usableBandwidthFraction);
    plan.stepOverlap = in.getFloat("stepOverlap", defaults.stepOverlap);
    plan.dcGuardFraction = in.getFloat("dcGuardFraction", defaults.dcGuardFraction);
    plan.dwellSeconds = in.getFloat("dwellSeconds", defaults.dwellSeconds);
    plan.averageCount = getU32(in, "averageCount", defaults.averageCount);
    plan.window = getEnum(in, "window", defaults.window, 6);
    plan.windowBeta = in.getFloat("windowBeta", defaults.windowBeta);
    plan.fftOverlap = in.getFloat("fftOverlap", defaults.fftOverlap);
    plan.overlapResolution = getEnum(in, "overlapResolution", defaults.overlapResolution, 3);
    plan.continuous = in.getBool("continuous", defaults.continuous);
    plan.antennaRouting = in.getBool("antennaRouting", defaults.antennaRouting);
    plan.portStrategy = getEnum(in, "portStrategy", defaults.portStrategy, 4);
    return plan;
}

Metadata encodePipeline(const PipelineConfig& config) {
    Metadata out;
    out.setInt("fftSize", config.fftSize);
    setEnum(out, "window", config.window);
    out.setFloat("windowBeta", config.windowBeta);
    out.setFloat("overlap", config.overlap);
    out.setInt("workerCount", config.workerCount);
    setEnum(out, "throttleMode", config.throttleMode);
    out.setInt("everyNth", config.everyNth);
    out.setInt("averageCount", config.averageCount);
    setEnum(out, "planQuality", config.planQuality);
    out.setFloat("targetFrameRate", config.targetFrameRate);
    out.setFloat("dbfsToDbmOffset", config.dbfsToDbmOffset);
    return out;
}

PipelineConfig decodePipeline(const Metadata& in) {
    const PipelineConfig defaults;
    PipelineConfig config;
    config.fftSize = getU32(in, "fftSize", defaults.fftSize);
    config.window = getEnum(in, "window", defaults.window, 6);
    config.windowBeta = in.getFloat("windowBeta", defaults.windowBeta);
    config.overlap = in.getFloat("overlap", defaults.overlap);
    config.workerCount = getU32(in, "workerCount", defaults.workerCount);
    config.throttleMode = getEnum(in, "throttleMode", defaults.throttleMode, 3);
    config.everyNth = getU32(in, "everyNth", defaults.everyNth);
    config.averageCount = getU32(in, "averageCount", defaults.averageCount);
    config.planQuality = getEnum(in, "planQuality", defaults.planQuality, 3);
    config.targetFrameRate = in.getFloat("targetFrameRate", defaults.targetFrameRate);
    config.dbfsToDbmOffset = in.getFloat("dbfsToDbmOffset", defaults.dbfsToDbmOffset);
    return config;
}

Metadata encodeSchedule(const ScheduleSummary& schedule) {
    Metadata out;
    setU64(out, "stepCount", schedule.stepCount);
    out.setInt("fftSize", schedule.fftSize);
    out.setFloat("actualRbwHz", schedule.actualRbwHz);
    out.setFloat("gridStartHz", schedule.gridStartHz);
    out.setFloat("gridBinWidthHz", schedule.gridBinWidthHz);
    setU64(out, "gridBinCount", schedule.gridBinCount);
    out.setFloat("estimatedPassSeconds", schedule.estimatedPassSeconds);
    out.setFloat("estimatedSweepRateHzPerSec", schedule.estimatedSweepRateHzPerSec);
    out.setFloat("retuneOverheadFraction", schedule.retuneOverheadFraction);
    out.set("unroutedHz", hashArray(std::span<const std::pair<double, double>>(schedule.unroutedHz),
                                    encodeRange));
    out.setInt("portSwitches", schedule.portSwitches);
    return out;
}

ScheduleSummary decodeSchedule(const Metadata& in) {
    return ScheduleSummary{
        .stepCount = getSize(in, "stepCount"),
        .fftSize = getU32(in, "fftSize"),
        .actualRbwHz = in.getFloat("actualRbwHz"),
        .gridStartHz = in.getFloat("gridStartHz"),
        .gridBinWidthHz = in.getFloat("gridBinWidthHz"),
        .gridBinCount = getSize(in, "gridBinCount"),
        .estimatedPassSeconds = in.getFloat("estimatedPassSeconds"),
        .estimatedSweepRateHzPerSec = in.getFloat("estimatedSweepRateHzPerSec"),
        .retuneOverheadFraction = in.getFloat("retuneOverheadFraction"),
        .unroutedHz = hashList(in, "unroutedHz", decodeRange),
        .portSwitches = getU32(in, "portSwitches"),
    };
}

Metadata encodeEngineStats(const EngineStats& stats) {
    Metadata out;
    setU64(out, "stitched", stats.stitched);
    setU64(out, "unsettled", stats.unsettled);
    setU64(out, "unattributed", stats.unattributed);
    setU64(out, "tooShort", stats.tooShort);
    out.setFloat("lastPassCoverage", stats.lastPassCoverage);
    out.setFloat("measuredSweepRateHzPerSec", stats.measuredSweepRateHzPerSec);
    setU64(out, "passCount", stats.passCount);
    return out;
}

EngineStats decodeEngineStats(const Metadata& in) {
    return EngineStats{.stitched = getU64(in, "stitched"),
                       .unsettled = getU64(in, "unsettled"),
                       .unattributed = getU64(in, "unattributed"),
                       .tooShort = getU64(in, "tooShort"),
                       .lastPassCoverage = in.getFloat("lastPassCoverage"),
                       .measuredSweepRateHzPerSec = in.getFloat("measuredSweepRateHzPerSec"),
                       .passCount = getU64(in, "passCount")};
}

Metadata encodeBackends(std::span<const FftBackendInfo> backends) {
    Metadata out;
    out.set("items", hashArray(backends, encodeBackend));
    return out;
}

std::vector<FftBackendInfo> decodeBackends(const Metadata& in) {
    return hashList(in, "items", decodeBackend);
}

// ---- corrections ----------------------------------------------------------------

Metadata encodeCorrectionSettings(const CorrectionSettings& settings) {
    Metadata out;
    out.setBool("dcRemoval", settings.dcRemoval);
    out.setBool("flatten", settings.flatten);
    out.setBool("spurMask", settings.spurMask);
    out.setBool("autoSpurs", settings.autoSpurs);
    return out;
}

CorrectionSettings decodeCorrectionSettings(const Metadata& in) {
    const CorrectionSettings defaults;
    return CorrectionSettings{.dcRemoval = in.getBool("dcRemoval", defaults.dcRemoval),
                              .flatten = in.getBool("flatten", defaults.flatten),
                              .spurMask = in.getBool("spurMask", defaults.spurMask),
                              .autoSpurs = in.getBool("autoSpurs", defaults.autoSpurs)};
}

Metadata encodeCorrectionSummary(const CorrectionSummary& summary) {
    Metadata out;
    out.setBool("present", summary.present);
    setU64(out, "floorPoints", summary.floorPoints);
    setU64(out, "spurs", summary.spurs);
    setU64(out, "automaticSpurs", summary.automaticSpurs);
    out.setString("learnedAt", summary.learnedAt);
    out.setString("floorStaleReason", summary.floorStaleReason);
    return out;
}

CorrectionSummary decodeCorrectionSummary(const Metadata& in) {
    return CorrectionSummary{.present = in.getBool("present"),
                             .floorPoints = getSize(in, "floorPoints"),
                             .spurs = getSize(in, "spurs"),
                             .automaticSpurs = getSize(in, "automaticSpurs"),
                             .learnedAt = in.getString("learnedAt"),
                             .floorStaleReason = in.getString("floorStaleReason")};
}

// ---- antennas -------------------------------------------------------------------

Metadata encodeAntennas(std::span<const Antenna> antennas) {
    Metadata out;
    out.set("items", hashArray(antennas, encodeAntenna));
    return out;
}

std::vector<Antenna> decodeAntennas(const Metadata& in) {
    return hashList(in, "items", decodeAntenna);
}

Metadata encodeAssignments(const AntennaAssignments& assignments) {
    Metadata out;
    out.set("entries", hashArray(assignments.entries(), encodeAssignment));
    out.set("fallbackPorts", hashArray(assignments.fallbackPorts(), encodeFallback));
    return out;
}

AntennaAssignments decodeAssignments(const Metadata& in) {
    return AntennaAssignments::of(hashList(in, "entries", decodeAssignment),
                                  hashList(in, "fallbackPorts", decodeFallback));
}

Metadata encodeSwitcherViews(std::span<const SwitcherView> views) {
    Metadata out;
    out.set("items", hashArray(views, encodeSwitcherView));
    return out;
}

std::vector<SwitcherView> decodeSwitcherViews(const Metadata& in) {
    return hashList(in, "items", decodeSwitcherView);
}

Metadata encodeSwitcherInfos(std::span<const RfPathInfo> infos) {
    Metadata out;
    out.set("items", hashArray(infos, encodeSwitcherInfo));
    return out;
}

std::vector<RfPathInfo> decodeSwitcherInfos(const Metadata& in) {
    return hashList(in, "items", decodeSwitcherInfo);
}

Metadata encodeRfLegs(std::span<const RfLegView> legs) {
    Metadata out;
    out.set("items", hashArray(legs, encodeRfLeg));
    return out;
}

std::vector<RfLegView> decodeRfLegs(const Metadata& in) {
    return hashList(in, "items", decodeRfLeg);
}

Metadata encodeRanges(std::span<const std::pair<double, double>> ranges) {
    Metadata out;
    out.set("items", hashArray(ranges, encodeRange));
    return out;
}

std::vector<std::pair<double, double>> decodeRanges(const Metadata& in) {
    return hashList(in, "items", decodeRange);
}

// ---- telemetry ------------------------------------------------------------------

Metadata encodeHealth(std::span<const SdrHealthReading> readings) {
    Metadata out;
    out.set("items", hashArray(readings, encodeReading));
    return out;
}

std::vector<SdrHealthReading> decodeHealth(const Metadata& in) {
    return hashList(in, "items", decodeReading);
}

Metadata encodeStreamStats(const StreamStats& stats) {
    Metadata out;
    out.setFloat("configuredSps", stats.configuredSps);
    out.setFloat("measuredSps", stats.measuredSps);
    out.setFloat("bytesPerSecIn", stats.bytesPerSecIn);
    out.setFloat("linkCapacityBytesPerSec", stats.linkCapacityBytesPerSec);
    out.setFloat("linkUtilisation", stats.linkUtilisation);
    setU64(out, "samplesDelivered", stats.samplesDelivered);
    setU64(out, "samplesDropped", stats.samplesDropped);
    setU64(out, "samplesLostAtSource", stats.samplesLostAtSource);
    setU64(out, "deviceOverruns", stats.deviceOverruns);
    setU64(out, "ringFullEvents", stats.ringFullEvents);
    setU64(out, "poolExhaustedEvents", stats.poolExhaustedEvents);
    setU64(out, "sequenceGaps", stats.sequenceGaps);
    out.setFloat("dropRatePerSec", stats.dropRatePerSec);
    out.setFloat("dropFraction", stats.dropFraction);
    out.setFloat("ringFillFraction", static_cast<double>(stats.ringFillFraction));
    return out;
}

StreamStats decodeStreamStats(const Metadata& in) {
    return StreamStats{.configuredSps = in.getFloat("configuredSps"),
                       .measuredSps = in.getFloat("measuredSps"),
                       .bytesPerSecIn = in.getFloat("bytesPerSecIn"),
                       .linkCapacityBytesPerSec = in.getFloat("linkCapacityBytesPerSec"),
                       .linkUtilisation = in.getFloat("linkUtilisation"),
                       .samplesDelivered = getU64(in, "samplesDelivered"),
                       .samplesDropped = getU64(in, "samplesDropped"),
                       .samplesLostAtSource = getU64(in, "samplesLostAtSource"),
                       .deviceOverruns = getU64(in, "deviceOverruns"),
                       .ringFullEvents = getU64(in, "ringFullEvents"),
                       .poolExhaustedEvents = getU64(in, "poolExhaustedEvents"),
                       .sequenceGaps = getU64(in, "sequenceGaps"),
                       .dropRatePerSec = in.getFloat("dropRatePerSec"),
                       .dropFraction = in.getFloat("dropFraction"),
                       .ringFillFraction = getF32(in, "ringFillFraction")};
}

Metadata encodeProcessStats(const ProcessStats& stats) {
    Metadata out;
    out.setFloat("fftsPerSec", stats.fftsPerSec);
    setU64(out, "fftsComputed", stats.fftsComputed);
    setU64(out, "fftsSkipped", stats.fftsSkipped);
    setU64(out, "samplesProcessedTotal", stats.samplesProcessedTotal);
    out.setFloat("processedFraction", stats.processedFraction);
    out.setFloat("framesPerSec", stats.framesPerSec);
    setU64(out, "sweepPassesCompleted", stats.sweepPassesCompleted);
    out.setFloat("retunesPerSec", stats.retunesPerSec);
    out.setFloat("fftLatencyP50Us", static_cast<double>(stats.fftLatencyP50Us));
    out.setFloat("fftLatencyP99Us", static_cast<double>(stats.fftLatencyP99Us));
    out.setFloat("fftLatencyMaxUs", static_cast<double>(stats.fftLatencyMaxUs));
    out.setFloat("workerUtilisation", stats.workerUtilisation);
    out.setInt("workerCount", stats.workerCount);
    setEnum(out, "throttleReason", stats.throttleReason);
    out.setFloat("sweepSpeedHzPerSec", stats.sweepSpeedHzPerSec);
    return out;
}

ProcessStats decodeProcessStats(const Metadata& in) {
    return ProcessStats{
        .fftsPerSec = in.getFloat("fftsPerSec"),
        .fftsComputed = getU64(in, "fftsComputed"),
        .fftsSkipped = getU64(in, "fftsSkipped"),
        .samplesProcessedTotal = getU64(in, "samplesProcessedTotal"),
        .processedFraction = in.getFloat("processedFraction"),
        .framesPerSec = in.getFloat("framesPerSec"),
        .sweepPassesCompleted = getU64(in, "sweepPassesCompleted"),
        .retunesPerSec = in.getFloat("retunesPerSec"),
        .fftLatencyP50Us = getF32(in, "fftLatencyP50Us"),
        .fftLatencyP99Us = getF32(in, "fftLatencyP99Us"),
        .fftLatencyMaxUs = getF32(in, "fftLatencyMaxUs"),
        .workerUtilisation = in.getFloat("workerUtilisation"),
        .workerCount = getU32(in, "workerCount"),
        .throttleReason = getEnum(in, "throttleReason", ThrottleReason::Stopped, 7),
        .sweepSpeedHzPerSec = in.getFloat("sweepSpeedHzPerSec"),
    };
}

Metadata encodeNotice(const InstrumentNotice& notice) {
    Metadata out;
    setEnum(out, "kind", notice.kind);
    out.setString("text", notice.text);
    return out;
}

InstrumentNotice decodeNotice(const Metadata& in) {
    return InstrumentNotice{.kind = getEnum(in, "kind", InstrumentNotice::Kind::Info, 6),
                            .text = in.getString("text")};
}

} // namespace sweeppp::remote
