// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/// One switch, shared by every facet wrapper.
///
/// Nothing about it is specific to a facet: an SDR driver's `open` and an FFT
/// backend's `createPlan` both answer with a `Result` and both have to put its
/// code on the wire.
///
/// It is not in `Plugin.hpp` for the reason stated at the top of that header --
/// it depends on `PluginAbi.h` and the standard library and nothing else, so
/// that a plugin can use it without linking libsweeppp at all. `ErrorCode` is
/// libsweepsfile's, and including it there would end that.

#include "sweeppp/core/Result.hpp"
#include "sweeppp/plugin/PluginAbi.h"

namespace sweeppp::plugin {

/// Switches rather than casts. The two enumerations are unrelated and their
/// numbering is not; a code added to `ErrorCode` and not handled here is a
/// compile error, rather than a status the host reads as something else.
[[nodiscard]] constexpr sweeppp_plugin_status_t toAbiStatus(ErrorCode code) noexcept {
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

} // namespace sweeppp::plugin
