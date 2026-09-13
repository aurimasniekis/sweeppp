// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Log.hpp"
#include "sweeppp/core/Result.hpp"
#include "sweeppp/plugin/PluginAbi.h"
#include "sweeppp/sdr/SampleFormat.hpp"
#include "sweeppp/sdr/SdrParameter.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace sweeppp::plugin_abi {

/// The category everything about plugins is logged under. Named here so the
/// host and every facet shim agree, and so grepping a log for plugin trouble
/// is one word.
inline constexpr std::string_view kLogCategory = "plugin";

[[nodiscard]] inline std::string_view view(sweeppp_str_t text) noexcept {
    return text.data != nullptr ? std::string_view(text.data, text.len) : std::string_view{};
}

[[nodiscard]] inline std::string owned(sweeppp_str_t text) {
    return std::string(view(text));
}

/// A borrowed view of a string the caller keeps alive for the call.
[[nodiscard]] inline sweeppp_str_t borrow(std::string_view text) noexcept {
    return sweeppp_str_t{.data = text.empty() ? "" : text.data(), .len = text.size()};
}

/// Whether a struct built by the other side reaches as far as a field.
///
/// The `struct_size` rule in one place: both sides read only
/// `min(their size, the other's)`, so every access to a field that was not in
/// the first release of an ABI is guarded by this. A `struct_size` of 0 fails
/// every check, which is what makes a zeroed struct inert rather than a struct
/// full of plausible defaults.
template <typename T>
[[nodiscard]] constexpr bool reaches(const T* value, std::size_t endOffset) noexcept {
    return value != nullptr && value->struct_size >= endOffset;
}

#define SWEEPPP_ABI_HAS(ptr, field)                                                                \
    ::sweeppp::plugin_abi::reaches((ptr), offsetof(std::remove_cvref_t<decltype(*(ptr))>, field) + \
                                              sizeof((ptr)->field))

/// Walks an array whose element type carries `struct_size`.
///
/// The stride is the FIRST element's `struct_size`, per the array rule in the
/// ABI header: all elements of one array come from one build, so one stride is
/// well-defined, and indexing by our own `sizeof` would walk off into the
/// middle of elements the moment a plugin was built against a later ABI.
template <typename T, typename Fn>
void forEachElement(const T* array, std::uint32_t count, Fn&& callback) {
    if (array == nullptr || count == 0) {
        return;
    }

    const std::size_t stride = array->struct_size;
    if (stride < sizeof(std::uint32_t)) {
        // A zero or nonsensical stride would make every element the same one.
        logWarn(kLogCategory, "ignoring an array of {} whose element size is {}", count, stride);
        return;
    }

    const auto* bytes = reinterpret_cast<const std::byte*>(array);
    for (std::uint32_t i = 0; i < count; ++i) {
        callback(*reinterpret_cast<const T*>(bytes + (static_cast<std::size_t>(i) * stride)));
    }
}

/// The ABI status for one of our errors, and back.
///
/// A cast would work -- the numberings are deliberately identical -- but a
/// switch is what makes that identity checkable rather than assumed, and it is
/// what catches an `ErrorCode` gaining a value this ABI has no name for.
[[nodiscard]] sweeppp_plugin_status_t toStatus(ErrorCode code) noexcept;
[[nodiscard]] ErrorCode toErrorCode(sweeppp_plugin_status_t status) noexcept;

/// What a status means, for a message an operator reads.
[[nodiscard]] std::string_view statusName(sweeppp_plugin_status_t status) noexcept;

[[nodiscard]] sweeppp_log_level_t toAbiLevel(LogLevel level) noexcept;
[[nodiscard]] LogLevel fromAbiLevel(sweeppp_log_level_t level) noexcept;

/// A typed scalar, both ways.
///
/// An absent or unrecognised type produces nothing rather than a default, so a
/// plugin cannot write a hole into a profile or a parameter and have it read
/// back as a plausible zero.
///
/// `toAbiValue` BORROWS a string alternative: `out.text` points into `value`,
/// so the caller keeps `value` alive for as long as `out` is read.
[[nodiscard]] std::optional<SdrValue> fromAbiValue(const sweeppp_value_t& value);
void toAbiValue(const SdrValue& value, sweeppp_value_t& out);

/// A sample format, both ways.
///
/// Switches rather than casts, although the numbering is deliberately
/// identical: an enumerator added to one enum and not the other is then a
/// compile error in this file, rather than a buffer read at the wrong stride
/// somewhere downstream.
[[nodiscard]] SampleFormat fromAbiFormat(sweeppp_sdr_format_t format) noexcept;
[[nodiscard]] sweeppp_sdr_format_t toAbiFormat(SampleFormat format) noexcept;

[[nodiscard]] SdrParameterType fromAbiParameterType(sweeppp_sdr_parameter_type_t type) noexcept;

} // namespace sweeppp::plugin_abi
