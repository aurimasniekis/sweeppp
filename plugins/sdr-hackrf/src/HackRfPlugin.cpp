// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The HackRF driver, as a plugin.
//
// It used to be compiled into libsweeppp, which linked libhackrf into every
// binary the project produces -- the GUI, the CLI, the server, the test suite
// -- whether or not a HackRF was ever attached. Here it is one module that is
// loaded when it is present and simply absent when it is not.
//
// It links `sweeppp::sweeppp`, and only for values: SdrParameter, SampleFormat
// and Result. It must never reach a singleton -- see the rule at the top of
// <sweeppp/plugin/Plugin.hpp>. In particular it does NOT call
// SdrDeviceManager::instance(): the plugin's copy of that registry is not the
// host's, and registering into it would register into nothing.
#include "HackRfDriver.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace sweeppp::hackrf {
namespace {

constexpr std::string_view kPluginId = "org.sweeppp.sdr-hackrf";

/// The facet id is the DRIVER NAME, and it must stay exactly "hackrf".
///
/// A saved profile persists `device.driver`, and the CLI takes it as
/// `--device hackrf`. Changing this string does not rename anything: it breaks
/// every profile in the field and every script anyone has written, with a
/// "no SDR driver named 'hackrf'" that points nowhere near the cause.
constexpr std::string_view kDriverName = "hackrf";

const std::array<sweeppp_facet_t, 1>& facets() {
    static const auto kSdrVtable = plugin::makeSdrDriverVtable<plugin::Driver>();
    static const std::array kFacets{
        plugin::facet(SWEEPPP_FACET_SDR_DEVICE, kDriverName, "HackRF",
                      "HackRF One and compatible radios, over libhackrf.", &kSdrVtable),
    };
    return kFacets;
}

class HackRfPlugin final : public plugin::Plugin {
public:
    [[nodiscard]] bool activate(plugin::Host& host) override {
        log() = host.as("hackrf");

        const sweeppp_plugin_status_t status = host.registerFacet(facets()[0], &driver());
        if (status != SWEEPPP_PLUGIN_OK) {
            // Losing the name race is worth reporting and is not worth failing
            // over: the plugin still loaded, and the listing should say which
            // of the two claimants got the name rather than showing an
            // unexplained inactive row.
            host.reportFacet(facets()[0], "another driver already claims the name 'hackrf'");
            return true;
        }

        log().debug("registered the '{}' driver", kDriverName);
        return true;
    }

    /// Deliberately empty.
    ///
    /// `shutdown()` withdraws every facet whether or not a device is still
    /// open, so calling `hackrf_exit()` here would pull the library out from
    /// under a live radio. The device destructor releases it instead, and that
    /// release is reference counted across every open device -- which is what
    /// makes the last one closing the right moment rather than this one.
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
                .name = plugin::str("HackRF"),
                .description = plugin::str(
                    "Drives HackRF One and compatible radios over libhackrf: 1 MHz to 6 GHz, "
                    "CS8 delivered straight into the host's pool."),
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
} // namespace sweeppp::hackrf

SWEEPPP_PLUGIN_MAIN(sweeppp::hackrf::HackRfPlugin, sweeppp::hackrf::describe)
