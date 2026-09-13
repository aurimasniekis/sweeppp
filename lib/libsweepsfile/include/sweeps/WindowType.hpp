// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#pragma once

#include "sweeps/Result.hpp"

#include <cstdint>
#include <string_view>

namespace sweeps {

/// The FFT window a recording was taken with.
///
/// Part of the format, not of any particular DSP implementation: the numeric
/// value is stored in every `SegmentOpen` record, and a reader that wants to
/// interpret levels needs to know which window produced them. Values are
/// frozen -- see §4.3.1 of the specification.
///
/// The window generator itself deliberately stays in the application. A reader
/// needs the *name* of the window, never its coefficients.
enum class WindowType : std::uint8_t {
    Rectangular = 0,
    Hann = 1,
    Hamming = 2,
    BlackmanHarris = 3,
    FlatTop = 4,
    Kaiser = 5,
};

/// The canonical spelling: "hann", "blackman-harris", "flat-top".
///
/// This one reaches file bytes. A segment opened because the operator changed
/// window records the reason as "window hann -> flat-top", and that string is
/// written into the `SegmentOpen` record.
[[nodiscard]] std::string_view toString(WindowType type) noexcept;

/// Parses the spellings people actually write, including ones no UI produces:
/// these values come from hand-edited configuration as well as from files.
/// Case-insensitive.
[[nodiscard]] Result<WindowType> windowTypeFromString(std::string_view name);

} // namespace sweeps
