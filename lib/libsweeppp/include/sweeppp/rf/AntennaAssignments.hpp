// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/sdr/SdrDeviceInfo.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sweeppp {

/// One link in the chain in front of the tuner.
///
/// Two shapes, distinguished by which fields are filled:
///
///  * `device` + `port`, carrying either an `antenna` or a `switcher` --
///    what is screwed onto that connector;
///  * `switcher` + `input`, carrying an `antenna` -- what is screwed onto that
///    input of that box.
///
/// One record type rather than two, because the file is one table and the
/// chain is read as a whole: port -> switcher -> input -> antenna.
struct AntennaAssignment {
    std::string device;   ///< `AntennaAssignments::deviceKey`, not a label
    std::string port;     ///< `SdrRxPort::id`
    std::string switcher; ///< "driver:id" of an RF path, or empty
    std::string input;    ///< `RfPathInput::id`, or empty
    std::string antenna;  ///< `Antenna::id`
};

/// Which antenna is on which connector, per radio.
///
/// Deliberately not in `Profile`. What is screwed onto RX1 is a fact about the
/// bench rather than about the job, and loading a sweep profile must not
/// silently rewire it -- the same argument `PluginSettings` gives for keeping
/// plugin state out of profiles, and `plugins.toml` is the existing precedent
/// for a per-installation store profiles do not touch.
class AntennaAssignments {
public:
    /// A missing file is the normal first-run state and not an error; a
    /// malformed one is logged and treated as empty, because an assignment
    /// nobody can read is better lost than allowed to route a sweep through
    /// half a table.
    [[nodiscard]] static AntennaAssignments load(const std::filesystem::path& path);

    [[nodiscard]] Status save(const std::filesystem::path& path) const;

    /// What a radio is filed under: its serial where it has one, its id
    /// otherwise, always qualified by the driver.
    ///
    /// The serial rather than the id, because the id is whatever the driver
    /// happened to hand back on this bus scan -- on some it is a USB address,
    /// which moves when the cable does. An antenna assignment that forgot
    /// itself because the radio was replugged into another socket would be
    /// worse than useless.
    [[nodiscard]] static std::string deviceKey(const SdrDeviceInfo& info);

    /// The antenna id on `port`, or empty.
    [[nodiscard]] std::string_view antennaFor(std::string_view device, std::string_view port) const;

    /// Assigns, or clears when `antennaId` is empty. Replaces a switcher that
    /// was on the same port: a connector carries one thing.
    void assign(std::string_view device, std::string_view port, std::string_view antennaId);

    /// The switcher behind `port`, or empty.
    [[nodiscard]] std::string_view switcherFor(std::string_view device,
                                               std::string_view port) const;

    /// Puts a switcher behind a connector, or clears it when `switcherKey` is
    /// empty.
    ///
    /// A switcher may be behind **one** port only, so this detaches it from
    /// wherever it was. It is a physical box with one output, and two ports
    /// claiming it would have the planner switching it against itself
    /// mid-pass -- a fault that shows up as the wrong antenna's spectrum
    /// rather than as an error.
    void assignSwitcher(std::string_view device, std::string_view port,
                        std::string_view switcherKey);

    /// Where a switcher is attached, as `{device, port}`, or empty strings.
    [[nodiscard]] std::pair<std::string, std::string>
    portOfSwitcher(std::string_view switcherKey) const;

    /// The antenna on one input of a switcher, or empty.
    [[nodiscard]] std::string_view antennaOnInput(std::string_view switcherKey,
                                                  std::string_view inputId) const;

    void assignInput(std::string_view switcherKey, std::string_view inputId,
                     std::string_view antennaId);

    /// Which connector measures the frequencies no assigned antenna covers,
    /// or empty to leave whichever is selected alone.
    ///
    /// A bench fact like the rest of this file, and the operator's answer to
    /// "the sweep has to point *something* at that band -- which?". Without
    /// one, an uncovered range is measured through whatever the previous step
    /// happened to leave selected, which is reproducible but arbitrary.
    [[nodiscard]] std::string_view fallbackPort(std::string_view device) const;
    void setFallbackPort(std::string_view device, std::string_view portId);

    [[nodiscard]] std::span<const AntennaAssignment> entries() const noexcept { return m_entries; }

private:
    std::vector<AntennaAssignment> m_entries;

    /// Per device, the connector uncovered ranges are swept on. A separate
    /// table rather than a flag on an assignment: the port it names may have
    /// no antenna at all, which is the ordinary case for a wideband whip left
    /// on RX1 for exactly this.
    std::vector<std::pair<std::string, std::string>> m_fallbackPorts;
};

} // namespace sweeppp
