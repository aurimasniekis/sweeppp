// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// Named channels and beacons, as a plugin.
//
// The second contributor, and the first user of SWEEPPP_CONTRIBUTION_CHANNEL
// and SWEEPPP_CONTRIBUTION_SPOT. A band plan answers "what is this region
// allocated to"; this answers "what is this, exactly" -- Wi-Fi channel 6,
// raceband R4, ADS-B -- which is what an operator is usually pointing at.
//
// It is not a band plan in a different hat, and the difference is scale: there
// are several hundred shipped entries and an operator wants most of them off
// most of the time. A single "show on plot" tick per plugin cannot say "FPV
// analog 5.8 GHz raceband, yes; everything else FPV, no". So this plugin
// carries a tree the operator ticks, and -- because several hundred entries
// will never cover what someone is actually hunting -- an editor for their
// own, seeded from the marker they are looking at.
//
// Two facets: the contributor, which hands the host data and lets it draw;
// and the UI extension, which is the tree and the editor and nothing else. The
// contributor keeps answering when the UI facet cannot register, which is what
// splitting a plugin into facets is for.
//
// It links `sweeppp::sweeppp`, and only for values: Color, toml_util, Result,
// PluginSettings. It must never reach a singleton -- see the rule at the top
// of <sweeppp/plugin/Plugin.hpp>.
#include "ChannelSet.hpp"

#include <sweeppp/core/Toml.hpp>
#include <sweeppp/plugin/Plugin.hpp>
#include <sweeppp/plugin/PluginSettings.hpp>

#if defined(SWEEPPP_PLUGIN_HAS_UI)
#include <imgui.h>
#include <sweeppp/plugin/PluginChrome.hpp>
// For ImGuiItemFlags_MixedValue alone, which is what draws a parent whose
// children disagree as a dash rather than as a lie in either direction.
// `PushItemFlag` itself is public.
#include <imgui_internal.h>
#endif

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace channels {
namespace {

namespace plugin = sweeppp::plugin;
namespace toml_util = sweeppp::toml_util;

constexpr std::string_view kPluginId = "org.sweeppp.channels";

#if defined(SWEEPPP_PLUGIN_HAS_UI)
/// The bar button's glyph, in the host's merged icon font: flag-variant, F0240.
///
/// A flag, because that is already what these are called everywhere they are
/// drawn: the entries are flags on the plot, and were "Channel flags" in the
/// panel this button replaced.
constexpr const char* kIcon = "\xF3\xB0\x89\x80";

/// The button's own ids, qualified because the bar shares one ImGui id stack
/// between the host's controls and every plugin's -- see the note on
/// SWEEPPP_UI_SPOT_TOOLBAR. Only these two need it: everything inside the
/// popover is scoped by the popover's own window.
constexpr const char* kButtonId = "##org.sweeppp.channels.button";
constexpr const char* kPopupId = "##org.sweeppp.channels.tree";
#endif

/// The one file this plugin writes. Everything else it reads is shipped.
constexpr std::string_view kUserSetId = "custom";
constexpr std::string_view kUserSetName = "My channels";
constexpr std::string_view kUserFileName = "custom.toml";

/// A group's persistence key: the set's id and the group's, joined.
///
/// Qualified because a group id is only unique inside its own file -- two
/// files may both call a branch "24" -- and this key outlives both in the
/// settings file.
std::string qualify(std::string_view setId, std::string_view groupId) {
    return std::format("{}/{}", setId, groupId);
}

/// One loaded file, with everything the tree and the queries read per frame
/// worked out once at load rather than per group per frame.
struct LoadedSet {
    ChannelSet set;

    /// `resolveEnabled` and `resolveEntries`, rebuilt whenever the disabled
    /// set changes.
    std::vector<std::uint8_t> on;
    std::vector<std::uint8_t> entryOn;

    /// Children by index, and the roots. The format promises only that a
    /// parent is declared before its children, not that the file is in
    /// pre-order, so the tree is drawn from these rather than from a walk of
    /// the flat vector.
    std::vector<std::vector<std::size_t>> children;
    std::vector<std::size_t> roots;

    /// Per group, counting its whole subtree: how many channels are under it
    /// and what they span. What the dim second line of a tree row says, so a
    /// branch is legible without expanding it.
    std::vector<std::size_t> counts;
    std::vector<double> lowHz;
    std::vector<double> highHz;

    /// Per group, its own channels only -- what expanding that row lists.
    std::vector<std::size_t> own;
};

sweeppp_contribution_type_t abiType(ChannelKind kind) noexcept {
    switch (kind) {
    case ChannelKind::Channel:
        return SWEEPPP_CONTRIBUTION_CHANNEL;
    case ChannelKind::Spot:
        return SWEEPPP_CONTRIBUTION_SPOT;
    case ChannelKind::Band:
        break;
    }
    return SWEEPPP_CONTRIBUTION_BAND;
}

/// Named channels, served and ticked.
class ChannelsPlugin final : public plugin::Plugin {
public:
    [[nodiscard]] bool activate(plugin::Host& host) override;
    void deactivate() override;

    void saveProfile(const plugin::ProfileWriter& writer) override;
    void loadProfile(const plugin::ProfileReader& reader) override;

    // ---- contributor -----------------------------------------------------

    /// Zero, and that is the answer rather than an omission: every set is
    /// loaded at once and the tree is the selector, so a dataset combo would
    /// be a second, worse answer to the same question.
    [[nodiscard]] std::uint32_t datasetCount() const { return 0; }
    [[nodiscard]] std::string_view datasetName(std::uint32_t) const { return {}; }
    [[nodiscard]] std::string_view datasetDescription(std::uint32_t) const { return {}; }
    [[nodiscard]] std::uint32_t activeDataset() const { return 0; }
    [[nodiscard]] bool selectDataset(std::uint32_t) { return false; }

    [[nodiscard]] std::uint32_t contributionsIn(double fromHz, double toHz,
                                                sweeppp_contribution_t* out,
                                                std::uint32_t capacity) const;
    [[nodiscard]] std::uint32_t contributionsAt(double hz, sweeppp_contribution_t* out,
                                                std::uint32_t capacity) const;

    /// The operator ctrl-clicked this entry's flag on the plot.
    ///
    /// The same untick the tree offers, reached from the other end: what makes
    /// it worth having is that six hundred entries are only ever a nuisance
    /// one at a time, and the one being pointed at is exactly the one to
    /// remove. It goes through the same disabled set, so the tree comes back
    /// agreeing with the plot rather than holding a second opinion.
    [[nodiscard]] bool hide(const sweeppp_contribution_t& which);

#if defined(SWEEPPP_PLUGIN_HAS_UI)
    // ---- ui --------------------------------------------------------------

    /// One button on the host's bar: the flags on and off, and the tree
    /// behind the right button.
    void drawToolbar();
    void drawWindow();

    /// The operator pressed shift+C. Recorded rather than acted on -- see the
    /// note on `open_panel` in the ABI.
    void openPanel() { m_panelRequested = true; }
#endif

private:
    /// What the operator has typed into the editor but not yet saved.
    struct UserEntry {
        std::string name;
        std::string group;
        double centerHz = 0.0;
        double widthHz = 0.0;
        ChannelKind kind = ChannelKind::Spot;
    };

    void reload();
    void rebuildMasks();
    void seedDefaults();
    void persistDisabled();

    /// The disabled set as one newline-joined string, for a profile.
    [[nodiscard]] std::string joinDisabled() const;

    /// Ticks or unticks one key -- a set, a group or an entry -- and nothing
    /// else.
    ///
    /// Unticking a group therefore hides its subtree without touching what is
    /// ticked inside it, so ticking it again brings back the selection the
    /// operator had rather than everything under it.
    void setKey(std::string key, bool on);

    /// Clears the whole subtree's own flags, and whatever above it was keeping
    /// the subtree off. `kNoParent` means the whole set.
    ///
    /// The escape hatch for a branch that is on but empty because everything
    /// inside it was individually unticked: there the operator clicking a tick
    /// that is already set has to mean "show me all of this", or the click
    /// does nothing at all.
    void enableSubtree(const LoadedSet& loaded, std::size_t index);

    /// The same, for one channel rather than a branch.
    void enableEntry(const LoadedSet& loaded, std::size_t entryIndex);

    /// Clears whatever above `index` is hiding it, switching that ancestor's
    /// other branches off in its place. Shared, because getting it right once
    /// and wrong once is exactly what happened.
    void liftAncestors(const LoadedSet& loaded, std::size_t index);

    [[nodiscard]] std::filesystem::path userFile() const;
    [[nodiscard]] sweeppp::Status saveUserFile() const;

#if defined(SWEEPPP_PLUGIN_HAS_UI)
    /// Which sets, groups and entries are ticked. Inside the button's own
    /// popover, which is the only place it is drawn.
    void drawTree();
    void drawGroupRow(LoadedSet& loaded, std::size_t index, std::span<const std::uint8_t> allOn,
                      std::span<const std::uint8_t> anyOn);
    void drawEntryRows(LoadedSet& loaded, std::size_t index);
    void drawEditorTable();
    void drawEditorForm();
    void addFromMarker(std::string_view label);
#endif

    static void onMarker(void* user, const sweeppp_marker_event_t& event);

    plugin::Host m_host;
    sweeppp::PluginSettings m_settings;
    std::vector<LoadedSet> m_sets;

    /// Which groups are off, by qualified id. A disabled set rather than an
    /// enabled one, the same idiom as `plugins.disabled`: a group that appears
    /// in a later version of a data file is on rather than hiding until
    /// someone finds the file.
    std::set<std::string, std::less<>> m_disabled;

    /// Where each marker was when it last moved, by label. Filled from
    /// `SWEEPPP_EVENT_MARKER`, which the host publishes on the UI thread once
    /// per resting position -- so the editor can offer "add marker 1" without
    /// reaching into the host's state.
    std::map<std::string, std::pair<double, double>, std::less<>> m_markers;
    std::uint64_t m_markerSubscription = 0;

    /// What the user file holds, as the editor works on it. Not gated on the
    /// UI: a headless build still loads that file and still serves what is in
    /// it, it simply has no window to change it from.
    std::vector<UserEntry> m_userEntries;

#if defined(SWEEPPP_PLUGIN_HAS_UI)
    bool m_editorOpen = false;

    /// A keystroke asking for the popover, waiting for the frame that can act
    /// on it. Cleared by `drawToolbar`, which is the only place an ImGui id
    /// for it exists.
    bool m_panelRequested = false;

    /// Under the button when the button opened it, at the pointer when the
    /// keyboard did.
    plugin::chrome::PopoverAnchor m_popover;

    /// The form's fields. Fixed buffers rather than `std::string`, because
    /// `InputText` over a `std::string` lives in ImGui's misc/cpp helper and
    /// this plugin compiles the library itself -- one more source file for a
    /// four-field form is not the trade.
    std::array<char, 64> m_draftName{};
    std::array<char, 128> m_draftGroup{};
    std::array<char, 32> m_draftCenter{};
    std::array<char, 32> m_draftWidth{};
    std::string m_editorMessage;
#endif
};

// The vtables and the facets that name them, at namespace scope so the
// manifest can declare them without activating anything. That is what lets the
// Plugins panel say what this plugin does while it is switched off.

const sweeppp_contributor_vtable_t& contributorVtable() {
    static const sweeppp_contributor_vtable_t value =
        plugin::makeContributorVtable<ChannelsPlugin>();
    return value;
}

#if defined(SWEEPPP_PLUGIN_HAS_UI)
const sweeppp_ui_vtable_t& uiVtable() {
    // A toolbar button and a window, and nothing else. `makeUiVtable` wires
    // only the slots this class defines, so the three it does not are null
    // rather than four empty overrides.
    //
    // The button carries the tree that used to be a settings section, and the
    // editor stays a window of its own: it is worked in for minutes at a time
    // while watching the plot, which is the one thing a popover cannot do --
    // it closes the moment attention goes anywhere else.
    static const sweeppp_ui_vtable_t value = plugin::makeUiVtable<ChannelsPlugin>(
        SWEEPPP_UI_SPOT_BIT(SWEEPPP_UI_SPOT_TOOLBAR) | SWEEPPP_UI_SPOT_BIT(SWEEPPP_UI_SPOT_WINDOW),
        SWEEPPP_UI_LAYER_UNDER,
        // What the button switches, which is what sends shift+C here rather
        // than to whoever draws the allocations.
        SWEEPPP_CONTRIBUTION_CHANNEL);
    return value;
}

constexpr std::size_t kFacetCount = 2;
#else
constexpr std::size_t kFacetCount = 1;
#endif

std::span<const sweeppp_facet_t> facets() {
    static const std::array<sweeppp_facet_t, kFacetCount> value{
        plugin::facet(SWEEPPP_FACET_CONTRIBUTOR, "channels", "Named channels",
                      "Channels, beacons and single frequencies, by service", &contributorVtable()),
#if defined(SWEEPPP_PLUGIN_HAS_UI)
        plugin::facet(SWEEPPP_FACET_UI_EXTENSION, "editor", "Channels button and editor",
                      "The bar's channel switch, the tree behind it, and the operator's own "
                      "entries",
                      &uiVtable()),
#endif
    };
    return value;
}

// --------------------------------------------------------------- lifetime

bool ChannelsPlugin::activate(plugin::Host& host) {
    m_host = host;

    std::string settingsProblem;
    m_settings = sweeppp::PluginSettings::load(host.settingsPath(), &settingsProblem);
    if (!settingsProblem.empty()) {
        host.warn("{}", settingsProblem);
    }

    // Absent rather than empty: an empty array is "the operator turned
    // everything on", and seeding defaults over that would undo their choice
    // on every start.
    const bool configured = toml_util::at(m_settings.table(), "channels.disabled").is_array();
    for (std::string& id : m_settings.getStringArray("channels.disabled")) {
        m_disabled.insert(std::move(id));
    }

    reload();
    if (!configured) {
        seedDefaults();
        rebuildMasks();
    }

    if (const sweeppp_plugin_status_t status = host.registerFacet(facets()[0], this);
        status != SWEEPPP_PLUGIN_OK) {
        host.warn("the channel data facet could not register");
    }

    m_markerSubscription = host.subscribe<sweeppp_marker_event_t>(&ChannelsPlugin::onMarker, this);

#if defined(SWEEPPP_PLUGIN_HAS_UI)
    // Adopting the host's ImGui is what makes drawing legal, and a refusal
    // here is reported and survivable: the contributor facet still answers the
    // marker and still paints the plot, because the host draws what it hands
    // over.
    if (std::string reason; !host.adoptImGui(reason)) {
        host.warn("not drawing: {}", reason);
        // Reported, not merely skipped: a facet the manifest declares and the
        // plugin then declines would otherwise sit in the listing marked
        // inactive with nothing said about it.
        host.reportFacet(facets()[1], reason);
        return true;
    }

    if (const sweeppp_plugin_status_t status = host.registerFacet(facets()[1], this);
        status != SWEEPPP_PLUGIN_OK) {
        host.warn("the channel tree facet could not register");
    }
#endif

    return true;
}

void ChannelsPlugin::deactivate() {
    if (m_markerSubscription != 0) {
        m_host.unsubscribe(m_markerSubscription);
        m_markerSubscription = 0;
    }

    // Nothing else to stop: no threads, nothing held open. The settings are
    // written when they change rather than here, because a plugin that saves
    // only on the way out loses everything to the one exit that never runs it.
    m_sets.clear();
    m_markers.clear();
    m_userEntries.clear();
}

std::string ChannelsPlugin::joinDisabled() const {
    // One newline-joined string, because a profile carries scalars only. The
    // ids are the same ones the settings file holds, so a profile written on
    // one machine and read on another agrees about what is off even when the
    // two have different data files.
    std::string joined;
    for (const std::string& id : m_disabled) {
        if (!joined.empty()) {
            joined.push_back('\n');
        }
        joined.append(id);
    }
    return joined;
}

void ChannelsPlugin::saveProfile(const plugin::ProfileWriter& writer) {
    writer.set("disabled", std::string_view(joinDisabled()));
}

void ChannelsPlugin::loadProfile(const plugin::ProfileReader& reader) {
    // The profile wins over the settings file while it is loaded, and is not
    // written back to it: a profile is a setup an operator visits, and coming
    // back from it should leave their default where they left it.
    //
    // The fallback is what is already loaded, not the empty string. Every
    // profile saved before this plugin existed -- including the unnamed one
    // the application loads at startup -- carries no key at all, and reading
    // that as "nothing is off" would switch six hundred spans on for anyone
    // who has not yet saved a profile since installing it.
    const std::string current = joinDisabled();
    const std::string joined = reader.getString("disabled", current);
    if (joined == current) {
        return;
    }

    m_disabled.clear();
    std::size_t start = 0;
    while (start <= joined.size()) {
        const std::size_t end = joined.find('\n', start);
        const std::string_view id =
            std::string_view(joined).substr(start, end == std::string::npos ? end : end - start);
        if (!id.empty()) {
            m_disabled.emplace(id);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    rebuildMasks();
}

// ------------------------------------------------------------------ data

void ChannelsPlugin::reload() {
    // Built-ins first, then the operator's own, so a user file carrying the
    // same [channels].id replaces the shipped one outright -- the override
    // rule every other asset in the application follows. Both directories come
    // from the host: a plugin asking `Paths::instance()` would get its own
    // copy, which ignores `--config-dir` and every test's override.
    const std::array<std::filesystem::path, 2> directories{
        m_host.resourcesDir() / "channels",
        m_host.path(SWEEPPP_PATH_CONFIG_DIR) / "channels",
    };

    std::vector<std::string> problems;
    std::vector<ChannelSet> sets = ChannelSet::discover(directories, &problems);
    for (const std::string& problem : problems) {
        m_host.warn("{}", problem);
    }
    if (sets.empty()) {
        m_host.warn("no channel files under {} or {}", directories[0].string(),
                    directories[1].string());
    }

    m_sets.clear();
    m_sets.reserve(sets.size());
    for (ChannelSet& set : sets) {
        LoadedSet loaded;
        loaded.set = std::move(set);

        const std::size_t count = loaded.set.groups().size();
        loaded.children.resize(count);
        loaded.counts.assign(count, 0);
        loaded.own.assign(count, 0);
        loaded.lowHz.assign(count, 0.0);
        loaded.highHz.assign(count, 0.0);

        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t parent = loaded.set.groups()[i].parent;
            if (parent == kNoParent) {
                loaded.roots.push_back(i);
            } else {
                loaded.children[parent].push_back(i);
            }
        }

        for (const ChannelEntry& entry : loaded.set.entries()) {
            ++loaded.own[entry.group];

            std::size_t& n = loaded.counts[entry.group];
            if (n == 0) {
                loaded.lowHz[entry.group] = entry.startHz;
                loaded.highHz[entry.group] = entry.stopHz;
            } else {
                loaded.lowHz[entry.group] = std::min(loaded.lowHz[entry.group], entry.startHz);
                loaded.highHz[entry.group] = std::max(loaded.highHz[entry.group], entry.stopHz);
            }
            ++n;
        }

        // Rolled up into the ancestors. A reverse pass suffices because a
        // parent is always declared before its children.
        for (std::size_t i = count; i-- > 0;) {
            const std::size_t parent = loaded.set.groups()[i].parent;
            if (parent == kNoParent || loaded.counts[i] == 0) {
                continue;
            }
            if (loaded.counts[parent] == 0) {
                loaded.lowHz[parent] = loaded.lowHz[i];
                loaded.highHz[parent] = loaded.highHz[i];
            } else {
                loaded.lowHz[parent] = std::min(loaded.lowHz[parent], loaded.lowHz[i]);
                loaded.highHz[parent] = std::max(loaded.highHz[parent], loaded.highHz[i]);
            }
            loaded.counts[parent] += loaded.counts[i];
        }

        m_sets.push_back(std::move(loaded));
    }

    rebuildMasks();

    // The editor's working copy, read back from whatever the user file now
    // holds, so the table and the tree cannot disagree about what is in it.
    m_userEntries.clear();
    for (const LoadedSet& loaded : m_sets) {
        if (loaded.set.id() != kUserSetId) {
            continue;
        }
        for (const ChannelEntry& entry : loaded.set.entries()) {
            m_userEntries.push_back(UserEntry{
                .name = entry.name,
                .group = loaded.set.groups()[entry.group].id,
                .centerHz = (entry.startHz + entry.stopHz) * 0.5,
                .widthHz = entry.widthHz(),
                .kind = entry.kind,
            });
        }
    }
}

void ChannelsPlugin::rebuildMasks() {
    for (LoadedSet& loaded : m_sets) {
        // The plugin's ids are qualified with the set they came from, because
        // a group id is only unique inside its own file; the model's are not,
        // because it only ever sees one file. Stripping the qualifier here
        // keeps that asymmetry in one function.
        std::set<std::string, std::less<>> local;
        for (const ChannelGroup& group : loaded.set.groups()) {
            if (m_disabled.contains(qualify(loaded.set.id(), group.id))) {
                local.insert(group.id);
            }
        }
        for (const ChannelEntry& entry : loaded.set.entries()) {
            std::string key = ChannelSet::entryKey(loaded.set.groups()[entry.group].id, entry.name);
            if (m_disabled.contains(qualify(loaded.set.id(), key))) {
                local.insert(std::move(key));
            }
        }

        loaded.on = loaded.set.resolveEnabled(local);

        // A whole set switched off, which is the row above every group in the
        // tree. Its key is the bare set id; a group's always carries a '/'.
        if (m_disabled.contains(loaded.set.id())) {
            std::ranges::fill(loaded.on, std::uint8_t{0});
        }

        loaded.entryOn = loaded.set.resolveEntries(loaded.on, local);
    }
}

void ChannelsPlugin::seedDefaults() {
    m_disabled.clear();

    for (const LoadedSet& loaded : m_sets) {
        const std::vector<std::uint8_t> want = loaded.set.resolveDefaults();
        for (std::size_t i = 0; i < want.size(); ++i) {
            const ChannelGroup& group = loaded.set.groups()[i];
            const bool parentOn = group.parent == kNoParent || want[group.parent] != 0;

            // Only where the answer differs from the parent's, so an entirely
            // off family is one id rather than thirty.
            if (parentOn && want[i] == 0) {
                m_disabled.insert(qualify(loaded.set.id(), group.id));
            }
        }
    }
}

void ChannelsPlugin::persistDisabled() {
    const std::vector<std::string> ids(m_disabled.begin(), m_disabled.end());
    m_settings.set("channels.disabled", std::span<const std::string>(ids));
    if (auto saved = m_settings.save(); !saved) {
        m_host.warn("{}", saved.error().describe());
    }
}

void ChannelsPlugin::setKey(std::string key, bool on) {
    if (on) {
        if (const auto found = m_disabled.find(key); found != m_disabled.end()) {
            m_disabled.erase(found);
        }
    } else {
        m_disabled.insert(std::move(key));
    }

    persistDisabled();
    rebuildMasks();
}

void ChannelsPlugin::enableSubtree(const LoadedSet& loaded, std::size_t index) {
    const std::vector<ChannelGroup>& groups = loaded.set.groups();
    const std::string_view setId = loaded.set.id();

    // A parent is always declared before its children, so one forward pass
    // marks the whole subtree.
    std::vector<std::uint8_t> inside(groups.size(), 0);
    if (index == kNoParent) {
        std::ranges::fill(inside, std::uint8_t{1});
        m_disabled.erase(std::string(setId));
    } else {
        inside[index] = 1;
        for (std::size_t i = index + 1; i < groups.size(); ++i) {
            const std::size_t parent = groups[i].parent;
            inside[i] =
                static_cast<std::uint8_t>(parent != kNoParent && inside[parent] != 0 ? 1 : 0);
        }
    }

    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (inside[i] != 0) {
            m_disabled.erase(qualify(setId, groups[i].id));
        }
    }
    for (const ChannelEntry& entry : loaded.set.entries()) {
        if (inside[entry.group] != 0) {
            m_disabled.erase(
                qualify(setId, ChannelSet::entryKey(groups[entry.group].id, entry.name)));
        }
    }

    liftAncestors(loaded, index);

    persistDisabled();
    rebuildMasks();
}

void ChannelsPlugin::liftAncestors(const LoadedSet& loaded, std::size_t index) {
    const std::vector<ChannelGroup>& groups = loaded.set.groups();
    const std::string_view setId = loaded.set.id();

    // Whatever above it was keeping the row off has to go too, and that is
    // only honest if the ancestor's *other* branches stay off: turning on one
    // raceband inside a switched-off FPV tree must not switch on every other
    // FPV band with it.
    std::size_t top = index;
    for (std::size_t child = index, parent = index == kNoParent ? kNoParent : groups[index].parent;
         parent != kNoParent; child = parent, parent = groups[parent].parent) {
        top = parent;
        if (!m_disabled.contains(qualify(setId, groups[parent].id))) {
            continue;
        }
        m_disabled.erase(qualify(setId, groups[parent].id));
        for (const std::size_t sibling : loaded.children[parent]) {
            if (sibling != child) {
                m_disabled.insert(qualify(setId, groups[sibling].id));
            }
        }
    }

    // And the file above those, which is the same rule one level further up.
    // Clearing it without switching its other roots off is how clicking one
    // group inside a switched-off file switched the whole file on.
    if (!m_disabled.contains(setId)) {
        return;
    }
    m_disabled.erase(std::string(setId));
    if (top == kNoParent) {
        return;
    }
    for (const std::size_t root : loaded.roots) {
        if (root != top) {
            m_disabled.insert(qualify(setId, groups[root].id));
        }
    }
}

void ChannelsPlugin::enableEntry(const LoadedSet& loaded, std::size_t entryIndex) {
    const std::vector<ChannelGroup>& groups = loaded.set.groups();
    const std::string_view setId = loaded.set.id();
    const ChannelEntry& entry = loaded.set.entries()[entryIndex];

    m_disabled.erase(qualify(setId, ChannelSet::entryKey(groups[entry.group].id, entry.name)));

    // Its own tick was already on and it still is not showing, so what hides
    // it is above it -- starting with its own group, whose other children stay
    // off in its place.
    if (m_disabled.contains(qualify(setId, groups[entry.group].id))) {
        m_disabled.erase(qualify(setId, groups[entry.group].id));
        for (const ChannelEntry& sibling : loaded.set.entries()) {
            if (sibling.group == entry.group && &sibling != &entry) {
                m_disabled.insert(
                    qualify(setId, ChannelSet::entryKey(groups[sibling.group].id, sibling.name)));
            }
        }
        for (const std::size_t child : loaded.children[entry.group]) {
            m_disabled.insert(qualify(setId, groups[child].id));
        }
    }

    liftAncestors(loaded, entry.group);

    persistDisabled();
    rebuildMasks();
}

void ChannelsPlugin::onMarker(void* user, const sweeppp_marker_event_t& event) {
    // On the publishing thread, which for markers is the host's UI thread --
    // the same thread the editor draws on, so this needs no lock and must not
    // take one.
    auto* self = static_cast<ChannelsPlugin*>(user);
    self->m_markers[std::string(plugin::view(event.label))] = {event.frequency_hz, event.level_dbm};
}

// ------------------------------------------------------------- contributor

bool ChannelsPlugin::hide(const sweeppp_contribution_t& which) {
    const std::string_view category = plugin::view(which.category);
    const std::string_view name = plugin::view(which.name);

    for (const LoadedSet& loaded : m_sets) {
        if (loaded.set.name() != category) {
            continue;
        }
        for (const ChannelEntry& entry : loaded.set.entries()) {
            // The span as well as the name: two files may use the same name
            // for two frequencies, and the operator pointed at one of them.
            if (entry.name != name || entry.startHz != which.start_hz ||
                entry.stopHz != which.stop_hz) {
                continue;
            }
            setKey(qualify(loaded.set.id(),
                           ChannelSet::entryKey(loaded.set.groups()[entry.group].id, entry.name)),
                   false);
            return true;
        }
    }
    return false;
}

std::uint32_t ChannelsPlugin::contributionsIn(double fromHz, double toHz,
                                              sweeppp_contribution_t* out,
                                              std::uint32_t capacity) const {
    std::uint32_t total = 0;
    for (const LoadedSet& loaded : m_sets) {
        for (const ChannelEntry* entry : loaded.set.entriesIn(fromHz, toHz, loaded.entryOn)) {
            // Written while there is room, counted always: the full count is
            // what lets the host size a buffer with one call and fill it with
            // a second.
            if (total < capacity) {
                const Color& color = loaded.set.groups()[entry->group].color;
                const float rgba[4]{color.r, color.g, color.b, color.a};
                // The set's NAME, not its id: the category crosses the ABI
                // into the marker's hover line, where "Beacons" is what an
                // operator reads and "beacons" is what a file is keyed on.
                out[total] =
                    plugin::contribution(abiType(entry->kind), entry->name, loaded.set.name(),
                                         entry->startHz, entry->stopHz, rgba, entry->description);
            }
            ++total;
        }
    }
    return total;
}

std::uint32_t ChannelsPlugin::contributionsAt(double hz, sweeppp_contribution_t* out,
                                              std::uint32_t capacity) const {
    // Narrowest first across every set, not per set: a 20 MHz Wi-Fi channel is
    // the more specific answer than a 125 MHz FPV block whichever file each
    // came from, and the host reads the first entry as the title.
    struct Hit {
        const ChannelEntry* entry;
        const LoadedSet* owner;
    };

    std::vector<Hit> found;
    for (const LoadedSet& loaded : m_sets) {
        for (const ChannelEntry* entry : loaded.set.entriesAt(hz, loaded.entryOn)) {
            found.push_back(Hit{.entry = entry, .owner = &loaded});
        }
    }

    std::ranges::stable_sort(
        found, [](const Hit& a, const Hit& b) { return a.entry->widthHz() < b.entry->widthHz(); });

    const auto taken = std::min(capacity, static_cast<std::uint32_t>(found.size()));
    for (std::uint32_t i = 0; i < taken; ++i) {
        const ChannelEntry& entry = *found[i].entry;
        const Color& color = found[i].owner->set.groups()[entry.group].color;
        const float rgba[4]{color.r, color.g, color.b, color.a};
        out[i] = plugin::contribution(abiType(entry.kind), entry.name, found[i].owner->set.name(),
                                      entry.startHz, entry.stopHz, rgba, entry.description);
    }
    return static_cast<std::uint32_t>(found.size());
}

// -------------------------------------------------------------- user file

std::filesystem::path ChannelsPlugin::userFile() const {
    return m_host.path(SWEEPPP_PATH_CONFIG_DIR) / "channels" / std::string(kUserFileName);
}

sweeppp::Status ChannelsPlugin::saveUserFile() const {
    ::toml::table root;

    ::toml::table header;
    header.insert_or_assign("id", std::string(kUserSetId));
    header.insert_or_assign("name", std::string(kUserSetName));
    root.insert_or_assign("channels", std::move(header));

    // Every group path an entry names, and every ancestor of one, so the
    // parent rule holds when this is read back. Sorted, which puts an ancestor
    // before its descendants: a path is a strict prefix of its children's and
    // '/' sorts below every character an id may carry.
    std::set<std::string> paths;
    for (const UserEntry& entry : m_userEntries) {
        std::string_view path = entry.group;
        while (!path.empty()) {
            paths.emplace(path);
            const std::size_t slash = path.rfind('/');
            path = slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
        }
    }

    ::toml::array groups;
    for (const std::string& path : paths) {
        ::toml::table group;
        group.insert_or_assign("id", path);

        const std::size_t slash = path.rfind('/');
        group.insert_or_assign("name", slash == std::string::npos ? path : path.substr(slash + 1));

        ::toml::array rows;
        for (const UserEntry& entry : m_userEntries) {
            if (entry.group != path) {
                continue;
            }
            ::toml::table row;
            row.insert_or_assign("name", entry.name);
            // Hz, as a number: this file is written by the editor and read by
            // the loader, and a formatted "2.4 GHz" would round the operator's
            // frequency every time they opened the window.
            row.insert_or_assign("center", entry.centerHz);
            row.insert_or_assign("width", entry.widthHz);
            row.insert_or_assign("type",
                                 std::string(entry.kind == ChannelKind::Spot   ? "spot"
                                             : entry.kind == ChannelKind::Band ? "band"
                                                                               : "channel"));
            rows.push_back(std::move(row));
        }
        if (!rows.empty()) {
            group.insert_or_assign("channel", std::move(rows));
        }
        groups.push_back(std::move(group));
    }
    root.insert_or_assign("group", std::move(groups));

    const std::filesystem::path path = userFile();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    return toml_util::save(path, root, "Sweep++ channels -- written by the channel editor");
}

// -------------------------------------------------------------------- ui

#if defined(SWEEPPP_PLUGIN_HAS_UI)

namespace {

std::string spanText(double lowHz, double highHz) {
    if (lowHz == highHz) {
        return toml_util::formatFrequencyShort(lowHz);
    }
    return std::format("{} – {}", toml_util::formatFrequencyShort(lowHz),
                       toml_util::formatFrequencyShort(highHz));
}

const char* kindName(ChannelKind kind) {
    switch (kind) {
    case ChannelKind::Band:
        return "band";
    case ChannelKind::Spot:
        return "spot";
    case ChannelKind::Channel:
        break;
    }
    return "channel";
}

/// Truncating rather than overflowing: every one of these buffers is bigger
/// than what fits in the field it backs, so a truncation here is a name nobody
/// could have typed.
template <std::size_t N>
void copyInto(std::array<char, N>& buffer, std::string_view text) {
    const std::size_t taken = std::min(text.size(), N - 1);
    std::ranges::copy(text.substr(0, taken), buffer.begin());
    buffer[taken] = '\0';
}

using plugin::drawTick;
using plugin::TickAction;

/// A small filled square in the group's own colour, so a row in the tree and a
/// flag on the plot read as the same thing without either naming the other.
void drawSwatch(const Color& color) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float size = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(at.x, at.y + (size * 0.2F)), ImVec2(at.x + (size * 0.5F), at.y + (size * 0.8F)),
        ImGui::GetColorU32(ImVec4(color.r, color.g, color.b, 1.0F)), 2.0F);
    ImGui::Dummy(ImVec2((size * 0.5F) + 4.0F, size));
    ImGui::SameLine();
}

} // namespace

void ChannelsPlugin::drawGroupRow(LoadedSet& loaded, std::size_t index,
                                  std::span<const std::uint8_t> allOn,
                                  std::span<const std::uint8_t> anyOn) {
    const ChannelGroup& group = loaded.set.groups()[index];
    const std::string key = qualify(loaded.set.id(), group.id);

    ImGui::PushID(static_cast<int>(index));

    if (const auto action =
            drawTick(anyOn[index] != 0, allOn[index] != 0, !m_disabled.contains(key))) {
        switch (*action) {
        case TickAction::Hide:
            setKey(key, false);
            break;
        case TickAction::Unhide:
            setKey(key, true);
            break;
        case TickAction::ShowAll:
            enableSubtree(loaded, index);
            break;
        }
    }
    ImGui::SameLine();
    drawSwatch(group.color);

    // A group's own channels hang off it as rows of their own, so the finest
    // thing the operator can reach is one channel rather than the eight it
    // came with. They are behind the disclosure arrow because three hundred
    // tick rows is a list nobody scrolls -- the group is the usual unit and
    // the individual channel is the exception.
    const bool hasChildren = !loaded.children[index].empty() || loaded.own[index] > 0;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!hasChildren) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }

    const bool open = ImGui::TreeNodeEx("##node", flags, "%s", group.name.c_str());
    const bool nodeHovered = ImGui::IsItemHovered();

    if (loaded.counts[index] > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%zu · %s", loaded.counts[index],
                            spanText(loaded.lowHz[index], loaded.highHz[index]).c_str());
    }
    if (!group.description.empty() && nodeHovered) {
        ImGui::SetTooltip("%s", group.description.c_str());
    }

    if (open && hasChildren) {
        for (const std::size_t child : loaded.children[index]) {
            drawGroupRow(loaded, child, allOn, anyOn);
        }
        drawEntryRows(loaded, index);
        ImGui::TreePop();
    }

    ImGui::PopID();
}

void ChannelsPlugin::drawEntryRows(LoadedSet& loaded, std::size_t index) {
    const ChannelGroup& group = loaded.set.groups()[index];

    for (std::size_t i = 0; i < loaded.set.entries().size(); ++i) {
        const ChannelEntry& entry = loaded.set.entries()[i];
        if (entry.group != index) {
            continue;
        }

        ImGui::PushID(static_cast<int>(i) + 100000);

        const std::string key =
            qualify(loaded.set.id(), ChannelSet::entryKey(group.id, entry.name));
        const bool own = !m_disabled.contains(key);

        // No third state on a leaf: there is nothing under it to disagree.
        if (const auto action = drawTick(loaded.entryOn[i] != 0, loaded.entryOn[i] != 0, own)) {
            switch (*action) {
            case TickAction::Hide:
                setKey(key, false);
                break;
            case TickAction::Unhide:
                setKey(key, true);
                break;
            case TickAction::ShowAll:
                enableEntry(loaded, i);
                break;
            }
        }
        ImGui::SameLine();

        ImGui::TextUnformatted(entry.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", entry.stopHz > entry.startHz
                                      ? spanText(entry.startHz, entry.stopHz).c_str()
                                      : toml_util::formatFrequencyShort(entry.startHz).c_str());

        ImGui::PopID();
    }
}

void ChannelsPlugin::drawToolbar() {
    namespace chrome = plugin::chrome;

    // The channel switch, which the host also paints spots with: a beacon is a
    // channel of no width, and switching it separately would be a third state
    // for one row of flags.
    const bool shown = m_host.contributionsShown(SWEEPPP_CONTRIBUTION_CHANNEL);

    // Shift+C, taken before anything is submitted: the popover has to be
    // opened ahead of the Begin that draws it.
    const bool shutPanel = m_popover.takeRequest(kPopupId, m_panelRequested);

    if (chrome::toolbarToggle(m_host.icon(kIcon, "Channels").append(kButtonId).c_str(), shown)) {
        m_host.setContributionsShown(SWEEPPP_CONTRIBUTION_CHANNEL, !shown);
    }
    if (ImGui::IsItemHovered()) {
        // The state in words as well as in the button's own colour, because a
        // tooltip is what an operator reaches for when the colour did not
        // settle it.
        ImGui::SetTooltip("Named channels and beacons on the spectrum: %s. The C key does the "
                          "same.\n\nClick to %s them, right-click for the tree and the editor "
                          "-- or shift+C.",
                          shown ? "shown" : "hidden", shown ? "hide" : "show");
    }

    // After the tooltip, never before it: BeginTooltip is a Begin, and it
    // would consume the position and constraints meant for the popover.
    m_popover.openOnItemClick(kPopupId);
    m_popover.place();
    {
        const chrome::PanelMetrics metrics;
        if (ImGui::BeginPopup(kPopupId)) {
            if (shutPanel) {
                ImGui::CloseCurrentPopup();
            }
            drawTree();
            ImGui::EndPopup();
        }
    }

    // The bar lays its controls out in a row and the host has no way to know
    // this button was submitted, so the plugin leaves the cursor where the
    // next one goes.
    ImGui::SameLine();
}

void ChannelsPlugin::drawTree() {
    if (m_sets.empty()) {
        ImGui::TextDisabled("No channel files found.");
        return;
    }

    std::size_t on = 0;
    std::size_t total = 0;
    for (const LoadedSet& loaded : m_sets) {
        total += loaded.entryOn.size();
        on += static_cast<std::size_t>(std::ranges::count(loaded.entryOn, std::uint8_t{1}));
    }

    ImGui::TextDisabled("%zu of %zu channels on", on, total);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset")) {
        seedDefaults();
        persistDisabled();
        rebuildMasks();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Back to what each file ships with");
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Edit channels…")) {
        m_editorOpen = true;

        // The popover goes, or it sits over the window it just opened until
        // the operator clicks somewhere to dismiss it.
        ImGui::CloseCurrentPopup();
    }

    // Unticking here is not a drawing setting, and the operator should not
    // have to discover that by watching the chip.
    ImGui::TextDisabled("Unticked rows leave the plot and the marker's chip.");

    // Bounded, because this body draws inside the menu popup: several hundred
    // channels in ninety groups would otherwise make the popup taller than the
    // screen and put its bottom out of reach.
    const float height = ImGui::GetTextLineHeightWithSpacing() * 14.0F;
    if (ImGui::BeginChild("##tree", ImVec2(0.0F, height), ImGuiChildFlags_Borders)) {
        for (LoadedSet& loaded : m_sets) {
            // Per set, so two files that both call a branch "analog" cannot
            // share an ImGui id and fight over which was clicked.
            ImGui::PushID(loaded.set.id().c_str());

            // Tri-state over the whole tree, entries included: a group whose
            // channels have been unticked one at a time has to read as "some",
            // or the tick is describing the group's own flag rather than what
            // is actually on screen.
            const std::size_t count = loaded.set.groups().size();
            std::vector<std::uint8_t> anyOn(count, 0);
            std::vector<std::uint8_t> allOn(count, 1);
            std::vector<std::uint8_t> hasEntries(count, 0);

            for (std::size_t i = 0; i < loaded.set.entries().size(); ++i) {
                const std::size_t group = loaded.set.entries()[i].group;
                hasEntries[group] = 1;
                if (loaded.entryOn[i] != 0) {
                    anyOn[group] = 1;
                } else {
                    allOn[group] = 0;
                }
            }
            for (std::size_t i = 0; i < count; ++i) {
                // A group with nothing under it at all answers for itself.
                if (hasEntries[i] == 0 && loaded.children[i].empty()) {
                    anyOn[i] = loaded.on[i];
                    allOn[i] = loaded.on[i];
                }
            }
            for (std::size_t i = count; i-- > 0;) {
                const std::size_t parent = loaded.set.groups()[i].parent;
                if (parent == kNoParent) {
                    continue;
                }
                anyOn[parent] = static_cast<std::uint8_t>(anyOn[parent] != 0 || anyOn[i] != 0);
                allOn[parent] = static_cast<std::uint8_t>(allOn[parent] != 0 && allOn[i] != 0);
            }

            // The file itself is a row with a tick of its own, so switching a
            // whole family off is one click rather than one per root -- and,
            // because it only sets the file's own flag, switching it back on
            // restores the selection inside rather than everything.
            bool setAny = false;
            bool setAll = !loaded.roots.empty();
            for (const std::size_t root : loaded.roots) {
                setAny = setAny || anyOn[root] != 0;
                setAll = setAll && allOn[root] != 0;
            }

            if (const auto action =
                    drawTick(setAny, setAll, !m_disabled.contains(loaded.set.id()))) {
                switch (*action) {
                case TickAction::Hide:
                    setKey(loaded.set.id(), false);
                    break;
                case TickAction::Unhide:
                    setKey(loaded.set.id(), true);
                    break;
                case TickAction::ShowAll:
                    enableSubtree(loaded, kNoParent);
                    break;
                }
            }
            ImGui::SameLine();

            if (ImGui::TreeNodeEx(
                    "##set", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth,
                    "%s", loaded.set.name().c_str())) {
                for (const std::size_t root : loaded.roots) {
                    drawGroupRow(loaded, root, allOn, anyOn);
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void ChannelsPlugin::addFromMarker(std::string_view label) {
    const auto found = m_markers.find(label);
    if (found == m_markers.end()) {
        return;
    }

    // A spot at the marker, named for where it is. A width typed in afterwards
    // turns it into a channel, which is the usual second step rather than a
    // separate mode.
    const std::string center = toml_util::formatFrequencyShort(found->second.first);
    copyInto(m_draftCenter, center);
    m_draftWidth[0] = '\0';
    if (m_draftName[0] == '\0') {
        copyInto(m_draftName, std::format("{} ({})", center, found->first));
    }
    m_editorMessage.clear();
}

void ChannelsPlugin::drawEditorTable() {
    if (m_userEntries.empty()) {
        ImGui::TextDisabled("Nothing here yet.");
        return;
    }

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;

    const float height = ImGui::GetTextLineHeightWithSpacing() * 10.0F;
    if (!ImGui::BeginTable("##entries", 6, kFlags, ImVec2(0.0F, height))) {
        return;
    }

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Group");
    ImGui::TableSetupColumn("Type");
    ImGui::TableSetupColumn("Centre");
    ImGui::TableSetupColumn("Width");
    ImGui::TableSetupColumn("##delete", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();

    std::size_t remove = m_userEntries.size();
    for (std::size_t i = 0; i < m_userEntries.size(); ++i) {
        const UserEntry& entry = m_userEntries[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(entry.name.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(entry.group.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(kindName(entry.kind));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(toml_util::formatFrequencyShort(entry.centerHz).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(
            entry.widthHz > 0.0 ? toml_util::formatFrequencyShort(entry.widthHz).c_str() : "—");
        ImGui::TableNextColumn();
        if (ImGui::SmallButton("Remove")) {
            remove = i;
        }

        ImGui::PopID();
    }
    ImGui::EndTable();

    if (remove < m_userEntries.size()) {
        m_userEntries.erase(m_userEntries.begin() + static_cast<std::ptrdiff_t>(remove));
    }
}

void ChannelsPlugin::drawEditorForm() {
    ImGui::SeparatorText("Add");

    // One button per marker this plugin has actually been told about, rather
    // than a fixed pair: the host's markers are a list the operator builds, and
    // it is only the events that say which of them exist here.
    if (m_markers.empty()) {
        ImGui::TextDisabled("Place a marker on the spectrum to add one from it.");
    } else {
        bool first = true;
        for (const auto& [label, position] : m_markers) {
            if (!first) {
                ImGui::SameLine();
            }
            first = false;

            if (ImGui::Button(std::format("Add {}", label).c_str())) {
                addFromMarker(label);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", toml_util::formatFrequencyShort(position.first).c_str());
            }
        }
    }

    ImGui::InputTextWithHint("Name", "what to call it", m_draftName.data(), m_draftName.size());

    // Free text, with a combo of the paths already in the file beside it: a
    // dropdown alone could not name a branch that does not exist yet, and free
    // text alone would make a typo a second branch.
    ImGui::InputTextWithHint("Group", "custom/mine", m_draftGroup.data(), m_draftGroup.size());
    if (!m_userEntries.empty()) {
        ImGui::SameLine();
        if (ImGui::BeginCombo("##groups", "…", ImGuiComboFlags_NoPreview)) {
            std::set<std::string> paths;
            for (const UserEntry& entry : m_userEntries) {
                paths.insert(entry.group);
            }
            for (const std::string& path : paths) {
                if (ImGui::Selectable(path.c_str())) {
                    copyInto(m_draftGroup, path);
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0F);
    ImGui::InputTextWithHint("Centre", "5.8G", m_draftCenter.data(), m_draftCenter.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0F);
    ImGui::InputTextWithHint("Width", "blank for a spot", m_draftWidth.data(), m_draftWidth.size());

    if (!ImGui::Button("Add")) {
        return;
    }

    // The same parser every config file goes through, so "5.8G" and
    // "5800 MHz" both work here exactly as they do in the file.
    const auto center = toml_util::parseFrequency(m_draftCenter.data());
    if (!center) {
        m_editorMessage = std::format("'{}' is not a frequency", m_draftCenter.data());
        return;
    }

    double width = 0.0;
    if (m_draftWidth[0] != '\0') {
        const auto parsed = toml_util::parseFrequency(m_draftWidth.data());
        if (!parsed) {
            m_editorMessage = std::format("'{}' is not a width", m_draftWidth.data());
            return;
        }
        width = *parsed;
    }

    m_userEntries.push_back(UserEntry{
        .name = m_draftName[0] != '\0' ? std::string(m_draftName.data())
                                       : toml_util::formatFrequencyShort(*center),
        .group =
            m_draftGroup[0] != '\0' ? std::string(m_draftGroup.data()) : std::string(kUserSetId),
        .centerHz = *center,
        .widthHz = width,
        .kind = width > 0.0 ? ChannelKind::Channel : ChannelKind::Spot,
    });

    m_draftName[0] = '\0';
    m_draftCenter[0] = '\0';
    m_draftWidth[0] = '\0';
    m_editorMessage.clear();
}

void ChannelsPlugin::drawWindow() {
    if (!m_editorOpen) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(560.0F, 460.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Channel editor", &m_editorOpen)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("%s", userFile().string().c_str());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The one file this window writes. Shipped files are read-only.");
    }

    drawEditorTable();
    drawEditorForm();

    ImGui::Separator();
    if (ImGui::Button("Save")) {
        if (auto saved = saveUserFile(); !saved) {
            m_editorMessage = saved.error().describe();
        } else {
            // Reloaded rather than patched in place, so the tree, the plot and
            // the chip agree in this frame rather than after a restart.
            reload();
            m_editorMessage = "Saved";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        m_editorOpen = false;
    }

    if (!m_editorMessage.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_editorMessage.c_str());
    }

    ImGui::End();
}

#endif

// -------------------------------------------------------------- manifest

const sweeppp_plugin_desc_t& describe() {
    static const std::array kAuthors{plugin::author("Sweep++")};

    static const std::array kLinks{
        plugin::link(SWEEPPP_LINK_REPOSITORY, "https://github.com/aurimasniekis/sweeppp"),
        plugin::link(SWEEPPP_LINK_DOCUMENTATION,
                     "https://github.com/aurimasniekis/sweeppp/blob/main/docs/plugins.md"),
    };

    static const sweeppp_plugin_desc_t desc{
        .struct_size = sizeof(sweeppp_plugin_desc_t),
        .manifest =
            sweeppp_manifest_t{
                .struct_size = sizeof(sweeppp_manifest_t),
                .id = plugin::str(kPluginId),
                .version = plugin::str("1.0.0"),
                .name = plugin::str("Channels"),
                .description = plugin::str(
                    "Names the specific channel or beacon at a frequency -- Wi-Fi, FPV, "
                    "LoRa, cellular -- from a tree of groups the operator ticks."),
                .authors = kAuthors.data(),
                .author_count = static_cast<std::uint32_t>(kAuthors.size()),
                .links = kLinks.data(),
                .link_count = static_cast<std::uint32_t>(kLinks.size()),
                .dependencies = nullptr,
                .dependency_count = 0,
                .min_host_version = plugin::str("0.1.0"),
                // Everything it registers can be withdrawn live: the facets
                // hand out copies and hold nothing.
                .requires_restart_to_disable = 0,
            },
        .facets = facets().data(),
        .facet_count = static_cast<std::uint32_t>(facets().size()),
        // Filled in by SWEEPPP_PLUGIN_MAIN.
        .activate = nullptr,
        .deactivate = nullptr,
    };
    return desc;
}

} // namespace
} // namespace channels

SWEEPPP_PLUGIN_MAIN(channels::ChannelsPlugin, channels::describe)
