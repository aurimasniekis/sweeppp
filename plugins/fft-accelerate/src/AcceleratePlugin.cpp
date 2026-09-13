// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The Accelerate/vDSP backend, as a plugin.
//
// Accelerate is a macOS system framework: nothing is fetched, nothing is
// probed, and the module carries no third-party licence of any kind. On macOS
// that makes a `-DSWEEPPP_WITH_FFTW=OFF -DSWEEPPP_WITH_HACKRF=OFF` install the
// first one with a working transform and no GPL module in it -- see
// THIRD_PARTY.md.
//
// It links `sweeppp::sweeppp`, and only for values: IFftBackend, IFftPlan and
// Result. It must never reach a singleton -- see the rule at the top of
// <sweeppp/plugin/Plugin.hpp>. In particular it does NOT call
// FftBackendManager::instance(): the plugin's copy of that registry is not the
// host's, and registering into it would register into nothing.
#include "AccelerateBackend.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace sweeppp::accel {
namespace {

constexpr std::string_view kPluginId = "org.sweeppp.fft-accelerate";

/// The facet id is the BACKEND NAME, and it must stay exactly "accelerate".
///
/// A saved profile persists the backend name, and the CLI takes it as
/// `--fft-backend accelerate`. Changing this string does not rename anything:
/// it breaks every profile in the field and every script anyone has written,
/// with a "no FFT backend named 'accelerate'" that points nowhere near the
/// cause.
constexpr std::string_view kBackendName = "accelerate";

/// No version to report. vDSP exposes none, and the framework's is the OS's --
/// which `sweeppp-cli info` already prints beside the platform.
constexpr std::string_view kDisplayName = "Accelerate / vDSP";

AccelerateBackend& backend() {
    static AccelerateBackend instance;
    return instance;
}

const std::array<sweeppp_facet_t, 1>& facets() {
    static const auto kFftVtable = plugin::makeFftBackendVtable<AccelerateBackend>();
    static const std::array kFacets{
        plugin::facet(SWEEPPP_FACET_FFT_BACKEND, kBackendName, kDisplayName,
                      "Apple's system DSP library. One setup is shared by every worker, which "
                      "vDSP documents as safe. Power-of-two sizes only, so achievable "
                      "resolution bandwidth is coarser than a backend accepting any size, and "
                      "planning is instant at every quality because vDSP does not measure.",
                      &kFftVtable),
    };
    return kFacets;
}

class AcceleratePlugin final : public plugin::Plugin {
public:
    [[nodiscard]] bool activate(plugin::Host& host) override {
        log() = host.as("fft");

        const sweeppp_plugin_status_t status = host.registerFacet(facets()[0], &backend());
        if (status != SWEEPPP_PLUGIN_OK) {
            // Losing the name race is worth reporting and is not worth failing
            // over: the plugin still loaded, and the listing should say which
            // of the two claimants got the name rather than showing an
            // unexplained inactive row.
            host.reportFacet(facets()[0], "another backend already claims the name 'accelerate'");
            return true;
        }

        log().debug("registered the '{}' backend", kBackendName);
        return true;
    }

    /// Deliberately empty.
    ///
    /// The registry already refuses to withdraw a backend the pipeline has
    /// acquired, because every plan made from it has a vtable in this image;
    /// that refusal is what becomes the "restart needed" marker. There is
    /// nothing process-wide of vDSP's to tear down besides -- a setup's memory
    /// dies with the plan that owns it.
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
                .name = plugin::str("Accelerate"),
                .description = plugin::str(
                    "The pipeline's transform over Accelerate's modern vDSP DFT interface: one "
                    "setup per size, executed concurrently by every worker. Sizes up to 4096 "
                    "run through vDSP's interleaved entry point and copy nothing; larger ones "
                    "deinterleave into split-complex scratch, because that is the only form "
                    "vDSP offers above 4096. Setting SWEEPPP_ACCELERATE_FORCE_SPLIT=1 forces "
                    "the split path everywhere, which is how the two are compared."),
                .authors = kAuthors.data(),
                .author_count = static_cast<std::uint32_t>(kAuthors.size()),
                .links = kLinks.data(),
                .link_count = static_cast<std::uint32_t>(kLinks.size()),
                .dependencies = nullptr,
                .dependency_count = 0,
                .min_host_version = plugin::str("0.1.0"),
                // Withdrawable live, but only once nothing holds a plan made
                // from it -- the registry refuses while the pipeline has it,
                // because those plans' vtables live in this image.
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
} // namespace sweeppp::accel

SWEEPPP_PLUGIN_MAIN(sweeppp::accel::AcceleratePlugin, sweeppp::accel::describe)
