// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <sweeppp/core/Result.hpp>
#include <sweeppp/ui/ColorMap.hpp>
#include <vector>

namespace channels {

using sweeppp::Result;
using sweeppp::ui::Color;

/// What kind of claim one entry makes, and so how the host draws it.
///
/// A channel list is mostly channels, but the file is also the natural home
/// for the frequencies that have no width at all -- ADS-B on 1090, GPS L1 --
/// and for the occasional wide block a group of channels sits inside.
enum class ChannelKind : std::uint8_t { Band, Channel, Spot };

/// No parent: this group is a root of its file's tree.
inline constexpr std::size_t kNoParent = static_cast<std::size_t>(-1);

/// One branch of the tree the operator ticks.
struct ChannelGroup {
    std::string id;   ///< "analog/58/r", unique within the set
    std::string name; ///< what the tree row says
    std::string description;
    std::size_t parent = kNoParent;
    Color color;
    bool defaultOn = false; ///< ticked on a fresh install
};

/// One named channel, beacon or block.
struct ChannelEntry {
    std::string name;
    std::string description;
    double startHz = 0.0;
    double stopHz = 0.0; ///< == startHz for a spot
    ChannelKind kind = ChannelKind::Channel;
    std::size_t group = 0; ///< index into `groups()`

    [[nodiscard]] double widthHz() const noexcept { return stopHz - startHz; }

    /// Half-open, so abutting channels do not both claim their shared edge.
    /// A spot has no width and is closed instead, or it would contain nothing.
    [[nodiscard]] bool contains(double hz) const noexcept {
        return stopHz > startHz ? (hz >= startHz && hz < stopHz) : hz == startHz;
    }
};

/// One channel file: a tree of groups and the entries hanging off it.
///
/// A value type over `toml_util` -- no ImGui, no singletons -- so the tests
/// compile it directly rather than going through the module. It takes its
/// directories as arguments for the same reason `BandPlan` does: a plugin
/// links libsweeppp for values and never touches a singleton, and
/// `Paths::instance()` inside a plugin is a second copy that would ignore
/// `--config-dir` and every test's override.
///
/// Groups are flattened into one vector with parents by index, which is what
/// makes "is this group on" a walk up `parent` rather than a search. Entries
/// are sorted by start frequency, so a range query is a scan.
class ChannelSet {
public:
    /// A file that will not parse at all fails; a group or channel inside one
    /// that will not is appended to `problems` and skipped, so one bad row
    /// costs its own row rather than the whole file.
    [[nodiscard]] static Result<ChannelSet> load(const std::filesystem::path& path,
                                                 std::vector<std::string>* problems = nullptr);

    /// Every set found under `directories`, in order, a later directory's file
    /// replacing an earlier one carrying the same `[channels].id`.
    ///
    /// Never fails: a file that will not parse is appended to `problems` and
    /// skipped, because one broken file in a directory should not cost the
    /// operator every other set in it.
    [[nodiscard]] static std::vector<ChannelSet>
    discover(std::span<const std::filesystem::path> directories,
             std::vector<std::string>* problems = nullptr);

    [[nodiscard]] const std::string& id() const noexcept { return m_id; }
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] const std::string& description() const noexcept { return m_description; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

    [[nodiscard]] const std::vector<ChannelGroup>& groups() const noexcept { return m_groups; }
    [[nodiscard]] const std::vector<ChannelEntry>& entries() const noexcept { return m_entries; }

    /// Index of the group with this id, or `kNoParent` when there is none.
    [[nodiscard]] std::size_t findGroup(std::string_view id) const;

    /// One byte per group: 1 when neither it nor any ancestor is in
    /// `disabled`.
    ///
    /// A disabled-id set rather than an enabled one, the same idiom as
    /// `plugins.disabled`: a group that appears in a later version of a data
    /// file is on rather than hidden until someone finds the file. The whole
    /// chain is walked because unticking a parent has to take its subtree with
    /// it -- off the plot and out of the marker's chip alike, which is one
    /// mask feeding both queries rather than two rules that can drift.
    [[nodiscard]] std::vector<std::uint8_t>
    resolveEnabled(const std::set<std::string, std::less<>>& disabled) const;

    /// One byte per group: 1 when it or any ancestor declares `default`.
    /// What a fresh install starts from, and what *Reset* returns to.
    [[nodiscard]] std::vector<std::uint8_t> resolveDefaults() const;

    /// One byte per *entry*: 1 when its group chain is on and its own key is
    /// not in `disabled`.
    ///
    /// The one mask both queries take, so "unticked leaves the plot and the
    /// marker's chip" is true by construction rather than by two pieces of
    /// code agreeing.
    [[nodiscard]] std::vector<std::uint8_t>
    resolveEntries(std::span<const std::uint8_t> groupOn,
                   const std::set<std::string, std::less<>>& disabled) const;

    /// The persistence key for one entry: its group's id, a '#', its name.
    ///
    /// '#' rather than '/', so an entry key can never be mistaken for the
    /// group id of a deeper branch.
    [[nodiscard]] static std::string entryKey(std::string_view groupId, std::string_view name);

    /// Entries overlapping a range, in start order. For drawing.
    ///
    /// `entryOn` is `resolveEntries`'s answer, or empty for "everything".
    [[nodiscard]] std::vector<const ChannelEntry*>
    entriesIn(double fromHz, double toHz, std::span<const std::uint8_t> entryOn = {}) const;

    /// Every entry containing a frequency, narrowest first.
    ///
    /// All of them rather than only the narrowest, because entries nest: a
    /// Wi-Fi channel sits inside the ISM allocation and a raceband channel
    /// overlaps its neighbours. Narrowest leads because it is the more
    /// specific answer and the host reads the first entry as the title.
    [[nodiscard]] std::vector<const ChannelEntry*>
    entriesAt(double hz, std::span<const std::uint8_t> entryOn = {}) const;

private:
    std::string m_id;
    std::string m_name;
    std::string m_description;
    std::filesystem::path m_path;
    std::vector<ChannelGroup> m_groups;
    std::vector<ChannelEntry> m_entries;
};

} // namespace channels
