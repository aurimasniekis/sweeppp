// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/PluginAbiSupport.hpp"

namespace sweeppp::plugin_abi {

sweeppp_plugin_status_t toStatus(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::InvalidArgument:
        return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
    case ErrorCode::NotFound:
        return SWEEPPP_PLUGIN_ERR_NOT_FOUND;
    case ErrorCode::Unsupported:
        return SWEEPPP_PLUGIN_ERR_UNSUPPORTED;
    case ErrorCode::Unavailable:
        return SWEEPPP_PLUGIN_ERR_UNAVAILABLE;
    case ErrorCode::IoError:
        return SWEEPPP_PLUGIN_ERR_IO;
    case ErrorCode::ParseError:
        return SWEEPPP_PLUGIN_ERR_PARSE;
    case ErrorCode::OutOfRange:
        return SWEEPPP_PLUGIN_ERR_OUT_OF_RANGE;
    case ErrorCode::OutOfMemory:
        return SWEEPPP_PLUGIN_ERR_OUT_OF_MEMORY;
    case ErrorCode::TimedOut:
        return SWEEPPP_PLUGIN_ERR_TIMED_OUT;
    case ErrorCode::Cancelled:
        return SWEEPPP_PLUGIN_ERR_CANCELLED;
    case ErrorCode::PermissionDenied:
        return SWEEPPP_PLUGIN_ERR_PERMISSION_DENIED;
    case ErrorCode::AlreadyExists:
        return SWEEPPP_PLUGIN_ERR_ALREADY_EXISTS;
    case ErrorCode::DeviceError:
        return SWEEPPP_PLUGIN_ERR_DEVICE;
    case ErrorCode::ProtocolError:
        return SWEEPPP_PLUGIN_ERR_PROTOCOL;
    case ErrorCode::Corrupt:
        return SWEEPPP_PLUGIN_ERR_CORRUPT;
    case ErrorCode::Unknown:
        break;
    }
    return SWEEPPP_PLUGIN_ERR_UNKNOWN;
}

ErrorCode toErrorCode(sweeppp_plugin_status_t status) noexcept {
    switch (status) {
    case SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT:
        return ErrorCode::InvalidArgument;
    case SWEEPPP_PLUGIN_ERR_NOT_FOUND:
        return ErrorCode::NotFound;
    case SWEEPPP_PLUGIN_ERR_UNSUPPORTED:
        return ErrorCode::Unsupported;
    case SWEEPPP_PLUGIN_ERR_UNAVAILABLE:
        return ErrorCode::Unavailable;
    case SWEEPPP_PLUGIN_ERR_IO:
        return ErrorCode::IoError;
    case SWEEPPP_PLUGIN_ERR_PARSE:
        return ErrorCode::ParseError;
    case SWEEPPP_PLUGIN_ERR_OUT_OF_RANGE:
        return ErrorCode::OutOfRange;
    case SWEEPPP_PLUGIN_ERR_OUT_OF_MEMORY:
        return ErrorCode::OutOfMemory;
    case SWEEPPP_PLUGIN_ERR_TIMED_OUT:
        return ErrorCode::TimedOut;
    case SWEEPPP_PLUGIN_ERR_CANCELLED:
        return ErrorCode::Cancelled;
    case SWEEPPP_PLUGIN_ERR_PERMISSION_DENIED:
        return ErrorCode::PermissionDenied;
    case SWEEPPP_PLUGIN_ERR_ALREADY_EXISTS:
        return ErrorCode::AlreadyExists;
    case SWEEPPP_PLUGIN_ERR_DEVICE:
        return ErrorCode::DeviceError;
    case SWEEPPP_PLUGIN_ERR_PROTOCOL:
        return ErrorCode::ProtocolError;
    case SWEEPPP_PLUGIN_ERR_CORRUPT:
        return ErrorCode::Corrupt;
    case SWEEPPP_PLUGIN_OK:
    case SWEEPPP_PLUGIN_ERR_UNKNOWN:
    case SWEEPPP_PLUGIN_STATUS_FORCE_INT32:
        break;
    }
    return ErrorCode::Unknown;
}

std::string_view statusName(sweeppp_plugin_status_t status) noexcept {
    switch (status) {
    case SWEEPPP_PLUGIN_OK:
        return "ok";
    case SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case SWEEPPP_PLUGIN_ERR_NOT_FOUND:
        return "not found";
    case SWEEPPP_PLUGIN_ERR_UNSUPPORTED:
        return "unsupported";
    case SWEEPPP_PLUGIN_ERR_UNAVAILABLE:
        return "unavailable";
    case SWEEPPP_PLUGIN_ERR_IO:
        return "io error";
    case SWEEPPP_PLUGIN_ERR_PARSE:
        return "parse error";
    case SWEEPPP_PLUGIN_ERR_OUT_OF_RANGE:
        return "out of range";
    case SWEEPPP_PLUGIN_ERR_OUT_OF_MEMORY:
        return "out of memory";
    case SWEEPPP_PLUGIN_ERR_TIMED_OUT:
        return "timed out";
    case SWEEPPP_PLUGIN_ERR_CANCELLED:
        return "cancelled";
    case SWEEPPP_PLUGIN_ERR_PERMISSION_DENIED:
        return "permission denied";
    case SWEEPPP_PLUGIN_ERR_ALREADY_EXISTS:
        return "already exists";
    case SWEEPPP_PLUGIN_ERR_DEVICE:
        return "device error";
    case SWEEPPP_PLUGIN_ERR_PROTOCOL:
        return "protocol error";
    case SWEEPPP_PLUGIN_ERR_CORRUPT:
        return "corrupt";
    case SWEEPPP_PLUGIN_ERR_UNKNOWN:
    case SWEEPPP_PLUGIN_STATUS_FORCE_INT32:
        break;
    }
    return "unknown error";
}

sweeppp_log_level_t toAbiLevel(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return SWEEPPP_LOG_TRACE;
    case LogLevel::Debug:
        return SWEEPPP_LOG_DEBUG;
    case LogLevel::Warn:
        return SWEEPPP_LOG_WARN;
    case LogLevel::Error:
        return SWEEPPP_LOG_ERROR;
    case LogLevel::Info:
        break;
    }
    return SWEEPPP_LOG_INFO;
}

LogLevel fromAbiLevel(sweeppp_log_level_t level) noexcept {
    switch (level) {
    case SWEEPPP_LOG_TRACE:
        return LogLevel::Trace;
    case SWEEPPP_LOG_DEBUG:
        return LogLevel::Debug;
    case SWEEPPP_LOG_WARN:
        return LogLevel::Warn;
    case SWEEPPP_LOG_ERROR:
        return LogLevel::Error;
    case SWEEPPP_LOG_INFO:
    case SWEEPPP_LOG_FORCE_INT32:
        break;
    }
    return LogLevel::Info;
}

std::optional<SdrValue> fromAbiValue(const sweeppp_value_t& value) {
    switch (value.type) {
    case SWEEPPP_VALUE_STRING:
        return SdrValue{owned(value.text)};
    case SWEEPPP_VALUE_INT:
        return SdrValue{value.integer};
    case SWEEPPP_VALUE_FLOAT:
        return SdrValue{value.number};
    case SWEEPPP_VALUE_BOOL:
        return SdrValue{value.boolean != 0};
    case SWEEPPP_VALUE_ABSENT:
    case SWEEPPP_VALUE_TYPE_FORCE_INT32:
        break;
    }
    return std::nullopt;
}

void toAbiValue(const SdrValue& value, sweeppp_value_t& out) {
    out = sweeppp_value_t{};
    out.struct_size = sizeof(sweeppp_value_t);
    std::visit(
        [&out](const auto& held) {
            using Held = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<Held, bool>) {
                out.type = SWEEPPP_VALUE_BOOL;
                out.boolean = held ? 1 : 0;
            } else if constexpr (std::is_same_v<Held, std::int64_t>) {
                out.type = SWEEPPP_VALUE_INT;
                out.integer = held;
            } else if constexpr (std::is_same_v<Held, double>) {
                out.type = SWEEPPP_VALUE_FLOAT;
                out.number = held;
            } else {
                out.type = SWEEPPP_VALUE_STRING;
                out.text = borrow(held);
            }
        },
        value);
}

SampleFormat fromAbiFormat(sweeppp_sdr_format_t format) noexcept {
    switch (format) {
    case SWEEPPP_SDR_FORMAT_CU8:
        return SampleFormat::Cu8;
    case SWEEPPP_SDR_FORMAT_CS8:
        return SampleFormat::Cs8;
    case SWEEPPP_SDR_FORMAT_CU12:
        return SampleFormat::Cu12;
    case SWEEPPP_SDR_FORMAT_CS12:
        return SampleFormat::Cs12;
    case SWEEPPP_SDR_FORMAT_CU16:
        return SampleFormat::Cu16;
    case SWEEPPP_SDR_FORMAT_CS16:
        return SampleFormat::Cs16;
    case SWEEPPP_SDR_FORMAT_CU32:
        return SampleFormat::Cu32;
    case SWEEPPP_SDR_FORMAT_CS32:
        return SampleFormat::Cs32;
    case SWEEPPP_SDR_FORMAT_CF16:
        return SampleFormat::Cf16;
    case SWEEPPP_SDR_FORMAT_CF32:
        return SampleFormat::Cf32;
    case SWEEPPP_SDR_FORMAT_CF64:
        return SampleFormat::Cf64;
    case SWEEPPP_SDR_FORMAT_FORCE_INT32:
        break;
    }
    // A format this build has no name for. Cs8 is the narrowest of them, so a
    // block sized for it cannot be read past the end of whatever it really was.
    return SampleFormat::Cs8;
}

sweeppp_sdr_format_t toAbiFormat(SampleFormat format) noexcept {
    switch (format) {
    case SampleFormat::Cu8:
        return SWEEPPP_SDR_FORMAT_CU8;
    case SampleFormat::Cs8:
        return SWEEPPP_SDR_FORMAT_CS8;
    case SampleFormat::Cu12:
        return SWEEPPP_SDR_FORMAT_CU12;
    case SampleFormat::Cs12:
        return SWEEPPP_SDR_FORMAT_CS12;
    case SampleFormat::Cu16:
        return SWEEPPP_SDR_FORMAT_CU16;
    case SampleFormat::Cs16:
        return SWEEPPP_SDR_FORMAT_CS16;
    case SampleFormat::Cu32:
        return SWEEPPP_SDR_FORMAT_CU32;
    case SampleFormat::Cs32:
        return SWEEPPP_SDR_FORMAT_CS32;
    case SampleFormat::Cf16:
        return SWEEPPP_SDR_FORMAT_CF16;
    case SampleFormat::Cf32:
        return SWEEPPP_SDR_FORMAT_CF32;
    case SampleFormat::Cf64:
        return SWEEPPP_SDR_FORMAT_CF64;
    }
    return SWEEPPP_SDR_FORMAT_CS8;
}

SdrParameterType fromAbiParameterType(sweeppp_sdr_parameter_type_t type) noexcept {
    switch (type) {
    case SWEEPPP_SDR_PARAM_BOOL:
        return SdrParameterType::Bool;
    case SWEEPPP_SDR_PARAM_INT:
        return SdrParameterType::Int;
    case SWEEPPP_SDR_PARAM_DOUBLE:
        return SdrParameterType::Double;
    case SWEEPPP_SDR_PARAM_ENUM:
        return SdrParameterType::Enum;
    case SWEEPPP_SDR_PARAM_STRING:
        return SdrParameterType::String;
    case SWEEPPP_SDR_PARAM_TYPE_FORCE_INT32:
        break;
    }
    return SdrParameterType::Double;
}

} // namespace sweeppp::plugin_abi
