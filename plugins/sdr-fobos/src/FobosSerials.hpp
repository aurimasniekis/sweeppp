// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The radios both Fobos libraries list, and the ids they are known by.
//
// Header-only so it is testable without a radio. The listing format is the one
// place a mistake does not fail loudly: an entry parsed one position out opens
// the wrong radio, under the right name.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace sweeppp::fobos {

/// Which firmware a radio runs, and so which library opens it.
enum class Firmware : std::uint8_t {
    Standard,
    Agile,
};

/// What both libraries list in place of a serial they could not read: a radio
/// seen on the bus that would not open, which on Linux is almost always a
/// missing udev rule.
inline constexpr std::string_view kUnreadableSerial = "XXXXXXXXXXXX";

/// A `*_list_devices()` result, one entry per radio, in the order the same
/// library's `*_open()` counts them. Unreadable serials come back empty.
///
/// Every entry is followed by exactly one space, and an entry can itself be
/// empty -- a radio that opened but has no serial string. So this splits on
/// each space rather than on runs of them, which would pull every later radio
/// one index forward. `count` is the library's own return value, and the
/// answer is always exactly that long.
[[nodiscard]] inline std::vector<std::string> parseSerials(std::string_view list,
                                                           std::size_t count) {
    std::vector<std::string> serials;
    serials.reserve(count);

    while (serials.size() < count && !list.empty()) {
        const std::size_t space = list.find(' ');
        serials.emplace_back(list.substr(0, space));
        list = space == std::string_view::npos ? std::string_view{} : list.substr(space + 1);
    }
    serials.resize(count);

    for (std::string& serial : serials) {
        if (serial == kUnreadableSerial) {
            serial.clear();
        }
    }
    return serials;
}

struct ListedRadio {
    Firmware firmware = Firmware::Standard;
    std::uint32_t index = 0; ///< Within its own library's list
    std::string serial;
    std::string id;
};

/// Where a radio sits in its library's list, as an id: "index-0",
/// "agile-index-0". The prefix keeps the two lists' positions apart.
[[nodiscard]] inline std::string positionalId(Firmware firmware, std::uint32_t index) {
    return std::format("{}index-{}", firmware == Firmware::Agile ? "agile-" : "", index);
}

/// Every radio both libraries list, standard firmware first.
///
/// The id is the serial wherever that is readable and names one radio across
/// both lists, because a serial is what survives replugging. Otherwise it is
/// the radio's position, which is at least unambiguous until something is
/// plugged in or out.
[[nodiscard]] inline std::vector<ListedRadio> listedRadios(const std::vector<std::string>& standard,
                                                           const std::vector<std::string>& agile) {
    std::vector<ListedRadio> radios;
    radios.reserve(standard.size() + agile.size());

    const auto append = [&radios](Firmware firmware, const std::vector<std::string>& serials) {
        for (std::size_t i = 0; i < serials.size(); ++i) {
            radios.push_back({.firmware = firmware,
                              .index = static_cast<std::uint32_t>(i),
                              .serial = serials[i],
                              .id = {}});
        }
    };
    append(Firmware::Standard, standard);
    append(Firmware::Agile, agile);

    for (ListedRadio& radio : radios) {
        const bool unique = !radio.serial.empty() &&
                            std::ranges::count_if(radios, [&radio](const ListedRadio& other) {
                                return other.serial == radio.serial;
                            }) == 1;
        radio.id = unique ? radio.serial : positionalId(radio.firmware, radio.index);
    }
    return radios;
}

} // namespace sweeppp::fobos
