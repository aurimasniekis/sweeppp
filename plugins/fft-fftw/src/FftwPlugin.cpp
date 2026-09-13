// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The FFTW backend, as a plugin.
//
// FFTW is GPL-2.0-or-later and is linked here and nowhere else, which is what
// leaves the application it loads into linking no GPL component.
//
// It links `sweeppp::sweeppp`, and only for values: IFftBackend, IFftPlan and
// Result. It must never reach a singleton -- see the rule at the top of
// <sweeppp/plugin/Plugin.hpp>. In particular it does NOT call
// FftBackendManager::instance(): the plugin's copy of that registry is not the
// host's, and registering into it would register into nothing.
#include "FftwBackend.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace sweeppp::fftw {
namespace {

constexpr std::string_view kPluginId = "org.sweeppp.fft-fftw";

/// The facet id is the BACKEND NAME, and it must stay exactly "fftw".
///
/// A saved profile persists the backend name, and the CLI takes it as
/// `--fft-backend fftw`. Changing this string does not rename anything: it
/// breaks every profile in the field and every script anyone has written, with
/// a "no FFT backend named 'fftw'" that points nowhere near the cause.
constexpr std::string_view kBackendName = "fftw";

/// The version the build pinned, so the FFT panel and `sweeppp-cli info` name
/// the copy actually linked rather than "FFTW" and nothing else. Spelled as a
/// literal because everything reachable from a facet must have static storage
/// duration, which a formatted string would not.
#ifdef SWEEPPP_FFTW_VERSION
constexpr std::string_view kDisplayName = "FFTW " SWEEPPP_FFTW_VERSION;
#else
constexpr std::string_view kDisplayName = "FFTW 3";
#endif

FftwBackend& backend() {
    static FftwBackend instance;
    return instance;
}

const std::array<sweeppp_facet_t, 1>& facets() {
    static const auto kFftVtable = plugin::makeFftBackendVtable<FftwBackend>();
    static const std::array kFacets{
        plugin::facet(SWEEPPP_FACET_FFT_BACKEND, kBackendName, kDisplayName,
                      "Mature, well-optimised CPU FFT with NEON/AVX kernels. Plans are "
                      "measured once and shared across all worker threads.",
                      &kFftVtable),
    };
    return kFacets;
}

class FftwPlugin final : public plugin::Plugin {
public:
    [[nodiscard]] bool activate(plugin::Host& host) override {
        log() = host.as("fft");

        const sweeppp_plugin_status_t status = host.registerFacet(facets()[0], &backend());
        if (status != SWEEPPP_PLUGIN_OK) {
            // Losing the name race is worth reporting and is not worth failing
            // over: the plugin still loaded, and the listing should say which
            // of the two claimants got the name rather than showing an
            // unexplained inactive row.
            host.reportFacet(facets()[0], "another backend already claims the name 'fftw'");
            return true;
        }

        log().debug("registered the '{}' backend", kBackendName);
        return true;
    }

    /// Deliberately empty.
    ///
    /// The registry already refuses to withdraw a backend the pipeline has
    /// acquired, because every plan made from it has a vtable in this image;
    /// that refusal is what becomes the "restart needed" marker. Calling
    /// fftwf_cleanup() here would instead pull the planner's tables out from
    /// under plans that are still executing.
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
                .name = plugin::str("FFTW"),
                .description = plugin::str(
                    "The pipeline's transform, over FFTW's single-precision interface: one "
                    "measured plan per size, executed concurrently by every worker."),
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
} // namespace sweeppp::fftw

SWEEPPP_PLUGIN_MAIN(sweeppp::fftw::FftwPlugin, sweeppp::fftw::describe)
