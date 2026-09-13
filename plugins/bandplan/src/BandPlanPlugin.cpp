// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The band plan, as a plugin.
//
// It used to be four places in the application: a loader in libsweeppp, the
// drawing in MainWindowPlots, a selector in the settings panel and a chip in
// the status bar. All of it is here now -- and then most of it went away
// again, because a contributor facet hands the host data and the host draws
// it. What is left is a TOML loader, a tree of ticks, and seven functions.
//
// Every plan is loaded at once and the tree decides which of them answer, the
// same arrangement the channels plugin uses. A selector that admitted only one
// would be the wrong control here for the same reason it is there: "the ITU
// allocations and my own corrections" is a normal thing to want on screen
// together, and a combo can only say "one of these".
//
// It links `sweeppp::sweeppp`, and only for values: Color, toml_util, Result,
// PluginSettings. It must never reach a singleton -- see the rule at the top
// of <sweeppp/plugin/Plugin.hpp>. Without that allowance the port would have
// been a rewrite of the TOML loader rather than a relocation of it.
#include "BandPlan.hpp"

#include <sweeppp/core/Toml.hpp>
#include <sweeppp/plugin/Plugin.hpp>
#include <sweeppp/plugin/PluginSettings.hpp>

#if defined(SWEEPPP_PLUGIN_HAS_UI)
#include <imgui.h>
#include <sweeppp/plugin/PluginChrome.hpp>
#endif

#include <algorithm>
#include <array>
#include <format>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bandplan {
namespace {

namespace plugin = sweeppp::plugin;
namespace toml_util = sweeppp::toml_util;

constexpr std::string_view kPluginId = "org.sweeppp.bandplan";

#if defined(SWEEPPP_PLUGIN_HAS_UI)
/// The bar button's glyph, in the host's merged icon font: chart-gantt, F066C.
///
/// The plugin's own choice, not the host's -- the host has no idea what this
/// button means. Spans laid along an axis, which is what the allocations are
/// on the plot, and deliberately not another of the chart glyphs already on
/// the bar.
constexpr const char* kIcon = "\xF3\xB0\x99\xAC";

/// The button's own ids, qualified because the bar shares one ImGui id stack
/// between the host's controls and every plugin's -- see the note on
/// SWEEPPP_UI_SPOT_TOOLBAR. Only these two need it: everything inside the
/// popover is scoped by the popover's own window.
constexpr const char* kButtonId = "##org.sweeppp.bandplan.button";
constexpr const char* kPopupId = "##org.sweeppp.bandplan.services";
#endif

/// A key is qualified with the plan it belongs to: two plans use the same
/// category names for different sets of allocations, and an operator hiding
/// "Fixed" in one has said nothing about the other.
std::string qualify(std::string_view planName, std::string_view key) {
    return std::format("{}/{}", planName, key);
}

/// One loaded plan, with the masks its queries read per frame worked out once
/// when the ticks change rather than per band per frame.
struct LoadedPlan {
    BandPlan plan;
    std::vector<std::uint8_t> groupOn;
    std::vector<std::uint8_t> bandOn;
};

/// The band plan, served and ticked.
class BandPlanPlugin final : public plugin::Plugin {
public:
    [[nodiscard]] bool activate(plugin::Host& host) override;
    void deactivate() override;

    void saveProfile(const plugin::ProfileWriter& writer) override;
    void loadProfile(const plugin::ProfileReader& reader) override;

    // ---- contributor -----------------------------------------------------

    /// Zero, and that is the answer rather than an omission: every plan is
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

    /// The operator ctrl-clicked this allocation's label on the plot. The same
    /// untick the tree offers, reached from the other end.
    [[nodiscard]] bool hide(const sweeppp_contribution_t& which);

#if defined(SWEEPPP_PLUGIN_HAS_UI)
    /// One button on the host's bar: the allocations on and off, and the
    /// service tree behind the right button.
    void drawToolbar();

    /// The operator pressed shift+B. Recorded rather than acted on -- see the
    /// note on `open_panel` in the ABI.
    void openPanel() { m_panelRequested = true; }
#endif

private:
#if defined(SWEEPPP_PLUGIN_HAS_UI)
    /// Which plans, services and allocations are ticked. Inside the button's
    /// own popover, which is the only place it is drawn.
    void drawTree();
#endif

    void rebuildMasks();
    void seedDefaults();
    void persistDisabled();
    [[nodiscard]] std::string joinDisabled() const;

    /// Ticks or unticks one key -- a plan, a service category or an allocation
    /// -- and nothing else, so hiding a category leaves what is ticked inside
    /// it alone and ticking it again brings that selection back.
    void setKey(std::string key, bool on);

    /// Makes one row actually visible: clears its own flag and everything
    /// under it, and clears whatever above it was hiding it -- switching that
    /// ancestor's *other* branches off in its place, so un-hiding one service
    /// does not un-hide the whole plan with it.
    ///
    /// `kAll` for `bandIndex` means the whole category; `kAll` for both means
    /// the whole plan, which is what a plan row's "show me all of this" is.
    void enablePath(const LoadedPlan& loaded, std::size_t groupIndex, std::size_t bandIndex);

    static constexpr std::size_t kAll = static_cast<std::size_t>(-1);

    plugin::Host m_host;
    sweeppp::PluginSettings m_settings;
    std::vector<LoadedPlan> m_plans;

    /// Which plans, categories and allocations are off, by qualified key.
    ///
    /// A disabled-id set rather than an enabled one, the same idiom as
    /// `plugins.disabled`: a category that appears in a later version of a
    /// plan file is on rather than hidden until someone finds the file.
    std::set<std::string, std::less<>> m_disabled;

#if defined(SWEEPPP_PLUGIN_HAS_UI)
    /// A keystroke asking for the popover, waiting for the frame that can act
    /// on it. Cleared by `drawToolbar`, which is the only place an ImGui id
    /// for it exists.
    bool m_panelRequested = false;

    /// Under the button when the button opened it, at the pointer when the
    /// keyboard did.
    plugin::chrome::PopoverAnchor m_popover;
#endif
};

sweeppp_contribution_type_t abiType(ContributionKind kind) noexcept {
    switch (kind) {
    case ContributionKind::Channel:
        return SWEEPPP_CONTRIBUTION_CHANNEL;
    case ContributionKind::Spot:
        return SWEEPPP_CONTRIBUTION_SPOT;
    case ContributionKind::Band:
        break;
    }
    return SWEEPPP_CONTRIBUTION_BAND;
}

sweeppp_contribution_t contributionFor(const Band& band) {
    const float color[4]{band.color.r, band.color.g, band.color.b, band.color.a};
    return plugin::contribution(abiType(band.kind), band.name, band.group, band.startHz,
                                band.stopHz, color, band.description);
}

// The vtables and the facets that name them, at namespace scope so the
// manifest can declare them without activating anything. That is what lets the
// Plugins panel say what this plugin does while it is switched off.

const sweeppp_contributor_vtable_t& contributorVtable() {
    static const sweeppp_contributor_vtable_t value =
        plugin::makeContributorVtable<BandPlanPlugin>();
    return value;
}

#if defined(SWEEPPP_PLUGIN_HAS_UI)
const sweeppp_ui_vtable_t& uiVtable() {
    // A toolbar button and nothing else: the spans are still drawn by the host
    // from what the contributor facet hands over, and this plugin still ships
    // no drawing code. What it draws is one button, and behind it the tree
    // that decides which allocations are handed over at all.
    //
    // Not the settings spot as well. That would put the same tree in the menu
    // and in the popover, and two ways to reach one control is one more than
    // an operator needs to learn.
    static const sweeppp_ui_vtable_t value = plugin::makeUiVtable<BandPlanPlugin>(
        SWEEPPP_UI_SPOT_BIT(SWEEPPP_UI_SPOT_TOOLBAR), SWEEPPP_UI_LAYER_UNDER,
        // What the button switches, which is what sends shift+B here rather
        // than to whoever draws the channels.
        SWEEPPP_CONTRIBUTION_BAND);
    return value;
}

constexpr std::size_t kFacetCount = 2;
#else
constexpr std::size_t kFacetCount = 1;
#endif

std::span<const sweeppp_facet_t> facets() {
    static const std::array<sweeppp_facet_t, kFacetCount> value{
        plugin::facet(SWEEPPP_FACET_CONTRIBUTOR, "bands", "Band allocations",
                      "Named frequency allocations, by service", &contributorVtable()),
#if defined(SWEEPPP_PLUGIN_HAS_UI)
        plugin::facet(SWEEPPP_FACET_UI_EXTENSION, "services", "Allocations button",
                      "The bar's allocations switch, and the tree of plans and services "
                      "behind it",
                      &uiVtable()),
#endif
    };
    return value;
}

// --------------------------------------------------------------- lifetime

bool BandPlanPlugin::activate(plugin::Host& host) {
    m_host = host;

    std::string settingsProblem;
    m_settings = sweeppp::PluginSettings::load(host.settingsPath(), &settingsProblem);
    if (!settingsProblem.empty()) {
        host.warn("{}", settingsProblem);
    }

    // Built-ins first, then the operator's own, so a user file of the same
    // plan name replaces the shipped one outright -- the same override rule
    // every other asset in the application follows. Both directories come from
    // the host: a plugin asking `Paths::instance()` would get its own copy,
    // which ignores `--config-dir` and every test's override.
    const std::array<std::filesystem::path, 2> directories{
        host.resourcesDir() / "bandplans",
        host.path(SWEEPPP_PATH_CONFIG_DIR) / "bandplans",
    };

    std::vector<std::string> problems;
    for (BandPlan& plan : BandPlan::discover(directories, &problems)) {
        m_plans.push_back(LoadedPlan{.plan = std::move(plan), .groupOn = {}, .bandOn = {}});
    }
    for (const std::string& problem : problems) {
        host.warn("{}", problem);
    }
    if (m_plans.empty()) {
        host.warn("no band plans under {} or {}", directories[0].string(), directories[1].string());
    }

    // Absent rather than empty: an empty array is "the operator turned
    // everything on", and seeding over that would undo their choice on every
    // start.
    const bool configured = toml_util::at(m_settings.table(), "bandplan.disabled").is_array();
    for (std::string& id : m_settings.getStringArray("bandplan.disabled")) {
        m_disabled.insert(std::move(id));
    }
    if (!configured) {
        seedDefaults();
    }
    rebuildMasks();

    if (const sweeppp_plugin_status_t status = host.registerFacet(facets()[0], this);
        status != SWEEPPP_PLUGIN_OK) {
        host.warn("the band data facet could not register");
    }

#if defined(SWEEPPP_PLUGIN_HAS_UI)
    // Adopting the host's ImGui is what makes drawing legal, and a refusal
    // here is reported and survivable: the contributor facet still answers the
    // marker and still paints the plot, because the host draws what it hands
    // over. What is lost is only the operator's ability to change what that is.
    if (std::string reason; !host.adoptImGui(reason)) {
        host.warn("not drawing: {}", reason);
        host.reportFacet(facets()[1], reason);
        return true;
    }

    if (const sweeppp_plugin_status_t status = host.registerFacet(facets()[1], this);
        status != SWEEPPP_PLUGIN_OK) {
        host.warn("the service tree facet could not register");
    }
#endif

    return true;
}

void BandPlanPlugin::deactivate() {
    // Nothing to stop: no threads, nothing held open. The settings are written
    // when they change rather than here, because a plugin that saves only on
    // the way out loses everything to the one exit that never runs this.
    m_plans.clear();
}

// ---------------------------------------------------------------- ticking

void BandPlanPlugin::seedDefaults() {
    // One plan on, the rest off. Every plan describes the same spectrum, so
    // two of them switched on paint every allocation twice -- readable only
    // once the operator has found this tree, which is not where a fresh
    // install should start them.
    m_disabled.clear();
    for (std::size_t i = 1; i < m_plans.size(); ++i) {
        m_disabled.insert(std::string(m_plans[i].plan.name()));
    }
}

void BandPlanPlugin::rebuildMasks() {
    for (LoadedPlan& loaded : m_plans) {
        const std::string_view name = loaded.plan.name();

        // The plugin's keys are qualified with the plan they belong to; the
        // model's are not, because it only ever sees one plan. Stripping the
        // qualifier keeps that asymmetry in one function.
        std::set<std::string, std::less<>> local;
        for (const std::string& group : loaded.plan.groups()) {
            if (m_disabled.contains(qualify(name, group))) {
                local.insert(group);
            }
        }
        for (const Band& band : loaded.plan.bands()) {
            std::string key = BandPlan::bandKey(band.group, band.name);
            if (m_disabled.contains(qualify(name, key))) {
                local.insert(std::move(key));
            }
        }

        loaded.groupOn = loaded.plan.resolveEnabled(local);

        // A whole plan switched off, which is the row above every category in
        // the tree. Its key is the bare plan name; a category's always carries
        // the plan and a '/'.
        if (m_disabled.contains(name)) {
            std::ranges::fill(loaded.groupOn, std::uint8_t{0});
        }

        loaded.bandOn = loaded.plan.resolveBands(loaded.groupOn, local);
    }
}

void BandPlanPlugin::persistDisabled() {
    const std::vector<std::string> ids(m_disabled.begin(), m_disabled.end());
    m_settings.set("bandplan.disabled", std::span<const std::string>(ids));
    if (auto saved = m_settings.save(); !saved) {
        m_host.warn("{}", saved.error().describe());
    }
}

void BandPlanPlugin::setKey(std::string key, bool on) {
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

void BandPlanPlugin::enablePath(const LoadedPlan& loaded, std::size_t groupIndex,
                                std::size_t bandIndex) {
    const std::string_view name = loaded.plan.name();
    const std::vector<std::string>& groups = loaded.plan.groups();
    const std::vector<Band>& bands = loaded.plan.bands();

    const auto bandAt = [&](std::size_t i) {
        return qualify(name, BandPlan::bandKey(bands[i].group, bands[i].name));
    };

    // The row itself, and everything under it.
    for (std::size_t i = 0; i < bands.size(); ++i) {
        const bool inside = bandIndex != kAll
                                ? i == bandIndex
                                : groupIndex == kAll || bands[i].groupIndex == groupIndex;
        if (inside) {
            m_disabled.erase(bandAt(i));
        }
    }
    if (bandIndex == kAll) {
        for (std::size_t g = 0; g < groups.size(); ++g) {
            if (groupIndex == kAll || g == groupIndex) {
                m_disabled.erase(qualify(name, groups[g]));
            }
        }
    }

    // Whatever above it was hiding it goes too -- and that ancestor's OTHER
    // branches go off in its place.
    //
    // Without the second half, clicking one service inside a switched-off plan
    // switches the whole plan on, which is a click that did far more than it
    // said. The operator asked for this row, not for its neighbours.
    const std::size_t group = bandIndex == kAll ? groupIndex : bands[bandIndex].groupIndex;

    if (group != kAll && m_disabled.contains(qualify(name, groups[group]))) {
        m_disabled.erase(qualify(name, groups[group]));
        for (std::size_t i = 0; i < bands.size(); ++i) {
            if (bands[i].groupIndex == group && i != bandIndex) {
                m_disabled.insert(bandAt(i));
            }
        }
    }

    if (m_disabled.contains(name)) {
        m_disabled.erase(std::string(name));
        for (std::size_t g = 0; g < groups.size(); ++g) {
            if (group != kAll && g != group) {
                m_disabled.insert(qualify(name, groups[g]));
            }
        }
    }

    persistDisabled();
    rebuildMasks();
}

std::string BandPlanPlugin::joinDisabled() const {
    // One newline-joined string, because a profile carries scalars only.
    std::string joined;
    for (const std::string& id : m_disabled) {
        if (!joined.empty()) {
            joined.push_back('\n');
        }
        joined.append(id);
    }
    return joined;
}

void BandPlanPlugin::saveProfile(const plugin::ProfileWriter& writer) {
    // Which plans and services are switched off, and nothing else. Not the
    // plans themselves: those are files on disk, and copying them into a
    // profile would make it stale the moment one is corrected, which is the
    // opposite of what a band plan is for.
    writer.set("disabled", std::string_view(joinDisabled()));
}

void BandPlanPlugin::loadProfile(const plugin::ProfileReader& reader) {
    // The profile wins over the settings file while it is loaded, and is not
    // written back to it: a profile is a setup an operator visits, and coming
    // back from it should leave their default where they left it.
    //
    // The fallback is what is already loaded, not the empty string. Every
    // profile saved before this key existed -- including the unnamed one the
    // application loads at startup -- carries none, and reading that as
    // "nothing is off" would switch every plan on at once for anyone who has
    // not saved a profile since.
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

// ------------------------------------------------------------- contributor

std::uint32_t BandPlanPlugin::contributionsIn(double fromHz, double toHz,
                                              sweeppp_contribution_t* out,
                                              std::uint32_t capacity) const {
    std::uint32_t total = 0;
    for (const LoadedPlan& loaded : m_plans) {
        for (const Band* band : loaded.plan.bandsIn(fromHz, toHz, loaded.bandOn)) {
            // Written while there is room, counted always: the full count is
            // what lets the host size a buffer with one call and fill it with
            // a second.
            if (total < capacity) {
                out[total] = contributionFor(*band);
            }
            ++total;
        }
    }
    return total;
}

std::uint32_t BandPlanPlugin::contributionsAt(double hz, sweeppp_contribution_t* out,
                                              std::uint32_t capacity) const {
    // Narrowest first across every plan, not per plan: a 200 kHz allocation is
    // the more specific answer than a 30 MHz one whichever file each came
    // from, and the host reads the first entry as the title.
    std::vector<const Band*> found;
    for (const LoadedPlan& loaded : m_plans) {
        for (const Band* band : loaded.plan.bandsAt(hz, loaded.bandOn)) {
            found.push_back(band);
        }
    }

    std::ranges::stable_sort(
        found, [](const Band* a, const Band* b) { return a->widthHz() < b->widthHz(); });

    const auto taken = std::min(capacity, static_cast<std::uint32_t>(found.size()));
    for (std::uint32_t i = 0; i < taken; ++i) {
        out[i] = contributionFor(*found[i]);
    }
    return static_cast<std::uint32_t>(found.size());
}

bool BandPlanPlugin::hide(const sweeppp_contribution_t& which) {
    const std::string_view name = plugin::view(which.name);

    for (const LoadedPlan& loaded : m_plans) {
        for (const Band& band : loaded.plan.bands()) {
            // The span as well as the name: a plan may reuse a name across two
            // allocations, and the operator pointed at one of them.
            if (band.name != name || band.startHz != which.start_hz ||
                band.stopHz != which.stop_hz) {
                continue;
            }
            setKey(qualify(loaded.plan.name(), BandPlan::bandKey(band.group, band.name)), false);
            return true;
        }
    }
    return false;
}

// -------------------------------------------------------------------- ui

#if defined(SWEEPPP_PLUGIN_HAS_UI)

namespace {

/// A small filled square in the service's own colour, so a row in the tree and
/// a span on the plot read as the same thing without either naming the other.
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

void BandPlanPlugin::drawToolbar() {
    namespace chrome = plugin::chrome;

    const bool shown = m_host.contributionsShown(SWEEPPP_CONTRIBUTION_BAND);

    // Shift+B, taken before anything is submitted: the popover has to be
    // opened ahead of the Begin that draws it.
    const bool shutPanel = m_popover.takeRequest(kPopupId, m_panelRequested);

    if (chrome::toolbarToggle(m_host.icon(kIcon, "Bands").append(kButtonId).c_str(), shown)) {
        m_host.setContributionsShown(SWEEPPP_CONTRIBUTION_BAND, !shown);
    }
    if (ImGui::IsItemHovered()) {
        // The state in words as well as in the button's own colour, because a
        // tooltip is what an operator reaches for when the colour did not
        // settle it.
        ImGui::SetTooltip("Band allocations on the spectrum: %s. The B key does the same."
                          "\n\nClick to %s them, right-click for the plans and services "
                          "behind them -- or shift+B.",
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

void BandPlanPlugin::drawTree() {
    if (m_plans.empty()) {
        ImGui::TextDisabled("No band plans found.");
        return;
    }

    std::size_t on = 0;
    std::size_t total = 0;
    for (const LoadedPlan& loaded : m_plans) {
        total += loaded.bandOn.size();
        on += static_cast<std::size_t>(std::ranges::count(loaded.bandOn, std::uint8_t{1}));
    }
    ImGui::TextDisabled("%zu of %zu allocations on", on, total);
    ImGui::TextDisabled("Unticked rows leave the plot and the marker's chip.");

    // Bounded, because this body draws inside the menu popup: five hundred
    // allocations in thirteen services would otherwise make the popup taller
    // than the screen and put its bottom out of reach.
    const float height = ImGui::GetTextLineHeightWithSpacing() * 14.0F;
    if (ImGui::BeginChild("##services", ImVec2(0.0F, height), ImGuiChildFlags_Borders)) {
        for (const LoadedPlan& loaded : m_plans) {
            const BandPlan& plan = loaded.plan;
            const std::string_view planName = plan.name();

            // Per plan, so two plans that both call a service "Mobile" cannot
            // share an ImGui id and fight over which was clicked.
            ImGui::PushID(plan.name().c_str());

            // Tri-state over the allocations, not over a category's own flag:
            // a service whose allocations have been unticked one at a time has
            // to read as "some", or the tick describes a flag rather than what
            // is on screen.
            const std::size_t count = plan.groups().size();
            std::vector<std::uint8_t> anyOn(count, 0);
            std::vector<std::uint8_t> allOn(count, 1);
            std::vector<std::size_t> counts(count, 0);
            std::vector<Color> colors(count);

            for (std::size_t i = 0; i < plan.bands().size(); ++i) {
                const std::size_t group = plan.bands()[i].groupIndex;
                if (counts[group]++ == 0) {
                    colors[group] = plan.bands()[i].color;
                }
                if (loaded.bandOn[i] != 0) {
                    anyOn[group] = 1;
                } else {
                    allOn[group] = 0;
                }
            }

            bool planAny = false;
            bool planAll = count > 0;
            for (std::size_t g = 0; g < count; ++g) {
                planAny = planAny || anyOn[g] != 0;
                planAll = planAll && allOn[g] != 0;
            }

            // The plan itself is a row with a tick of its own, so switching a
            // whole table off is one click -- and, because it only sets the
            // plan's own flag, switching it back on restores the selection
            // inside rather than everything.
            if (const auto action =
                    plugin::drawTick(planAny, planAll, !m_disabled.contains(planName))) {
                switch (*action) {
                case plugin::TickAction::Hide:
                    setKey(std::string(planName), false);
                    break;
                case plugin::TickAction::Unhide:
                    setKey(std::string(planName), true);
                    break;
                case plugin::TickAction::ShowAll:
                    enablePath(loaded, kAll, kAll);
                    break;
                }
            }
            ImGui::SameLine();

            const bool planOpen = ImGui::TreeNodeEx(
                "##plan", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth, "%s",
                plan.name().c_str());
            if (!plan.description().empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", plan.description().c_str());
            }

            if (planOpen) {
                for (std::size_t g = 0; g < count; ++g) {
                    const std::string& group = plan.groups()[g];
                    ImGui::PushID(static_cast<int>(g));

                    if (const auto action =
                            plugin::drawTick(anyOn[g] != 0, allOn[g] != 0,
                                             !m_disabled.contains(qualify(planName, group)))) {
                        switch (*action) {
                        case plugin::TickAction::Hide:
                            setKey(qualify(planName, group), false);
                            break;
                        case plugin::TickAction::Unhide:
                            setKey(qualify(planName, group), true);
                            break;
                        case plugin::TickAction::ShowAll:
                            enablePath(loaded, g, kAll);
                            break;
                        }
                    }
                    ImGui::SameLine();
                    drawSwatch(colors[g]);

                    const bool open = ImGui::TreeNodeEx("##service",
                                                        ImGuiTreeNodeFlags_OpenOnArrow |
                                                            ImGuiTreeNodeFlags_SpanAvailWidth,
                                                        "%s", group.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("%zu", counts[g]);

                    if (open) {
                        for (std::size_t i = 0; i < plan.bands().size(); ++i) {
                            const Band& band = plan.bands()[i];
                            if (band.groupIndex != g) {
                                continue;
                            }
                            ImGui::PushID(static_cast<int>(i) + 100000);

                            const std::string key =
                                qualify(planName, BandPlan::bandKey(band.group, band.name));
                            // No third state on a leaf: there is nothing under
                            // it to disagree.
                            if (const auto action =
                                    plugin::drawTick(loaded.bandOn[i] != 0, loaded.bandOn[i] != 0,
                                                     !m_disabled.contains(key))) {
                                switch (*action) {
                                case plugin::TickAction::Hide:
                                    setKey(key, false);
                                    break;
                                case plugin::TickAction::Unhide:
                                    setKey(key, true);
                                    break;
                                case plugin::TickAction::ShowAll:
                                    // Its own tick was already on and it still
                                    // is not showing, so what is hiding it is
                                    // above it -- and that is what this clears.
                                    enablePath(loaded, g, i);
                                    break;
                                }
                            }
                            ImGui::SameLine();
                            ImGui::TextUnformatted(band.name.c_str());
                            ImGui::SameLine();
                            ImGui::TextDisabled(
                                "%s – %s", toml_util::formatFrequencyShort(band.startHz).c_str(),
                                toml_util::formatFrequencyShort(band.stopHz).c_str());
                            if (!band.description.empty() && ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("%s", band.description.c_str());
                            }

                            ImGui::PopID();
                        }
                        ImGui::TreePop();
                    }

                    ImGui::PopID();
                }
                ImGui::TreePop();
            }

            ImGui::PopID();
        }
    }
    ImGui::EndChild();
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
                .name = plugin::str("Band plan"),
                .description =
                    plugin::str("Names the allocated service at any frequency, and hands those "
                                "allocations to the host to colour the spectrum with."),
                .authors = kAuthors.data(),
                .author_count = static_cast<std::uint32_t>(kAuthors.size()),
                .links = kLinks.data(),
                .link_count = static_cast<std::uint32_t>(kLinks.size()),
                .dependencies = nullptr,
                .dependency_count = 0,
                .min_host_version = plugin::str("0.1.0"),
                // Everything it registers can be withdrawn live: the facets
                // hand out copies and hold nothing, so unticking the plugin
                // clears the spans immediately.
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
} // namespace bandplan

SWEEPPP_PLUGIN_MAIN(bandplan::BandPlanPlugin, bandplan::describe)
