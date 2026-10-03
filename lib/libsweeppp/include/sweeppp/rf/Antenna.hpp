// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sweeppp {

/// One antenna the operator owns.
///
/// Host-side rather than in a plugin because the sweep planner reads it: a
/// routed pass has to know which frequencies each connector can actually hear,
/// and a plugin's data on the far side of the ABI would have to be marshalled
/// per plan.
struct Antenna {
    std::string id;       ///< Stable; what an assignment stores. "d190"
    std::string name;     ///< "Diamond D-190"
    std::string category; ///< What it is for: "Wideband", "ADS-B", "GPS"
    std::string type;     ///< What it is: "discone", "yagi", "whip"
    double startHz = 0.0;
    double stopHz = 0.0;
    double gainDbi = 0.0;
    bool needsBiasT = false; ///< Wants DC on the feed: an active antenna or an LNA
    std::string notes;

    /// Shipped with the build. Listed but not editable, so a built-in never
    /// silently becomes something else; editing one writes a copy carrying the
    /// same id into the user file, which then shadows it.
    bool builtin = false;

    /// Closed at both ends. An antenna specified 100 MHz - 1 GHz does cover
    /// 1 GHz, which is not true of the half-open channel ranges elsewhere.
    [[nodiscard]] bool covers(double hz) const noexcept;

    /// The *whole* span, not any part of it. Routing asks "can this antenna
    /// measure this step", and a step half inside the passband is a step half
    /// measured through a stopband.
    [[nodiscard]] bool covers(double fromHz, double toHz) const noexcept;

    [[nodiscard]] double widthHz() const noexcept { return stopHz - startHz; }

    /// "10 MHz - 1.5 GHz"
    [[nodiscard]] std::string describeRange() const;
};

/// Every antenna the operator has told the application about.
///
/// A value type over `toml_util`, taking its directories as arguments rather
/// than reaching for `Paths::instance()`, so the tests drive it directly.
class AntennaLibrary {
public:
    /// A file that will not parse at all fails; a row inside one that will not
    /// is appended to `problems` and skipped, so one bad entry costs its own
    /// entry rather than the whole file.
    [[nodiscard]] static Result<AntennaLibrary> load(const std::filesystem::path& path,
                                                     std::vector<std::string>* problems = nullptr);

    /// Every antenna found under `directories`, in **decreasing precedence** --
    /// the order `Paths::searchPath` hands back, user directory first, so a
    /// user entry shadows a shipped one carrying the same id.
    ///
    /// Note this is the opposite direction from the channels plugin's loader,
    /// which merges whole files and is handed its directories built-ins-first.
    /// Here the unit is the row, and matching `searchPath`'s own documented
    /// "walk in order, take the first hit" is what keeps the call site from
    /// having to reverse anything.
    [[nodiscard]] static AntennaLibrary discover(std::span<const std::filesystem::path> directories,
                                                 std::vector<std::string>* problems = nullptr);

    /// Entries as another library listed them, shipped ones still marked so:
    /// a copy of one kept elsewhere, as a remote instrument's is.
    [[nodiscard]] static AntennaLibrary of(std::vector<Antenna> entries);

    [[nodiscard]] const Antenna* find(std::string_view id) const;
    [[nodiscard]] std::span<const Antenna> entries() const noexcept { return m_entries; }
    [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }

    /// Adds or replaces by id. An id that names a shipped entry adds an
    /// editable copy over it -- which is what "edit a built-in" means here.
    void add(Antenna antenna);

    /// Removes a user entry. A shipped one cannot be removed; deleting the
    /// copy that shadows it is what restores the original.
    void remove(std::string_view id);

    /// Writes the entries that did not ship with the build.
    [[nodiscard]] Status saveUserFile(const std::filesystem::path& path) const;

    /// Turns a name into an id nothing else in the library is using.
    [[nodiscard]] std::string makeId(std::string_view name) const;

private:
    void sort();

    std::vector<Antenna> m_entries;
};

} // namespace sweeppp
