// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include "sweeps/Result.hpp"

namespace sweeps {

std::string_view toString(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Unknown:
        return "Unknown";
    case ErrorCode::InvalidArgument:
        return "InvalidArgument";
    case ErrorCode::NotFound:
        return "NotFound";
    case ErrorCode::Unsupported:
        return "Unsupported";
    case ErrorCode::Unavailable:
        return "Unavailable";
    case ErrorCode::IoError:
        return "IoError";
    case ErrorCode::ParseError:
        return "ParseError";
    case ErrorCode::OutOfRange:
        return "OutOfRange";
    case ErrorCode::OutOfMemory:
        return "OutOfMemory";
    case ErrorCode::TimedOut:
        return "TimedOut";
    case ErrorCode::Cancelled:
        return "Cancelled";
    case ErrorCode::PermissionDenied:
        return "PermissionDenied";
    case ErrorCode::AlreadyExists:
        return "AlreadyExists";
    case ErrorCode::DeviceError:
        return "DeviceError";
    case ErrorCode::ProtocolError:
        return "ProtocolError";
    case ErrorCode::Corrupt:
        return "Corrupt";
    }
    return "Unknown";
}

std::string Error::describe() const {
    if (m_message.empty()) {
        return std::string(toString(m_code));
    }
    return detail::format("{}: {}", toString(m_code), m_message);
}

Error Error::withContext(std::string_view context) const {
    if (m_message.empty()) {
        return Error{m_code, std::string(context)};
    }
    return Error{m_code, detail::format("{}: {}", context, m_message)};
}

} // namespace sweeps
