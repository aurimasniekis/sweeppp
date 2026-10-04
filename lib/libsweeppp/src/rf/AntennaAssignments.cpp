// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/rf/AntennaAssignments.hpp"

#include "sweeppp/core/Log.hpp"
#include "sweeppp/core/Toml.hpp"

#include <algorithm>
#include <format>

namespace sweeppp {
namespace {

/// Whether an entry describes a connector rather than a switcher input.
bool isPortEntry(const AntennaAssignment& entry) {
    return !entry.device.empty();
}

} // namespace

AntennaAssignments
AntennaAssignments::of(std::vector<AntennaAssignment> entries,
                       std::vector<std::pair<std::string, std::string>> fallbackPorts) {
    AntennaAssignments assignments;
    assignments.m_entries = std::move(entries);
    assignments.m_fallbackPorts = std::move(fallbackPorts);
    return assignments;
}

AntennaAssignments AntennaAssignments::load(const std::filesystem::path& path) {
    AntennaAssignments assignments;

    auto table = toml_util::load(path);
    if (!table) {
        // Absent is what a bench nobody has described yet looks like.
        if (table.error().code() != ErrorCode::NotFound) {
            logWarn("antennas", "{}", table.error().describe());
        }
        return assignments;
    }

    if (const auto* fallbacks = toml_util::at(*table, "fallback").as_array()) {
        for (const auto& node : *fallbacks) {
            const auto* declared = node.as_table();
            if (declared == nullptr) {
                continue;
            }
            assignments.setFallbackPort(toml_util::getString(*declared, "device", ""),
                                        toml_util::getString(*declared, "port", ""));
        }
    }

    const auto* rows = toml_util::at(*table, "assignment").as_array();
    if (rows == nullptr) {
        return assignments;
    }

    for (const auto& node : *rows) {
        const auto* declared = node.as_table();
        if (declared == nullptr) {
            continue;
        }

        AntennaAssignment entry;
        entry.device = toml_util::getString(*declared, "device", "");
        entry.port = toml_util::getString(*declared, "port", "");
        entry.switcher = toml_util::getString(*declared, "switcher", "");
        entry.input = toml_util::getString(*declared, "input", "");
        entry.antenna = toml_util::getString(*declared, "antenna", "");

        if (isPortEntry(entry)) {
            if (!entry.switcher.empty()) {
                assignments.assignSwitcher(entry.device, entry.port, entry.switcher);
            } else if (!entry.antenna.empty()) {
                assignments.assign(entry.device, entry.port, entry.antenna);
            } else {
                logWarn("antennas",
                        "{}: a connector entry names neither an antenna nor a "
                        "switcher",
                        path.string());
            }
            continue;
        }

        if (entry.switcher.empty() || entry.input.empty() || entry.antenna.empty()) {
            logWarn("antennas", "{}: an entry names no device and no switcher input",
                    path.string());
            continue;
        }
        assignments.assignInput(entry.switcher, entry.input, entry.antenna);
    }

    return assignments;
}

Status AntennaAssignments::save(const std::filesystem::path& path) const {
    ::toml::table root;
    ::toml::array rows;

    for (const AntennaAssignment& entry : m_entries) {
        ::toml::table row;
        if (!entry.device.empty()) {
            row.insert_or_assign("device", entry.device);
            row.insert_or_assign("port", entry.port);
        }
        if (!entry.switcher.empty()) {
            row.insert_or_assign("switcher", entry.switcher);
        }
        if (!entry.input.empty()) {
            row.insert_or_assign("input", entry.input);
        }
        if (!entry.antenna.empty()) {
            row.insert_or_assign("antenna", entry.antenna);
        }
        rows.push_back(std::move(row));
    }

    ::toml::array fallbacks;
    for (const auto& [device, port] : m_fallbackPorts) {
        ::toml::table row;
        row.insert_or_assign("device", device);
        row.insert_or_assign("port", port);
        fallbacks.push_back(std::move(row));
    }

    root.insert_or_assign("assignment", std::move(rows));
    root.insert_or_assign("fallback", std::move(fallbacks));
    return toml_util::save(path, root, "Sweep++ antenna assignments -- what is on which connector");
}

std::string AntennaAssignments::deviceKey(const SdrDeviceInfo& info) {
    const std::string& stable = info.serial.empty() ? info.id : info.serial;
    return std::format("{}:{}", info.driver, stable);
}

std::string_view AntennaAssignments::antennaFor(std::string_view device,
                                                std::string_view port) const {
    const auto match =
        std::ranges::find_if(m_entries, [device, port](const AntennaAssignment& entry) {
            return entry.device == device && entry.port == port;
        });
    return match != m_entries.end() ? std::string_view(match->antenna) : std::string_view{};
}

void AntennaAssignments::assign(std::string_view device, std::string_view port,
                                std::string_view antennaId) {
    const auto match =
        std::ranges::find_if(m_entries, [device, port](const AntennaAssignment& entry) {
            return entry.device == device && entry.port == port;
        });

    if (antennaId.empty()) {
        if (match != m_entries.end()) {
            m_entries.erase(match);
        }
        return;
    }

    if (match != m_entries.end()) {
        // A connector carries one thing. Putting an antenna on a port that had
        // a switcher detaches the switcher rather than leaving the chain with
        // two heads.
        match->switcher.clear();
        match->antenna = antennaId;
        return;
    }

    m_entries.push_back(AntennaAssignment{.device = std::string(device),
                                          .port = std::string(port),
                                          .antenna = std::string(antennaId)});
}

std::string_view AntennaAssignments::switcherFor(std::string_view device,
                                                 std::string_view port) const {
    const auto match =
        std::ranges::find_if(m_entries, [device, port](const AntennaAssignment& entry) {
            return entry.device == device && entry.port == port;
        });
    return match != m_entries.end() ? std::string_view(match->switcher) : std::string_view{};
}

void AntennaAssignments::assignSwitcher(std::string_view device, std::string_view port,
                                        std::string_view switcherKey) {
    if (!switcherKey.empty()) {
        // One box, one output, one port. Detached from wherever it was rather
        // than refused: the operator has physically moved the cable, and an
        // error would leave the file describing a bench that no longer exists.
        for (AntennaAssignment& entry : m_entries) {
            if (entry.switcher == switcherKey && isPortEntry(entry) &&
                !(entry.device == device && entry.port == port)) {
                entry.switcher.clear();
            }
        }
        std::erase_if(m_entries, [](const AntennaAssignment& entry) {
            return isPortEntry(entry) && entry.switcher.empty() && entry.antenna.empty();
        });
    }

    const auto match =
        std::ranges::find_if(m_entries, [device, port](const AntennaAssignment& entry) {
            return entry.device == device && entry.port == port;
        });

    if (switcherKey.empty()) {
        if (match != m_entries.end()) {
            m_entries.erase(match);
        }
        return;
    }

    if (match != m_entries.end()) {
        match->antenna.clear();
        match->switcher = switcherKey;
        return;
    }

    m_entries.push_back(AntennaAssignment{.device = std::string(device),
                                          .port = std::string(port),
                                          .switcher = std::string(switcherKey)});
}

std::pair<std::string, std::string>
AntennaAssignments::portOfSwitcher(std::string_view switcherKey) const {
    const auto match =
        std::ranges::find_if(m_entries, [switcherKey](const AntennaAssignment& entry) {
            return isPortEntry(entry) && entry.switcher == switcherKey;
        });
    if (match == m_entries.end()) {
        return {};
    }
    return {match->device, match->port};
}

std::string_view AntennaAssignments::antennaOnInput(std::string_view switcherKey,
                                                    std::string_view inputId) const {
    const auto match =
        std::ranges::find_if(m_entries, [switcherKey, inputId](const AntennaAssignment& entry) {
            return !isPortEntry(entry) && entry.switcher == switcherKey && entry.input == inputId;
        });
    return match != m_entries.end() ? std::string_view(match->antenna) : std::string_view{};
}

void AntennaAssignments::assignInput(std::string_view switcherKey, std::string_view inputId,
                                     std::string_view antennaId) {
    const auto match =
        std::ranges::find_if(m_entries, [switcherKey, inputId](const AntennaAssignment& entry) {
            return !isPortEntry(entry) && entry.switcher == switcherKey && entry.input == inputId;
        });

    if (antennaId.empty()) {
        if (match != m_entries.end()) {
            m_entries.erase(match);
        }
        return;
    }

    if (match != m_entries.end()) {
        match->antenna = antennaId;
        return;
    }

    m_entries.push_back(AntennaAssignment{.switcher = std::string(switcherKey),
                                          .input = std::string(inputId),
                                          .antenna = std::string(antennaId)});
}

std::string_view AntennaAssignments::fallbackPort(std::string_view device) const {
    const auto match = std::ranges::find_if(
        m_fallbackPorts, [device](const auto& entry) { return entry.first == device; });
    return match != m_fallbackPorts.end() ? std::string_view(match->second) : std::string_view{};
}

void AntennaAssignments::setFallbackPort(std::string_view device, std::string_view portId) {
    if (device.empty()) {
        return;
    }

    const auto match = std::ranges::find_if(
        m_fallbackPorts, [device](const auto& entry) { return entry.first == device; });

    if (portId.empty()) {
        if (match != m_fallbackPorts.end()) {
            m_fallbackPorts.erase(match);
        }
        return;
    }

    if (match != m_fallbackPorts.end()) {
        match->second = portId;
        return;
    }
    m_fallbackPorts.emplace_back(std::string(device), std::string(portId));
}

} // namespace sweeppp
