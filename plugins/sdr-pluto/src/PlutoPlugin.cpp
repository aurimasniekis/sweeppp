// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The PlutoSDR driver, as a plugin.
//
// It links `sweeppp::sweeppp`, and only for values: SdrParameter, SampleFormat
// and Result. It must never reach a singleton -- see the rule at the top of
// <sweeppp/plugin/Plugin.hpp>. In particular it does NOT call
// SdrDeviceManager::instance(): the plugin's copy of that registry is not the
// host's, and registering into it would register into nothing.
#include "PlutoDriver.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace sweeppp::pluto {
namespace {

constexpr std::string_view kPluginId = "org.sweeppp.sdr-pluto";

/// The facet id is the DRIVER NAME, and it must stay exactly "pluto".
///
/// A saved profile persists `device.driver`, and the CLI takes it as
/// `--device pluto`. Changing this string breaks every profile and script that
/// names it.
constexpr std::string_view kDriverName = "pluto";

const std::array<sweeppp_facet_t, 1>& facets() {
    static const auto kSdrVtable = plugin::makeSdrDriverVtable<plugin::Driver>();
    static const std::array kFacets{
        plugin::facet(SWEEPPP_FACET_SDR_DEVICE, kDriverName, "PlutoSDR",
                      "ADALM-PLUTO and compatible radios, over libiio.", &kSdrVtable),
    };
    return kFacets;
}

class PlutoPlugin final : public plugin::Plugin {
public:
    [[nodiscard]] bool activate(plugin::Host& host) override {
        log() = host.as("pluto");

        const sweeppp_plugin_status_t status = host.registerFacet(facets()[0], &driver());
        if (status != SWEEPPP_PLUGIN_OK) {
            host.reportFacet(facets()[0], "another driver already claims the name 'pluto'");
            return true;
        }

        log().debug("registered the '{}' driver", kDriverName);
        return true;
    }

    /// Deliberately empty: every open radio owns its own libiio context, and
    /// the library keeps no process-wide state to release.
    void deactivate() override {}
};

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
                .name = plugin::str("PlutoSDR"),
                .description = plugin::str(
                    "Drives the ADALM-PLUTO over libiio, by USB or over the network: up to "
                    "70 MHz to 6 GHz depending on the radio, CS16 delivered into the host's pool."),
                .authors = kAuthors.data(),
                .author_count = static_cast<std::uint32_t>(kAuthors.size()),
                .links = kLinks.data(),
                .link_count = static_cast<std::uint32_t>(kLinks.size()),
                .dependencies = nullptr,
                .dependency_count = 0,
                .min_host_version = plugin::str("0.1.0"),
                // Withdrawable live, but only once every radio it handed out
                // has been closed -- the registry refuses while one is open,
                // because that device's vtable lives in this image.
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
} // namespace sweeppp::pluto

SWEEPPP_PLUGIN_MAIN(sweeppp::pluto::PlutoPlugin, sweeppp::pluto::describe)
