// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The Fobos SDR driver, as a plugin.
//
// It links `sweeppp::sweeppp`, and only for values: SdrParameter, SampleFormat
// and Result. It must never reach a singleton -- see the rule at the top of
// <sweeppp/plugin/Plugin.hpp>. In particular it does NOT call
// SdrDeviceManager::instance(): the plugin's copy of that registry is not the
// host's, and registering into it would register into nothing.
#include "FobosDriver.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace sweeppp::fobos {
namespace {

constexpr std::string_view kPluginId = "org.sweeppp.sdr-fobos";

/// The facet id is the DRIVER NAME, and it must stay exactly "fobos".
///
/// A saved profile persists `device.driver`, and the CLI takes it as
/// `--device fobos`. Changing this string does not rename anything: it breaks
/// every profile in the field and every script anyone has written, with a
/// "no SDR driver named 'fobos'" that points nowhere near the cause.
constexpr std::string_view kDriverName = "fobos";

const std::array<sweeppp_facet_t, 1>& facets() {
    static const auto kSdrVtable = plugin::makeSdrDriverVtable<plugin::Driver>();
    static const std::array kFacets{
        plugin::facet(SWEEPPP_FACET_SDR_DEVICE, kDriverName, "Fobos SDR",
                      "Fobos SDR radios on either firmware, over RigExpert's libraries.",
                      &kSdrVtable),
    };
    return kFacets;
}

class FobosPlugin final : public plugin::Plugin {
public:
    [[nodiscard]] bool activate(plugin::Host& host) override {
        log() = host.as("fobos");

        const sweeppp_plugin_status_t status = host.registerFacet(facets()[0], &driver());
        if (status != SWEEPPP_PLUGIN_OK) {
            // Losing the name race is worth reporting and is not worth failing
            // over: the plugin still loaded, and the listing should say which
            // of the two claimants got the name rather than showing an
            // unexplained inactive row.
            host.reportFacet(facets()[0], "another driver already claims the name 'fobos'");
            return true;
        }

        log().debug("registered the '{}' driver", kDriverName);
        return true;
    }

    /// Deliberately empty. Neither library keeps process-wide state -- each
    /// open radio carries its own libusb context -- and every radio this handed
    /// out closes itself when destroyed.
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
                .name = plugin::str("Fobos SDR"),
                .description = plugin::str(
                    "Drives Fobos SDR radios on the standard and the agile firmware: 50 MHz to "
                    "6.9 GHz, calibrated float samples delivered into the host's pool."),
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
} // namespace sweeppp::fobos

SWEEPPP_PLUGIN_MAIN(sweeppp::fobos::FobosPlugin, sweeppp::fobos::describe)
