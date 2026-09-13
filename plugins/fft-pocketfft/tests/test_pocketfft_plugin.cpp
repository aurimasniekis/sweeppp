// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The PocketFFT plugin, through the real loader.
//
// It `dlopen`s the module it was built alongside rather than compiling the
// backend in, because there is an ABI here: the transform reaches the pipeline
// through `sweeppp_fft_backend_vtable_t`, and "the facet registers under the
// right name" is a promise about the built binary, not about the source.
//
// Nothing here needs hardware, so every case runs but the benchmark, which is
// skipped because it measures rather than checks.
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
#include <sweeppp/fft/FftBenchmark.hpp>
#include <sweeppp/fft/Window.hpp>
#include <sweeppp/pipeline/Pipeline.hpp>
#include <sweeppp/plugin/PluginAbi.h>
#include <sweeppp/plugin/PluginHost.hpp>
#include <thread>
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
        : m_path(fs::temp_directory_path() /
                 std::format("sweeppp-pocketfft-test-{}", ++s_counter)) {
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

/// Discovers the directory this plugin was built into. That directory holds
/// every built plugin -- `discover` scans it and does not filter by id -- which
/// is exactly what the benchmark below needs to reach the other backends too.
const PluginInfo* discover() {
    const std::array<fs::path, 1> directories{fs::path{SWEEPPP_PLUGIN_BINARY_DIR}};
    PluginManager::instance().discover(directories);

    const std::vector<PluginInfo> plugins = PluginManager::instance().enumerate();
    const auto found = std::ranges::find_if(
        plugins, [](const PluginInfo& plugin) { return plugin.id == "org.sweeppp.fft-pocketfft"; });
    if (found == plugins.end()) {
        return nullptr;
    }
    // Returned by pointer into storage that outlives this call, rather than
    // into the vector above, which dies with it.
    static PluginInfo copy;
    copy = *found;
    return &copy;
}

/// The backend as the pipeline gets it: through the registry, across the ABI.
IFftBackend& backend() {
    REQUIRE(discover() != nullptr);
    auto acquired = FftBackendManager::instance().acquire("pocketfft");
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

/// A deterministic but non-trivial signal: several tones plus a ramp, so no
/// symmetry can hide a sign or an ordering error.
std::vector<std::complex<float>> testSignal(std::size_t size) {
    std::vector<std::complex<float>> input(size);
    for (std::size_t i = 0; i < size; ++i) {
        const double t = static_cast<double>(i);
        input[i] = {static_cast<float>(std::sin(0.1 * t) + 0.5 * std::cos(0.37 * t) + 0.01 * t),
                    static_cast<float>(std::cos(0.23 * t) - 0.25 * std::sin(0.71 * t))};
    }
    return input;
}

} // namespace

TEST_CASE("the module loads at the host's own ABI version") {
    const ScopedConfigRoot root;

    const PluginInfo* plugin = discover();
    REQUIRE(plugin != nullptr);
    CHECK(plugin->loaded);
    CHECK(plugin->failureReason.empty());
    CHECK(plugin->abiVersion == SWEEPPP_PLUGIN_ABI_VERSION);
    CHECK(plugin->name == "PocketFFT");
    CHECK_FALSE(plugin->version.empty());
}

TEST_CASE("the facet id is exactly \"pocketfft\"") {
    // Its own test case, because this string is not a label: profiles persist
    // it and the CLI takes it as `--fft-backend pocketfft`.
    const ScopedConfigRoot root;

    const PluginInfo* plugin = discover();
    REQUIRE(plugin != nullptr);

    REQUIRE(plugin->facets.size() == 1);
    CHECK(plugin->facets[0].kind == PluginFacetKind::FftBackend);
    CHECK(plugin->facets[0].id == "pocketfft");
}

TEST_CASE("the backend reaches the registry with its capabilities intact") {
    const ScopedConfigRoot root;
    REQUIRE(discover() != nullptr);

    const auto info = FftBackendManager::instance().info("pocketfft");
    REQUIRE(info.has_value());
    CHECK(info->available);
    CHECK(info->displayName == "PocketFFT");
    CHECK_FALSE(info->description.empty());

    CHECK(info->capabilities.type == FftBackendType::Cpu);
    CHECK(info->capabilities.minSize == 2);
    CHECK(info->capabilities.maxSize == (1U << 24U));
    CHECK(info->capabilities.sizeConstraint == FftSizeConstraint::Any);
    CHECK_FALSE(info->capabilities.supportsBatch);
    CHECK(info->capabilities.supportsInPlace);
    CHECK(info->capabilities.threadSafeExecute);
    CHECK(info->capabilities.threadSafePlanning);
}

TEST_CASE("FFT matches a reference DFT") {
    const ScopedConfigRoot root;
    IFftBackend& fft = backend();

    // Powers of two, composites with odd factors, and 1021 -- a prime large
    // enough that PocketFFT takes its Bluestein path rather than FFTPACK's.
    for (const std::size_t size : {16U, 24U, 64U, 100U, 256U, 1021U}) {
        CAPTURE(size);

        auto plan = fft.createPlan({.size = size, .quality = FftPlanQuality::Fast});
        REQUIRE(plan.has_value());

        const std::vector<std::complex<float>> input = testSignal(size);
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

TEST_CASE("one plan executes concurrently from every worker") {
    // One plan, every thread, separate buffers -- which is what the pipeline
    // does with the plan it builds per FFT size. A plan that kept scratch of
    // its own would have two threads producing each other's spectra here.
    const ScopedConfigRoot root;
    IFftBackend& fft = backend();

    constexpr std::size_t kSize = 4096;
    constexpr std::size_t kThreads = 8;
    constexpr int kRepeats = 32;

    auto plan = fft.createPlan({.size = kSize, .quality = FftPlanQuality::Fast});
    REQUIRE(plan.has_value());

    // A different signal per thread, so a plan that mixed two threads' work
    // together fails rather than producing the same right answer twice.
    std::vector<std::vector<std::complex<float>>> inputs(kThreads);
    std::vector<std::vector<std::complex<float>>> expected(kThreads);
    for (std::size_t t = 0; t < kThreads; ++t) {
        inputs[t].resize(kSize);
        for (std::size_t i = 0; i < kSize; ++i) {
            const double x = static_cast<double>(i) * (1.0 + static_cast<double>(t));
            inputs[t][i] = {static_cast<float>(std::sin(0.013 * x)),
                            static_cast<float>(std::cos(0.021 * x))};
        }
        expected[t].resize(kSize);
        (*plan)->execute(inputs[t].data(), expected[t].data());
    }

    std::vector<std::vector<std::complex<float>>> outputs(kThreads);
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (std::size_t t = 0; t < kThreads; ++t) {
        outputs[t].resize(kSize);
        workers.emplace_back([&plan, &inputs, &outputs, t] {
            for (int r = 0; r < kRepeats; ++r) {
                (*plan)->execute(inputs[t].data(), outputs[t].data());
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    // Bit-exact, not approximate: the same plan over the same input must
    // produce the same bytes whatever thread ran it.
    for (std::size_t t = 0; t < kThreads; ++t) {
        CAPTURE(t);
        CHECK(outputs[t] == expected[t]);
    }
}

TEST_CASE("forward then inverse FFT round-trips") {
    const ScopedConfigRoot root;
    IFftBackend& fft = backend();

    // A power of two, a composite FFTPACK factors, and a Bluestein prime.
    for (const std::size_t size : {1024U, 3000U, 1021U}) {
        CAPTURE(size);

        auto forward = fft.createPlan({.size = size, .quality = FftPlanQuality::Fast});
        auto inverse =
            fft.createPlan({.size = size, .inverse = true, .quality = FftPlanQuality::Fast});
        REQUIRE(forward.has_value());
        REQUIRE(inverse.has_value());

        std::vector<std::complex<float>> original(size);
        for (std::size_t i = 0; i < size; ++i) {
            const double t = static_cast<double>(i);
            original[i] = {static_cast<float>(std::sin(0.05 * t)),
                           static_cast<float>(std::cos(0.11 * t))};
        }

        std::vector<std::complex<float>> spectrum(size);
        std::vector<std::complex<float>> restored(size);
        (*forward)->execute(original.data(), spectrum.data());
        (*inverse)->execute(spectrum.data(), restored.data());

        // Unnormalised in both directions, so the round trip scales by exactly
        // N -- FFTW's convention, and the one `dsp::magnitudeToDbfs` assumes.
        const auto scale = 1.0F / static_cast<float>(size);
        for (std::size_t i = 0; i < size; ++i) {
            CAPTURE(i);
            CHECK(restored[i].real() * scale == doctest::Approx(original[i].real()).epsilon(1e-4));
            CHECK(restored[i].imag() * scale == doctest::Approx(original[i].imag()).epsilon(1e-4));
        }
    }
}

TEST_CASE("a synthetic tone lands in the right bin at the right level") {
    // The scaling check, and the only one here that would catch a silent
    // constant dB offset: a backend applying a 1/N or a 2x scale somewhere
    // still matches its own inverse and still peaks in the right bin.
    const ScopedConfigRoot root;
    IFftBackend& fft = backend();
    constexpr std::size_t kSize = 8192;
    constexpr double kSampleRate = 20e6;
    constexpr int kToneBin = 1234;
    constexpr double kAmplitude = 0.5;

    const auto window = Window::create(WindowType::Hann, kSize);
    REQUIRE(window.has_value());

    auto plan = fft.createPlan({.size = kSize, .quality = FftPlanQuality::Fast});
    REQUIRE(plan.has_value());

    // Exactly on a bin centre, so there is no scalloping error to account for
    // and the measured level must match the theoretical one directly.
    const double toneHz = kSampleRate * kToneBin / static_cast<double>(kSize);

    std::vector<std::complex<float>> input(kSize);
    for (std::size_t i = 0; i < kSize; ++i) {
        const double phase = 2.0 * kPi * toneHz * static_cast<double>(i) / kSampleRate;
        const auto w = static_cast<double>(window->coefficients()[i]);
        input[i] = {static_cast<float>(kAmplitude * std::cos(phase) * w),
                    static_cast<float>(kAmplitude * std::sin(phase) * w)};
    }

    std::vector<std::complex<float>> spectrum(kSize);
    (*plan)->execute(input.data(), spectrum.data());

    std::size_t peakBin = 0;
    double peakMagnitude = 0.0;
    for (std::size_t k = 0; k < kSize; ++k) {
        const double magnitude = std::abs(std::complex<double>(
            static_cast<double>(spectrum[k].real()), static_cast<double>(spectrum[k].imag())));
        if (magnitude > peakMagnitude) {
            peakMagnitude = magnitude;
            peakBin = k;
        }
    }

    CHECK(peakBin == static_cast<std::size_t>(kToneBin));

    const double recovered = peakMagnitude * static_cast<double>(window->amplitudeScale());
    CHECK(recovered == doctest::Approx(kAmplitude).epsilon(1e-3));
}

TEST_CASE("batched execution matches single execution") {
    // `supportsBatch = false`, so `executeBatch` loops over `execute`. It is
    // still on the interface and the host may still call it.
    const ScopedConfigRoot root;
    IFftBackend& fft = backend();
    constexpr std::size_t kSize = 256;
    constexpr std::size_t kBatch = 8;

    auto plan = fft.createPlan({.size = kSize, .quality = FftPlanQuality::Fast});
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

    CHECK(batched == individually);
}

TEST_CASE("in-place execution matches out-of-place execution") {
    // PocketFFT only transforms in place; out-of-place is a copy first. Both
    // run the same arithmetic on the same values, so they must agree exactly.
    const ScopedConfigRoot root;
    IFftBackend& fft = backend();
    constexpr std::size_t kSize = 3000;

    auto plan = fft.createPlan({.size = kSize, .inPlace = true, .quality = FftPlanQuality::Fast});
    REQUIRE(plan.has_value());

    const std::vector<std::complex<float>> input = testSignal(kSize);

    std::vector<std::complex<float>> outOfPlace(kSize);
    (*plan)->execute(input.data(), outOfPlace.data());

    std::vector<std::complex<float>> inPlace = input;
    (*plan)->execute(inPlace.data(), inPlace.data());

    CHECK(inPlace == outOfPlace);
}

TEST_CASE("any size in range is accepted as it is") {
    const ScopedConfigRoot root;
    IFftBackend& fft = backend();

    CHECK(fft.supportsSize(4096));
    CHECK(fft.supportsSize(3000));
    CHECK(fft.supportsSize(1021));
    CHECK(fft.supportsSize(2));
    CHECK_FALSE(fft.supportsSize(1));

    // Not rounded to a power of two: the sweep planner calls this on
    // sampleRate / RBW, and an exact size is the resolution asked for.
    CHECK(fft.snapSize(3000) == 3000);
    CHECK(fft.snapSize(1) == 2);

    CHECK(fft.createPlan({.size = 3000}).has_value());
    CHECK_FALSE(fft.createPlan({.size = 1}).has_value());
    CHECK_FALSE(fft.createPlan({.size = 0}).has_value());
    CHECK_FALSE(fft.createPlan({.size = 4096, .batchCount = 0}).has_value());
    CHECK_FALSE(fft.createPlan({.size = fft.capabilities().maxSize * 2}).has_value());
}

// ------------------------------------------------------------ benchmark

// No comma in the name: doctest splits `-tc=` filter values on commas, so a
// case with one in its title cannot be selected by name at all.
TEST_CASE("PocketFFT against the other backends per transform" * doctest::skip()) {
    // Skipped by default: this measures rather than checks, and a timing
    // assertion on a shared machine is a flaky test. Run it with
    //     sweeppp-plugin-fft-pocketfft-tests --no-skip
    //         -tc="PocketFFT against the other backends per transform"
    //
    // Runs `benchmarkFftBackends` -- the same call the FFT panel's Benchmark
    // button makes -- so a table printed here and one drawn in the GUI cannot
    // disagree.
    const ScopedConfigRoot root;
    REQUIRE(discover() != nullptr);

    // At one thread and at the pipeline's worker count: PocketFFT allocates
    // scratch per transform, and eight workers allocating at once is where
    // that shows.
    FftBenchmarkConfig config = defaultFftBenchmarkConfig();
    config.threadCounts = {1, defaultWorkerCount()};
    config.sizes = {1024, 4096, 16384, 65536, 262144, 1048576};

    const std::vector<FftBenchmarkEntry> results = benchmarkFftBackends(config);
    REQUIRE_FALSE(results.empty());

    const auto pocketfft = std::ranges::find_if(
        results, [](const FftBenchmarkEntry& e) { return e.backend == "pocketfft"; });
    REQUIRE(pocketfft != results.end());
    if (results.size() == 1) {
        MESSAGE("only one backend is registered; there is nothing to compare against");
    }

    MESSAGE(std::format("{:>8} {:>3}  {:>12}  {:>10}  {:>10}  {:>10}  {:>6}", "size", "thr",
                        "backend", "p50 (us)", "p99 (us)", "FFT/s", "cores"));
    for (const FftBenchmarkEntry& entry : results) {
        CAPTURE(entry.backend);
        CHECK(entry.error.empty());
        for (const FftBenchmarkSample& sample : entry.samples) {
            if (!sample.skipped.empty()) {
                MESSAGE(std::format("{:>8} {:>3}  {:>12}  {}", sample.size, sample.threads,
                                    entry.backend, sample.skipped));
                continue;
            }
            MESSAGE(std::format("{:>8} {:>3}  {:>12}  {:>10.3f}  {:>10.3f}  {:>10.0f}  {:>6.2f}",
                                sample.size, sample.threads, entry.backend, sample.p50Seconds * 1e6,
                                sample.p99Seconds * 1e6, sample.throughputPerSecond,
                                sample.cpuCores));
        }
    }
}
