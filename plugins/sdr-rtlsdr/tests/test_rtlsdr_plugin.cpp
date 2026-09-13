// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The RTL-SDR plugin, through the real loader.
//
// It `dlopen`s the module it was built alongside rather than compiling the
// driver in, because there is an ABI here: the driver reaches the registry
// through `sweeppp_sdr_factory_vtable_t`, and "the facet registers under the
// right name" is a promise about the built binary, not about the source.
//
// Nothing here needs a radio. The checks that do are marked skipped and run
// only when asked for.
#include <algorithm>
#include <array>
#include <complex>
#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <string>
#include <sweeppp/plugin/PluginAbi.h>
#include <sweeppp/plugin/PluginHost.hpp>
#include <sweeppp/sdr/ISdrDevice.hpp>
#include <vector>

using namespace sweeppp;

namespace {

namespace fs = std::filesystem;

/// A temporary config root, so the enablement file a test writes cannot land
/// in the developer's own configuration.
class ScopedConfigRoot {
public:
    ScopedConfigRoot()
        : m_path(fs::temp_directory_path() / std::format("sweeppp-rtlsdr-test-{}", ++s_counter)) {
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

private:
    static inline int s_counter = 0;
    fs::path m_path;
};

/// Discovers only the directory this plugin was built into, so whatever else
/// the developer has installed cannot change what a test sees.
const PluginInfo* discover() {
    const std::array<fs::path, 1> directories{fs::path{SWEEPPP_PLUGIN_BINARY_DIR}};
    PluginManager::instance().discover(directories);

    const std::vector<PluginInfo> plugins = PluginManager::instance().enumerate();
    const auto found = std::ranges::find_if(
        plugins, [](const PluginInfo& plugin) { return plugin.id == "org.sweeppp.sdr-rtlsdr"; });
    if (found == plugins.end()) {
        return nullptr;
    }
    // Returned by pointer into a copy that outlives this call, rather than
    // into the vector above, which does not.
    static PluginInfo copy;
    copy = *found;
    return &copy;
}

} // namespace

TEST_CASE("the module loads at the host's own ABI version") {
    const ScopedConfigRoot root;

    const PluginInfo* plugin = discover();
    REQUIRE(plugin != nullptr);
    CHECK(plugin->loaded);
    CHECK(plugin->failureReason.empty());
    CHECK(plugin->abiVersion == SWEEPPP_PLUGIN_ABI_VERSION);
    CHECK(plugin->name == "RTL-SDR");
    CHECK_FALSE(plugin->version.empty());
}

TEST_CASE("the facet id is exactly \"rtlsdr\"") {
    // Its own test case, because this string is not a label.
    //
    // A saved profile persists `device.driver`, and the CLI takes the same
    // word as `--device rtlsdr`. If the facet ever registered under anything
    // else, every profile in the field would fail to reopen its radio, with a
    // message pointing nowhere near the change that caused it.
    const ScopedConfigRoot root;

    const PluginInfo* plugin = discover();
    REQUIRE(plugin != nullptr);

    REQUIRE(plugin->facets.size() == 1);
    CHECK(plugin->facets[0].kind == PluginFacetKind::SdrDevice);
    CHECK(plugin->facets[0].id == "rtlsdr");
}

TEST_CASE("the driver reaches the registry, and leaves it when disabled") {
    const ScopedConfigRoot root;
    REQUIRE(discover() != nullptr);

    SdrDeviceManager& devices = SdrDeviceManager::instance();
    CHECK(devices.hasDriver("rtlsdr"));

    const std::vector<std::string> drivers = devices.drivers();
    CHECK(std::ranges::find(drivers, "rtlsdr") != drivers.end());

    REQUIRE(PluginManager::instance().setEnabled("org.sweeppp.sdr-rtlsdr", false).has_value());
    CHECK_FALSE(devices.hasDriver("rtlsdr"));

    // And back: a plugin disabled and re-enabled is a normal thing to do to a
    // radio that has been unplugged.
    REQUIRE(PluginManager::instance().setEnabled("org.sweeppp.sdr-rtlsdr", true).has_value());
    CHECK(devices.hasDriver("rtlsdr"));
}

TEST_CASE("enumerating with no radio attached is an empty answer, not a failure") {
    // The common case on a build machine, and the one that must not throw:
    // `enumerateAll()` runs this on a timer from the UI thread.
    const ScopedConfigRoot root;
    REQUIRE(discover() != nullptr);

    std::vector<SdrDeviceInfo> found;
    CHECK_NOTHROW(found = SdrDeviceManager::instance().enumerateAll());

    for (const SdrDeviceInfo& info : found) {
        if (info.driver == "rtlsdr") {
            // If one IS attached, what it says about itself still has to be
            // usable: an id is what a profile stores to find it again.
            CHECK_FALSE(info.id.empty());
            CHECK_FALSE(info.label.empty());
            CHECK(info.maxFrequencyHz > info.minFrequencyHz);
        }
    }
}

TEST_CASE("opening a device that is not there fails rather than hangs") {
    const ScopedConfigRoot root;
    REQUIRE(discover() != nullptr);

    for (const char* id : {"no-such-serial", "index-4096"}) {
        const auto opened = SdrDeviceManager::instance().open("rtlsdr", id);
        CHECK_FALSE(opened.has_value());
        if (!opened) {
            CHECK_FALSE(opened.error().message().empty());
        }
    }
}

// ------------------------------------------------------------- hardware

TEST_CASE("a real RTL-SDR streams" * doctest::skip()) {
    // Skipped by default: this needs a dongle plugged in. Run it with
    //     sweeppp-plugin-sdr-rtlsdr-tests --no-skip
    const ScopedConfigRoot root;
    REQUIRE(discover() != nullptr);

    const std::vector<SdrDeviceInfo> found = SdrDeviceManager::instance().enumerateAll();
    const auto entry = std::ranges::find_if(
        found, [](const SdrDeviceInfo& info) { return info.driver == "rtlsdr"; });
    REQUIRE(entry != found.end());

    auto device = SdrDeviceManager::instance().open("rtlsdr", entry->id);
    REQUIRE(device.has_value());

    CHECK((*device)->nativeFormat() == SampleFormat::Cu8);
    CHECK_FALSE((*device)->info().hardwareRevision.empty());
    CHECK_FALSE((*device)->parameters().empty());

    std::vector<std::complex<float>> samples(65536);
    const auto read = (*device)->readSamples(samples.data(), samples.size());
    REQUIRE(read.has_value());
    CHECK(*read == samples.size());
}
