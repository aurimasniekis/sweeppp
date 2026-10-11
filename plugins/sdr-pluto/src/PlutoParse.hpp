// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The strings libiio and the Pluto's kernel hand back, read into values.
//
// Header-only and free of libiio, so it is testable without a radio. Every one
// of these is text a firmware update could reword, and a parse that goes wrong
// quietly -- a range read as zero, a serial cut short -- opens the wrong radio
// or offers the wrong span rather than failing.

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sweeppp::pluto {

/// The Pluto's USB identity, which the scan is narrowed to. Everything else
/// libiio can reach over USB is some other board.
inline constexpr std::string_view kUsbScan = "usb=0456:b673";

/// An IIO "available" attribute in its range form, `[min step max]`.
struct IioRange {
    double min = 0.0;
    double step = 0.0;
    double max = 0.0;
};

/// The board, as its `hw_model` context attribute names it.
struct HardwareModel {
    std::string revision; ///< "Rev.C"; empty when the model does not say.
    std::string chip;     ///< "AD9363A"; empty when the model does not say.
};

/// The serial in a USB scan entry's description, which libiio writes as
/// `0456:b673 (Analog Devices Inc. PlutoSDR (ADALM-PLUTO)), serial=104473...`.
/// Empty when there is none.
[[nodiscard]] inline std::string serialFromDescription(std::string_view description) {
    constexpr std::string_view kKey = "serial=";
    const std::size_t at = description.rfind(kKey);
    if (at == std::string_view::npos) {
        return {};
    }
    std::string_view serial = description.substr(at + kKey.size());
    serial = serial.substr(0, serial.find_first_of(" ,)\t\r\n"));
    return std::string(serial);
}

/// Whether a device id is a libiio URI rather than a serial.
///
/// A serial is hex digits and never contains a colon; every libiio backend
/// prefix ends in one. So `ip:192.168.2.1` is opened as given, and anything
/// else is looked up among the radios the scan found.
[[nodiscard]] inline bool isUri(std::string_view id) noexcept {
    constexpr std::array<std::string_view, 4> kSchemes{"usb:", "ip:", "serial:", "local:"};
    return std::ranges::any_of(kSchemes,
                               [id](std::string_view scheme) { return id.starts_with(scheme); });
}

/// Whether a URI reaches the radio over the network rather than USB.
[[nodiscard]] inline bool isNetworkUri(std::string_view uri) noexcept {
    return uri.starts_with("ip:");
}

/// Reads `[min step max]`. Nothing when the text is a list of values instead,
/// or not a range at all.
///
/// Integers only: every range the AD9361 driver reports is in whole hertz or
/// whole decibels, and the floating-point `from_chars` is not in this
/// deployment target's library.
[[nodiscard]] inline std::optional<IioRange> parseRange(std::string_view text) {
    const std::size_t open = text.find('[');
    const std::size_t close = text.find(']', open);
    if (open == std::string_view::npos || close == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view inside = text.substr(open + 1, close - open - 1);

    std::array<long long, 3> values{};
    for (long long& value : values) {
        const std::size_t start = inside.find_first_not_of(' ');
        if (start == std::string_view::npos) {
            return std::nullopt;
        }
        inside.remove_prefix(start);
        const auto [end, ec] = std::from_chars(inside.data(), inside.data() + inside.size(), value);
        if (ec != std::errc{}) {
            return std::nullopt;
        }
        inside.remove_prefix(static_cast<std::size_t>(end - inside.data()));
    }
    if (inside.find_first_not_of(' ') != std::string_view::npos || values[0] > values[2]) {
        return std::nullopt;
    }
    return IioRange{.min = static_cast<double>(values[0]),
                    .step = static_cast<double>(values[1]),
                    .max = static_cast<double>(values[2])};
}

/// Splits a space-separated list, as `gain_control_mode_available` is.
[[nodiscard]] inline std::vector<std::string> splitWords(std::string_view text) {
    std::vector<std::string> words;
    while (!text.empty()) {
        const std::size_t start = text.find_first_not_of(" \t\r\n");
        if (start == std::string_view::npos) {
            break;
        }
        text.remove_prefix(start);
        const std::size_t end = text.find_first_of(" \t\r\n");
        words.emplace_back(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end);
    }
    return words;
}

/// `Analog Devices PlutoSDR Rev.C (Z7010-AD9363A)` into `Rev.C` and `AD9363A`.
[[nodiscard]] inline HardwareModel parseHardwareModel(std::string_view model) {
    HardwareModel parsed;

    const std::size_t rev = model.find("Rev.");
    if (rev != std::string_view::npos) {
        std::string_view revision = model.substr(rev);
        parsed.revision = std::string(revision.substr(0, revision.find_first_of(" (")));
    }

    // The part in parentheses is `<FPGA>-<RF chip>`; the chip is the last
    // dash-separated piece.
    const std::size_t open = model.rfind('(');
    const std::size_t close = model.rfind(')');
    if (open != std::string_view::npos && close != std::string_view::npos && close > open) {
        const std::string_view inside = model.substr(open + 1, close - open - 1);
        const std::size_t dash = inside.rfind('-');
        parsed.chip =
            std::string(dash == std::string_view::npos ? inside : inside.substr(dash + 1));
    }
    return parsed;
}

} // namespace sweeppp::pluto
