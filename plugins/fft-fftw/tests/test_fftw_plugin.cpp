// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The FFTW plugin, through the real loader.
//
// It `dlopen`s the module it was built alongside rather than compiling the
// backend in, because there is an ABI here: the transform reaches the pipeline
// through `sweeppp_fft_backend_vtable_t`, and "the facet registers under the
// right name" is a promise about the built binary, not about the source.
//
// Nothing here needs hardware, so every case runs.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <numbers>
#include <string>
#include <sweeppp/fft/FftBackendManager.hpp>
#include <sweeppp/plugin/PluginAbi.h>
#include <sweeppp/plugin/PluginHost.hpp>
#include <vector>

using namespace sweeppp;

namespace {

namespace fs = std::filesystem;

constexpr double kPi = std::numbers::pi;

/// A temporary config root, so the enablement file a test writes cannot land
/// in the developer's own configuration.
class ScopedConfigRoot {
public:
    ScopedConfigRoot()
        : m_path(fs::temp_directory_path() / std::format("sweeppp-fftw-test-{}", ++s_counter)) {
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
        plugins, [](const PluginInfo& plugin) { return plugin.id == "org.sweeppp.fft-fftw"; });
    if (found == plugins.end()) {
        return nullptr;
    }
    // Returned by pointer into the manager's own storage rather than the
    // vector above, which dies with this call.
    static PluginInfo copy;
    copy = *found;
    return &copy;
}

/// The backend as the pipeline gets it: through the registry, across the ABI.
IFftBackend& backend() {
    REQUIRE(discover() != nullptr);
    auto acquired = FftBackendManager::instance().acquire("fftw");
    REQUIRE(acquired.has_value());
    return **acquired;
}

/// Naive O(N^2) DFT. Slow on purpose -- it shares no code with the backend, so
/// agreement between them is real evidence rather than the same bug twice.
std::vector<std::complex<double>> referenceDft(const std::vector<std::complex<float>>& input) {
    const std::size_t n = input.size();
    std::vector<std::complex<double>> output(n);

    for (std::size_t k = 0; k < n; ++k) {
        std::complex<double> sum{0.0, 0.0};
        for (std::size_t t = 0; t < n; ++t) {
            const double angle = -2.0 * kPi * static_cast<double>(k) * static_cast<double>(t) /
                                 static_cast<double>(n);
            sum += std::complex<double>(static_cast<double>(input[t].real()),
                                        static_cast<double>(input[t].imag())) *
                   std::complex<double>(std::cos(angle), std::sin(angle));
        }
        output[k] = sum;
    }
    return output;
}

} // namespace

TEST_CASE("the module loads at the host's own ABI version") {
    const ScopedConfigRoot root;

    const PluginInfo* plugin = discover();
    REQUIRE(plugin != nullptr);
    CHECK(plugin->loaded);
    CHECK(plugin->failureReason.empty());
    CHECK(plugin->abiVersion == SWEEPPP_PLUGIN_ABI_VERSION);
    CHECK(plugin->name == "FFTW");
    CHECK_FALSE(plugin->version.empty());
}

TEST_CASE("the facet id is exactly \"fftw\"") {
    // Its own test case, because this string is not a label.
    //
    // A saved profile persists the backend name, and the CLI takes the same
    // word as `--fft-backend fftw`. If the facet ever registered under
    // anything else -- the plugin's reverse-DNS id, say, or "FFTW" -- every
    // profile in the field would fail to resolve its backend, and the message
    // would be "no FFT backend named 'fftw'", which points nowhere near the
    // change that caused it.
    const ScopedConfigRoot root;

    const PluginInfo* plugin = discover();
    REQUIRE(plugin != nullptr);

    REQUIRE(plugin->facets.size() == 1);
    CHECK(plugin->facets[0].kind == PluginFacetKind::FftBackend);
    CHECK(plugin->facets[0].id == "fftw");
}

TEST_CASE("the backend reaches the registry with its capabilities intact") {
    const ScopedConfigRoot root;
    REQUIRE(discover() != nullptr);

    const auto info = FftBackendManager::instance().info("fftw");
    REQUIRE(info.has_value());
    CHECK(info->available);
    CHECK(info->displayName.starts_with("FFTW"));
    CHECK_FALSE(info->description.empty());

    // `thread_safe_execute` is the one the pipeline acts on: it is what lets
    // several workers share one plan instead of each building its own.
    CHECK(info->capabilities.threadSafeExecute);
    CHECK_FALSE(info->capabilities.threadSafePlanning);
    CHECK(info->capabilities.supportsBatch);
    CHECK(info->capabilities.sizeConstraint == FftSizeConstraint::Any);
    CHECK(info->capabilities.minSize == 2);
}

TEST_CASE("FFT matches a reference DFT") {
    const ScopedConfigRoot root;
    IFftBackend& fft = backend();

    for (const std::size_t size : {16U, 64U, 256U}) {
        CAPTURE(size);

        auto plan = fft.createPlan({.size = size, .quality = FftPlanQuality::Fast});
        REQUIRE(plan.has_value());

        // A deterministic but non-trivial signal: several tones plus a ramp,
        // so no symmetry can hide a sign or ordering error.
        std::vector<std::complex<float>> input(size);
        for (std::size_t i = 0; i < size; ++i) {
            const double t = static_cast<double>(i);
            input[i] = {static_cast<float>(std::sin(0.1 * t) + 0.5 * std::cos(0.37 * t) + 0.01 * t),
                        static_cast<float>(std::cos(0.23 * t) - 0.25 * std::sin(0.71 * t))};
        }

        std::vector<std::complex<float>> output(size);
        (*plan)->execute(input.data(), output.data());

        const std::vector<std::complex<double>> expected = referenceDft(input);

        for (std::size_t k = 0; k < size; ++k) {
            CAPTURE(k);
            CHECK(
                static_cast<double>(output[k].real()) ==
                doctest::Approx(expected[k].real()).epsilon(1e-4).scale(static_cast<double>(size)));
            CHECK(
                static_cast<double>(output[k].imag()) ==
                doctest::Approx(expected[k].imag()).epsilon(1e-4).scale(static_cast<double>(size)));
        }
    }
}

TEST_CASE("forward then inverse FFT round-trips") {
    const ScopedConfigRoot root;
    IFftBackend& fft = backend();
    constexpr std::size_t kSize = 1024;

    auto forward = fft.createPlan({.size = kSize, .quality = FftPlanQuality::Fast});
    auto inverse =
        fft.createPlan({.size = kSize, .inverse = true, .quality = FftPlanQuality::Fast});
    REQUIRE(forward.has_value());
    REQUIRE(inverse.has_value());

    std::vector<std::complex<float>> original(kSize);
    for (std::size_t i = 0; i < kSize; ++i) {
        const double t = static_cast<double>(i);
        original[i] = {static_cast<float>(std::sin(0.05 * t)),
                       static_cast<float>(std::cos(0.11 * t))};
    }

    std::vector<std::complex<float>> spectrum(kSize);
    std::vector<std::complex<float>> restored(kSize);
    (*forward)->execute(original.data(), spectrum.data());
    (*inverse)->execute(spectrum.data(), restored.data());

    // FFTW's transforms are unnormalised, so the round trip scales by N.
    const auto scale = 1.0F / static_cast<float>(kSize);
    for (std::size_t i = 0; i < kSize; ++i) {
        CAPTURE(i);
        CHECK(restored[i].real() * scale == doctest::Approx(original[i].real()).epsilon(1e-4));
        CHECK(restored[i].imag() * scale == doctest::Approx(original[i].imag()).epsilon(1e-4));
    }
}

TEST_CASE("batched execution matches single execution") {
    const ScopedConfigRoot root;
    IFftBackend& fft = backend();
    constexpr std::size_t kSize = 256;
    constexpr std::size_t kBatch = 8;

    auto plan =
        fft.createPlan({.size = kSize, .batchCount = kBatch, .quality = FftPlanQuality::Fast});
    REQUIRE(plan.has_value());

    std::vector<std::complex<float>> input(kSize * kBatch);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = {static_cast<float>(std::sin(0.017 * static_cast<double>(i))),
                    static_cast<float>(std::cos(0.029 * static_cast<double>(i)))};
    }

    std::vector<std::complex<float>> batched(kSize * kBatch);
    (*plan)->executeBatch(input.data(), batched.data(), kBatch);

    std::vector<std::complex<float>> individually(kSize * kBatch);
    for (std::size_t b = 0; b < kBatch; ++b) {
        (*plan)->execute(input.data() + b * kSize, individually.data() + b * kSize);
    }

    for (std::size_t i = 0; i < input.size(); ++i) {
        CAPTURE(i);
        CHECK(batched[i].real() == doctest::Approx(individually[i].real()).epsilon(1e-5));
        CHECK(batched[i].imag() == doctest::Approx(individually[i].imag()).epsilon(1e-5));
    }
}

TEST_CASE("size snapping respects backend constraints") {
    const ScopedConfigRoot root;
    IFftBackend& fft = backend();

    // FFTW accepts any size, so snapping is the identity above the minimum.
    CHECK(fft.supportsSize(4096));
    CHECK(fft.supportsSize(3000));
    CHECK_FALSE(fft.supportsSize(1));
    CHECK(fft.snapSize(1) >= fft.capabilities().minSize);
    CHECK(fft.snapSize(4096) == 4096);
    CHECK(fft.snapSize(3000) == 3000);
}

TEST_CASE("invalid plan requests are rejected rather than clamped") {
    const ScopedConfigRoot root;
    IFftBackend& fft = backend();

    CHECK_FALSE(fft.createPlan({.size = 0}).has_value());
    CHECK_FALSE(fft.createPlan({.size = 4096, .batchCount = 0}).has_value());
    CHECK_FALSE(fft.createPlan({.size = fft.capabilities().maxSize * 2}).has_value());
}
