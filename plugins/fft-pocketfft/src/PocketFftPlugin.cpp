// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The PocketFFT backend, as a plugin.
//
// PocketFFT is BSD-3-Clause and header-only, fetched at build time. Off macOS
// it is what makes a `-DSWEEPPP_WITH_FFTW=OFF -DSWEEPPP_WITH_HACKRF=OFF
// -DSWEEPPP_WITH_RTLSDR=OFF` install able to compute a spectrum with no GPL
// module in it -- see THIRD_PARTY.md.
//
// It links `sweeppp::sweeppp`, and only for values: IFftBackend, IFftPlan and
// Result. It must never reach a singleton -- see the rule at the top of
// <sweeppp/plugin/Plugin.hpp>. In particular it does NOT call
// FftBackendManager::instance(): the plugin's copy of that registry is not the
// host's, and registering into it would register into nothing.
#include "PocketFftBackend.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace sweeppp::pocket {
namespace {

constexpr std::string_view kPluginId = "org.sweeppp.fft-pocketfft";

/// The facet id is the BACKEND NAME, and it must stay exactly "pocketfft".
///
/// A saved profile persists the backend name, and the CLI takes it as
/// `--fft-backend pocketfft`. Changing this string breaks every profile and
/// script naming it, with a "no FFT backend named 'pocketfft'" that points
/// nowhere near the cause.
constexpr std::string_view kBackendName = "pocketfft";

/// No version: upstream has no releases, and the pinned commit is in
/// THIRD_PARTY.md.
constexpr std::string_view kDisplayName = "PocketFFT";

PocketFftBackend& backend() {
    static PocketFftBackend instance;
    return instance;
}

const std::array<sweeppp_facet_t, 1>& facets() {
    static const auto kFftVtable = plugin::makeFftBackendVtable<PocketFftBackend>();
    static const std::array kFacets{
        plugin::facet(SWEEPPP_FACET_FFT_BACKEND, kBackendName, kDisplayName,
                      "A BSD-licensed transform with no dependencies, built on every platform. "
                      "Any size is accepted, so resolution bandwidth is not rounded to a power "
                      "of two, and planning is instant at every quality because nothing is "
                      "measured. One plan is shared by every worker.",
                      &kFftVtable),
    };
    return kFacets;
}

class PocketFftPlugin final : public plugin::Plugin {
public:
    [[nodiscard]] bool activate(plugin::Host& host) override {
        log() = host.as("fft");

        const sweeppp_plugin_status_t status = host.registerFacet(facets()[0], &backend());
        if (status != SWEEPPP_PLUGIN_OK) {
            // Losing the name race is worth reporting and is not worth failing
            // over: the plugin still loaded, and the listing should say which
            // of the two claimants got the name.
            host.reportFacet(facets()[0], "another backend already claims the name 'pocketfft'");
            return true;
        }

        log().debug("registered the '{}' backend", kBackendName);
        return true;
    }

    /// Deliberately empty.
    ///
    /// The registry already refuses to withdraw a backend the pipeline has
    /// acquired, because every plan made from it has a vtable in this image.
    /// PocketFFT keeps nothing process-wide besides: a plan's tables die with
    /// the plan.
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
                .name = plugin::str("PocketFFT"),
                .description = plugin::str(
                    "The pipeline's transform, over BSD-licensed PocketFFT: any size, one plan "
                    "per size, executed concurrently by every worker."),
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
} // namespace sweeppp::pocket

SWEEPPP_PLUGIN_MAIN(sweeppp::pocket::PocketFftPlugin, sweeppp::pocket::describe)
