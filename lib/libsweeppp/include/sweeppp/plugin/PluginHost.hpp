// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/plugin/Plugin.hpp"
#include "sweeppp/plugin/PluginAbi.h"
#include "sweeppp/sdr/SdrParameter.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sweeppp {

class EventBus;
class FrameBus;
struct Profile;

namespace session {
class SessionRecorder;
} // namespace session

// ---------------------------------------------------------------------------
// The manifest, as the host holds it.
//
// Owned copies of what `sweeppp_manifest_t` borrows. The ABI's strings live in
// the plugin's own image and would be fine to keep pointers to -- the module
// is never unloaded -- but a `std::string` here means the panel, the CLI and
// the settings store all handle one ordinary type instead of a view whose
// validity depends on a rule three files away.
// ---------------------------------------------------------------------------

struct PluginAuthor {
    std::string name;
    std::string email;
};

enum class PluginLinkType : std::uint8_t {
    Homepage,
    Repository,
    Documentation,
    Issues,
    Funding,
    Other,
};

[[nodiscard]] std::string_view toString(PluginLinkType type) noexcept;

struct PluginLink {
    PluginLinkType type = PluginLinkType::Other;
    std::string url;
};

struct PluginDependency {
    std::string pluginId;
    std::string minVersion;
    std::string maxVersion;
    bool optional = false;
};

enum class PluginFacetKind : std::uint8_t {
    FrameProcessor,
    SdrDevice,
    FftBackend,
    Contributor,
    /// An antenna switcher: a box between a receive port and several
    /// antennas. Not an SDR device -- no samples, no tuning, no stream.
    RfPath,
    UiExtension,
};

[[nodiscard]] std::string_view toString(PluginFacetKind kind) noexcept;

/// One thing a plugin contributes, and whether it is currently doing so.
///
/// `active` false with a `failureReason` is the interesting case and the one
/// worth listing: a plugin whose FFT backend lost a name race to another
/// plugin still draws its overlay, and hiding the half that failed would make
/// the working half look like the whole story.
struct PluginFacetInfo {
    PluginFacetKind kind = PluginFacetKind::UiExtension;
    std::string id;
    std::string name;
    std::string description;
    bool active = false;
    std::string failureReason;
};

/// What the Plugins panel shows for one plugin.
///
/// Modelled on `FftBackendInfo`, which is already the project's "list it even
/// when it is unusable, with a reason" shape. Every failure -- a dlopen error,
/// a missing entry point, an ABI mismatch, a host too old, an unmet
/// dependency, a duplicate id -- becomes one of these with `loaded` false and
/// `failureReason` set. Nothing is ever silently skipped: a plugin that is
/// present but not working is exactly the thing an operator needs to be told
/// about, and a directory that appears to contain nothing teaches them
/// nothing.
struct PluginInfo {
    std::string id;
    std::string version;
    std::string name;
    std::string description;

    std::vector<PluginAuthor> authors;
    std::vector<PluginLink> links;
    std::vector<PluginDependency> dependencies;

    std::string minHostVersion;
    std::filesystem::path path;
    std::vector<PluginFacetInfo> facets;

    /// The ABI the binary reported, whether or not it was acceptable. Zero
    /// when the module would not even load.
    std::uint32_t abiVersion = 0;

    /// The module loaded and its manifest was read. False means `failureReason`
    /// says why, and every other field but `path` may be empty.
    bool loaded = false;

    /// The operator wants this plugin on. Persisted; survives the plugin
    /// itself going missing, so removing and restoring a file does not lose
    /// the setting.
    bool enabled = false;

    /// Activated and registered right now. Differs from `enabled` for a plugin
    /// that is on but failed to activate, and for one disabled since the last
    /// start that could not be withdrawn.
    bool active = false;

    /// Something this plugin holds cannot be withdrawn without a restart --
    /// either the plugin said so in its manifest, or a facet refused because
    /// it is in use.
    bool requiresRestart = false;

    std::string failureReason;
};

/// What kind of thing a contribution is.
///
/// The distinction a plain range could not carry, and the reason the ranked
/// list reads sensibly: a 83.5 MHz allocation and a 20 MHz channel inside it
/// are both "true at this frequency" and are not the same claim.
enum class ContributionType : std::uint8_t { Band, Channel, Spot };

[[nodiscard]] std::string_view toString(ContributionType type) noexcept;

/// One thing one plugin knows about a stretch of the frequency axis.
///
/// The generalisation of a band: whoever contributes it, a named span with a
/// category and a colour is the same thing to everything that consumes one.
struct Contribution {
    std::string pluginId;
    std::string pluginName; ///< For the second line of the hover list.
    ContributionType type = ContributionType::Band;
    std::string name;
    std::string description;
    std::string category;
    double startHz = 0.0;
    double stopHz = 0.0;
    std::array<float, 4> color{0.5F, 0.5F, 0.5F, 1.0F};

    /// Whether the host paints this one, or the contributor draws it itself.
    ///
    /// From the facet's `sweeppp_contributor_vtable_t::render`, which the ABI
    /// has always documented and the host had never read. A contributor that
    /// says NONE still answers every query -- the marker's readout, the hover
    /// list, the panels all keep naming what is at a frequency -- it simply
    /// wants its own spans drawn by its own overlay, because "a labelled
    /// region" is not what it has to say about them.
    bool hostRendered = true;

    [[nodiscard]] double widthHz() const noexcept { return stopHz - startHz; }
};

/// One row of a contributor's tick tree, copied out of the plugin.
using ContributorTreeRow = plugin::TreeRow;

/// A version as it is shown: "v1.2.0".
///
/// Display only. `PluginInfo::version` stays the bare dotted decimal the
/// manifest declared, because that is what `compareVersions` reads -- a stored
/// "v1.2.0" would compare as a non-numeric first component and stop every
/// comparison dead at zero, which would silently make every dependency bound
/// and every `min_host_version` pass.
///
/// Empty in, empty out: a lone "v" says less than nothing. A version that
/// already carries one is left alone, since a plugin is free to declare
/// "v2.0" and "vv2.0" helps nobody.
[[nodiscard]] std::string displayVersion(std::string_view version);

/// Compares dotted-decimal versions. Negative, zero or positive, like strcmp.
///
/// Missing components read as zero, so "1.2" and "1.2.0" are equal and a
/// manifest may be as precise as it likes. Anything non-numeric in a component
/// stops the comparison there and the components compared so far decide it --
/// which makes "1.2.3-rc1" sort with "1.2.3" rather than throwing.
[[nodiscard]] int compareVersions(std::string_view left, std::string_view right);

/// One directory to look in, and how much of it to believe.
struct PluginSearchEntry {
    std::filesystem::path directory;

    /// Whether only files named `sweeppp-plugin-*` are candidates here.
    ///
    /// A directory that exists to hold plugins is a statement of intent, so
    /// anything in it is a candidate whatever it is called. A directory shared
    /// with the rest of the system is not: `/usr/lib` holds thousands of
    /// shared objects, and finding out whether one is a plugin means
    /// `dlopen`ing it, which runs its initialisers. The name is what makes
    /// that question answerable without asking it.
    bool requiresPrefix = false;
};

/// Directories plugins are looked for in, highest precedence first.
///
/// The same "user overrides built-in" rule as `Paths::searchPath()`, extended
/// with an environment override at the front and the platform's own locations
/// at the back:
///
///     $SWEEPPP_PLUGIN_PATH        list, platform separator
///     <configDir>/plugins         the operator's own
///     <exeDir>/plugins            what shipped with the build
///     <prefix>/lib/<appId>/plugins  packaged, in a directory of ours
///     <prefix>/lib                packaged, sharing a directory -- prefixed only
///
/// First wins: two files declaring the same id means the earlier directory's
/// copy loads and the later one is listed as shadowed.
[[nodiscard]] std::vector<PluginSearchEntry> pluginSearchPath();

/// Every plugin binary under `entries`, in the order given.
///
/// Accepts `.so`, `.dylib` **and** `.dll` on every platform. Guessing the
/// extension from the host platform would be wrong here: CMake's
/// `add_library(MODULE)` with `PREFIX ""` emits `bandplan.so` on macOS, so a
/// macOS host that only looked for `.dylib` would fail to find the plugins it
/// had just built.
///
/// `Paths::listResources()` cannot be reused -- it is hardcoded to `.toml`.
[[nodiscard]] std::vector<std::filesystem::path>
findPluginBinaries(std::span<const PluginSearchEntry> entries);

/// The same over plain directories, each treated as one of ours -- which is
/// what an explicitly named directory is, whether it came from a test or from
/// `$SWEEPPP_PLUGIN_PATH`.
[[nodiscard]] std::vector<std::filesystem::path>
findPluginBinaries(std::span<const std::filesystem::path> directories);

/// Whether a filename carries the packaging prefix. Exposed because the same
/// answer decides what a packager should call a file and what the host will
/// pick up out of a shared directory.
[[nodiscard]] bool hasPluginFilePrefix(const std::filesystem::path& path);

/// The plugin host.
///
/// A singleton beside `SdrDeviceManager` and `FftBackendManager`, and for the
/// same reason: the facets a plugin registers land in those two, so a second
/// host would be a second set of registrations into one registry.
///
/// ---------------------------------------------------------------------------
/// A module is never unloaded
/// ---------------------------------------------------------------------------
///
/// **`dlclose` is not called anywhere in this project.** Disabling a plugin
/// withdraws every facet, stops every callback and drops the instance, but the
/// image stays mapped until the process exits.
///
/// This is a decision, not an oversight. Unmapping a module means unmapping
/// every `std::function` target, every vtable, every `shared_ptr` deleter and
/// every exception type it ever handed out -- and unlike a leaked allocation,
/// a stale pointer into an unmapped page is a crash with no useful stack.
/// Re-enabling in the same session re-activates the module that is already
/// there, which is both cheaper and safer than a reload. The cost is address
/// space and whatever statics the module holds, and neither is worth the class
/// of bug the alternative buys.
class PluginManager {
public:
    [[nodiscard]] static PluginManager& instance();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    /// Where enablement (`<root>/plugins.toml`) and per-plugin settings
    /// (`<root>/plugins/<id>.toml`) live. Defaults to the config directory;
    /// tests point it at a temporary root, which is why this exists rather
    /// than the manager reaching for `Paths::instance()` at every use.
    void setConfigRoot(std::filesystem::path root);

    /// The display bus plugin frame processors are attached to. Must be set
    /// before a frame-processor facet can register; without it such a facet is
    /// listed with "this host publishes no frames" rather than failing the
    /// plugin. The bus must outlive the manager's shutdown.
    void setFrameBus(FrameBus* bus) noexcept;

    /// The host's ImGui context, allocators and data layout. Set by the GUI
    /// before `discover()`; left unset by the CLI and by tests, where a UI
    /// facet is listed and never drawn.
    void setImGuiBinding(const sweeppp_imgui_binding_t& binding);

    /// The application's own chrome, for a plugin that draws in it.
    ///
    /// Accessors rather than values: a plugin's toolbar button reads these
    /// every frame, and the profile published by `setProfile` is a snapshot
    /// taken four times a second -- a button lit from it would lag the key
    /// that unlit the spans by a quarter of a second.
    ///
    /// Unset outside the GUI, where every switch reads as off and setting one
    /// does nothing. That is the honest answer for a host that paints nothing,
    /// and it keeps the CLI from having to own a display flag it never reads.
    struct Chrome {
        std::function<bool(sweeppp_contribution_type_t)> contributionsShown;
        std::function<void(sweeppp_contribution_type_t, bool)> setContributionsShown;
        std::function<bool()> iconsAvailable;

        /// Where the operator wants a file saved, or empty if they cancelled.
        ///
        /// A hook rather than a direct call, for the same reason the two
        /// above are: the dialog is the application's, and this library builds
        /// headless. Unset, plugins are told there is no dialog and fall back
        /// to writing into their own data directory.
        std::function<std::string(std::string_view title, std::string_view suggestedName,
                                  std::string_view extension)>
            saveFile;
    };
    void setChrome(Chrome chrome);

    /// Publishes the running configuration for plugins to read.
    ///
    /// Flattened to the same dotted keys `Profile::save` writes -- "sweep.start",
    /// "display.grid", "device.driver" -- rather than to a second vocabulary
    /// invented for the ABI. A key a plugin reads is therefore a key the
    /// operator can find in their own `settings.toml`, and the flattening has
    /// one definition instead of two that drift.
    ///
    /// Read-only from the plugin's side. Call it whenever the configuration
    /// changes; cheap enough to call once a second, too expensive per frame.
    void setProfile(const Profile& profile);

    /// The session a plugin's records and events are written into, or null
    /// when none is open. The recorder must outlive the call that clears it.
    void setSessionRecorder(session::SessionRecorder* recorder) noexcept;

    /// What every active plugin wants stored in the profile being saved, under
    /// `plugins.<plugin id>.<key>`.
    ///
    /// Called while building a profile, not on a timer: a plugin's hook runs
    /// on the UI thread with nothing else in flight, which is what lets it
    /// read its own live state without a lock.
    [[nodiscard]] std::vector<std::pair<std::string, SdrValue>> collectProfileValues();

    /// Hands a loaded profile's values back to the plugins that wrote them.
    ///
    /// A plugin sees only its own keys, and a plugin whose keys are absent --
    /// a profile saved before it was installed -- is simply not called, so it
    /// keeps whatever its settings file gave it.
    void applyProfileValues(std::span<const std::pair<std::string, SdrValue>> values);

    /// Scans the search path, reads every manifest, and activates what is
    /// enabled. Safe to call once; a second call rescans for binaries that
    /// have appeared and leaves what is already loaded alone.
    void discover();

    /// The same over an explicit directory list, for tests and for
    /// `--plugin-dir`.
    void discover(std::span<const std::filesystem::path> directories);

    /// Every plugin found, working or not, sorted with working ones first.
    [[nodiscard]] std::vector<PluginInfo> enumerate() const;

    [[nodiscard]] Result<PluginInfo> info(std::string_view id) const;

    /// Turns a plugin on or off, live where it can be.
    ///
    /// Enabling activates and registers. Disabling withdraws every facet; a
    /// facet that refuses -- an open radio on its driver, an FFT backend the
    /// pipeline has acquired -- leaves the plugin marked `requiresRestart` and
    /// still active, and the returned status says which one and why. The
    /// preference is persisted either way, so the restart does what was asked.
    [[nodiscard]] Status setEnabled(std::string_view id, bool enabled);

    [[nodiscard]] bool isEnabled(std::string_view id) const;

    // ---- the event channel -----------------------------------------------
    //
    // Typed, keyed on the kinds `.sweeps` already froze. Open to the
    // application as well as to plugins, because a channel only plugins can
    // speak on is a channel nothing answers: this is how the host observes
    // what a plugin published, and what the tests watch.
    //
    // An exception escaping a handler is caught and logged; the remaining
    // handlers still run.

    /// Listens for one event type, named by the type.
    ///
    /// The same shape `EventBus::subscribe<Event>` has, and deliberately: the
    /// application should not have to know that a plugin event arrives as a
    /// name and a `void*` any more than a plugin does. The payload's size is
    /// checked before the cast, so an event published by something built
    /// against a smaller struct is dropped rather than read past the end of.
    template <plugin::EventPayload Event>
    [[nodiscard]] std::uint64_t subscribeEvent(std::function<void(const Event&)> handler) {
        return subscribeRaw(
            std::string(plugin::EventTraits<Event>::name),
            [handler = std::move(handler)](const sweeppp_event_t& event) {
                // Same shape, then same size. A schema version says the layout
                // changed in a way growing it cannot express, so a mismatch is
                // dropped rather than reinterpreted -- reading a v2 payload as
                // a v1 is exactly the plausible-garbage failure this channel
                // is typed to avoid.
                if (event.schema_version != plugin::EventTraits<Event>::schemaVersion) {
                    return;
                }
                if (event.payload == nullptr || event.payload_size < sizeof(Event)) {
                    return;
                }
                const auto* payload = static_cast<const Event*>(event.payload);
                if (payload->struct_size < sizeof(Event)) {
                    return;
                }
                handler(*payload);
            });
    }

    /// Every event, whatever its type. The one place the application handles a
    /// raw envelope, because "everything" has no single payload type.
    using RawEventCallback = std::function<void(const sweeppp_event_t& event)>;
    [[nodiscard]] std::uint64_t subscribeAllEvents(RawEventCallback callback) {
        return subscribeRaw({}, std::move(callback));
    }

    void unsubscribeEvent(std::uint64_t subscription);

    /// Feeds the application's own `EventBus` into the plugin channel.
    ///
    /// One direction only, and deliberately: a plugin publishes
    /// `SWEEPPP_EVENT_PLUGIN` and nothing else, so a plugin cannot make the
    /// session say the radio retuned when it did not. Call once, with a bus
    /// that outlives `shutdown()`.
    void attachEvents(EventBus& bus);

    // ---- contributors ----------------------------------------------------

    /// Everything known about `hz`, ranked, most authoritative first.
    ///
    /// The one ordering rule the rest of the application reads from:
    ///
    ///  1. the operator's `plugins.contributors` position; every plugin not
    ///     listed comes after every plugin that is, in load order;
    ///  2. within one plugin, the order that plugin returned -- it knows its
    ///     own nesting, and the band plan already returns narrowest first.
    ///
    /// So the first entry titles the marker, and it is a preference rather
    /// than an accident of which span happens to be narrower. An empty result
    /// is a normal answer.
    [[nodiscard]] std::vector<Contribution> contributionsAt(double hz) const;

    /// Contributions overlapping [fromHz, toHz], in the same rank order.
    [[nodiscard]] std::vector<Contribution> contributionsIn(double fromHz, double toHz) const;

    /// Asks the contributor that produced `entry` to stop offering it.
    ///
    /// What the operator dismissing a label on the plot means: it is a request
    /// to the plugin that owns the entry, not a hidden list the host keeps,
    /// because the plugin's own settings have to agree about it afterwards --
    /// a channel dismissed on the plot should come back unticked in the tree
    /// that lists it.
    ///
    /// False when the contributor does not offer the operation, which is the
    /// normal answer for one whose entries are not individually dismissable.
    [[nodiscard]] bool hideContribution(const Contribution& entry);

    /// The operator's priority order, highest first. Ids only; a plugin the
    /// order does not name answers after every one it does.
    [[nodiscard]] std::vector<std::string> contributorOrder() const;

    [[nodiscard]] Status setContributorOrder(std::span<const std::string> ids);

    /// Whether this contributor's spans are drawn on the plots. A hidden
    /// contributor still answers `contributionsAt`.
    [[nodiscard]] bool contributorShown(std::string_view id) const;

    [[nodiscard]] Status setContributorShown(std::string_view id, bool shown);

    /// One contributor's datasets, as (name, description), in its own order.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>>
    datasets(std::string_view id) const;

    [[nodiscard]] std::uint32_t activeDataset(std::string_view id) const;

    [[nodiscard]] Status selectDataset(std::string_view id, std::uint32_t index);

    /// One contributor's tick tree, in drawing order. Empty for one that has
    /// none.
    [[nodiscard]] std::vector<ContributorTreeRow> contributorTree(std::string_view id) const;

    /// The operator clicked one row's tick in that tree.
    [[nodiscard]] Status toggleContributorRow(std::string_view id, std::string_view key);

    /// Counts every change to what the contributors hand out -- shown, order,
    /// dataset, a tick, a hidden entry -- so a client knows to ask again.
    [[nodiscard]] std::uint64_t contributionsGeneration() const noexcept;

    /// Dataset names every active contributor offers, as "plugin id: dataset".
    [[nodiscard]] std::vector<std::string> providedDatasets() const;

    // ---- UI extension dispatch -------------------------------------------
    //
    // Called on the UI thread, once per frame, never concurrently. Every call
    // is wrapped: an exception escaping a facet disables that facet with the
    // reason shown in the panel, which is the same containment
    // `FrameBus::publish` already gives frame consumers. The alternative is a
    // plugin's bad frame taking down the instrument.

    /// Every overlay facet registered for `context.spot` and `context.layer`.
    void drawOverlay(const sweeppp_plot_context_t& context);

    /// Every facet registered for a spot that carries no geometry -- the
    /// status chip and the toolbar.
    void drawSpot(sweeppp_ui_spot_t spot);

    /// One plugin's settings body. False means it has nothing to show, which
    /// the caller renders as such rather than leaving a gap.
    /// Asks whoever draws the toolbar panel for this contribution type to open
    /// it -- what shift+B and shift+C reach.
    ///
    /// By type, not by plugin id: the keys are the host's and describe what is
    /// on the plot, so a channel list nobody has heard of gets the same
    /// keystroke by declaring what its button governs. Nothing happens when no
    /// plugin claims the type, which is the right amount to happen.
    void openToolbarPanel(sweeppp_contribution_type_t type);

    [[nodiscard]] bool drawSettings(std::string_view id);

    /// Plugins offering a settings body, as (id, display name), in load order.
    ///
    /// So the settings menu can give each one a section of its own beside
    /// Display and Theme. A plugin's settings are settings: burying them in
    /// the row that manages the plugin would put "which band plan" two levels
    /// deeper than "show the grid", for no reason an operator would recognise.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> settingsSections() const;

    /// Whether any active facet draws in `spot`, so a caller can skip the
    /// separator and padding around an empty region.
    [[nodiscard]] bool hasUiSpot(sweeppp_ui_spot_t spot) const;

    /// Deactivates everything, in reverse load order. Called at shutdown; the
    /// modules stay mapped, see the note above.
    void shutdown();

    /// Drops every record and every registration, for tests that need a clean
    /// manager. Not for application use: a plugin whose facets have been
    /// withdrawn is still mapped, and rediscovery re-activates the same image.
    void reset();

private:
    PluginManager();
    ~PluginManager();

    /// What the typed subscribe helpers above are built on. Private because a
    /// caller that names the type as a string has stepped outside the shape
    /// the rest of this class is trying to keep.
    [[nodiscard]] std::uint64_t subscribeRaw(std::string type, RawEventCallback callback);

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    /// Recursive, and it has to be: a plugin's `activate` runs under this lock
    /// and registers its facets by calling straight back into the host, so a
    /// plain mutex would deadlock on the first plugin that did the one thing
    /// activation is for.
    mutable std::recursive_mutex m_mutex;
};

} // namespace sweeppp
