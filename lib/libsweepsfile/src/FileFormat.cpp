// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include "sweeps/FileFormat.hpp"

#include <array>
#include <cstring>
#include <utility>

namespace sweeps {

using detail::fail;
namespace {

/// CRC32 (IEEE 802.3) table, built once at first use.
const std::array<std::uint32_t, 256>& crcTable() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> generated{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) != 0U ? (0xEDB8'8320U ^ (value >> 1U)) : (value >> 1U);
            }
            generated[i] = value;
        }
        return generated;
    }();
    return table;
}

} // namespace

std::string_view toString(RecordType type) noexcept {
    switch (type) {
    case RecordType::Manifest:
        return "manifest";
    case RecordType::SegmentOpen:
        return "segment-open";
    case RecordType::SegmentClose:
        return "segment-close";
    case RecordType::Event:
        return "event";
    case RecordType::Tile:
        return "tile";
    case RecordType::Index:
        return "index";
    case RecordType::EndOfStream:
        return "end-of-stream";
    case RecordType::Telemetry:
        return "telemetry";
    case RecordType::PluginData:
        return "plugin-data";
    }
    return "unknown";
}

std::string_view toString(SessionEvent::Kind kind) noexcept {
    switch (kind) {
    case SessionEvent::Kind::Retune:
        return "retune";
    case SessionEvent::Kind::ParameterChanged:
        return "parameter";
    case SessionEvent::Kind::SweepPass:
        return "sweep-pass";
    case SessionEvent::Kind::Marker:
        return "marker";
    case SessionEvent::Kind::Annotation:
        return "annotation";
    case SessionEvent::Kind::Alert:
        return "alert";
    case SessionEvent::Kind::SegmentBoundary:
        return "segment";
    case SessionEvent::Kind::ThrottleChanged:
        return "throttle";
    case SessionEvent::Kind::DeviceError:
        return "device-error";
    case SessionEvent::Kind::Plugin:
        return "plugin";
    }
    return "unknown";
}

std::string_view eventKindName(std::uint16_t kind) noexcept {
    // Any u16 is a valid value of a scoped enum with a u16 underlying type, so
    // the cast is defined and the switch's fallthrough answers for the rest.
    return toString(static_cast<SessionEvent::Kind>(kind));
}

Result<SessionEvent::Kind> eventKindFromString(std::string_view name) {
    if (name == "retune") {
        return SessionEvent::Kind::Retune;
    }
    if (name == "parameter") {
        return SessionEvent::Kind::ParameterChanged;
    }
    if (name == "sweep-pass") {
        return SessionEvent::Kind::SweepPass;
    }
    if (name == "marker") {
        return SessionEvent::Kind::Marker;
    }
    if (name == "annotation") {
        return SessionEvent::Kind::Annotation;
    }
    if (name == "alert") {
        return SessionEvent::Kind::Alert;
    }
    if (name == "segment") {
        return SessionEvent::Kind::SegmentBoundary;
    }
    if (name == "throttle") {
        return SessionEvent::Kind::ThrottleChanged;
    }
    if (name == "device-error") {
        return SessionEvent::Kind::DeviceError;
    }
    if (name == "plugin") {
        return SessionEvent::Kind::Plugin;
    }
    return fail<SessionEvent::Kind>(ErrorCode::ParseError, "unknown event kind '{}'", name);
}

Metadata eventBodyMetadata(const SessionEvent& event) {
    Metadata body;

    if (const auto* data = event.as<RetuneData>()) {
        body.setFloat("center_hz", data->centerHz);
        body.setInt("step_index", data->stepIndex);
    } else if (const auto* data = event.as<ParameterChangedData>()) {
        body.setString("key", data->key);
        body.setString("value", data->value);
        body.setBool("grid_affecting", data->gridAffecting);
        body.setBool("calibration_affecting", data->calibrationAffecting);
    } else if (const auto* data = event.as<SweepPassData>()) {
        body.setInt("pass_id", static_cast<std::int64_t>(data->passId));
        body.setFloat("start_hz", data->startHz);
        body.setFloat("stop_hz", data->stopHz);
        body.setFloat("duration_seconds", data->durationSeconds);
    } else if (const auto* data = event.as<MarkerData>()) {
        body.setString("label", data->label);
        body.setFloat("frequency_hz", data->frequencyHz);
        body.setFloat("level_dbm", data->levelDbm);
    } else if (const auto* data = event.as<AnnotationData>()) {
        body.setString("text", data->text);
        body.setFloat("start_hz", data->startHz);
        body.setFloat("stop_hz", data->stopHz);
    } else if (const auto* data = event.as<SegmentBoundaryData>()) {
        body.setString("reason", data->reason);
    } else if (const auto* data = event.as<ThrottleChangedData>()) {
        body.setString("reason", data->reason);
        body.setFloat("processed_fraction", data->processedFraction);
    } else if (const auto* data = event.as<DeviceErrorData>()) {
        body.setString("device_id", data->deviceId);
        body.setString("message", data->message);
    } else if (const auto* data = event.as<PluginEventData>()) {
        body.setString("plugin_id", data->pluginId);
        body.setString("event", data->eventName);
        body.setHash("fields", data->fields);
    } else if (const auto* data = event.as<UnknownEventData>()) {
        body.setBytes("raw", data->body);
    }

    return body;
}

namespace {

/// The body an event of this kind carries, or a default-constructed one.
///
/// The *kind* determines the payload, so a body that does not match the kind is
/// not written in its place. A mismatch can only come from a caller that built
/// the two independently, and writing the kind's own empty body keeps the record
/// decodable.
template <typename Body>
[[nodiscard]] Body bodyOf(const EventBody& body) {
    if (const Body* value = std::get_if<Body>(&body)) {
        return *value;
    }
    return Body{};
}

} // namespace

void encodeEvent(std::vector<std::byte>& out, const SessionEvent& event) {
    writeU16(out, event.kind);
    writeU64(out, event.monotonicNs);
    writeU64(out, event.wallNs);
    writeU32(out, event.segmentId);

    switch (event.kindEnum()) {
    case SessionEvent::Kind::Retune: {
        const RetuneData data = bodyOf<RetuneData>(event.body);
        writeF64(out, data.centerHz);
        writeU32(out, data.stepIndex);
        break;
    }

    case SessionEvent::Kind::ParameterChanged: {
        const ParameterChangedData data = bodyOf<ParameterChangedData>(event.body);
        writeString(out, data.key);
        writeString(out, data.value);
        writeU8(out, data.gridAffecting ? 1U : 0U);
        writeU8(out, data.calibrationAffecting ? 1U : 0U);
        break;
    }

    case SessionEvent::Kind::SweepPass: {
        const SweepPassData data = bodyOf<SweepPassData>(event.body);
        writeU64(out, data.passId);
        writeF64(out, data.startHz);
        writeF64(out, data.stopHz);
        writeF64(out, data.durationSeconds);
        break;
    }

    case SessionEvent::Kind::Marker: {
        const MarkerData data = bodyOf<MarkerData>(event.body);
        writeString(out, data.label);
        writeF64(out, data.frequencyHz);
        writeF64(out, data.levelDbm);
        break;
    }

    case SessionEvent::Kind::Annotation: {
        const AnnotationData data = bodyOf<AnnotationData>(event.body);
        writeString(out, data.text);
        writeF64(out, data.startHz);
        writeF64(out, data.stopHz);
        break;
    }

    case SessionEvent::Kind::SegmentBoundary: {
        const SegmentBoundaryData data = bodyOf<SegmentBoundaryData>(event.body);
        writeString(out, data.reason);
        break;
    }

    case SessionEvent::Kind::ThrottleChanged: {
        const ThrottleChangedData data = bodyOf<ThrottleChangedData>(event.body);
        writeString(out, data.reason);
        writeF64(out, data.processedFraction);
        break;
    }

    case SessionEvent::Kind::DeviceError: {
        const DeviceErrorData data = bodyOf<DeviceErrorData>(event.body);
        writeString(out, data.deviceId);
        writeString(out, data.message);
        break;
    }

    case SessionEvent::Kind::Plugin: {
        const PluginEventData data = bodyOf<PluginEventData>(event.body);
        writeString(out, data.pluginId);
        writeString(out, data.eventName);
        data.fields.encode(out);
        break;
    }

    case SessionEvent::Kind::Alert:
    default: {
        // A reserved kind, or one this build has never heard of. Either way the
        // body travels exactly as it arrived, which is what lets an unrecognised
        // event survive a read and a write rather than being quietly dropped.
        const UnknownEventData data = bodyOf<UnknownEventData>(event.body);
        if (!data.body.empty()) {
            writeBytes(out, data.body.data(), data.body.size());
        }
        break;
    }
    }
}

Result<SessionEvent> decodeEvent(ByteReader& in) {
    SessionEvent event;

    auto kind = in.readU16();
    auto monotonicNs = in.readU64();
    auto wallNs = in.readU64();
    auto segmentId = in.readU32();
    if (!kind || !monotonicNs || !wallNs || !segmentId) {
        return fail<SessionEvent>(ErrorCode::Corrupt, "truncated event record");
    }

    event.kind = *kind;
    event.monotonicNs = *monotonicNs;
    event.wallNs = *wallNs;
    event.segmentId = *segmentId;

    const auto malformed = [&event] {
        return fail<SessionEvent>(ErrorCode::Corrupt, "malformed {} event body",
                                  eventKindName(event.kind));
    };

    switch (event.kindEnum()) {
    case SessionEvent::Kind::Retune: {
        auto centerHz = in.readF64();
        auto stepIndex = in.readU32();
        if (!centerHz || !stepIndex) {
            return malformed();
        }
        RetuneData data;
        data.centerHz = *centerHz;
        data.stepIndex = *stepIndex;
        event.body = std::move(data);
        break;
    }

    case SessionEvent::Kind::ParameterChanged: {
        auto key = in.readString();
        auto value = in.readString();
        auto gridAffecting = in.readU8();
        auto calibrationAffecting = in.readU8();
        if (!key || !value || !gridAffecting || !calibrationAffecting) {
            return malformed();
        }
        ParameterChangedData data;
        data.key = std::move(*key);
        data.value = std::move(*value);
        data.gridAffecting = *gridAffecting != 0;
        data.calibrationAffecting = *calibrationAffecting != 0;
        event.body = std::move(data);
        break;
    }

    case SessionEvent::Kind::SweepPass: {
        auto passId = in.readU64();
        auto startHz = in.readF64();
        auto stopHz = in.readF64();
        auto durationSeconds = in.readF64();
        if (!passId || !startHz || !stopHz || !durationSeconds) {
            return malformed();
        }
        SweepPassData data;
        data.passId = *passId;
        data.startHz = *startHz;
        data.stopHz = *stopHz;
        data.durationSeconds = *durationSeconds;
        event.body = std::move(data);
        break;
    }

    case SessionEvent::Kind::Marker: {
        auto label = in.readString();
        auto frequencyHz = in.readF64();
        auto levelDbm = in.readF64();
        if (!label || !frequencyHz || !levelDbm) {
            return malformed();
        }
        MarkerData data;
        data.label = std::move(*label);
        data.frequencyHz = *frequencyHz;
        data.levelDbm = *levelDbm;
        event.body = std::move(data);
        break;
    }

    case SessionEvent::Kind::Annotation: {
        auto text = in.readString();
        auto startHz = in.readF64();
        auto stopHz = in.readF64();
        if (!text || !startHz || !stopHz) {
            return malformed();
        }
        AnnotationData data;
        data.text = std::move(*text);
        data.startHz = *startHz;
        data.stopHz = *stopHz;
        event.body = std::move(data);
        break;
    }

    case SessionEvent::Kind::SegmentBoundary: {
        auto reason = in.readString();
        if (!reason) {
            return malformed();
        }
        SegmentBoundaryData data;
        data.reason = std::move(*reason);
        event.body = std::move(data);
        break;
    }

    case SessionEvent::Kind::ThrottleChanged: {
        auto reason = in.readString();
        auto processedFraction = in.readF64();
        if (!reason || !processedFraction) {
            return malformed();
        }
        ThrottleChangedData data;
        data.reason = std::move(*reason);
        data.processedFraction = *processedFraction;
        event.body = std::move(data);
        break;
    }

    case SessionEvent::Kind::DeviceError: {
        auto deviceId = in.readString();
        auto message = in.readString();
        if (!deviceId || !message) {
            return malformed();
        }
        DeviceErrorData data;
        data.deviceId = std::move(*deviceId);
        data.message = std::move(*message);
        event.body = std::move(data);
        break;
    }

    case SessionEvent::Kind::Plugin: {
        auto pluginId = in.readString();
        auto eventName = in.readString();
        if (!pluginId || !eventName) {
            return malformed();
        }
        auto fields = Metadata::decode(in);
        if (!fields) {
            return unexpected<Error>(fields.error());
        }
        PluginEventData data;
        data.pluginId = std::move(*pluginId);
        data.eventName = std::move(*eventName);
        data.fields = std::move(*fields);
        event.body = std::move(data);
        break;
    }

    case SessionEvent::Kind::Alert:
    default: {
        // Kept verbatim. The record header already carries `payloadBytes`, so an
        // unrecognised kind is skippable -- but skipping loses it, and a reader
        // that hands the bytes back is one an extraction can copy through.
        UnknownEventData data;
        data.body.resize(in.remaining());
        if (!data.body.empty()) {
            if (auto read = in.readBytes(data.body.data(), data.body.size()); !read) {
                return unexpected<Error>(read.error());
            }
        }
        event.body = std::move(data);
        break;
    }
    }

    // Deliberately no check that the payload is exhausted: a newer minor version
    // may have appended fields to a body, and ignoring a tail is what makes such
    // a bump invisible to this reader rather than fatal. See §11.5.
    return event;
}

std::uint32_t crc32(const void* data, std::size_t bytes, std::uint32_t seed) noexcept {
    const std::array<std::uint32_t, 256>& table = crcTable();
    const auto* input = static_cast<const std::uint8_t*>(data);

    std::uint32_t crc = ~seed;
    for (std::size_t i = 0; i < bytes; ++i) {
        crc = table[(crc ^ input[i]) & 0xFFU] ^ (crc >> 8U);
    }
    return ~crc;
}

// -------------------------------------------------------------- encoding

void writeU8(std::vector<std::byte>& out, std::uint8_t value) {
    out.push_back(static_cast<std::byte>(value));
}

void writeU16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFFU));
    out.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

void writeU32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}

void writeU64(std::vector<std::byte>& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}

void writeF32(std::vector<std::byte>& out, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    writeU32(out, bits);
}

void writeF64(std::vector<std::byte>& out, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    writeU64(out, bits);
}

void writeString(std::vector<std::byte>& out, std::string_view value) {
    writeU32(out, static_cast<std::uint32_t>(value.size()));
    writeBytes(out, value.data(), value.size());
}

void writeBytes(std::vector<std::byte>& out, const void* data, std::size_t bytes) {
    const auto* input = static_cast<const std::byte*>(data);
    out.insert(out.end(), input, input + bytes);
}

// -------------------------------------------------------------- decoding

Result<std::uint8_t> ByteReader::readU8() {
    if (remaining() < 1) {
        return fail<std::uint8_t>(ErrorCode::Corrupt, "truncated at offset {}", m_offset);
    }
    const auto value = static_cast<std::uint8_t>(m_data[m_offset]);
    m_offset += 1;
    return value;
}

Result<std::uint16_t> ByteReader::readU16() {
    if (remaining() < 2) {
        return fail<std::uint16_t>(ErrorCode::Corrupt, "truncated at offset {}", m_offset);
    }
    const auto low = static_cast<std::uint16_t>(m_data[m_offset]);
    const auto high = static_cast<std::uint16_t>(m_data[m_offset + 1]);
    m_offset += 2;
    return static_cast<std::uint16_t>(low | (high << 8U));
}

Result<std::uint32_t> ByteReader::readU32() {
    if (remaining() < 4) {
        return fail<std::uint32_t>(ErrorCode::Corrupt, "truncated at offset {}", m_offset);
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(m_data[m_offset + static_cast<std::size_t>(i)])
                 << (8 * i);
    }
    m_offset += 4;
    return value;
}

Result<std::uint64_t> ByteReader::readU64() {
    if (remaining() < 8) {
        return fail<std::uint64_t>(ErrorCode::Corrupt, "truncated at offset {}", m_offset);
    }
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(m_data[m_offset + static_cast<std::size_t>(i)])
                 << (8 * i);
    }
    m_offset += 8;
    return value;
}

Result<float> ByteReader::readF32() {
    auto bits = readU32();
    if (!bits) {
        return unexpected<Error>(bits.error());
    }
    float value = 0.0F;
    std::memcpy(&value, &*bits, sizeof(value));
    return value;
}

Result<double> ByteReader::readF64() {
    auto bits = readU64();
    if (!bits) {
        return unexpected<Error>(bits.error());
    }
    double value = 0.0;
    std::memcpy(&value, &*bits, sizeof(value));
    return value;
}

Result<std::string> ByteReader::readString() {
    auto length = readU32();
    if (!length) {
        return unexpected<Error>(length.error());
    }
    if (remaining() < *length) {
        // A corrupt or hostile length field would otherwise allocate wildly.
        return fail<std::string>(ErrorCode::Corrupt,
                                 "string of {} bytes at offset {} exceeds the {} remaining",
                                 *length, m_offset, remaining());
    }
    std::string value(reinterpret_cast<const char*>(m_data + m_offset), *length);
    m_offset += *length;
    return value;
}

Status ByteReader::readBytes(void* destination, std::size_t bytes) {
    if (remaining() < bytes) {
        return fail(ErrorCode::Corrupt, "wanted {} bytes at offset {}, only {} remain", bytes,
                    m_offset, remaining());
    }
    // memcpy declares both pointers non-null, so a zero-length read must not
    // reach it even though it would copy nothing: an empty payload is decoded
    // into an empty vector, and vector::data() on that is allowed to be null.
    // A format in which absent and empty are different values reads empty
    // often enough for this to be the ordinary path, not an edge case.
    if (bytes == 0) {
        return ok();
    }
    std::memcpy(destination, m_data + m_offset, bytes);
    m_offset += bytes;
    return ok();
}

} // namespace sweeps
