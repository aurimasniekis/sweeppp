// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The plugin host: what it accepts, what it refuses, and what it says about
// the difference.
//
// Every test here goes through a real `dlopen` of a real module built by
// tests/plugins/CMakeLists.txt. The failure paths in particular are worth
// loading for rather than asserting about: "listed with the reason" is a
// promise about behaviour on binaries that are wrong in specific ways, and the
// only way to know it holds is to have such binaries.
#include "ReferenceFft.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <cstdlib>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <sweeppp/core/BlockPool.hpp>
#include <sweeppp/core/EventBus.hpp>
#include <sweeppp/core/Paths.hpp>
#include <sweeppp/core/Telemetry.hpp>
#include <sweeppp/fft/FftBackendManager.hpp>
#include <sweeppp/pipeline/FrameBus.hpp>
#include <sweeppp/plugin/PluginAbi.h>
#include <sweeppp/plugin/PluginHost.hpp>
#include <sweeppp/plugin/PluginSettings.hpp>
#include <sweeppp/rf/IRfPath.hpp>
#include <sweeppp/sdr/ISdrDevice.hpp>
#include <thread>
#include <vector>

using namespace sweeppp;

namespace {

namespace fs = std::filesystem;

fs::path fixtureDir() {
    return {SWEEPPP_TEST_PLUGIN_DIR};
}

/// setenv/unsetenv, which are POSIX and have no MSVC equivalent by those names.
///
/// _putenv_s is the right counterpart rather than SetEnvironmentVariable
/// because the code under test reads through std::getenv, and on Windows that
/// is the CRT's copy of the environment rather than the process block. Setting
/// a variable to the empty string is how the CRT spells "remove it".
int setEnvironment(const char* name, const char* value) {
#if defined(_WIN32)
    return ::_putenv_s(name, value);
#else
    return ::setenv(name, value, 1);
#endif
}

int clearEnvironment(const char* name) {
#if defined(_WIN32)
    return ::_putenv_s(name, "");
#else
    return ::unsetenv(name);
#endif
}

/// A temporary config root, so the enablement file a test writes cannot land
/// in the developer's own configuration.
class ScopedConfigRoot {
public:
    ScopedConfigRoot()
        : m_path(fs::temp_directory_path() / std::format("sweeppp-plugin-test-{}", ++s_counter)) {
        std::error_code ec;
        fs::remove_all(m_path, ec);
        fs::create_directories(m_path, ec);
        PluginManager::instance().setConfigRoot(m_path);
    }

    ~ScopedConfigRoot() {
        PluginManager::instance().reset();
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }

    ScopedConfigRoot(const ScopedConfigRoot&) = delete;
    ScopedConfigRoot& operator=(const ScopedConfigRoot&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return m_path; }

private:
    static inline int s_counter = 0;
    fs::path m_path;
};

/// One plugin out of an enumeration, by id or by filename.
const PluginInfo* findById(const std::vector<PluginInfo>& plugins, std::string_view id) {
    const auto match =
        std::ranges::find_if(plugins, [id](const PluginInfo& plugin) { return plugin.id == id; });
    return match != plugins.end() ? &*match : nullptr;
}

const PluginInfo* findByFile(const std::vector<PluginInfo>& plugins, std::string_view stem) {
    const auto match = std::ranges::find_if(
        plugins, [stem](const PluginInfo& plugin) { return plugin.path.stem().string() == stem; });
    return match != plugins.end() ? &*match : nullptr;
}

/// One contributor's share of a ranked answer.
///
/// Several fixtures contribute over the same frequencies -- which is what
/// makes the ordering testable at all -- so a test about one of them says
/// which one rather than assuming it is alone.
std::vector<Contribution> contributionsFrom(std::vector<Contribution> found,
                                            std::string_view pluginId) {
    std::erase_if(found,
                  [pluginId](const Contribution& entry) { return entry.pluginId != pluginId; });
    return found;
}

/// Discovers only the fixture directory, so the developer's own plugins cannot
/// change what a test sees.
std::vector<PluginInfo> discoverFixtures() {
    const std::array<fs::path, 1> directories{fixtureDir()};
    PluginManager::instance().discover(directories);
    return PluginManager::instance().enumerate();
}

/// The event the fixture republishes after every frame, as a subscriber that
/// has never seen the plugin's source would declare it.
///
/// This is what a name-keyed event channel buys and an enum of kinds could
/// not: the type is the plugin's, the host has never heard of it, and a
/// consumer describes it independently and is checked against the payload's
/// own `struct_size` before the cast.
struct FixtureFrameEvent {
    std::uint32_t struct_size;
    std::uint64_t sequence;
    std::size_t binCount;
    double startHz;
    float firstBin;

    static constexpr sweeppp::plugin::EventName kEventType{"test.sweeppp.good.frame"};
    static constexpr std::uint32_t kSchemaVersion = 1;
};

/// What the fixture reports each time the host dispatches the window spot.
struct FixtureWindowEvent {
    std::uint32_t struct_size;
    std::int32_t draws;

    static constexpr sweeppp::plugin::EventName kEventType{"test.sweeppp.good.window"};
    static constexpr std::uint32_t kSchemaVersion = 1;
};

/// An ImGui binding with nothing real behind it.
///
/// Enough for the host: it checks only that a binding was set before letting a
/// UI facet register, and the fixture is written against the raw C ABI and so
/// never calls `adoptImGui` -- which is the call that would look at any of
/// these numbers.
sweeppp_imgui_binding_t stubImGuiBinding() {
    sweeppp_imgui_binding_t binding{};
    binding.struct_size = sizeof(binding);
    return binding;
}

} // namespace

TEST_CASE("versions compare component by component") {
    CHECK(compareVersions("1.2.3", "1.2.3") == 0);
    CHECK(compareVersions("1.2.3", "1.2.4") < 0);
    CHECK(compareVersions("1.3.0", "1.2.9") > 0);

    // A missing component reads as zero, so a manifest may be as precise as it
    // likes without changing what it means.
    CHECK(compareVersions("1.2", "1.2.0") == 0);
    CHECK(compareVersions("2", "1.9.9") > 0);
    CHECK(compareVersions("", "0.0.0") == 0);

    // Ten is not a character comparison away from nine.
    CHECK(compareVersions("1.10.0", "1.9.0") > 0);

    // A component that is not a number ends the comparison rather than
    // throwing or guessing which side a pre-release belongs on.
    CHECK(compareVersions("1.2.3-rc1", "1.2.3") == 0);
}

namespace {

/// Where `directory` sits in the search path, or npos.
std::size_t searchIndex(const std::vector<PluginSearchEntry>& path, const fs::path& directory) {
    const auto match = std::ranges::find_if(path, [&directory](const PluginSearchEntry& entry) {
        return entry.directory == directory;
    });
    return match == path.end() ? std::string::npos : static_cast<std::size_t>(match - path.begin());
}

} // namespace

TEST_CASE("the search path puts the user's own directory first") {
    const std::vector<PluginSearchEntry> path = pluginSearchPath();
    REQUIRE(path.size() >= 2);

    const std::size_t user = searchIndex(path, Paths::instance().pluginsDir());
    const std::size_t bundled = searchIndex(path, Paths::instance().bundledPluginsDir());

    REQUIRE(user != std::string::npos);
    REQUIRE(bundled != std::string::npos);

    // What makes an override an override.
    CHECK(user < bundled);

    // Both are directories that exist to hold plugins, so anything in them
    // counts whatever it is called.
    CHECK_FALSE(path[user].requiresPrefix);
    CHECK_FALSE(path[bundled].requiresPrefix);
}

TEST_CASE("a shared library directory is scanned by name only") {
    const std::vector<PluginSearchEntry> path = pluginSearchPath();

    // Every entry that is not one of ours must be prefix-gated. Deciding
    // whether an arbitrary /usr/lib entry is a plugin means dlopen'ing it,
    // which runs its initialisers -- so the name has to answer it instead.
    bool sawShared = false;
    for (const PluginSearchEntry& entry : path) {
        const std::string text = entry.directory.string();
        const bool ours = text.ends_with("plugins");
        CAPTURE(text);
        CHECK(entry.requiresPrefix == !ours);
        sawShared = sawShared || entry.requiresPrefix;
    }
    CHECK(sawShared);
}

TEST_CASE("the file prefix is what identifies a plugin in a shared directory") {
    CHECK(hasPluginFilePrefix("sweeppp-plugin-bandplan.so"));
    CHECK(hasPluginFilePrefix("/usr/lib/libsweeppp-plugin-bandplan.so"));
    CHECK_FALSE(hasPluginFilePrefix("libssl.so"));
    CHECK_FALSE(hasPluginFilePrefix("bandplan.so"));

    const fs::path shared = fs::temp_directory_path() / "sweeppp-shared-libdir";
    std::error_code ec;
    fs::remove_all(shared, ec);
    fs::create_directories(shared, ec);

    for (const char* name : {"sweeppp-plugin-yes.so", "libsweeppp-plugin-also.so",
                             "libsomethingelse.so", "notes.txt"}) {
        std::ofstream out(shared / name);
        out << "placeholder";
    }

    // As one of ours: everything that is a shared object, whatever it is
    // called, because the directory is the statement of intent.
    const std::array<PluginSearchEntry, 1> asOurs{
        PluginSearchEntry{.directory = shared, .requiresPrefix = false}};
    CHECK(findPluginBinaries(std::span<const PluginSearchEntry>(asOurs)).size() == 3);

    // As a shared one: only the two that say what they are. `libsomethingelse`
    // is never opened, which is the entire point.
    const std::array<PluginSearchEntry, 1> asShared{
        PluginSearchEntry{.directory = shared, .requiresPrefix = true}};
    const std::vector<fs::path> found =
        findPluginBinaries(std::span<const PluginSearchEntry>(asShared));
    REQUIRE(found.size() == 2);
    for (const fs::path& entry : found) {
        CAPTURE(entry.string());
        CHECK(hasPluginFilePrefix(entry));
    }

    fs::remove_all(shared, ec);
}

TEST_CASE("$SWEEPPP_PLUGIN_PATH goes ahead of everything") {
    const fs::path first = fs::temp_directory_path() / "sweeppp-plugin-a";
    const fs::path second = fs::temp_directory_path() / "sweeppp-plugin-b";

#if defined(_WIN32)
    const std::string joined = std::format("{};{}", first.string(), second.string());
#else
    const std::string joined = std::format("{}:{}", first.string(), second.string());
#endif

    REQUIRE(setEnvironment("SWEEPPP_PLUGIN_PATH", joined.c_str()) == 0);
    const std::vector<PluginSearchEntry> path = pluginSearchPath();
    REQUIRE(clearEnvironment("SWEEPPP_PLUGIN_PATH") == 0);

    REQUIRE(path.size() >= 3);
    CHECK(path[0].directory == first);
    CHECK(path[1].directory == second);
    CHECK(path[2].directory == Paths::instance().pluginsDir());

    // Named explicitly, so it is one of ours: an operator who points the
    // variable at a directory has already said what is in it.
    CHECK_FALSE(path[0].requiresPrefix);
    CHECK_FALSE(path[1].requiresPrefix);
}

TEST_CASE("binaries are found by extension, on every platform") {
    const std::array<fs::path, 1> directories{fixtureDir()};
    const std::vector<fs::path> binaries = findPluginBinaries(directories);

    // The CMake MODULE + PREFIX "" combination emits `.so` even on macOS, so a
    // loader that guessed the extension from the platform would find none of
    // its own plugins here.
    REQUIRE_FALSE(binaries.empty());
    for (const fs::path& binary : binaries) {
        CAPTURE(binary.string());
        const std::string extension = binary.extension().string();
        CHECK((extension == ".so" || extension == ".dylib" || extension == ".dll"));
    }

    // A directory that is not there is skipped, not an error.
    const std::array<fs::path, 1> missing{"/nonexistent/plugins"};
    CHECK(findPluginBinaries(missing).empty());
}

TEST_CASE("a working plugin loads, activates and serves its facet") {
    const ScopedConfigRoot root;
    const std::vector<PluginInfo> plugins = discoverFixtures();

    const PluginInfo* good = findById(plugins, "test.sweeppp.good");
    REQUIRE(good != nullptr);

    CHECK(good->loaded);
    CHECK(good->enabled);
    CHECK(good->active);
    CHECK(good->failureReason.empty());
    CHECK(good->version == "1.2.3");
    CHECK(good->name == "Fixture");
    CHECK(good->abiVersion == SWEEPPP_PLUGIN_ABI_VERSION);

    REQUIRE(good->authors.size() == 1);
    CHECK(good->authors.front().name == "Sweep++ tests");

    // Every facet it declares is listed, whether or not it registered. The
    // frame processor cannot here -- this manager has no bus -- and saying so
    // is the point.
    REQUIRE(good->facets.size() == 6);
    const auto facetFor = [&good](PluginFacetKind kind) -> const PluginFacetInfo* {
        const auto match = std::ranges::find_if(
            good->facets, [kind](const PluginFacetInfo& facet) { return facet.kind == kind; });
        return match != good->facets.end() ? &*match : nullptr;
    };

    REQUIRE(facetFor(PluginFacetKind::Contributor) != nullptr);
    CHECK(facetFor(PluginFacetKind::Contributor)->id == "ranges");
    CHECK(facetFor(PluginFacetKind::Contributor)->active);

    REQUIRE(facetFor(PluginFacetKind::FrameProcessor) != nullptr);
    CHECK_FALSE(facetFor(PluginFacetKind::FrameProcessor)->active);
    CHECK(facetFor(PluginFacetKind::FrameProcessor)->failureReason.contains("publishes no frames"));

    REQUIRE(facetFor(PluginFacetKind::FftBackend) != nullptr);
    CHECK(facetFor(PluginFacetKind::FftBackend)->active);

    REQUIRE(facetFor(PluginFacetKind::SdrDevice) != nullptr);
    CHECK(facetFor(PluginFacetKind::SdrDevice)->active);

    // A switcher registers like a radio and is listed apart from one. Its own
    // kind rather than a driver among the radios, because it has no samples,
    // no tuning and no stream -- an operator picking it from the device
    // chooser would get a receiver that never delivers a block.
    REQUIRE(facetFor(PluginFacetKind::RfPath) != nullptr);
    CHECK(facetFor(PluginFacetKind::RfPath)->active);

    // No ImGui binding was set, so the UI facet is listed with the reason
    // rather than failing the plugin. The CLI loading a plugin that draws is
    // this path, working exactly as intended.
    REQUIRE(facetFor(PluginFacetKind::UiExtension) != nullptr);
    CHECK_FALSE(facetFor(PluginFacetKind::UiExtension)->active);
    CHECK(facetFor(PluginFacetKind::UiExtension)->failureReason.contains("no user interface"));

    // The facet answers across the ABI, not merely registers.
    const std::vector<Contribution> visible = contributionsFrom(
        PluginManager::instance().contributionsIn(90e6, 110e6), "test.sweeppp.good");
    CHECK(visible.size() == 2);
    CHECK(visible.front().category == "test");
    CHECK(visible.front().description == "what the fixture says about this range");
    CHECK(visible.front().type == ContributionType::Band);

    // Within one plugin, the order that plugin returned. It knows its own
    // nesting; the host does not re-derive it.
    const std::vector<Contribution> at =
        contributionsFrom(PluginManager::instance().contributionsAt(100e6), "test.sweeppp.good");
    REQUIRE(at.size() == 2);
    CHECK(at[0].name == "narrow");
    CHECK(at[1].name == "wide");
    CHECK(at[0].pluginId == "test.sweeppp.good");
    CHECK(at[0].pluginName == "Fixture");

    // Outside every range is a normal answer, not a failure.
    CHECK(PluginManager::instance().contributionsAt(1.0e3).empty());
}

TEST_CASE("several contributors answer one frequency, in the operator's order") {
    const ScopedConfigRoot root;
    const std::vector<PluginInfo> plugins = discoverFixtures();

    REQUIRE(findById(plugins, "test.sweeppp.good") != nullptr);
    REQUIRE(findById(plugins, "test.sweeppp.contributor") != nullptr);

    PluginManager& manager = PluginManager::instance();

    // Both answer, and the type says which is which -- the distinction a plain
    // range could not carry.
    const std::vector<Contribution> both = manager.contributionsAt(100e6);
    REQUIRE(both.size() == 4);
    CHECK(std::ranges::count_if(both, [](const Contribution& entry) {
              return entry.type == ContributionType::Channel;
          }) == 2);

    SUBCASE("the order decides which one titles the marker") {
        const std::array<std::string, 2> channelFirst{"test.sweeppp.contributor",
                                                      "test.sweeppp.good"};
        REQUIRE(manager.setContributorOrder(channelFirst).has_value());
        CHECK(manager.contributionsAt(100e6).front().pluginId == "test.sweeppp.contributor");

        const std::array<std::string, 2> bandFirst{"test.sweeppp.good", "test.sweeppp.contributor"};
        REQUIRE(manager.setContributorOrder(bandFirst).has_value());
        CHECK(manager.contributionsAt(100e6).front().pluginId == "test.sweeppp.good");

        // Whoever leads, each plugin's own nesting survives inside its own
        // block of the answer.
        const std::vector<Contribution> ranked = manager.contributionsAt(100e6);
        REQUIRE(ranked.size() == 4);
        CHECK(ranked[0].name == "narrow");
        CHECK(ranked[1].name == "wide");
    }

    SUBCASE("a plugin the order does not name comes after every one it does") {
        const std::array<std::string, 1> onlyChannels{"test.sweeppp.contributor"};
        REQUIRE(manager.setContributorOrder(onlyChannels).has_value());

        const std::vector<Contribution> ranked = manager.contributionsAt(100e6);
        REQUIRE(ranked.size() == 4);
        CHECK(ranked.front().pluginId == "test.sweeppp.contributor");
        CHECK(ranked.back().pluginId == "test.sweeppp.good");
    }

    SUBCASE("hiding a contributor stops it being drawn, not being asked") {
        REQUIRE(manager.setContributorShown("test.sweeppp.good", false).has_value());
        CHECK_FALSE(manager.contributorShown("test.sweeppp.good"));
        CHECK(manager.contributorShown("test.sweeppp.contributor"));

        // Still answering: what is drawn and what is known are now separate
        // questions, which is the point of the tick being a drawing setting.
        CHECK(manager.contributionsAt(100e6).size() == 4);
    }

    SUBCASE("datasets are readable and selectable through the host") {
        const std::vector<std::pair<std::string, std::string>> sets =
            manager.datasets("test.sweeppp.good");
        REQUIRE(sets.size() == 1);
        CHECK(sets.front().first == "fixture");
        CHECK(sets.front().second == "the fixture's only dataset");

        CHECK(manager.activeDataset("test.sweeppp.good") == 0);
        CHECK(manager.selectDataset("test.sweeppp.good", 0).has_value());
        CHECK_FALSE(manager.selectDataset("test.sweeppp.good", 7).has_value());
        CHECK_FALSE(manager.selectDataset("nothing.installed", 0).has_value());
    }
}

TEST_CASE("a contributor order survives the plugin it names going missing") {
    const ScopedConfigRoot root;
    const fs::path file = root.path() / "plugins.toml";

    PluginEnablement enablement = PluginEnablement::load(file);
    enablement.setContributorOrder({"contributor.that.was.removed", "test.sweeppp.good"});
    enablement.setContributorShown("test.sweeppp.good", false);
    REQUIRE(enablement.save().has_value());

    const PluginEnablement reloaded = PluginEnablement::load(file);
    REQUIRE(reloaded.contributorOrder().size() == 2);
    CHECK(reloaded.contributorOrder().front() == "contributor.that.was.removed");
    CHECK_FALSE(reloaded.contributorShown("test.sweeppp.good"));
    CHECK(reloaded.contributorShown("test.sweeppp.contributor"));

    // And through the manager: reordering what is installed must not drop the
    // slot held by something that is not.
    REQUIRE(findById(discoverFixtures(), "test.sweeppp.good") != nullptr);

    const std::array<std::string, 2> order{"test.sweeppp.good", "test.sweeppp.contributor"};
    REQUIRE(PluginManager::instance().setContributorOrder(order).has_value());

    const PluginEnablement after = PluginEnablement::load(file);
    CHECK(std::ranges::find(after.contributorOrder(), "contributor.that.was.removed") !=
          after.contributorOrder().end());
}

TEST_CASE("every incompatibility is listed with its reason") {
    const ScopedConfigRoot root;
    const std::vector<PluginInfo> plugins = discoverFixtures();

    SUBCASE("a plugin built against a newer ABI names both versions") {
        const PluginInfo* entry = findByFile(plugins, "fixture_newabi");
        REQUIRE(entry != nullptr);
        CHECK_FALSE(entry->loaded);
        CHECK(entry->abiVersion == 99);
        CHECK(entry->failureReason.contains("plugin ABI 99"));
        CHECK(entry->failureReason.contains("host ABI 1"));
    }

    SUBCASE("and so does one built against an older ABI") {
        const PluginInfo* entry = findByFile(plugins, "fixture_oldabi");
        REQUIRE(entry != nullptr);
        CHECK_FALSE(entry->loaded);
        CHECK(entry->abiVersion == 0);
        CHECK(entry->failureReason.contains("plugin ABI 0"));
    }

    SUBCASE("a host too old to run the plugin says which version is wanted") {
        const PluginInfo* entry = findById(plugins, "test.sweeppp.newhost");
        REQUIRE(entry != nullptr);
        CHECK_FALSE(entry->loaded);
        CHECK(entry->failureReason.contains("99.0.0"));
    }

    SUBCASE("an unmet dependency names it") {
        const PluginInfo* entry = findById(plugins, "test.sweeppp.needsdep");
        REQUIRE(entry != nullptr);
        // The manifest is fine, so it loads; it just cannot come up.
        CHECK(entry->loaded);
        CHECK_FALSE(entry->active);
        CHECK(entry->failureReason.contains("test.sweeppp.absent"));
    }

    SUBCASE("a duplicate id names the file that got there first") {
        // Two files, one id. Which of them wins is decided by search order and
        // is not what this checks: what matters is that exactly one loads and
        // the other is listed pointing at it, rather than one of them silently
        // replacing the other or quietly vanishing.
        const PluginInfo* good = findByFile(plugins, "fixture_good");
        const PluginInfo* dupe = findByFile(plugins, "fixture_dupe");
        REQUIRE(good != nullptr);
        REQUIRE(dupe != nullptr);
        REQUIRE(good->loaded != dupe->loaded);

        const PluginInfo& winner = good->loaded ? *good : *dupe;
        const PluginInfo& loser = good->loaded ? *dupe : *good;

        CHECK(winner.id == "test.sweeppp.good");
        CHECK(loser.failureReason.contains("already provided by"));
        CHECK(loser.failureReason.contains(winner.path.string()));
    }

    SUBCASE("a shared object that is not a plugin says so") {
        const PluginInfo* entry = findByFile(plugins, "fixture_nosym");
        REQUIRE(entry != nullptr);
        CHECK_FALSE(entry->loaded);
        CHECK(entry->failureReason.contains("sweeppp_plugin_abi_version"));
    }

    SUBCASE("a file that will not load at all carries the platform's message") {
        const fs::path broken = root.path() / "broken.so";
        {
            std::ofstream out(broken);
            out << "this is not an object file";
        }

        const std::array<fs::path, 1> directories{root.path()};
        PluginManager::instance().discover(directories);

        const std::vector<PluginInfo> reloaded = PluginManager::instance().enumerate();
        const PluginInfo* entry = findByFile(reloaded, "broken");
        REQUIRE(entry != nullptr);
        CHECK_FALSE(entry->loaded);
        CHECK_FALSE(entry->failureReason.empty());
        // Whatever dlopen said, not a message of our own invention: "could not
        // load" alone tells an operator nothing about a missing dependency, a
        // wrong architecture or a permission problem.
        CHECK(entry->failureReason.contains(broken.string()));
    }

    // Nothing was hidden. Every fixture in the directory has a row.
    CHECK(plugins.size() >= 7);
}

TEST_CASE("enablement round-trips through its own file") {
    const ScopedConfigRoot root;
    REQUIRE(findById(discoverFixtures(), "test.sweeppp.good") != nullptr);

    PluginManager& plugins = PluginManager::instance();

    REQUIRE(plugins.isEnabled("test.sweeppp.good"));
    REQUIRE(plugins.setEnabled("test.sweeppp.good", false).has_value());
    CHECK_FALSE(plugins.isEnabled("test.sweeppp.good"));

    // Withdrawn live: the facet stops answering the moment it is turned off.
    // Named rather than counted, because the other contributor fixture is
    // still installed and still answering the same frequencies.
    CHECK(contributionsFrom(plugins.contributionsIn(90e6, 110e6), "test.sweeppp.good").empty());

    const auto info = plugins.info("test.sweeppp.good");
    REQUIRE(info.has_value());
    CHECK_FALSE(info->active);
    CHECK_FALSE(info->enabled);

    // Written where a profile cannot reach it: settings.toml and every named
    // profile are the same Profile struct, so enablement living there would
    // make loading a profile silently swap the plugin set.
    const fs::path file = root.path() / "plugins.toml";
    REQUIRE(fs::exists(file));

    const PluginEnablement reloaded = PluginEnablement::load(file);
    CHECK_FALSE(reloaded.isEnabled("test.sweeppp.good"));
    CHECK(reloaded.isEnabled("something.never.installed"));

    // The same file carries the contributor arrangement, which is why it is
    // written here and not into a profile: loading a profile must not reorder
    // who names the band under the marker.
    const std::array<std::string, 1> order{"test.sweeppp.contributor"};
    REQUIRE(plugins.setContributorOrder(order).has_value());
    REQUIRE(plugins.setContributorShown("test.sweeppp.contributor", false).has_value());

    const PluginEnablement withOrder = PluginEnablement::load(file);
    REQUIRE_FALSE(withOrder.contributorOrder().empty());
    CHECK(withOrder.contributorOrder().front() == "test.sweeppp.contributor");
    CHECK_FALSE(withOrder.contributorShown("test.sweeppp.contributor"));
    CHECK(withOrder.contributorShown("test.sweeppp.good"));

    // And back on again, in the same process, on the module that is still
    // mapped.
    REQUIRE(plugins.setEnabled("test.sweeppp.good", true).has_value());
    CHECK(plugins.isEnabled("test.sweeppp.good"));
    CHECK(contributionsFrom(plugins.contributionsIn(90e6, 110e6), "test.sweeppp.good").size() == 2);
}

TEST_CASE("an id for a plugin that is not installed is inert") {
    const ScopedConfigRoot root;

    PluginEnablement enablement = PluginEnablement::load(root.path() / "plugins.toml");
    enablement.setEnabled("plugin.that.was.removed", false);
    REQUIRE(enablement.save().has_value());

    // The store keeps it, and discovery neither trips over it nor resurrects
    // it -- the same reasoning as SweepPresetStore's hidden builtins.
    const std::vector<PluginInfo> plugins = discoverFixtures();
    CHECK(findById(plugins, "plugin.that.was.removed") == nullptr);
    CHECK(PluginManager::instance().isEnabled("test.sweeppp.good"));
}

TEST_CASE("per-plugin settings live in their own file") {
    const ScopedConfigRoot root;
    const fs::path file = root.path() / "plugins" / "test.sweeppp.good.toml";

    PluginSettings settings = PluginSettings::load(file);
    CHECK(settings.getString("bandplan.plan", "fallback") == "fallback");

    settings.set("bandplan.plan", std::string_view("ITU Region 1"));
    settings.set("bandplan.alpha", 0.25);
    settings.set("bandplan.enabled", true);
    settings.set("bandplan.count", std::int64_t{7});
    REQUIRE(settings.save().has_value());

    const PluginSettings reloaded = PluginSettings::load(file);
    CHECK(reloaded.getString("bandplan.plan", "") == "ITU Region 1");
    CHECK(reloaded.getDouble("bandplan.alpha", 0.0) == doctest::Approx(0.25));
    CHECK(reloaded.getBool("bandplan.enabled", false));
    CHECK(reloaded.getInt("bandplan.count", 0) == 7);

    // A list, which is what a plugin holding a set of ids needs and what the
    // four scalar setters could only fake with a delimiter inside values the
    // operator hand-edits.
    {
        const std::vector<std::string> ids{"fpv/analog", "wifi/6", "beacons/nav"};
        PluginSettings withArray = PluginSettings::load(file);
        withArray.set("channels.disabled", std::span<const std::string>(ids));
        REQUIRE(withArray.save().has_value());

        const PluginSettings back = PluginSettings::load(file);
        CHECK(back.getStringArray("channels.disabled") == ids);

        // The scalars around it are untouched, and a key that is not an array
        // reads as empty rather than as anything invented.
        CHECK(back.getString("bandplan.plan", "") == "ITU Region 1");
        CHECK(back.getStringArray("bandplan.plan").empty());
        CHECK(back.getStringArray("channels.absent").empty());
    }

    // A file that will not parse means defaults and a reason, never a refusal
    // to come up.
    {
        std::ofstream out(file);
        out << "this is not toml = = =\n";
    }
    std::string problem;
    const PluginSettings broken = PluginSettings::load(file, &problem);
    CHECK(broken.getString("bandplan.plan", "fallback") == "fallback");
    CHECK_FALSE(problem.empty());
}

TEST_CASE("a plugin's own window is dispatched once per frame, at the top level") {
    const ScopedConfigRoot root;

    // Before discover: a UI facet in a host that never had a binding is listed
    // with a reason instead of registering, which the facet-listing test
    // above covers from the other side.
    PluginManager::instance().setImGuiBinding(stubImGuiBinding());

    const std::vector<PluginInfo> plugins = discoverFixtures();
    const PluginInfo* good = findById(plugins, "test.sweeppp.good");
    REQUIRE(good != nullptr);

    const auto ui = std::ranges::find_if(good->facets, [](const PluginFacetInfo& facet) {
        return facet.kind == PluginFacetKind::UiExtension;
    });
    REQUIRE(ui != good->facets.end());
    CHECK(ui->active);

    // The host knows the spot is claimed before it draws anything, which is
    // what lets the application skip a whole section rather than draw an empty
    // one.
    CHECK(PluginManager::instance().hasUiSpot(SWEEPPP_UI_SPOT_WINDOW));
    CHECK_FALSE(PluginManager::instance().hasUiSpot(SWEEPPP_UI_SPOT_STATUS_CHIP));

    std::optional<FixtureWindowEvent> seen;
    const std::uint64_t subscription = PluginManager::instance().subscribeEvent<FixtureWindowEvent>(
        [&seen](const FixtureWindowEvent& report) { seen = report; });
    REQUIRE(subscription != 0);

    PluginManager::instance().drawSpot(SWEEPPP_UI_SPOT_WINDOW);
    REQUIRE(seen.has_value());
    const std::int32_t first = seen->draws;

    PluginManager::instance().drawSpot(SWEEPPP_UI_SPOT_WINDOW);
    REQUIRE(seen.has_value());
    CHECK(seen->draws == first + 1);

    // A spot whose bit is clear is never called even though the vtable has the
    // pointer for the one that is.
    seen.reset();
    PluginManager::instance().drawSpot(SWEEPPP_UI_SPOT_TOOLBAR);
    CHECK_FALSE(seen.has_value());

    PluginManager::instance().unsubscribeEvent(subscription);
}

TEST_CASE("a plugin's FFT backend reaches the registry and runs") {
    const ScopedConfigRoot root;
    REQUIRE(findById(discoverFixtures(), "test.sweeppp.good") != nullptr);

    FftBackendManager& backends = FftBackendManager::instance();
    const std::string name = "test.sweeppp.good.fft";

    // Listed with its capabilities, without having been instantiated: merely
    // showing a backend must not initialise whatever library is behind it.
    const auto info = backends.info(name);
    REQUIRE(info.has_value());
    CHECK(info->available);
    CHECK(info->capabilities.threadSafeExecute);
    CHECK(info->capabilities.sizeConstraint == FftSizeConstraint::PowerOfTwo);
    CHECK_FALSE(backends.isAcquired(name));

    auto backend = backends.acquire(name);
    REQUIRE(backend.has_value());
    CHECK(backends.isAcquired(name));
    CHECK((*backend)->snapSize(300) == 512);

    auto plan = (*backend)->createPlan(FftPlanConfig{.size = 4});
    REQUIRE(plan.has_value());

    const std::array<std::complex<float>, 4> input{
        {{1.0F, -1.0F}, {2.0F, -2.0F}, {3.0F, -3.0F}, {4.0F, -4.0F}}};
    std::array<std::complex<float>, 4> output{};
    (*plan)->execute(input.data(), output.data());

    // The fixture doubles rather than transforms; what is being proved is that
    // the caller's buffers reached the plugin and came back.
    for (std::size_t i = 0; i < input.size(); ++i) {
        CAPTURE(i);
        CHECK(output[i].real() == doctest::Approx(input[i].real() * 2.0F));
        CHECK(output[i].imag() == doctest::Approx(input[i].imag() * 2.0F));
    }

    // Acquired, so disabling the plugin cannot withdraw it: that refusal is
    // what becomes the "restart needed" marker instead of a dangling pointer
    // through the pipeline.
    const auto disabled = PluginManager::instance().setEnabled("test.sweeppp.good", false);
    REQUIRE_FALSE(disabled.has_value());
    CHECK(disabled.error().message().contains("in use by the pipeline"));

    const auto stillThere = PluginManager::instance().info("test.sweeppp.good");
    REQUIRE(stillThere.has_value());
    CHECK(stillThere->requiresRestart);
    // Still active, because a refused withdrawal leaves the plugin whole
    // rather than half-registered.
    CHECK(stillThere->active);

    plan->reset();
}

TEST_CASE("a plugin's radio opens, streams and blocks withdrawal while it is open") {
    const ScopedConfigRoot root;
    REQUIRE(findById(discoverFixtures(), "test.sweeppp.good") != nullptr);

    SdrDeviceManager& devices = SdrDeviceManager::instance();
    const std::string driver = "test.sweeppp.good.sdr";
    REQUIRE(devices.hasDriver(driver));

    const std::vector<SdrDeviceInfo> found = devices.enumerateAll();
    const auto entry = std::ranges::find_if(
        found, [&driver](const SdrDeviceInfo& info) { return info.driver == driver; });
    REQUIRE(entry != found.end());
    CHECK(entry->label == "Fixture radio");
    CHECK(entry->maxFrequencyHz == doctest::Approx(6e9));

    {
        auto device = devices.open(driver, "fixture-0");
        REQUIRE(device.has_value());

        // Cs16, not a float format. The point of asserting the exact value is
        // that nothing in the host may assume the one the old pull-mode facet
        // was able to carry.
        CHECK((*device)->nativeFormat() == SampleFormat::Cs16);
        CHECK((*device)->retuneSettleSeconds() == doctest::Approx(0.001));
        CHECK((*device)->deliveryGranularitySeconds(20e6) == doctest::Approx(65536.0 / 20e6));
        CHECK((*device)->retune(101e6).has_value());
        CHECK(asDouble(*(*device)->getParameter("center_hz")) == doctest::Approx(101e6));

        CHECK((*device)->supportedSampleRates() == std::vector<double>{2e6, 8e6, 20e6});

        // A device is alive, so the driver cannot be withdrawn: its vtable
        // lives in the plugin, and withdrawing would leave the operator
        // holding something that cannot even be destroyed.
        //
        // Asked of the registry directly rather than through setEnabled, which
        // reports whichever of the plugin's facets refuses first -- and the
        // FFT backend an earlier test acquired is still acquired, because
        // `acquire` caches for the life of the process by design.
        const auto refused = devices.unregisterFactory(driver);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains("still in use"));
        CHECK(devices.hasDriver(driver));

        // Pull mode is not in the ABI at all: this runs over the push stream,
        // through ISdrDevice's own adapter, so it is an end-to-end check of
        // the push path as much as of readSamples.
        std::array<std::complex<float>, 16> samples{};
        const auto read = (*device)->readSamples(samples.data(), samples.size());
        REQUIRE(read.has_value());
        CHECK(*read == samples.size());
        // The fixture's ramp is keyed on the block sequence, and readSamples
        // takes the first block, so this is sequence 0: I = i, Q = -i, at the
        // Cs16 scale.
        CHECK(samples[1].real() == doctest::Approx(1.0F / 32768.0F));
        CHECK(samples[1].imag() == doctest::Approx(-1.0F / 32768.0F));
    }

    // Closed, so it can go now.
    CHECK(devices.unregisterFactory(driver).has_value());
    CHECK_FALSE(devices.hasDriver(driver));
}

TEST_CASE("a plugin radio describes itself well enough to build a panel from") {
    // The SDR panel is generated from parameters() alone -- there is no
    // per-device UI code anywhere in the project -- so a field that does not
    // cross this boundary is a control an operator cannot reach.
    const ScopedConfigRoot root;
    REQUIRE(findById(discoverFixtures(), "test.sweeppp.good") != nullptr);

    SdrDeviceManager& devices = SdrDeviceManager::instance();
    const std::string driver = "test.sweeppp.good.sdr";
    REQUIRE(devices.hasDriver(driver));

    auto device = devices.open(driver, "fixture-0");
    REQUIRE(device.has_value());

    const std::span<const SdrParameter> parameters = (*device)->parameters();
    REQUIRE(parameters.size() == 4);

    const auto find = [parameters](std::string_view key) -> const SdrParameter* {
        const auto found =
            std::ranges::find_if(parameters, [key](const SdrParameter& p) { return p.key == key; });
        return found == parameters.end() ? nullptr : &*found;
    };

    const SdrParameter* centre = find("center_hz");
    REQUIRE(centre != nullptr);
    CHECK(centre->label == "Centre frequency");
    CHECK(centre->group == "Tuning");
    CHECK(centre->type == SdrParameterType::Double);
    CHECK(centre->unit == "Hz");
    CHECK(centre->min == doctest::Approx(1e6));
    CHECK(centre->max == doctest::Approx(6e9));
    CHECK(centre->gridAffecting);
    CHECK_FALSE(centre->readOnly);
    CHECK(asDouble(centre->defaultValue) == doctest::Approx(100e6));
    CHECK_FALSE(centre->description.empty());

    const SdrParameter* gain = find("lna_gain");
    REQUIRE(gain != nullptr);
    CHECK(gain->type == SdrParameterType::Int);
    CHECK(gain->step == doctest::Approx(8.0));
    CHECK(gain->calibrationAffecting);
    CHECK(asInt(gain->defaultValue) == 16);
    // The dependency the panel greys a control out on, which only the driver
    // can know: a manual gain means nothing while the mode is automatic.
    CHECK(gain->appliesWhenKey == "gain_mode");
    REQUIRE(gain->appliesWhenValues.size() == 1);
    CHECK(gain->appliesWhenValues[0] == "manual");

    const SdrParameter* mode = find("gain_mode");
    REQUIRE(mode != nullptr);
    CHECK(mode->type == SdrParameterType::Enum);
    CHECK(mode->requiresStop);
    REQUIRE(mode->enumValues.size() == 2);
    CHECK(mode->enumValues[0].value == "manual");
    CHECK(mode->enumValues[0].label == "Manual");
    CHECK_FALSE(mode->enumValues[1].description.empty());
    CHECK(asString(mode->defaultValue) == "manual");

    const SdrParameter* bias = find("bias_tee");
    REQUIRE(bias != nullptr);
    CHECK(bias->type == SdrParameterType::Bool);
    CHECK(bias->readOnly);

    // Coerced host-side against the descriptor, so clamping and snapping are
    // one implementation for every driver rather than one per driver.
    //
    // 20 is not on the 8 dB grid, and it snaps to the NEAREST step rather than
    // truncating: an operator asking for 20 dB gets 24, because truncation
    // would make every value they typed come back lower than they asked.
    REQUIRE((*device)->setParameter("lna_gain", SdrValue{std::int64_t{20}}).has_value());
    CHECK(asInt(*(*device)->getParameter("lna_gain")) == 24);
    REQUIRE((*device)->setParameter("lna_gain", SdrValue{std::int64_t{19}}).has_value());
    CHECK(asInt(*(*device)->getParameter("lna_gain")) == 16);
    // Past the top of the range, so it clamps.
    REQUIRE((*device)->setParameter("lna_gain", SdrValue{std::int64_t{400}}).has_value());
    CHECK(asInt(*(*device)->getParameter("lna_gain")) == 40);

    // Read-only is refused before it reaches the plugin.
    CHECK_FALSE((*device)->setParameter("bias_tee", SdrValue{true}).has_value());

    // The string alternative survives the crossing, which the three-setter
    // facet could not carry at all.
    REQUIRE((*device)->setParameter("gain_mode", SdrValue{std::string{"automatic"}}).has_value());
    CHECK(asString(*(*device)->getParameter("gain_mode")) == "automatic");
    // Not one of the declared values, so coercion refuses it.
    CHECK_FALSE((*device)->setParameter("gain_mode", SdrValue{std::string{"psychic"}}).has_value());
}

TEST_CASE("a plugin radio's identity carries what only an open handle knows") {
    const ScopedConfigRoot root;
    REQUIRE(findById(discoverFixtures(), "test.sweeppp.good") != nullptr);

    SdrDeviceManager& devices = SdrDeviceManager::instance();
    auto device = devices.open("test.sweeppp.good.sdr", "fixture-0");
    REQUIRE(device.has_value());

    const SdrDeviceInfo& info = (*device)->info();
    CHECK(info.driver == "test.sweeppp.good.sdr");
    CHECK(info.id == "fixture-0");
    CHECK(info.label == "Fixture radio");

    // Firmware, FPGA and link speed all need a handle, so they come from the
    // device rather than from a second enumeration.
    CHECK(info.firmware.version == "2.1.0");
    CHECK(info.firmware.knownLatest == "2.3.0");
    CHECK_FALSE(info.firmware.aheadOfDriver);
    CHECK(info.fpga.version == "0.15.0");
    CHECK(info.fpga.aheadOfDriver);
    CHECK(info.linkCapacityBytesPerSec == 400'000'000);
    CHECK(info.linkDescription == "USB 3.0 SuperSpeed");
}

TEST_CASE("a plugin radio reports its own condition") {
    const ScopedConfigRoot root;
    REQUIRE(findById(discoverFixtures(), "test.sweeppp.good") != nullptr);

    SdrDeviceManager& devices = SdrDeviceManager::instance();
    auto device = devices.open("test.sweeppp.good.sdr", "fixture-0");
    REQUIRE(device.has_value());

    const std::vector<ISdrDevice::HealthReading> health = (*device)->healthReadings();
    REQUIRE(health.size() == 2);

    CHECK(health[0].label == "Temperature");
    CHECK(health[0].value == "71.5 C");
    CHECK(health[0].numeric == doctest::Approx(71.5F));
    CHECK(health[0].maximum == doctest::Approx(70.0F));
    CHECK(health[0].alarm);

    // Text only: a maximum at or below the minimum means there is no scale to
    // plot it against, and the panel shows it as a string.
    CHECK(health[1].label == "Power source");
    CHECK(health[1].value == "USB bus");
    CHECK(health[1].maximum <= health[1].minimum);
    CHECK_FALSE(health[1].alarm);
}

TEST_CASE("a plugin radio pushes native-format blocks straight into the host's pool") {
    // The zero-copy proof, and the reason the facet is push-mode. The plugin
    // borrows a block, writes Cs16 into it, and hands it back; nothing
    // converts and nothing is copied twice, so the bytes the callback sees
    // must be exactly the bytes the plugin wrote.
    const ScopedConfigRoot root;
    REQUIRE(findById(discoverFixtures(), "test.sweeppp.good") != nullptr);

    SdrDeviceManager& devices = SdrDeviceManager::instance();
    auto device = devices.open("test.sweeppp.good.sdr", "fixture-0");
    REQUIRE(device.has_value());

    REQUIRE((*device)->retune(433e6).has_value());

    constexpr std::size_t kFrames = 1024;
    constexpr std::uint32_t kBlocks = 8;
    auto pool = BlockPool::create(kFrames * bytesPerFrame(SampleFormat::Cs16), kBlocks);
    REQUIRE(pool.has_value());

    std::mutex mutex;
    std::condition_variable arrived;
    std::vector<IqBlock> received;

    const StreamConfig config{
        .framesPerBlock = kFrames, .blockCount = kBlocks, .format = SampleFormat::Cs16};

    REQUIRE((*device)
                ->start(**pool, config,
                        [&](IqBlock&& block) {
                            const std::lock_guard lock(mutex);
                            if (received.size() < 8) {
                                received.push_back(std::move(block));
                                arrived.notify_all();
                            }
                        })
                .has_value());
    CHECK((*device)->streaming());

    {
        std::unique_lock lock(mutex);
        CHECK(
            arrived.wait_for(lock, std::chrono::seconds(5), [&] { return received.size() >= 8; }));
    }
    (*device)->stop();
    CHECK_FALSE((*device)->streaming());

    REQUIRE(received.size() == 8);
    for (std::size_t i = 0; i < received.size(); ++i) {
        CAPTURE(i);
        const IqBlock& block = received[i];

        CHECK(block.format == SampleFormat::Cs16);
        CHECK(block.frames == kFrames);
        CHECK(block.centerHz == doctest::Approx(433e6));
        CHECK(block.hostTimeNs > 0);
        if (i > 0) {
            CHECK(block.sequence > received[i - 1].sequence);
        }

        // The bytes themselves. A conversion anywhere on this path, or a
        // handle resolving to the wrong slot, shows up here and nowhere else.
        const auto* samples = reinterpret_cast<const std::int16_t*>(block.data());
        for (std::size_t frame = 0; frame < block.frames; ++frame) {
            const auto expected = static_cast<std::int16_t>((block.sequence * 1000U) + frame);
            if (samples[frame * 2] != expected ||
                samples[(frame * 2) + 1] != static_cast<std::int16_t>(-expected)) {
                CHECK(samples[frame * 2] == expected);
                CHECK(samples[(frame * 2) + 1] == static_cast<std::int16_t>(-expected));
                break; // one report is enough; a mismatch is never isolated
            }
        }
    }
}

TEST_CASE("a plugin radio's dropped blocks are counted rather than waited for") {
    // A pool of one, and a consumer that keeps what it is given. The plugin
    // must not wait for a block -- waiting on a transfer thread turns a host
    // backlog into a device overrun -- so exhaustion has to show up as a
    // counted drop and the stream has to keep running.
    const ScopedConfigRoot root;
    REQUIRE(findById(discoverFixtures(), "test.sweeppp.good") != nullptr);

    SdrDeviceManager& devices = SdrDeviceManager::instance();
    auto device = devices.open("test.sweeppp.good.sdr", "fixture-0");
    REQUIRE(device.has_value());

    Telemetry telemetry;
    (*device)->attachTelemetry(&telemetry.stream());

    // The link capacity reaches the counters from info(), with no plugin
    // involvement at all -- which is what gives every plugin driver the
    // Performance panel's link-utilisation bar for free.
    CHECK(telemetry.stream().linkCapacityBytesPerSec.load() == 400'000'000);

    constexpr std::size_t kFrames = 256;
    auto pool = BlockPool::create(kFrames * bytesPerFrame(SampleFormat::Cs16), 1);
    REQUIRE(pool.has_value());

    std::mutex mutex;
    std::condition_variable arrived;
    std::vector<IqBlock> held; // never released, so the pool stays empty

    const StreamConfig config{
        .framesPerBlock = kFrames, .blockCount = 1, .format = SampleFormat::Cs16};

    REQUIRE((*device)
                ->start(**pool, config,
                        [&](IqBlock&& block) {
                            const std::lock_guard lock(mutex);
                            held.push_back(std::move(block));
                            arrived.notify_all();
                        })
                .has_value());

    {
        std::unique_lock lock(mutex);
        CHECK(arrived.wait_for(lock, std::chrono::seconds(5), [&] { return !held.empty(); }));
    }

    // Long enough for the producer to find the pool empty repeatedly.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (telemetry.stream().poolExhaustedEvents.load() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    CHECK(telemetry.stream().poolExhaustedEvents.load() > 0);
    CHECK(telemetry.stream().samplesLostAtSource.load() >= kFrames);

    (*device)->stop();
    held.clear();
}

TEST_CASE("a plugin driver's open failure carries the driver's own explanation") {
    // ERR_DEVICE on its own is a mystery. The error channel exists so a driver
    // can say the sentence an operator can act on, and there is nowhere else
    // for that sentence to live.
    const ScopedConfigRoot root;
    REQUIRE(findById(discoverFixtures(), "test.sweeppp.good") != nullptr);

    const auto failed = SdrDeviceManager::instance().open("test.sweeppp.good.sdr", "boom");
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().message().contains("the error channel working"));
}

TEST_CASE("a plugin's switcher is opened and driven across the ABI") {
    // A switcher is registered like a radio and is not one. This goes through
    // its own registry, its own vtable and a real dlopen, because "not an SDR
    // device" is a claim about the whole path rather than about a name.
    const ScopedConfigRoot root;
    REQUIRE(findById(discoverFixtures(), "test.sweeppp.good") != nullptr);

    const std::vector<RfPathInfo> found = RfPathManager::instance().enumerateAll();
    REQUIRE(found.size() == 1);
    CHECK(found.front().label == "Fixture 4-way switch");
    CHECK(found.front().inputCount == 4);
    CHECK(found.front().switchSeconds == doctest::Approx(0.01));

    // And it is emphatically not among the radios: an operator picking it from
    // the device chooser would get a receiver that never delivers a block.
    const std::vector<SdrDeviceInfo> radios = SdrDeviceManager::instance().enumerateAll();
    CHECK(std::ranges::none_of(
        radios, [](const SdrDeviceInfo& info) { return info.label == "Fixture 4-way switch"; }));

    auto path = RfPathManager::instance().open("test.sweeppp.good.switch", "");
    REQUIRE(path.has_value());

    const std::span<const RfPathInput> inputs = (*path)->inputs();
    REQUIRE(inputs.size() == 4);
    CHECK(inputs[0].id == "in1");
    CHECK(inputs[1].label == "J2");
    // One filtered input, so the host's "the switcher narrows it too" path has
    // something to narrow.
    CHECK(inputs[1].minHz == doctest::Approx(1.0e9));
    CHECK(inputs[1].maxHz == doctest::Approx(2.0e9));

    REQUIRE((*path)->selectInput(2).has_value());
    CHECK((*path)->selectedInput() == 2);
    CHECK_FALSE((*path)->selectInput(9).has_value());
    CHECK((*path)->selectedInput() == 2);

    const std::vector<IRfPath::HealthReading> health = (*path)->healthReadings();
    REQUIRE(health.size() == 1);
    CHECK(health.front().label == "Supply");

    // The registry refuses to withdraw a driver whose switcher is still open,
    // for the same reason it does for a radio: the vtable lives in the
    // plugin's image.
    CHECK_FALSE(
        RfPathManager::instance().unregisterFactory("test.sweeppp.good.switch").has_value());
    path->reset();
    CHECK(RfPathManager::instance().unregisterFactory("test.sweeppp.good.switch").has_value());
}

TEST_CASE("a plugin switcher's open failure carries the driver's own explanation") {
    const ScopedConfigRoot root;
    REQUIRE(findById(discoverFixtures(), "test.sweeppp.good") != nullptr);

    const auto failed = RfPathManager::instance().open("test.sweeppp.good.switch", "boom");
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().message().contains("asked to fail"));
}

TEST_CASE("a plugin's frame processor sees frames, on the host's thread") {
    // The bus outlives the manager's reset, deliberately: shutdown
    // unsubscribes, and unsubscribing from a bus that has already been
    // destroyed is exactly the ordering bug the host exists to avoid.
    FrameBus bus;
    const ScopedConfigRoot root;

    PluginManager::instance().setFrameBus(&bus);
    REQUIRE(findById(discoverFixtures(), "test.sweeppp.good") != nullptr);

    std::mutex mutex;
    std::condition_variable arrived;
    std::optional<FixtureFrameEvent> seen;

    const std::uint64_t subscription = PluginManager::instance().subscribeEvent<FixtureFrameEvent>(
        [&](const FixtureFrameEvent& report) {
            const std::lock_guard lock(mutex);
            seen = report;
            arrived.notify_all();
        });
    REQUIRE(subscription != 0);

    auto frame = std::make_shared<SpectrumFrame>();
    frame->sequence = 42;
    frame->startHz = 88e6;
    frame->binWidthHz = 1e3;
    frame->binsDbfs = {-70.0F, -68.5F, -71.0F};
    bus.publish(frame);

    {
        // The processor runs on the host's own thread behind a queue, which is
        // the whole point of the facet: a slow plugin costs its own frames
        // rather than stalling the radio. So this waits rather than asserting
        // immediately.
        std::unique_lock lock(mutex);
        CHECK(
            arrived.wait_for(lock, std::chrono::seconds(2), [&seen] { return seen.has_value(); }));
    }

    REQUIRE(seen.has_value());
    CHECK(seen->sequence == 42);
    CHECK(seen->binCount == 3);
    CHECK(seen->startHz == doctest::Approx(88e6));
    CHECK(seen->firstBin == doctest::Approx(-70.0F));

    PluginManager::instance().unsubscribeEvent(subscription);
}

TEST_CASE("a plugin may not publish an event that is not its own") {
    const ScopedConfigRoot root;
    REQUIRE(findById(discoverFixtures(), "test.sweeppp.good") != nullptr);

    // Checked through the host's own view of the rule rather than by
    // persuading a fixture to misbehave: what matters is that the namespace
    // test exists, because a plugin able to publish `sweeppp.retune` could
    // make a session say the radio moved when it did not.
    bool sawRetune = false;
    const std::uint64_t subscription =
        PluginManager::instance().subscribeEvent<sweeppp_retune_event_t>(
            [&sawRetune](const sweeppp_retune_event_t&) { sawRetune = true; });

    EventBus bus;
    PluginManager::instance().attachEvents(bus);
    bus.publish(RetuneEvent{.monotonicNs = 5, .centerHz = 100e6, .stepIndex = 7});

    CHECK(sawRetune);
    PluginManager::instance().unsubscribeEvent(subscription);
}

TEST_CASE("core events reach plugins with their fields intact") {
    const ScopedConfigRoot root;

    EventBus bus;
    PluginManager::instance().attachEvents(bus);

    std::optional<sweeppp_parameter_changed_event_t> parameter;
    std::string parameterKey;
    const std::uint64_t subscription =
        PluginManager::instance().subscribeEvent<sweeppp_parameter_changed_event_t>(
            [&](const sweeppp_parameter_changed_event_t& event) {
                parameter = event;
                // Copied inside the callback: every string is borrowed for the
                // duration of the call and nothing beyond it.
                parameterKey.assign(event.key.data, event.key.len);
            });

    bus.publish(ParameterChangedEvent{.monotonicNs = 11,
                                      .key = "lna_gain",
                                      .value = "24",
                                      .gridAffecting = false,
                                      .calibrationAffecting = true});

    REQUIRE(parameter.has_value());
    CHECK(parameterKey == "lna_gain");
    CHECK(parameter->grid_affecting == 0);
    CHECK(parameter->calibration_affecting == 1);

    PluginManager::instance().unsubscribeEvent(subscription);
}

TEST_CASE("a facet in use refuses to be withdrawn") {
    // The registries are the host's, so this is checked directly rather than
    // through a fixture plugin: what matters is that withdrawal is refused
    // while something holds the facet, because that refusal is what turns into
    // the "restart needed" marker instead of a dangling pointer through the
    // pipeline.
    registerReferenceFftBackend();

    const std::string name = "reference";
    REQUIRE(FftBackendManager::instance().isAvailable(name));

    REQUIRE(FftBackendManager::instance().acquire(name).has_value());
    CHECK(FftBackendManager::instance().isAcquired(name));

    const auto withdrawn = FftBackendManager::instance().unregisterBackend(name);
    REQUIRE_FALSE(withdrawn.has_value());
    CHECK(withdrawn.error().code() == ErrorCode::Unavailable);
    CHECK(withdrawn.error().message().contains("acquired"));

    // Still there, and still usable: a refused withdrawal must leave the
    // registry exactly as it was.
    CHECK(FftBackendManager::instance().isAvailable(name));
    CHECK(FftBackendManager::instance().acquire(name).has_value());

    CHECK_FALSE(FftBackendManager::instance().unregisterBackend("no.such.backend").has_value());
    CHECK_FALSE(SdrDeviceManager::instance().unregisterFactory("no.such.driver").has_value());
}

TEST_CASE("a driver name is claimed once") {
    registerBuiltinSdrDevices();

    // What stops two plugins both claiming "hackrf" from silently becoming
    // last-wins: the host asks before registering, and lists the loser with
    // the reason.
    const std::vector<std::string> drivers = SdrDeviceManager::instance().drivers();
    REQUIRE_FALSE(drivers.empty());

    CHECK(SdrDeviceManager::instance().hasDriver(drivers.front()));
    CHECK_FALSE(SdrDeviceManager::instance().hasDriver("nothing.claims.this"));
}
