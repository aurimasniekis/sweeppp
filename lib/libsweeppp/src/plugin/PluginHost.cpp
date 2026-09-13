// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/plugin/PluginHost.hpp"

#include "plugin/DynamicLibrary.hpp"
#include "plugin/PluginAbiSupport.hpp"
#include "plugin/PluginFacets.hpp"
#include "sweeppp/core/Clock.hpp"
#include "sweeppp/core/Log.hpp"
#include "sweeppp/core/Paths.hpp"
#include "sweeppp/core/Toml.hpp"
#include "sweeppp/core/Version.hpp"
#include "sweeppp/fft/FftBackendManager.hpp"
#include "sweeppp/history/SessionRecorder.hpp"
#include "sweeppp/pipeline/FrameBus.hpp"
#include "sweeppp/plugin/PluginSettings.hpp"
#include "sweeppp/profile/Profile.hpp"
#include "sweeppp/rf/IRfPath.hpp"
#include "sweeppp/sdr/ISdrDevice.hpp"

#include <algorithm>
#include <charconv>
#include <set>

namespace fs = std::filesystem;

namespace sweeppp {
namespace {

using plugin_abi::borrow;
using plugin_abi::kLogCategory;
using plugin_abi::owned;
using plugin_abi::statusName;
using plugin_abi::view;

/// A path handed back through the ABI, valid until this thread's next host
/// call. Thread-local for the same reason `sweeps_last_error` is: a plugin
/// asking for a path on a worker must not see a string another thread is in
/// the middle of replacing.
thread_local std::string g_pathScratch;

/// The most contributions one query will collect. A contributor reporting a
/// nonsense count should cost a truncated answer rather than the address
/// space; the same reasoning as the device enumeration cap.
constexpr std::uint32_t kMaxContributions = 16'384;

/// One component of a dotted version, and where it ended.
struct VersionComponent {
    long value = 0;
    bool numeric = false;
};

VersionComponent nextComponent(std::string_view& text) {
    if (text.empty()) {
        return {.value = 0, .numeric = true};
    }

    const std::size_t dot = text.find('.');
    const std::string_view piece = text.substr(0, dot);
    text = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);

    VersionComponent component;
    const auto* end = piece.data() + piece.size();
    const auto result = std::from_chars(piece.data(), end, component.value);
    component.numeric = result.ec == std::errc{} && result.ptr == end;
    return component;
}

/// Reads what the manifest borrows into strings the host owns.
void readAuthors(const sweeppp_manifest_t& manifest, PluginInfo& info) {
    if (!SWEEPPP_ABI_HAS(&manifest, author_count)) {
        return;
    }
    plugin_abi::forEachElement(manifest.authors, manifest.author_count,
                               [&info](const sweeppp_author_t& author) {
                                   PluginAuthor entry;
                                   if (SWEEPPP_ABI_HAS(&author, name)) {
                                       entry.name = owned(author.name);
                                   }
                                   if (SWEEPPP_ABI_HAS(&author, email)) {
                                       entry.email = owned(author.email);
                                   }
                                   if (!entry.name.empty()) {
                                       info.authors.push_back(std::move(entry));
                                   }
                               });
}

PluginLinkType toLinkType(sweeppp_link_type_t type) noexcept {
    switch (type) {
    case SWEEPPP_LINK_HOMEPAGE:
        return PluginLinkType::Homepage;
    case SWEEPPP_LINK_REPOSITORY:
        return PluginLinkType::Repository;
    case SWEEPPP_LINK_DOCUMENTATION:
        return PluginLinkType::Documentation;
    case SWEEPPP_LINK_ISSUES:
        return PluginLinkType::Issues;
    case SWEEPPP_LINK_FUNDING:
        return PluginLinkType::Funding;
    case SWEEPPP_LINK_OTHER:
    case SWEEPPP_LINK_FORCE_INT32:
        break;
    }
    return PluginLinkType::Other;
}

void readLinks(const sweeppp_manifest_t& manifest, PluginInfo& info) {
    if (!SWEEPPP_ABI_HAS(&manifest, link_count)) {
        return;
    }
    plugin_abi::forEachElement(
        manifest.links, manifest.link_count, [&info](const sweeppp_link_t& link) {
            if (!SWEEPPP_ABI_HAS(&link, url)) {
                return;
            }
            PluginLink entry{.type = toLinkType(link.type), .url = owned(link.url)};
            if (!entry.url.empty()) {
                info.links.push_back(std::move(entry));
            }
        });
}

void readDependencies(const sweeppp_manifest_t& manifest, PluginInfo& info) {
    if (!SWEEPPP_ABI_HAS(&manifest, dependency_count)) {
        return;
    }
    plugin_abi::forEachElement(manifest.dependencies, manifest.dependency_count,
                               [&info](const sweeppp_dependency_t& dependency) {
                                   if (!SWEEPPP_ABI_HAS(&dependency, plugin_id)) {
                                       return;
                                   }
                                   PluginDependency entry;
                                   entry.pluginId = owned(dependency.plugin_id);
                                   if (SWEEPPP_ABI_HAS(&dependency, min_version)) {
                                       entry.minVersion = owned(dependency.min_version);
                                   }
                                   if (SWEEPPP_ABI_HAS(&dependency, max_version)) {
                                       entry.maxVersion = owned(dependency.max_version);
                                   }
                                   if (SWEEPPP_ABI_HAS(&dependency, optional)) {
                                       entry.optional = dependency.optional != 0;
                                   }
                                   if (!entry.pluginId.empty()) {
                                       info.dependencies.push_back(std::move(entry));
                                   }
                               });
}

PluginFacetKind toFacetKind(sweeppp_facet_kind_t kind) noexcept {
    switch (kind) {
    case SWEEPPP_FACET_FRAME_PROCESSOR:
        return PluginFacetKind::FrameProcessor;
    case SWEEPPP_FACET_SDR_DEVICE:
        return PluginFacetKind::SdrDevice;
    case SWEEPPP_FACET_FFT_BACKEND:
        return PluginFacetKind::FftBackend;
    case SWEEPPP_FACET_CONTRIBUTOR:
        return PluginFacetKind::Contributor;
    case SWEEPPP_FACET_RF_PATH:
        return PluginFacetKind::RfPath;
    case SWEEPPP_FACET_UI_EXTENSION:
    case SWEEPPP_FACET_KIND_FORCE_INT32:
        break;
    }
    return PluginFacetKind::UiExtension;
}

ContributionType toContributionType(sweeppp_contribution_type_t type) noexcept {
    switch (type) {
    case SWEEPPP_CONTRIBUTION_CHANNEL:
        return ContributionType::Channel;
    case SWEEPPP_CONTRIBUTION_SPOT:
        return ContributionType::Spot;
    case SWEEPPP_CONTRIBUTION_BAND:
    case SWEEPPP_CONTRIBUTION_TYPE_FORCE_INT32:
        break;
    }
    return ContributionType::Band;
}

sweeppp_contribution_type_t abiContributionType(ContributionType type) noexcept {
    switch (type) {
    case ContributionType::Channel:
        return SWEEPPP_CONTRIBUTION_CHANNEL;
    case ContributionType::Spot:
        return SWEEPPP_CONTRIBUTION_SPOT;
    case ContributionType::Band:
        break;
    }
    return SWEEPPP_CONTRIBUTION_BAND;
}

/// Whether this contributor asked to draw its own spans.
///
/// Guarded, because `render` is not in the oldest vtable layout: a plugin built
/// against that one has the host draw for it, which is the documented default
/// and the answer every existing plugin wants.
bool markedNotHostRendered(const sweeppp_contributor_vtable_t* vtable) noexcept {
    return SWEEPPP_ABI_HAS(vtable, render) && vtable->render == SWEEPPP_CONTRIBUTION_RENDER_NONE;
}

Contribution readContribution(const PluginInfo& plugin, const sweeppp_contribution_t& entry) {
    Contribution out;
    out.pluginId = plugin.id;
    out.pluginName = plugin.name;
    out.type = toContributionType(entry.type);
    if (SWEEPPP_ABI_HAS(&entry, name)) {
        out.name = owned(entry.name);
    }
    if (SWEEPPP_ABI_HAS(&entry, description)) {
        out.description = owned(entry.description);
    }
    if (SWEEPPP_ABI_HAS(&entry, category)) {
        out.category = owned(entry.category);
    }
    if (SWEEPPP_ABI_HAS(&entry, stop_hz)) {
        out.startHz = entry.start_hz;
        out.stopHz = entry.stop_hz;
    }
    if (SWEEPPP_ABI_HAS(&entry, color)) {
        out.color = {entry.color[0], entry.color[1], entry.color[2], entry.color[3]};
    }
    return out;
}

/// The two-call buffer protocol both contribution queries use.
///
/// Sized for the common case, then one resize and one retry if the contributor
/// says there were more than fitted. Only one: it told us the size, and
/// trusting it a second time would be trusting a number that already disagreed
/// with itself.
template <typename Query>
void collectContributions(const PluginInfo& plugin, Query&& query, std::vector<Contribution>& out) {
    const auto stamp = [](std::vector<sweeppp_contribution_t>& entries) {
        for (sweeppp_contribution_t& entry : entries) {
            entry.struct_size = sizeof(sweeppp_contribution_t);
        }
    };

    std::vector<sweeppp_contribution_t> buffer(64);
    stamp(buffer);

    std::uint32_t total = query(buffer.data(), static_cast<std::uint32_t>(buffer.size()));
    if (total > buffer.size()) {
        buffer.assign(std::min(total, kMaxContributions), sweeppp_contribution_t{});
        stamp(buffer);
        total = query(buffer.data(), static_cast<std::uint32_t>(buffer.size()));
    }

    const std::uint32_t taken =
        std::min<std::uint32_t>(total, static_cast<std::uint32_t>(buffer.size()));
    for (std::uint32_t i = 0; i < taken; ++i) {
        out.push_back(readContribution(plugin, buffer[i]));
    }
}

PluginFacetInfo readFacet(const sweeppp_facet_t& facet) {
    PluginFacetInfo info;
    info.kind = toFacetKind(facet.kind);
    if (SWEEPPP_ABI_HAS(&facet, id)) {
        info.id = owned(facet.id);
    }
    if (SWEEPPP_ABI_HAS(&facet, name)) {
        info.name = owned(facet.name);
    }
    if (SWEEPPP_ABI_HAS(&facet, description)) {
        info.description = owned(facet.description);
    }
    return info;
}

} // namespace

std::string_view toString(PluginLinkType type) noexcept {
    switch (type) {
    case PluginLinkType::Homepage:
        return "homepage";
    case PluginLinkType::Repository:
        return "repository";
    case PluginLinkType::Documentation:
        return "documentation";
    case PluginLinkType::Issues:
        return "issues";
    case PluginLinkType::Funding:
        return "funding";
    case PluginLinkType::Other:
        break;
    }
    return "link";
}

std::string_view toString(PluginFacetKind kind) noexcept {
    switch (kind) {
    case PluginFacetKind::FrameProcessor:
        return "frame processor";
    case PluginFacetKind::SdrDevice:
        return "SDR device";
    case PluginFacetKind::FftBackend:
        return "FFT backend";
    case PluginFacetKind::Contributor:
        return "contributor";
    case PluginFacetKind::RfPath:
        return "RF path";
    case PluginFacetKind::UiExtension:
        break;
    }
    return "UI extension";
}

std::string_view toString(ContributionType type) noexcept {
    switch (type) {
    case ContributionType::Channel:
        return "channel";
    case ContributionType::Spot:
        return "spot";
    case ContributionType::Band:
        break;
    }
    return "band";
}

std::string displayVersion(std::string_view version) {
    if (version.empty()) {
        return {};
    }
    if (version.starts_with('v') || version.starts_with('V')) {
        return std::string(version);
    }
    return std::format("v{}", version);
}

int compareVersions(std::string_view left, std::string_view right) {
    while (!left.empty() || !right.empty()) {
        const VersionComponent a = nextComponent(left);
        const VersionComponent b = nextComponent(right);

        // A component that is not a number ends the comparison: "1.2.3-rc1"
        // and "1.2.3" differ in a way this cannot rank, and guessing would put
        // a pre-release either side of its own release depending on the
        // spelling. Everything compared so far decides it.
        if (!a.numeric || !b.numeric) {
            return 0;
        }
        if (a.value != b.value) {
            return a.value < b.value ? -1 : 1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The host's own state.
// ---------------------------------------------------------------------------

struct PluginManager::Impl {
    /// One facet a plugin has actually registered, and what it takes to
    /// withdraw it again.
    struct ActiveFacet {
        PluginFacetKind kind = PluginFacetKind::UiExtension;
        std::string id;
        const void* vtable = nullptr;
        void* instance = nullptr;

        /// SDR facets only. Observed, not owned -- `SdrDeviceManager` holds
        /// the `unique_ptr` -- and cleared the moment it is withdrawn.
        plugin_facets::SdrFactory* sdrFactory = nullptr;

        /// RF path facets only, on the same terms.
        plugin_facets::RfPathFactory* rfPathFactory = nullptr;

        /// Frame-processor facets only.
        std::unique_ptr<plugin_facets::FrameProcessor> frameProcessor;
        FrameBus::SubscriptionId subscription = 0;

        /// Set when a callback threw. The facet stops being called and the
        /// reason is copied into its listing, which is the same containment
        /// `FrameBus::publish` gives frame consumers.
        bool broken = false;
    };

    struct Record {
        PluginInfo info;
        DynamicLibrary library;
        const sweeppp_plugin_desc_t* desc = nullptr;
        void* instance = nullptr;
        std::vector<ActiveFacet> facets;
    };

    /// One listener on the event channel, from either side of the boundary.
    ///
    /// Two callback shapes rather than one, because the two sides genuinely
    /// differ: a plugin can only be reached through a function pointer and a
    /// `void*`, and forcing the host to wrap a lambda into that pair would
    /// mean the host allocating something the plugin might outlive.
    struct EventSubscription {
        std::uint64_t id = 0;
        /// Empty means every type.
        std::string type;
        sweeppp_event_fn_t callback = nullptr;
        void* user = nullptr;
        RawEventCallback hostCallback;
    };

    fs::path configRoot;
    FrameBus* frameBus = nullptr;
    session::SessionRecorder* recorder = nullptr;

    /// The running configuration, flattened. Sorted by key, so `profile_keys`
    /// is stable between calls and a lookup is a binary search rather than a
    /// walk of a few dozen strings on every query.
    std::vector<std::pair<std::string, SdrValue>> profile;

    bool hasImGui = false;
    sweeppp_imgui_binding_t imgui{};
    Chrome chrome;
    sweeppp_host_api_t api{};

    // unique_ptr, so a record's address survives the vector growing during a
    // discovery pass -- an activating plugin calls back into the host, and the
    // thunks hold references while it does.
    std::vector<std::unique_ptr<Record>> records;
    PluginEnablement enablement;

    std::vector<EventSubscription> eventSubscriptions;
    std::uint64_t nextEventId = 1;

    [[nodiscard]] fs::path root() const {
        return configRoot.empty() ? Paths::instance().configDir() : configRoot;
    }

    [[nodiscard]] fs::path enablementFile() const { return root() / "plugins.toml"; }
    [[nodiscard]] fs::path settingsDir() const { return root() / "plugins"; }

    [[nodiscard]] Record* find(std::string_view id) {
        const auto match = std::ranges::find_if(
            records, [id](const std::unique_ptr<Record>& record) { return record->info.id == id; });
        return match != records.end() ? match->get() : nullptr;
    }

    [[nodiscard]] PluginFacetInfo* facetInfo(Record& record, PluginFacetKind kind,
                                             std::string_view id) {
        const auto match =
            std::ranges::find_if(record.info.facets, [kind, id](const PluginFacetInfo& candidate) {
                return candidate.kind == kind && candidate.id == id;
            });
        return match != record.info.facets.end() ? &*match : nullptr;
    }

    void buildApi();

    /// Loads whatever is new in `binaries` and brings up what is enabled.
    /// Shared by both `discover` overloads so the two cannot drift about what
    /// a rescan does to what is already loaded.
    void scan(const std::vector<fs::path>& binaries);

    /// Delivers to every listener for this type, containing what each throws.
    void dispatchEvent(const sweeppp_event_t& event);

    /// Publishes one of the host's own events, with the payload it names.
    template <typename Payload>
    void publishHostEvent(std::string_view type, std::uint64_t monotonicNs, Payload& payload) {
        payload.struct_size = sizeof(Payload);

        sweeppp_event_t event{};
        event.struct_size = sizeof(sweeppp_event_t);
        event.type = borrow(type);
        event.schema_version = 1;
        event.monotonic_ns = monotonicNs;
        event.wall_ns = wallClockNs();
        event.source = borrow({});
        event.payload = &payload;
        event.payload_size = sizeof(Payload);

        dispatchEvent(event);
    }

    void load(const fs::path& path);
    void activateEnabled();
    void activate(Record& record);
    [[nodiscard]] Status deactivate(Record& record);

    /// Why `record` cannot be withdrawn right now, or empty when it can be.
    /// Asked before anything is withdrawn, so a refusal leaves the plugin
    /// whole rather than half-registered.
    [[nodiscard]] std::string withdrawalBlocker(Record& record) const;

    /// Records a facet the plugin has just registered, and links it to the
    /// declaration in the manifest so the listing says it is active.
    ActiveFacet& adopt(Record& record, const sweeppp_facet_t& facet, PluginFacetKind kind);

    /// Every working contributor facet, highest priority first.
    ///
    /// The operator's `plugins.contributors` decides it, and everything that
    /// order does not name follows in load order. One definition, because
    /// every ranked answer -- the chip, the overlay, the menu -- has to agree
    /// about who is first or the readouts stop describing the same thing.
    [[nodiscard]] std::vector<std::pair<Record*, ActiveFacet*>> rankedContributors() {
        const std::vector<std::string>& order = enablement.contributorOrder();

        std::vector<std::pair<Record*, ActiveFacet*>> found;
        for (const std::unique_ptr<Record>& record : records) {
            for (ActiveFacet& facet : record->facets) {
                if (facet.kind == PluginFacetKind::Contributor && !facet.broken) {
                    found.emplace_back(record.get(), &facet);
                }
            }
        }

        const auto rank = [&order](const Record* record) {
            const auto at = std::ranges::find(order, record->info.id);
            return at != order.end() ? static_cast<std::size_t>(at - order.begin()) : order.size();
        };

        // Stable, so load order survives as the tie-break for everything the
        // operator has not ordered -- and so two facets of one plugin keep the
        // order that plugin declared them in.
        std::ranges::stable_sort(found, {},
                                 [&rank](const auto& entry) { return rank(entry.first); });
        return found;
    }

    /// Runs a plugin callback with the containment every one of them gets.
    template <typename Fn>
    void guarded(Record& record, ActiveFacet& facet, Fn&& body);
};

namespace {

sweeppp_str_t scratch(std::string text) {
    g_pathScratch = std::move(text);
    return borrow(g_pathScratch);
}

} // namespace

template <typename Fn>
void PluginManager::Impl::guarded(Record& record, ActiveFacet& facet, Fn&& body) {
    if (facet.broken) {
        return;
    }

    try {
        body();
    } catch (const std::exception& error) {
        facet.broken = true;
        if (PluginFacetInfo* listed = facetInfo(record, facet.kind, facet.id)) {
            listed->active = false;
            listed->failureReason = error.what();
        }
        logError(kLogCategory, "{}: facet '{}' threw and has been disabled: {}", record.info.id,
                 facet.id, error.what());
    } catch (...) {
        facet.broken = true;
        if (PluginFacetInfo* listed = facetInfo(record, facet.kind, facet.id)) {
            listed->active = false;
            listed->failureReason = "the facet threw an unrecognised exception";
        }
        logError(kLogCategory, "{}: facet '{}' threw and has been disabled", record.info.id,
                 facet.id);
    }
}

auto PluginManager::Impl::adopt(Record& record, const sweeppp_facet_t& facet, PluginFacetKind kind)
    -> ActiveFacet& {
    const std::string id = SWEEPPP_ABI_HAS(&facet, id) ? owned(facet.id) : std::string{};

    if (PluginFacetInfo* listed = facetInfo(record, kind, id)) {
        listed->active = true;
        listed->failureReason.clear();
    } else {
        // Registered without having been declared. Legal -- a plugin may
        // decide at activation that it can offer something -- and listed all
        // the same, because an unlisted facet is one nobody can turn off.
        PluginFacetInfo added = readFacet(facet);
        added.kind = kind;
        added.active = true;
        record.info.facets.push_back(std::move(added));
    }

    record.facets.push_back(ActiveFacet{.kind = kind, .id = id, .vtable = facet.vtable});
    return record.facets.back();
}

// --------------------------------------------------------------- host thunks

void PluginManager::Impl::buildApi() {
    api = sweeppp_host_api_t{};
    api.struct_size = sizeof(sweeppp_host_api_t);
    api.abi_version = SWEEPPP_PLUGIN_ABI_VERSION;
    api.host_version = borrow(versionString());
    api.host = this;

    api.log = [](void* host, sweeppp_log_level_t level, sweeppp_str_t category,
                 sweeppp_str_t message) {
        (void)host;
        const std::string_view name = view(category);
        Log::emit(plugin_abi::fromAbiLevel(level), name.empty() ? kLogCategory : name,
                  owned(message));
    };

    api.path = [](void* host, sweeppp_str_t pluginId, sweeppp_path_kind_t kind) -> sweeppp_str_t {
        auto* self = static_cast<Impl*>(host);
        const std::string id = owned(pluginId);

        switch (kind) {
        case SWEEPPP_PATH_CONFIG_DIR:
            return scratch(self->root().string());
        case SWEEPPP_PATH_RESOURCES_DIR:
            return scratch(Paths::instance().resourcesDir().string());
        case SWEEPPP_PATH_SESSIONS_DIR:
            return scratch(Paths::instance().sessionsDir().string());
        case SWEEPPP_PATH_PLUGIN_SETTINGS:
            return scratch((self->settingsDir() / std::format("{}.toml", id)).string());
        case SWEEPPP_PATH_PLUGIN_DATA_DIR: {
            const fs::path directory = self->settingsDir() / id;
            std::error_code ec;
            fs::create_directories(directory, ec);
            return scratch(directory.string());
        }
        case SWEEPPP_PATH_PLUGIN_BINARY_DIR: {
            const Record* record = self->find(id);
            return scratch(record != nullptr ? record->info.path.parent_path().string()
                                             : std::string{});
        }
        case SWEEPPP_PATH_FORCE_INT32:
            break;
        }
        return scratch({});
    };

    api.register_sdr_driver = [](void* host, sweeppp_str_t pluginId, const sweeppp_facet_t* facet,
                                 void* instance) -> sweeppp_plugin_status_t {
        auto* self = static_cast<Impl*>(host);
        Record* record = self->find(view(pluginId));
        if (record == nullptr || facet == nullptr || facet->vtable == nullptr) {
            return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
        }

        const std::string driver = owned(facet->id);
        if (driver.empty()) {
            return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
        }

        // A vtable that cannot open anything is refused here rather than
        // registered and found wanting later: the driver would appear in
        // `info`, in the device chip and in a profile, and fail only when an
        // operator picked it.
        if (const std::string wrong = plugin_facets::validateSdrFactory(
                static_cast<const sweeppp_sdr_factory_vtable_t*>(facet->vtable));
            !wrong.empty()) {
            if (PluginFacetInfo* listed =
                    self->facetInfo(*record, PluginFacetKind::SdrDevice, driver)) {
                listed->failureReason = wrong;
            }
            logWarn(kLogCategory, "'{}' offered an unusable SDR driver '{}': {}", view(pluginId),
                    driver, wrong);
            return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
        }

        // Refused rather than replaced. `registerFactory` replaces by driver
        // name, so two plugins both claiming "hackrf" would silently be
        // last-wins -- and a driver quietly becoming a different driver is not
        // something an operator can debug.
        if (SdrDeviceManager::instance().hasDriver(driver)) {
            if (PluginFacetInfo* listed =
                    self->facetInfo(*record, PluginFacetKind::SdrDevice, driver)) {
                listed->failureReason =
                    std::format("the driver name '{}' is already taken", driver);
            }
            return SWEEPPP_PLUGIN_ERR_ALREADY_EXISTS;
        }

        auto shim = std::make_unique<plugin_facets::SdrFactory>(
            driver, owned(facet->name),
            static_cast<const sweeppp_sdr_factory_vtable_t*>(facet->vtable), instance);
        plugin_facets::SdrFactory* observer = shim.get();
        SdrDeviceManager::instance().registerFactory(std::move(shim));

        ActiveFacet& active = self->adopt(*record, *facet, PluginFacetKind::SdrDevice);
        active.instance = instance;
        active.sdrFactory = observer;
        return SWEEPPP_PLUGIN_OK;
    };

    api.unregister_sdr_driver = [](void* host, sweeppp_str_t pluginId,
                                   sweeppp_str_t driver) -> sweeppp_plugin_status_t {
        auto* self = static_cast<Impl*>(host);
        Record* record = self->find(view(pluginId));
        if (record == nullptr) {
            return SWEEPPP_PLUGIN_ERR_NOT_FOUND;
        }

        const std::string name = owned(driver);
        auto withdrawn = SdrDeviceManager::instance().unregisterFactory(name);
        if (!withdrawn) {
            return plugin_abi::toStatus(withdrawn.error().code());
        }

        std::erase_if(record->facets, [&name](const ActiveFacet& facet) {
            return facet.kind == PluginFacetKind::SdrDevice && facet.id == name;
        });
        if (PluginFacetInfo* listed = self->facetInfo(*record, PluginFacetKind::SdrDevice, name)) {
            listed->active = false;
        }
        return SWEEPPP_PLUGIN_OK;
    };

    api.register_rf_path = [](void* host, sweeppp_str_t pluginId, const sweeppp_facet_t* facet,
                              void* instance) -> sweeppp_plugin_status_t {
        auto* self = static_cast<Impl*>(host);
        Record* record = self->find(view(pluginId));
        if (record == nullptr || facet == nullptr || facet->vtable == nullptr) {
            return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
        }

        const std::string driver = owned(facet->id);
        if (driver.empty()) {
            return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
        }

        if (const std::string wrong = plugin_facets::validateRfPathFactory(
                static_cast<const sweeppp_rf_path_factory_vtable_t*>(facet->vtable));
            !wrong.empty()) {
            if (PluginFacetInfo* listed =
                    self->facetInfo(*record, PluginFacetKind::RfPath, driver)) {
                listed->failureReason = wrong;
            }
            logWarn(kLogCategory, "'{}' offered an unusable RF path driver '{}': {}",
                    view(pluginId), driver, wrong);
            return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
        }

        if (RfPathManager::instance().hasDriver(driver)) {
            if (PluginFacetInfo* listed =
                    self->facetInfo(*record, PluginFacetKind::RfPath, driver)) {
                listed->failureReason =
                    std::format("the driver name '{}' is already taken", driver);
            }
            return SWEEPPP_PLUGIN_ERR_ALREADY_EXISTS;
        }

        auto shim = std::make_unique<plugin_facets::RfPathFactory>(
            driver, owned(facet->name),
            static_cast<const sweeppp_rf_path_factory_vtable_t*>(facet->vtable), instance);
        plugin_facets::RfPathFactory* observer = shim.get();
        RfPathManager::instance().registerFactory(std::move(shim));

        ActiveFacet& active = self->adopt(*record, *facet, PluginFacetKind::RfPath);
        active.instance = instance;
        active.rfPathFactory = observer;
        return SWEEPPP_PLUGIN_OK;
    };

    api.unregister_rf_path = [](void* host, sweeppp_str_t pluginId,
                                sweeppp_str_t driver) -> sweeppp_plugin_status_t {
        auto* self = static_cast<Impl*>(host);
        Record* record = self->find(view(pluginId));
        if (record == nullptr) {
            return SWEEPPP_PLUGIN_ERR_NOT_FOUND;
        }

        const std::string name = owned(driver);
        auto withdrawn = RfPathManager::instance().unregisterFactory(name);
        if (!withdrawn) {
            return plugin_abi::toStatus(withdrawn.error().code());
        }

        std::erase_if(record->facets, [&name](const ActiveFacet& facet) {
            return facet.kind == PluginFacetKind::RfPath && facet.id == name;
        });
        if (PluginFacetInfo* listed = self->facetInfo(*record, PluginFacetKind::RfPath, name)) {
            listed->active = false;
        }
        return SWEEPPP_PLUGIN_OK;
    };

    api.register_fft_backend = [](void* host, sweeppp_str_t pluginId, const sweeppp_facet_t* facet,
                                  void* instance) -> sweeppp_plugin_status_t {
        auto* self = static_cast<Impl*>(host);
        Record* record = self->find(view(pluginId));
        if (record == nullptr || facet == nullptr || facet->vtable == nullptr) {
            return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
        }

        const std::string name = owned(facet->id);
        if (name.empty()) {
            return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
        }
        if (FftBackendManager::instance().info(name).has_value()) {
            if (PluginFacetInfo* listed =
                    self->facetInfo(*record, PluginFacetKind::FftBackend, name)) {
                listed->failureReason = std::format("the backend name '{}' is already taken", name);
            }
            return SWEEPPP_PLUGIN_ERR_ALREADY_EXISTS;
        }

        const auto* vtable = static_cast<const sweeppp_fft_backend_vtable_t*>(facet->vtable);
        auto capabilities = plugin_facets::readCapabilities(vtable, instance);
        if (!capabilities) {
            if (PluginFacetInfo* listed =
                    self->facetInfo(*record, PluginFacetKind::FftBackend, name)) {
                listed->failureReason = capabilities.error().message();
            }
            return plugin_abi::toStatus(capabilities.error().code());
        }

        FftBackendInfo info;
        info.name = name;
        info.displayName = owned(facet->name);
        info.description = owned(facet->description);
        info.capabilities = *capabilities;

        // Lazily, through the registry's existing factory shape: merely
        // listing a backend must not instantiate whatever library is behind
        // it, and a backend that fails here is demoted with the reason rather
        // than failing the caller repeatedly.
        FftBackendManager::instance().registerBackend(
            std::move(info),
            [vtable, instance, name, display = owned(facet->name),
             caps = *capabilities]() -> Result<std::unique_ptr<IFftBackend>> {
                return std::make_unique<plugin_facets::FftBackend>(name, display, vtable, instance,
                                                                   caps);
            });

        ActiveFacet& active = self->adopt(*record, *facet, PluginFacetKind::FftBackend);
        active.instance = instance;
        return SWEEPPP_PLUGIN_OK;
    };

    api.unregister_fft_backend = [](void* host, sweeppp_str_t pluginId,
                                    sweeppp_str_t backend) -> sweeppp_plugin_status_t {
        auto* self = static_cast<Impl*>(host);
        Record* record = self->find(view(pluginId));
        if (record == nullptr) {
            return SWEEPPP_PLUGIN_ERR_NOT_FOUND;
        }

        const std::string name = owned(backend);
        auto withdrawn = FftBackendManager::instance().unregisterBackend(name);
        if (!withdrawn) {
            return plugin_abi::toStatus(withdrawn.error().code());
        }

        std::erase_if(record->facets, [&name](const ActiveFacet& facet) {
            return facet.kind == PluginFacetKind::FftBackend && facet.id == name;
        });
        if (PluginFacetInfo* listed = self->facetInfo(*record, PluginFacetKind::FftBackend, name)) {
            listed->active = false;
        }
        return SWEEPPP_PLUGIN_OK;
    };

    api.register_contributor = [](void* host, sweeppp_str_t pluginId, const sweeppp_facet_t* facet,
                                  void* instance) -> sweeppp_plugin_status_t {
        auto* self = static_cast<Impl*>(host);
        Record* record = self->find(view(pluginId));
        if (record == nullptr || facet == nullptr || facet->vtable == nullptr) {
            return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
        }

        ActiveFacet& active = self->adopt(*record, *facet, PluginFacetKind::Contributor);
        active.instance = instance;
        return SWEEPPP_PLUGIN_OK;
    };

    api.unregister_contributor = [](void* host, sweeppp_str_t pluginId,
                                    sweeppp_str_t id) -> sweeppp_plugin_status_t {
        auto* self = static_cast<Impl*>(host);
        Record* record = self->find(view(pluginId));
        if (record == nullptr) {
            return SWEEPPP_PLUGIN_ERR_NOT_FOUND;
        }

        const std::string name = owned(id);
        std::erase_if(record->facets, [&name](const ActiveFacet& facet) {
            return facet.kind == PluginFacetKind::Contributor && facet.id == name;
        });
        if (PluginFacetInfo* listed =
                self->facetInfo(*record, PluginFacetKind::Contributor, name)) {
            listed->active = false;
        }
        return SWEEPPP_PLUGIN_OK;
    };

    api.register_ui_extension = [](void* host, sweeppp_str_t pluginId, const sweeppp_facet_t* facet,
                                   void* instance) -> sweeppp_plugin_status_t {
        auto* self = static_cast<Impl*>(host);
        Record* record = self->find(view(pluginId));
        if (record == nullptr || facet == nullptr || facet->vtable == nullptr) {
            return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
        }

        if (!self->hasImGui) {
            // Listed, not failed. A UI facet in a host with no UI is a fact
            // worth reporting -- the CLI loading a plugin that draws is
            // working exactly as intended.
            if (PluginFacetInfo* listed =
                    self->facetInfo(*record, PluginFacetKind::UiExtension, owned(facet->id))) {
                listed->failureReason = "this build has no user interface";
            }
            return SWEEPPP_PLUGIN_ERR_UNSUPPORTED;
        }

        ActiveFacet& active = self->adopt(*record, *facet, PluginFacetKind::UiExtension);
        active.instance = instance;
        return SWEEPPP_PLUGIN_OK;
    };

    api.unregister_ui_extension = [](void* host, sweeppp_str_t pluginId,
                                     sweeppp_str_t id) -> sweeppp_plugin_status_t {
        auto* self = static_cast<Impl*>(host);
        Record* record = self->find(view(pluginId));
        if (record == nullptr) {
            return SWEEPPP_PLUGIN_ERR_NOT_FOUND;
        }

        const std::string name = owned(id);
        std::erase_if(record->facets, [&name](const ActiveFacet& facet) {
            return facet.kind == PluginFacetKind::UiExtension && facet.id == name;
        });
        if (PluginFacetInfo* listed =
                self->facetInfo(*record, PluginFacetKind::UiExtension, name)) {
            listed->active = false;
        }
        return SWEEPPP_PLUGIN_OK;
    };

    api.frame_subscribe = [](void* host, sweeppp_str_t pluginId, const sweeppp_facet_t* facet,
                             void* instance) -> std::uint64_t {
        auto* self = static_cast<Impl*>(host);
        Record* record = self->find(view(pluginId));
        if (record == nullptr || facet == nullptr || facet->vtable == nullptr) {
            return 0;
        }
        if (self->frameBus == nullptr) {
            if (PluginFacetInfo* listed =
                    self->facetInfo(*record, PluginFacetKind::FrameProcessor, owned(facet->id))) {
                listed->failureReason = "this host publishes no frames";
            }
            return 0;
        }

        ActiveFacet& active = self->adopt(*record, *facet, PluginFacetKind::FrameProcessor);
        active.instance = instance;
        active.frameProcessor = std::make_unique<plugin_facets::FrameProcessor>(
            std::format("{}:{}", record->info.id, active.id),
            static_cast<const sweeppp_frame_processor_vtable_t*>(facet->vtable), instance);
        active.subscription = self->frameBus->subscribe(active.frameProcessor.get());
        return active.subscription;
    };

    api.frame_unsubscribe = [](void* host, std::uint64_t subscription) {
        auto* self = static_cast<Impl*>(host);
        if (subscription == 0 || self->frameBus == nullptr) {
            return;
        }

        for (const std::unique_ptr<Record>& record : self->records) {
            const auto match =
                std::ranges::find_if(record->facets, [subscription](const ActiveFacet& facet) {
                    return facet.subscription == subscription;
                });
            if (match == record->facets.end()) {
                continue;
            }

            // Unsubscribe first, then drop the consumer: `FrameBus::publish`
            // holds the list while it fans out, so unsubscribe returning is
            // what proves no callback is in flight.
            self->frameBus->unsubscribe(subscription);
            if (PluginFacetInfo* listed =
                    self->facetInfo(*record, PluginFacetKind::FrameProcessor, match->id)) {
                listed->active = false;
            }
            record->facets.erase(match);
            return;
        }
    };

    api.publish_event = [](void* host, sweeppp_str_t pluginId,
                           const sweeppp_event_t* event) -> sweeppp_plugin_status_t {
        auto* self = static_cast<Impl*>(host);
        if (event == nullptr || event->struct_size == 0) {
            return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
        }

        // Its own namespace only. A plugin that could publish `sweeppp.retune`
        // could make a session say the radio moved when it did not, with
        // nothing downstream able to tell the difference.
        const std::string_view id = view(pluginId);
        const std::string_view type = view(event->type);
        if (id.empty() || !type.starts_with(id) || type.size() <= id.size() ||
            type[id.size()] != '.') {
            logWarn(kLogCategory, "{} tried to publish '{}', which is not its own", id, type);
            return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
        }

        // The source is the host's to state, not the publisher's: an event
        // that could name someone else as its origin is worse than one with no
        // origin at all.
        sweeppp_event_t stamped = *event;
        stamped.struct_size = sizeof(sweeppp_event_t);
        stamped.source = pluginId;
        self->dispatchEvent(stamped);
        return SWEEPPP_PLUGIN_OK;
    };

    api.subscribe_event = [](void* host, sweeppp_str_t type, sweeppp_event_fn_t callback,
                             void* user) -> std::uint64_t {
        auto* self = static_cast<Impl*>(host);
        if (callback == nullptr) {
            return 0;
        }
        const std::uint64_t id = self->nextEventId++;
        self->eventSubscriptions.push_back(
            EventSubscription{.id = id, .type = owned(type), .callback = callback, .user = user});
        return id;
    };

    api.unsubscribe_event = [](void* host, std::uint64_t subscription) {
        auto* self = static_cast<Impl*>(host);
        std::erase_if(self->eventSubscriptions, [subscription](const EventSubscription& entry) {
            return entry.id == subscription;
        });
    };

    api.report_facet = [](void* host, sweeppp_str_t pluginId, sweeppp_facet_kind_t kind,
                          sweeppp_str_t facetId, sweeppp_str_t reason) {
        auto* self = static_cast<Impl*>(host);
        Record* record = self->find(view(pluginId));
        if (record == nullptr) {
            return;
        }
        if (PluginFacetInfo* listed = self->facetInfo(*record, toFacetKind(kind), owned(facetId))) {
            listed->failureReason = owned(reason);
            if (!listed->failureReason.empty()) {
                listed->active = false;
            }
        }
    };

    api.profile_keys = [](void* host, sweeppp_str_t* out, std::uint32_t capacity) -> std::uint32_t {
        auto* self = static_cast<Impl*>(host);
        const auto total = static_cast<std::uint32_t>(self->profile.size());

        if (out != nullptr) {
            for (std::uint32_t i = 0; i < std::min(total, capacity); ++i) {
                out[i] = borrow(self->profile[i].first);
            }
        }
        return total;
    };

    api.profile_get = [](void* host, sweeppp_str_t key,
                         sweeppp_value_t* out) -> sweeppp_plugin_status_t {
        auto* self = static_cast<Impl*>(host);
        if (out == nullptr) {
            return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
        }

        const std::string_view wanted = view(key);
        const auto found = std::ranges::lower_bound(
            self->profile, wanted, {},
            [](const std::pair<std::string, SdrValue>& entry) -> std::string_view {
                return entry.first;
            });
        if (found == self->profile.end() || found->first != wanted) {
            out->struct_size = sizeof(sweeppp_value_t);
            out->type = SWEEPPP_VALUE_ABSENT;
            return SWEEPPP_PLUGIN_ERR_NOT_FOUND;
        }

        *out = sweeppp_value_t{};
        out->struct_size = sizeof(sweeppp_value_t);
        std::visit(
            [out](const auto& held) {
                using Held = std::decay_t<decltype(held)>;
                if constexpr (std::is_same_v<Held, bool>) {
                    out->type = SWEEPPP_VALUE_BOOL;
                    out->boolean = held ? 1 : 0;
                } else if constexpr (std::is_same_v<Held, std::int64_t>) {
                    out->type = SWEEPPP_VALUE_INT;
                    out->integer = held;
                } else if constexpr (std::is_same_v<Held, double>) {
                    out->type = SWEEPPP_VALUE_FLOAT;
                    out->number = held;
                } else {
                    out->type = SWEEPPP_VALUE_STRING;
                    out->text = borrow(held);
                }
            },
            found->second);
        return SWEEPPP_PLUGIN_OK;
    };

    api.session_open = [](void* host) -> std::int32_t {
        return static_cast<Impl*>(host)->recorder != nullptr ? 1 : 0;
    };

    api.session_write_record = [](void* host, sweeppp_str_t pluginId, sweeppp_str_t recordName,
                                  std::uint32_t schemaVersion, const void* body,
                                  std::size_t bodySize) -> sweeppp_plugin_status_t {
        auto* self = static_cast<Impl*>(host);
        if (self->recorder == nullptr) {
            return SWEEPPP_PLUGIN_ERR_UNAVAILABLE;
        }
        if (bodySize > 0 && body == nullptr) {
            return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
        }

        const auto* bytes = static_cast<const std::byte*>(body);
        self->recorder->recordPluginData(owned(pluginId), owned(recordName), schemaVersion,
                                         monotonicNs(),
                                         std::vector<std::byte>(bytes, bytes + bodySize));
        return SWEEPPP_PLUGIN_OK;
    };

    api.session_write_event = [](void* host, sweeppp_str_t pluginId, sweeppp_str_t eventName,
                                 const sweeppp_field_t* fields,
                                 std::uint32_t fieldCount) -> sweeppp_plugin_status_t {
        auto* self = static_cast<Impl*>(host);
        if (self->recorder == nullptr) {
            return SWEEPPP_PLUGIN_ERR_UNAVAILABLE;
        }

        session::Metadata metadata;
        plugin_abi::forEachElement(fields, fieldCount, [&metadata](const sweeppp_field_t& field) {
            if (!SWEEPPP_ABI_HAS(&field, value)) {
                return;
            }
            std::string key = owned(field.key);
            switch (field.value.type) {
            case SWEEPPP_VALUE_STRING:
                metadata.setString(std::move(key), owned(field.value.text));
                break;
            case SWEEPPP_VALUE_INT:
                metadata.setInt(std::move(key), field.value.integer);
                break;
            case SWEEPPP_VALUE_FLOAT:
                metadata.setFloat(std::move(key), field.value.number);
                break;
            case SWEEPPP_VALUE_BOOL:
                metadata.setBool(std::move(key), field.value.boolean != 0);
                break;
            case SWEEPPP_VALUE_ABSENT:
            case SWEEPPP_VALUE_TYPE_FORCE_INT32:
                break;
            }
        });

        self->recorder->recordEvent(session::SessionEvent::of(
            session::SessionEvent::Kind::Plugin, monotonicNs(), wallClockNs(),
            session::PluginEventData{.pluginId = owned(pluginId),
                                     .eventName = owned(eventName),
                                     .fields = std::move(metadata)}));
        return SWEEPPP_PLUGIN_OK;
    };

    // The chrome. Always wired, never null: the hooks behind them are what a
    // headless host leaves unset, so "this host paints nothing" is an answer
    // the plugin reads rather than a pointer it has to test.
    api.contributions_shown = [](void* host, sweeppp_contribution_type_t type) -> std::int32_t {
        auto* self = static_cast<Impl*>(host);
        return self->chrome.contributionsShown && self->chrome.contributionsShown(type) ? 1 : 0;
    };

    api.set_contributions_shown = [](void* host, sweeppp_contribution_type_t type,
                                     std::int32_t shown) {
        auto* self = static_cast<Impl*>(host);
        if (self->chrome.setContributionsShown) {
            self->chrome.setContributionsShown(type, shown != 0);
        }
    };

    api.icons_available = [](void* host) -> std::int32_t {
        auto* self = static_cast<Impl*>(host);
        return self->chrome.iconsAvailable && self->chrome.iconsAvailable() ? 1 : 0;
    };

    api.save_file_dialog = [](void* host, sweeppp_str_t title, sweeppp_str_t suggestedName,
                              sweeppp_str_t extension) -> sweeppp_str_t {
        auto* self = static_cast<Impl*>(host);
        if (!self->chrome.saveFile) {
            return borrow({});
        }
        return scratch(self->chrome.saveFile(view(title), view(suggestedName), view(extension)));
    };

    api.imgui = hasImGui ? &imgui : nullptr;
}

void PluginManager::Impl::dispatchEvent(const sweeppp_event_t& event) {
    // Copied before dispatch: a listener may unsubscribe from inside its own
    // callback, and iterating the live list would then walk a vector that has
    // moved underneath it.
    const std::string_view type = view(event.type);

    std::vector<EventSubscription> targets;
    for (const EventSubscription& subscription : eventSubscriptions) {
        if (subscription.type.empty() || subscription.type == type) {
            targets.push_back(subscription);
        }
    }

    for (const EventSubscription& subscription : targets) {
        try {
            if (subscription.hostCallback) {
                subscription.hostCallback(event);
            } else if (subscription.callback != nullptr) {
                subscription.callback(subscription.user, &event);
            }
        } catch (const std::exception& error) {
            logError(kLogCategory, "a handler for '{}' threw: {}", type, error.what());
        } catch (...) {
            logError(kLogCategory, "a handler for '{}' threw", type);
        }
    }
}

// ----------------------------------------------------------------- loading

void PluginManager::Impl::load(const fs::path& path) {
    PluginInfo info;
    info.path = path;

    const auto listFailure = [this, &info](std::string reason) {
        info.failureReason = std::move(reason);
        logWarn(kLogCategory, "{}: {}", info.path.string(), info.failureReason);
        auto record = std::make_unique<Record>();
        record->info = std::move(info);
        records.push_back(std::move(record));
    };

    auto library = DynamicLibrary::open(path);
    if (!library) {
        listFailure(library.error().message());
        return;
    }

    auto versionSymbol = library->symbol(SWEEPPP_PLUGIN_ABI_VERSION_SYMBOL);
    if (!versionSymbol) {
        listFailure(std::format("not a Sweep++ plugin: no {}", SWEEPPP_PLUGIN_ABI_VERSION_SYMBOL));
        return;
    }

    // The cheap probe first, and this is why it exists as a separate symbol: a
    // plugin built against a later ABI can be *reported* as such instead of
    // merely refusing, so the operator learns which half to update.
    const auto abiVersion = reinterpret_cast<sweeppp_plugin_abi_version_fn>(*versionSymbol)();
    info.abiVersion = abiVersion;

    if (abiVersion != SWEEPPP_PLUGIN_ABI_VERSION) {
        listFailure(
            std::format("plugin ABI {}, host ABI {}", abiVersion, SWEEPPP_PLUGIN_ABI_VERSION));
        return;
    }

    auto querySymbol = library->symbol(SWEEPPP_PLUGIN_QUERY_SYMBOL);
    if (!querySymbol) {
        listFailure(std::format("no {}", SWEEPPP_PLUGIN_QUERY_SYMBOL));
        return;
    }

    const sweeppp_plugin_desc_t* desc =
        reinterpret_cast<sweeppp_plugin_query_fn>(*querySymbol)(SWEEPPP_PLUGIN_ABI_VERSION);
    if (desc == nullptr) {
        listFailure(std::format("the plugin declined host ABI {}", SWEEPPP_PLUGIN_ABI_VERSION));
        return;
    }
    if (desc->struct_size == 0) {
        listFailure("the plugin returned a descriptor of size zero");
        return;
    }

    const sweeppp_manifest_t& manifest = desc->manifest;
    info.id = SWEEPPP_ABI_HAS(&manifest, id) ? owned(manifest.id) : std::string{};
    if (info.id.empty()) {
        listFailure("the manifest carries no id");
        return;
    }
    if (info.id.size() > SWEEPPP_MAX_PLUGIN_ID_BYTES) {
        // The cap is the container format's, not a preference: a plugin
        // writing records into a session is identified by this string on disk,
        // so an id accepted here and refused there would work until it
        // recorded.
        listFailure(std::format("the id is {} bytes; the limit is {}", info.id.size(),
                                SWEEPPP_MAX_PLUGIN_ID_BYTES));
        return;
    }

    if (const Record* existing = find(info.id); existing != nullptr) {
        listFailure(std::format("id '{}' is already provided by {}", info.id,
                                existing->info.path.string()));
        return;
    }

    if (SWEEPPP_ABI_HAS(&manifest, version)) {
        info.version = owned(manifest.version);
    }
    if (SWEEPPP_ABI_HAS(&manifest, name)) {
        info.name = owned(manifest.name);
    }
    if (SWEEPPP_ABI_HAS(&manifest, description)) {
        info.description = owned(manifest.description);
    }
    if (SWEEPPP_ABI_HAS(&manifest, min_host_version)) {
        info.minHostVersion = owned(manifest.min_host_version);
    }
    if (SWEEPPP_ABI_HAS(&manifest, requires_restart_to_disable)) {
        info.requiresRestart = manifest.requires_restart_to_disable != 0;
    }
    if (info.name.empty()) {
        info.name = info.id;
    }

    readAuthors(manifest, info);
    readLinks(manifest, info);
    readDependencies(manifest, info);

    if (!info.minHostVersion.empty() && compareVersions(versionString(), info.minHostVersion) < 0) {
        listFailure(std::format("needs Sweep++ {} or newer; this is {}",
                                displayVersion(info.minHostVersion),
                                displayVersion(versionString())));
        return;
    }

    if (SWEEPPP_ABI_HAS(desc, facet_count)) {
        plugin_abi::forEachElement(
            desc->facets, desc->facet_count,
            [&info](const sweeppp_facet_t& facet) { info.facets.push_back(readFacet(facet)); });
    }

    info.loaded = true;
    info.enabled = enablement.isEnabled(info.id);

    auto record = std::make_unique<Record>();
    record->info = std::move(info);
    record->library = std::move(*library);
    record->desc = desc;
    records.push_back(std::move(record));

    logInfo(kLogCategory, "found {} {} ({})", records.back()->info.id,
            displayVersion(records.back()->info.version), path.string());
}

void PluginManager::Impl::scan(const std::vector<fs::path>& binaries) {
    for (const fs::path& binary : binaries) {
        const bool known =
            std::ranges::any_of(records, [&binary](const std::unique_ptr<Record>& record) {
                return record->info.path == binary;
            });
        if (!known) {
            load(binary);
        }
    }

    // Enablement is read before the manifests, so a plugin's `enabled` is set
    // as it is loaded; a rescan still has to re-apply it to what was already
    // there.
    for (const std::unique_ptr<Record>& record : records) {
        if (record->info.loaded) {
            record->info.enabled = enablement.isEnabled(record->info.id);
        }
    }

    activateEnabled();
}

void PluginManager::Impl::activateEnabled() {
    // A fixpoint rather than a topological sort: dependency graphs here are
    // tiny, and a pass that activates nothing is the same answer a cycle
    // detector would give -- with the reason already attached to each plugin
    // that could not go.
    bool progressed = true;
    while (progressed) {
        progressed = false;
        for (const std::unique_ptr<Record>& record : records) {
            if (!record->info.loaded || !record->info.enabled || record->info.active) {
                continue;
            }
            if (!record->info.failureReason.empty()) {
                continue;
            }

            bool waiting = false;
            for (const PluginDependency& dependency : record->info.dependencies) {
                const Record* provider = find(dependency.pluginId);
                if (provider != nullptr && provider->info.active) {
                    if (!dependency.minVersion.empty() &&
                        compareVersions(provider->info.version, dependency.minVersion) < 0) {
                        record->info.failureReason =
                            std::format("needs {} {} or newer, found {}", dependency.pluginId,
                                        displayVersion(dependency.minVersion),
                                        displayVersion(provider->info.version));
                        break;
                    }
                    if (!dependency.maxVersion.empty() &&
                        compareVersions(provider->info.version, dependency.maxVersion) > 0) {
                        record->info.failureReason =
                            std::format("needs {} {} or older, found {}", dependency.pluginId,
                                        displayVersion(dependency.maxVersion),
                                        displayVersion(provider->info.version));
                        break;
                    }
                    continue;
                }
                if (dependency.optional) {
                    continue;
                }
                if (provider != nullptr && provider->info.enabled &&
                    provider->info.failureReason.empty()) {
                    // It is going to be activated on a later pass; wait.
                    waiting = true;
                    break;
                }
                record->info.failureReason =
                    std::format("needs {}, which is {}", dependency.pluginId,
                                provider == nullptr ? "not installed" : "not available");
                break;
            }

            if (waiting || !record->info.failureReason.empty()) {
                continue;
            }

            activate(*record);
            progressed = true;
        }
    }

    // Anything still waiting is waiting on something that will never arrive.
    for (const std::unique_ptr<Record>& record : records) {
        if (record->info.loaded && record->info.enabled && !record->info.active &&
            record->info.failureReason.empty()) {
            record->info.failureReason = "a dependency could not be resolved";
        }
    }
}

void PluginManager::Impl::activate(Record& record) {
    if (record.desc == nullptr || !SWEEPPP_ABI_HAS(record.desc, activate) ||
        record.desc->activate == nullptr) {
        // A plugin with no activate is legal and does nothing but exist. It is
        // listed as active because it is: everything it declared, it declared.
        record.info.active = true;
        return;
    }

    std::error_code ec;
    fs::create_directories(settingsDir(), ec);

    void* instance = nullptr;
    sweeppp_plugin_status_t status = SWEEPPP_PLUGIN_ERR_UNKNOWN;
    try {
        status = record.desc->activate(&api, &instance);
    } catch (const std::exception& error) {
        record.info.failureReason = std::format("activation threw: {}", error.what());
        logError(kLogCategory, "{}: {}", record.info.id, record.info.failureReason);
        return;
    } catch (...) {
        record.info.failureReason = "activation threw";
        logError(kLogCategory, "{}: {}", record.info.id, record.info.failureReason);
        return;
    }

    if (status != SWEEPPP_PLUGIN_OK) {
        record.info.failureReason = std::format("activation failed: {}", statusName(status));
        logWarn(kLogCategory, "{}: {}", record.info.id, record.info.failureReason);
        return;
    }

    record.instance = instance;
    record.info.active = true;
    record.info.failureReason.clear();
    logInfo(kLogCategory, "activated {} with {} facet(s)", record.info.id, record.facets.size());
}

std::string PluginManager::Impl::withdrawalBlocker(Record& record) const {
    if (!record.info.active) {
        return {};
    }

    for (const ActiveFacet& facet : record.facets) {
        switch (facet.kind) {
        case PluginFacetKind::FftBackend:
            if (FftBackendManager::instance().isAcquired(facet.id)) {
                return std::format("the FFT backend '{}' is in use by the pipeline", facet.id);
            }
            break;
        case PluginFacetKind::SdrDevice:
            if (facet.sdrFactory != nullptr) {
                if (std::string blocker = facet.sdrFactory->withdrawalBlocker(); !blocker.empty()) {
                    return std::format("the driver '{}': {}", facet.id, blocker);
                }
            }
            break;
        case PluginFacetKind::RfPath:
            if (facet.rfPathFactory != nullptr) {
                if (std::string blocker = facet.rfPathFactory->withdrawalBlocker();
                    !blocker.empty()) {
                    return std::format("the driver '{}': {}", facet.id, blocker);
                }
            }
            break;
        case PluginFacetKind::FrameProcessor:
        case PluginFacetKind::Contributor:
        case PluginFacetKind::UiExtension:
            break;
        }
    }

    return {};
}

Status PluginManager::Impl::deactivate(Record& record) {
    if (!record.info.active) {
        return ok();
    }

    if (record.info.requiresRestart) {
        return fail(ErrorCode::Unavailable, "{} declares that it cannot be withdrawn live",
                    record.info.id);
    }

    // Asked before anything is withdrawn, so a refusal leaves the plugin whole
    // rather than half-registered -- half a plugin is a state nothing else in
    // this system knows how to describe.
    if (const std::string blocker = withdrawalBlocker(record); !blocker.empty()) {
        return fail(ErrorCode::Unavailable, "{}", blocker);
    }

    for (ActiveFacet& facet : record.facets) {
        switch (facet.kind) {
        case PluginFacetKind::FrameProcessor:
            if (frameBus != nullptr && facet.subscription != 0) {
                frameBus->unsubscribe(facet.subscription);
            }
            facet.frameProcessor.reset();
            break;
        case PluginFacetKind::SdrDevice:
            if (auto withdrawn = SdrDeviceManager::instance().unregisterFactory(facet.id);
                !withdrawn) {
                logWarn(kLogCategory, "{}", withdrawn.error().describe());
            }
            facet.sdrFactory = nullptr;
            break;
        case PluginFacetKind::FftBackend:
            if (auto withdrawn = FftBackendManager::instance().unregisterBackend(facet.id);
                !withdrawn) {
                logWarn(kLogCategory, "{}", withdrawn.error().describe());
            }
            break;
        case PluginFacetKind::RfPath:
            if (auto withdrawn = RfPathManager::instance().unregisterFactory(facet.id);
                !withdrawn) {
                logWarn(kLogCategory, "{}", withdrawn.error().describe());
            }
            facet.rfPathFactory = nullptr;
            break;
        case PluginFacetKind::Contributor:
        case PluginFacetKind::UiExtension:
            break;
        }
    }

    record.facets.clear();
    for (PluginFacetInfo& listed : record.info.facets) {
        listed.active = false;
    }

    if (record.desc != nullptr && SWEEPPP_ABI_HAS(record.desc, deactivate) &&
        record.desc->deactivate != nullptr) {
        try {
            record.desc->deactivate(record.instance);
        } catch (const std::exception& error) {
            logError(kLogCategory, "{}: deactivation threw: {}", record.info.id, error.what());
        } catch (...) {
            logError(kLogCategory, "{}: deactivation threw", record.info.id);
        }
    }

    record.instance = nullptr;
    record.info.active = false;
    return ok();
}

// ---------------------------------------------------------------- manager

PluginManager::PluginManager() : m_impl(std::make_unique<Impl>()) {
    m_impl->buildApi();
}

PluginManager::~PluginManager() = default;

PluginManager& PluginManager::instance() {
    static PluginManager manager;
    return manager;
}

void PluginManager::setConfigRoot(fs::path root) {
    const std::lock_guard lock(m_mutex);
    m_impl->configRoot = std::move(root);
}

void PluginManager::setFrameBus(FrameBus* bus) noexcept {
    const std::lock_guard lock(m_mutex);
    m_impl->frameBus = bus;
}

void PluginManager::setImGuiBinding(const sweeppp_imgui_binding_t& binding) {
    const std::lock_guard lock(m_mutex);
    m_impl->imgui = binding;
    m_impl->hasImGui = true;
    m_impl->api.imgui = &m_impl->imgui;
}

void PluginManager::setChrome(Chrome chrome) {
    const std::lock_guard lock(m_mutex);
    m_impl->chrome = std::move(chrome);
}

void PluginManager::setProfile(const Profile& profile) {
    const std::lock_guard lock(m_mutex);

    m_impl->profile = profile.entries();
    std::ranges::sort(m_impl->profile, {}, &std::pair<std::string, SdrValue>::first);
}

void PluginManager::setSessionRecorder(session::SessionRecorder* recorder) noexcept {
    const std::lock_guard lock(m_mutex);
    m_impl->recorder = recorder;
}

namespace {

// fromAbiValue and toAbiValue moved to PluginAbiSupport: the SDR facet needs
// the same two conversions for its parameters, and a second copy of the type
// mapping is a second place for it to be wrong.
using plugin_abi::fromAbiValue;
using plugin_abi::toAbiValue;

/// What a plugin's `profile_save` writes into, and what namespaces its keys.
struct ProfileSink {
    std::string prefix;
    std::vector<std::pair<std::string, SdrValue>>* out = nullptr;
};

/// What its `profile_load` reads out of, filtered to its own keys.
struct ProfileSource {
    std::string prefix;
    std::span<const std::pair<std::string, SdrValue>> values;
    /// Keys with the prefix stripped, held so the borrowed views handed across
    /// the ABI stay alive for the duration of the call.
    std::vector<std::string> shortKeys;
};

} // namespace

std::vector<std::pair<std::string, SdrValue>> PluginManager::collectProfileValues() {
    const std::lock_guard lock(m_mutex);

    std::vector<std::pair<std::string, SdrValue>> collected;

    for (const std::unique_ptr<Impl::Record>& record : m_impl->records) {
        if (!record->info.active || record->desc == nullptr) {
            continue;
        }
        if (!SWEEPPP_ABI_HAS(record->desc, profile_save) || record->desc->profile_save == nullptr) {
            continue;
        }

        ProfileSink sink{.prefix = std::format("plugins.{}.", record->info.id), .out = &collected};

        sweeppp_profile_writer_t writer{};
        writer.struct_size = sizeof(writer);
        writer.sink = &sink;
        writer.set = [](void* target, sweeppp_str_t key, const sweeppp_value_t* value) {
            auto* into = static_cast<ProfileSink*>(target);
            const std::string_view name = view(key);
            if (name.empty() || value == nullptr) {
                return;
            }
            if (auto held = fromAbiValue(*value)) {
                into->out->emplace_back(into->prefix + std::string(name), std::move(*held));
            }
        };

        try {
            record->desc->profile_save(record->instance, &writer);
        } catch (const std::exception& error) {
            logError(kLogCategory, "{}: contributing to the profile threw: {}", record->info.id,
                     error.what());
        } catch (...) {
            logError(kLogCategory, "{}: contributing to the profile threw", record->info.id);
        }
    }

    return collected;
}

void PluginManager::applyProfileValues(std::span<const std::pair<std::string, SdrValue>> values) {
    const std::lock_guard lock(m_mutex);

    for (const std::unique_ptr<Impl::Record>& record : m_impl->records) {
        if (!record->info.active || record->desc == nullptr) {
            continue;
        }
        if (!SWEEPPP_ABI_HAS(record->desc, profile_load) || record->desc->profile_load == nullptr) {
            continue;
        }

        ProfileSource source{.prefix = std::format("plugins.{}.", record->info.id),
                             .values = values,
                             .shortKeys = {}};
        for (const auto& [key, value] : values) {
            if (key.starts_with(source.prefix)) {
                source.shortKeys.push_back(key.substr(source.prefix.size()));
            }
        }
        if (source.shortKeys.empty()) {
            // A profile written before this plugin existed. Not calling is the
            // right answer: the plugin keeps whatever its own settings file
            // gave it, rather than being handed a set of absent values it
            // would have to distinguish from real ones.
            continue;
        }

        sweeppp_profile_reader_t reader{};
        reader.struct_size = sizeof(reader);
        reader.source = &source;
        reader.get = [](void* from, sweeppp_str_t key,
                        sweeppp_value_t* out) -> sweeppp_plugin_status_t {
            const auto* profile = static_cast<const ProfileSource*>(from);
            if (out == nullptr) {
                return SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
            }

            const std::string wanted = profile->prefix + std::string(view(key));
            const auto found = std::ranges::find_if(
                profile->values, [&wanted](const auto& entry) { return entry.first == wanted; });
            if (found == profile->values.end()) {
                out->struct_size = sizeof(sweeppp_value_t);
                out->type = SWEEPPP_VALUE_ABSENT;
                return SWEEPPP_PLUGIN_ERR_NOT_FOUND;
            }

            toAbiValue(found->second, *out);
            return SWEEPPP_PLUGIN_OK;
        };
        reader.keys = [](void* from, sweeppp_str_t* out, std::uint32_t capacity) -> std::uint32_t {
            const auto* profile = static_cast<const ProfileSource*>(from);
            const auto total = static_cast<std::uint32_t>(profile->shortKeys.size());
            if (out != nullptr) {
                for (std::uint32_t i = 0; i < std::min(total, capacity); ++i) {
                    out[i] = borrow(profile->shortKeys[i]);
                }
            }
            return total;
        };

        try {
            record->desc->profile_load(record->instance, &reader);
        } catch (const std::exception& error) {
            logError(kLogCategory, "{}: reading the profile threw: {}", record->info.id,
                     error.what());
        } catch (...) {
            logError(kLogCategory, "{}: reading the profile threw", record->info.id);
        }
    }
}

void PluginManager::discover() {
    const std::vector<PluginSearchEntry> entries = pluginSearchPath();

    const std::lock_guard lock(m_mutex);
    m_impl->enablement = PluginEnablement::load(m_impl->enablementFile());
    m_impl->scan(findPluginBinaries(std::span<const PluginSearchEntry>(entries)));
}

void PluginManager::discover(std::span<const fs::path> directories) {
    const std::lock_guard lock(m_mutex);

    m_impl->enablement = PluginEnablement::load(m_impl->enablementFile());
    m_impl->scan(findPluginBinaries(directories));
}

std::vector<PluginInfo> PluginManager::enumerate() const {
    const std::lock_guard lock(m_mutex);

    std::vector<PluginInfo> result;
    result.reserve(m_impl->records.size());
    for (const std::unique_ptr<Impl::Record>& record : m_impl->records) {
        result.push_back(record->info);
    }

    // Working ones first, then by name. Failures stay in the list -- they are
    // the entries with something to say.
    std::ranges::stable_sort(result, [](const PluginInfo& a, const PluginInfo& b) {
        if (a.loaded != b.loaded) {
            return a.loaded;
        }
        if (a.active != b.active) {
            return a.active;
        }
        return a.name < b.name;
    });
    return result;
}

Result<PluginInfo> PluginManager::info(std::string_view id) const {
    const std::lock_guard lock(m_mutex);
    const Impl::Record* record = m_impl->find(id);
    if (record == nullptr) {
        return fail<PluginInfo>(ErrorCode::NotFound, "no plugin with id '{}'", id);
    }
    return record->info;
}

Status PluginManager::setEnabled(std::string_view id, bool enabled) {
    const std::lock_guard lock(m_mutex);

    Impl::Record* record = m_impl->find(id);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "no plugin with id '{}'", id);
    }

    // The preference is recorded whatever happens next, so a withdrawal that
    // needs a restart does what was asked when the restart comes.
    record->info.enabled = enabled;
    m_impl->enablement.setEnabled(id, enabled);
    if (auto saved = m_impl->enablement.save(); !saved) {
        logWarn(kLogCategory, "{}", saved.error().describe());
    }

    if (enabled) {
        if (record->info.active) {
            return ok();
        }
        record->info.failureReason.clear();
        m_impl->activateEnabled();
        if (!record->info.active) {
            return fail(ErrorCode::Unavailable, "{}",
                        record->info.failureReason.empty() ? "activation failed"
                                                           : record->info.failureReason);
        }
        return ok();
    }

    if (auto withdrawn = m_impl->deactivate(*record); !withdrawn) {
        record->info.requiresRestart = true;
        return withdrawn;
    }
    return ok();
}

bool PluginManager::isEnabled(std::string_view id) const {
    const std::lock_guard lock(m_mutex);
    const Impl::Record* record = m_impl->find(id);
    return record != nullptr && record->info.enabled;
}

// ----------------------------------------------------------------- events

std::uint64_t PluginManager::subscribeRaw(std::string type, RawEventCallback callback) {
    const std::lock_guard lock(m_mutex);
    if (!callback) {
        return 0;
    }

    const std::uint64_t id = m_impl->nextEventId++;
    m_impl->eventSubscriptions.push_back(Impl::EventSubscription{
        .id = id, .type = std::move(type), .hostCallback = std::move(callback)});
    return id;
}

void PluginManager::unsubscribeEvent(std::uint64_t subscription) {
    const std::lock_guard lock(m_mutex);
    std::erase_if(m_impl->eventSubscriptions, [subscription](const Impl::EventSubscription& entry) {
        return entry.id == subscription;
    });
}

void PluginManager::attachEvents(EventBus& bus) {
    const std::lock_guard lock(m_mutex);

    // Field for field with the bus event each comes from, the same way the
    // session recorder maps them. Nothing is summarised on the way across: a
    // plugin reading a live retune and one reading it back out of a recording
    // are looking at the same numbers.
    //
    // The payloads are built on the stack of the publishing thread and handed
    // over as borrowed views, so nothing here allocates on the sweep thread --
    // which publishes a retune per step, thousands of times a second.

    bus.subscribe<RetuneEvent>([this](const RetuneEvent& from) {
        sweeppp_retune_event_t payload{};
        payload.center_hz = from.centerHz;
        payload.step_index = from.stepIndex;
        m_impl->publishHostEvent(SWEEPPP_EVENT_RETUNE, from.monotonicNs, payload);
    });

    bus.subscribe<ParameterChangedEvent>([this](const ParameterChangedEvent& from) {
        sweeppp_parameter_changed_event_t payload{};
        payload.key = borrow(from.key);
        payload.value = borrow(from.value);
        payload.grid_affecting = from.gridAffecting ? 1 : 0;
        payload.calibration_affecting = from.calibrationAffecting ? 1 : 0;
        m_impl->publishHostEvent(SWEEPPP_EVENT_PARAMETER_CHANGED, from.monotonicNs, payload);
    });

    bus.subscribe<SweepPassEvent>([this](const SweepPassEvent& from) {
        sweeppp_sweep_pass_event_t payload{};
        payload.pass_id = from.passId;
        payload.start_hz = from.startHz;
        payload.stop_hz = from.stopHz;
        payload.duration_seconds = from.durationSeconds;
        m_impl->publishHostEvent(SWEEPPP_EVENT_SWEEP_PASS, from.monotonicNs, payload);
    });

    bus.subscribe<MarkerEvent>([this](const MarkerEvent& from) {
        sweeppp_marker_event_t payload{};
        payload.label = borrow(from.label);
        payload.frequency_hz = from.frequencyHz;
        payload.level_dbm = from.levelDbm;
        m_impl->publishHostEvent(SWEEPPP_EVENT_MARKER, from.monotonicNs, payload);
    });

    bus.subscribe<AnnotationEvent>([this](const AnnotationEvent& from) {
        sweeppp_annotation_event_t payload{};
        payload.text = borrow(from.text);
        payload.start_hz = from.startHz;
        payload.stop_hz = from.stopHz;
        m_impl->publishHostEvent(SWEEPPP_EVENT_ANNOTATION, from.monotonicNs, payload);
    });

    bus.subscribe<ThrottleChangedEvent>([this](const ThrottleChangedEvent& from) {
        sweeppp_throttle_changed_event_t payload{};
        payload.reason = borrow(from.reason);
        payload.processed_fraction = from.processedFraction;
        m_impl->publishHostEvent(SWEEPPP_EVENT_THROTTLE_CHANGED, from.monotonicNs, payload);
    });

    bus.subscribe<DeviceErrorEvent>([this](const DeviceErrorEvent& from) {
        sweeppp_device_error_event_t payload{};
        payload.device_id = borrow(from.deviceId);
        payload.message = borrow(from.message);
        m_impl->publishHostEvent(SWEEPPP_EVENT_DEVICE_ERROR, from.monotonicNs, payload);
    });

    bus.subscribe<DeviceOpenedEvent>([this](const DeviceOpenedEvent& from) {
        sweeppp_device_opened_event_t payload{};
        payload.device_id = borrow(from.deviceId);
        payload.label = borrow(from.label);
        payload.serial = borrow(from.serial);
        m_impl->publishHostEvent(SWEEPPP_EVENT_DEVICE_OPENED, from.monotonicNs, payload);
    });

    bus.subscribe<DeviceClosedEvent>([this](const DeviceClosedEvent& from) {
        sweeppp_device_closed_event_t payload{};
        payload.device_id = borrow(from.deviceId);
        payload.reason = borrow(from.reason);
        m_impl->publishHostEvent(SWEEPPP_EVENT_DEVICE_CLOSED, from.monotonicNs, payload);
    });
}

// -------------------------------------------------------------- contributors

std::vector<Contribution> PluginManager::contributionsAt(double hz) const {
    const std::lock_guard lock(m_mutex);

    std::vector<Contribution> found;

    for (const auto& [record, facet] : m_impl->rankedContributors()) {
        const auto* vtable = static_cast<const sweeppp_contributor_vtable_t*>(facet->vtable);
        if (!SWEEPPP_ABI_HAS(vtable, contributions_at) || vtable->contributions_at == nullptr) {
            continue;
        }

        m_impl->guarded(*record, *facet, [&] {
            collectContributions(
                record->info,
                [&](sweeppp_contribution_t* out, std::uint32_t capacity) {
                    return vtable->contributions_at(facet->instance, hz, out, capacity);
                },
                found);
        });
    }

    return found;
}

std::vector<Contribution> PluginManager::contributionsIn(double fromHz, double toHz) const {
    const std::lock_guard lock(m_mutex);

    std::vector<Contribution> found;

    for (const auto& [record, facet] : m_impl->rankedContributors()) {
        const auto* vtable = static_cast<const sweeppp_contributor_vtable_t*>(facet->vtable);
        if (!SWEEPPP_ABI_HAS(vtable, contributions_in) || vtable->contributions_in == nullptr) {
            continue;
        }

        const std::size_t before = found.size();
        m_impl->guarded(*record, *facet, [&] {
            collectContributions(
                record->info,
                [&](sweeppp_contribution_t* out, std::uint32_t capacity) {
                    return vtable->contributions_in(facet->instance, fromHz, toHz, out, capacity);
                },
                found);
        });

        // Stamped here rather than in `readContribution`, which is handed one
        // entry and knows nothing about the facet it came from. Marked on the
        // entries this contributor just added, so a caller that draws can skip
        // them and a caller that only wants to know what is there cannot tell
        // the difference.
        if (markedNotHostRendered(vtable)) {
            for (std::size_t i = before; i < found.size(); ++i) {
                found[i].hostRendered = false;
            }
        }
    }

    return found;
}

bool PluginManager::hideContribution(const Contribution& entry) {
    const std::lock_guard lock(m_mutex);

    Impl::Record* record = m_impl->find(entry.pluginId);
    if (record == nullptr) {
        return false;
    }

    // Handed back the way it arrived, so the plugin can match on whatever
    // identifies one of its own entries -- the name alone is not always enough
    // and the host has no business deciding what is.
    sweeppp_contribution_t which{};
    which.struct_size = sizeof(which);
    which.type = abiContributionType(entry.type);
    which.name = borrow(entry.name);
    which.description = borrow(entry.description);
    which.category = borrow(entry.category);
    which.start_hz = entry.startHz;
    which.stop_hz = entry.stopHz;
    which.color[0] = entry.color[0];
    which.color[1] = entry.color[1];
    which.color[2] = entry.color[2];
    which.color[3] = entry.color[3];

    bool hidden = false;
    for (Impl::ActiveFacet& facet : record->facets) {
        if (facet.kind != PluginFacetKind::Contributor || facet.broken) {
            continue;
        }
        const auto* vtable = static_cast<const sweeppp_contributor_vtable_t*>(facet.vtable);
        if (!SWEEPPP_ABI_HAS(vtable, hide) || vtable->hide == nullptr) {
            continue;
        }

        m_impl->guarded(*record, facet, [&] {
            hidden = vtable->hide(facet.instance, &which) == SWEEPPP_PLUGIN_OK || hidden;
        });
    }
    return hidden;
}

std::vector<std::string> PluginManager::contributorOrder() const {
    const std::lock_guard lock(m_mutex);

    std::vector<std::string> ids;
    for (const auto& [record, facet] : m_impl->rankedContributors()) {
        // One entry per plugin, not per facet: the order is stored as plugin
        // ids, so two contributor facets in one plugin cannot be ranked apart
        // and listing the id twice would offer a choice that does nothing.
        if (std::ranges::find(ids, record->info.id) == ids.end()) {
            ids.push_back(record->info.id);
        }
    }
    return ids;
}

Status PluginManager::setContributorOrder(std::span<const std::string> ids) {
    const std::lock_guard lock(m_mutex);

    const std::vector<std::string>& stored = m_impl->enablement.contributorOrder();

    // The caller names the contributors it can see. Ids it did not mention --
    // a plugin removed from disk since the order was written -- keep the slot
    // they held, so restoring the file restores the position too rather than
    // dropping it to the end of the list.
    std::vector<std::string> merged;
    merged.reserve(stored.size() + ids.size());

    std::size_t next = 0;
    for (const std::string& id : stored) {
        if (std::ranges::find(ids, id) == ids.end()) {
            merged.push_back(id);
        } else if (next < ids.size()) {
            merged.push_back(ids[next++]);
        }
    }
    for (; next < ids.size(); ++next) {
        if (std::ranges::find(merged, ids[next]) == merged.end()) {
            merged.push_back(ids[next]);
        }
    }

    m_impl->enablement.setContributorOrder(std::move(merged));
    return m_impl->enablement.save();
}

bool PluginManager::contributorShown(std::string_view id) const {
    const std::lock_guard lock(m_mutex);
    return m_impl->enablement.contributorShown(id);
}

Status PluginManager::setContributorShown(std::string_view id, bool shown) {
    const std::lock_guard lock(m_mutex);
    m_impl->enablement.setContributorShown(id, shown);
    return m_impl->enablement.save();
}

std::vector<std::pair<std::string, std::string>>
PluginManager::datasets(std::string_view id) const {
    const std::lock_guard lock(m_mutex);

    std::vector<std::pair<std::string, std::string>> found;

    for (const auto& [record, facet] : m_impl->rankedContributors()) {
        if (record->info.id != id) {
            continue;
        }
        const auto* vtable = static_cast<const sweeppp_contributor_vtable_t*>(facet->vtable);
        if (!SWEEPPP_ABI_HAS(vtable, dataset_name) || vtable->dataset_count == nullptr ||
            vtable->dataset_name == nullptr) {
            continue;
        }

        m_impl->guarded(*record, *facet, [&] {
            const std::uint32_t count =
                std::min(vtable->dataset_count(facet->instance), kMaxContributions);
            const bool described = SWEEPPP_ABI_HAS(vtable, dataset_description) &&
                                   vtable->dataset_description != nullptr;

            for (std::uint32_t i = 0; i < count; ++i) {
                found.emplace_back(owned(vtable->dataset_name(facet->instance, i)),
                                   described
                                       ? owned(vtable->dataset_description(facet->instance, i))
                                       : std::string{});
            }
        });
    }

    return found;
}

std::uint32_t PluginManager::activeDataset(std::string_view id) const {
    const std::lock_guard lock(m_mutex);

    std::uint32_t active = 0;
    for (const auto& [record, facet] : m_impl->rankedContributors()) {
        if (record->info.id != id) {
            continue;
        }
        const auto* vtable = static_cast<const sweeppp_contributor_vtable_t*>(facet->vtable);
        if (!SWEEPPP_ABI_HAS(vtable, active_dataset) || vtable->active_dataset == nullptr) {
            continue;
        }

        m_impl->guarded(*record, *facet, [&] { active = vtable->active_dataset(facet->instance); });
        break;
    }
    return active;
}

Status PluginManager::selectDataset(std::string_view id, std::uint32_t index) {
    const std::lock_guard lock(m_mutex);

    for (const auto& [record, facet] : m_impl->rankedContributors()) {
        if (record->info.id != id) {
            continue;
        }
        const auto* vtable = static_cast<const sweeppp_contributor_vtable_t*>(facet->vtable);
        if (!SWEEPPP_ABI_HAS(vtable, select_dataset) || vtable->select_dataset == nullptr) {
            return fail(ErrorCode::Unavailable, "'{}' has no dataset to choose", id);
        }

        sweeppp_plugin_status_t status = SWEEPPP_PLUGIN_ERR_UNAVAILABLE;
        m_impl->guarded(*record, *facet,
                        [&] { status = vtable->select_dataset(facet->instance, index); });
        if (status != SWEEPPP_PLUGIN_OK) {
            return fail(ErrorCode::InvalidArgument, "'{}' refused dataset {}: {}", id, index,
                        statusName(status));
        }
        return ok();
    }

    return fail(ErrorCode::NotFound, "no contributor with id '{}'", id);
}

std::vector<std::string> PluginManager::providedDatasets() const {
    const std::lock_guard lock(m_mutex);

    std::vector<std::string> names;

    for (const auto& [record, facet] : m_impl->rankedContributors()) {
        const auto* vtable = static_cast<const sweeppp_contributor_vtable_t*>(facet->vtable);
        if (!SWEEPPP_ABI_HAS(vtable, dataset_name) || vtable->dataset_count == nullptr ||
            vtable->dataset_name == nullptr) {
            continue;
        }

        m_impl->guarded(*record, *facet, [&] {
            const std::uint32_t count = vtable->dataset_count(facet->instance);
            for (std::uint32_t i = 0; i < std::min(count, kMaxContributions); ++i) {
                names.push_back(std::format("{}: {}", record->info.id,
                                            owned(vtable->dataset_name(facet->instance, i))));
            }
        });
    }

    return names;
}

// ------------------------------------------------------------------- ui

void PluginManager::drawOverlay(const sweeppp_plot_context_t& context) {
    const std::lock_guard lock(m_mutex);

    for (const std::unique_ptr<Impl::Record>& record : m_impl->records) {
        for (Impl::ActiveFacet& facet : record->facets) {
            if (facet.kind != PluginFacetKind::UiExtension || facet.broken) {
                continue;
            }
            const auto* vtable = static_cast<const sweeppp_ui_vtable_t*>(facet.vtable);
            if (!SWEEPPP_ABI_HAS(vtable, draw_overlay) || vtable->draw_overlay == nullptr) {
                continue;
            }
            if ((vtable->spots & SWEEPPP_UI_SPOT_BIT(context.spot)) == 0) {
                continue;
            }
            if (vtable->layer != context.layer) {
                continue;
            }

            m_impl->guarded(*record, facet,
                            [&] { vtable->draw_overlay(facet.instance, &context); });
        }
    }
}

void PluginManager::drawSpot(sweeppp_ui_spot_t spot) {
    const std::lock_guard lock(m_mutex);

    for (const std::unique_ptr<Impl::Record>& record : m_impl->records) {
        for (Impl::ActiveFacet& facet : record->facets) {
            if (facet.kind != PluginFacetKind::UiExtension || facet.broken) {
                continue;
            }
            const auto* vtable = static_cast<const sweeppp_ui_vtable_t*>(facet.vtable);
            if ((vtable->spots & SWEEPPP_UI_SPOT_BIT(spot)) == 0) {
                continue;
            }

            // No PushID around these, and it is not an oversight: the bar
            // spots share the host window's id stack, so a plugin's widget
            // ids have to be unique on their own -- see the note in the ABI.
            // Scoping them here would mean linking ImGui into a library that
            // builds headless, which is a far larger price than a rule a
            // plugin follows by qualifying its own ids.
            if (spot == SWEEPPP_UI_SPOT_STATUS_CHIP && SWEEPPP_ABI_HAS(vtable, draw_status_chip) &&
                vtable->draw_status_chip != nullptr) {
                m_impl->guarded(*record, facet, [&] { vtable->draw_status_chip(facet.instance); });
            } else if (spot == SWEEPPP_UI_SPOT_TOOLBAR && SWEEPPP_ABI_HAS(vtable, draw_toolbar) &&
                       vtable->draw_toolbar != nullptr) {
                m_impl->guarded(*record, facet, [&] { vtable->draw_toolbar(facet.instance); });
            } else if (spot == SWEEPPP_UI_SPOT_WINDOW && SWEEPPP_ABI_HAS(vtable, draw_window) &&
                       vtable->draw_window != nullptr) {
                m_impl->guarded(*record, facet, [&] { vtable->draw_window(facet.instance); });
            } else if (spot == SWEEPPP_UI_SPOT_STATUS_ACTION &&
                       SWEEPPP_ABI_HAS(vtable, draw_status_action) &&
                       vtable->draw_status_action != nullptr) {
                m_impl->guarded(*record, facet,
                                [&] { vtable->draw_status_action(facet.instance); });
            }
        }
    }
}

void PluginManager::openToolbarPanel(sweeppp_contribution_type_t type) {
    const std::lock_guard lock(m_mutex);

    for (const std::unique_ptr<Impl::Record>& record : m_impl->records) {
        for (Impl::ActiveFacet& facet : record->facets) {
            if (facet.kind != PluginFacetKind::UiExtension || facet.broken) {
                continue;
            }
            const auto* vtable = static_cast<const sweeppp_ui_vtable_t*>(facet.vtable);
            if (!SWEEPPP_ABI_HAS(vtable, open_panel) || vtable->open_panel == nullptr) {
                continue;
            }
            if (vtable->toolbar_governs != type) {
                continue;
            }
            m_impl->guarded(*record, facet, [&] { vtable->open_panel(facet.instance); });
        }
    }
}

bool PluginManager::drawSettings(std::string_view id) {
    const std::lock_guard lock(m_mutex);

    Impl::Record* record = m_impl->find(id);
    if (record == nullptr) {
        return false;
    }

    bool drew = false;
    for (Impl::ActiveFacet& facet : record->facets) {
        if (facet.kind != PluginFacetKind::UiExtension || facet.broken) {
            continue;
        }
        const auto* vtable = static_cast<const sweeppp_ui_vtable_t*>(facet.vtable);
        if (!SWEEPPP_ABI_HAS(vtable, draw_settings) || vtable->draw_settings == nullptr) {
            continue;
        }
        if ((vtable->spots & SWEEPPP_UI_SPOT_BIT(SWEEPPP_UI_SPOT_SETTINGS)) == 0) {
            continue;
        }

        m_impl->guarded(*record, facet,
                        [&] { drew = vtable->draw_settings(facet.instance) != 0 || drew; });
    }
    return drew;
}

std::vector<std::pair<std::string, std::string>> PluginManager::settingsSections() const {
    const std::lock_guard lock(m_mutex);

    std::vector<std::pair<std::string, std::string>> sections;

    for (const std::unique_ptr<Impl::Record>& record : m_impl->records) {
        if (!record->info.active) {
            continue;
        }
        for (const Impl::ActiveFacet& facet : record->facets) {
            if (facet.kind != PluginFacetKind::UiExtension || facet.broken) {
                continue;
            }
            const auto* vtable = static_cast<const sweeppp_ui_vtable_t*>(facet.vtable);
            if ((vtable->spots & SWEEPPP_UI_SPOT_BIT(SWEEPPP_UI_SPOT_SETTINGS)) == 0) {
                continue;
            }
            if (!SWEEPPP_ABI_HAS(vtable, draw_settings) || vtable->draw_settings == nullptr) {
                continue;
            }
            sections.emplace_back(record->info.id, record->info.name);
            break;
        }
    }

    return sections;
}

bool PluginManager::hasUiSpot(sweeppp_ui_spot_t spot) const {
    const std::lock_guard lock(m_mutex);

    for (const std::unique_ptr<Impl::Record>& record : m_impl->records) {
        for (const Impl::ActiveFacet& facet : record->facets) {
            if (facet.kind != PluginFacetKind::UiExtension || facet.broken) {
                continue;
            }
            const auto* vtable = static_cast<const sweeppp_ui_vtable_t*>(facet.vtable);
            if ((vtable->spots & SWEEPPP_UI_SPOT_BIT(spot)) != 0) {
                return true;
            }
        }
    }
    return false;
}

void PluginManager::shutdown() {
    const std::lock_guard lock(m_mutex);

    // Reverse load order, so a plugin that depends on another is stopped
    // before the thing it depends on.
    for (auto record = m_impl->records.rbegin(); record != m_impl->records.rend(); ++record) {
        if (!(*record)->info.active) {
            continue;
        }

        // Not `deactivate()`: this is shutdown, and a facet refusing to be
        // withdrawn is no reason to leave a plugin's threads running into
        // static destruction. The facets go regardless -- but they do go,
        // rather than being forgotten about, because a registry left holding a
        // shim whose plugin has been deactivated is worse than either.
        (*record)->info.requiresRestart = false;
        for (Impl::ActiveFacet& facet : (*record)->facets) {
            switch (facet.kind) {
            case PluginFacetKind::FrameProcessor:
                if (m_impl->frameBus != nullptr && facet.subscription != 0) {
                    m_impl->frameBus->unsubscribe(facet.subscription);
                }
                facet.frameProcessor.reset();
                break;
            case PluginFacetKind::SdrDevice:
                if (auto withdrawn = SdrDeviceManager::instance().unregisterFactory(facet.id);
                    !withdrawn) {
                    logWarn(kLogCategory, "{}", withdrawn.error().describe());
                }
                break;
            case PluginFacetKind::FftBackend:
                if (auto withdrawn = FftBackendManager::instance().unregisterBackend(facet.id);
                    !withdrawn) {
                    // Not a warning, unlike the driver above. `acquire()` has
                    // no counterpart -- the pipeline holds the backend for the
                    // life of the process -- so a backend that was ever used
                    // ALWAYS refuses here, and warning about it would put a
                    // line the operator cannot act on at the end of every
                    // clean run. On the disable path the same refusal is the
                    // "restart needed" marker and is reported.
                    logDebug(kLogCategory, "{}", withdrawn.error().describe());
                }
                break;
            case PluginFacetKind::RfPath:
                if (auto withdrawn = RfPathManager::instance().unregisterFactory(facet.id);
                    !withdrawn) {
                    logWarn(kLogCategory, "{}", withdrawn.error().describe());
                }
                break;
            case PluginFacetKind::Contributor:
            case PluginFacetKind::UiExtension:
                break;
            }
        }
        (*record)->facets.clear();

        if ((*record)->desc != nullptr && (*record)->desc->deactivate != nullptr) {
            try {
                (*record)->desc->deactivate((*record)->instance);
            } catch (...) {
                logError(kLogCategory, "{}: deactivation threw", (*record)->info.id);
            }
        }
        (*record)->instance = nullptr;
        (*record)->info.active = false;
    }

    m_impl->eventSubscriptions.clear();
    m_impl->frameBus = nullptr;
    m_impl->recorder = nullptr;

    // Dropped with the bus and the recorder, and for the same reason: the
    // hooks close over the window's own state, and this runs from that
    // window's destructor.
    m_impl->chrome = Chrome{};
}

void PluginManager::reset() {
    shutdown();

    const std::lock_guard lock(m_mutex);
    m_impl->records.clear();
    m_impl->enablement = PluginEnablement{};
}

} // namespace sweeppp
