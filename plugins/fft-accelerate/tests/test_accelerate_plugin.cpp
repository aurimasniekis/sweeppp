// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The Accelerate plugin, through the real loader.
//
// It `dlopen`s the module it was built alongside rather than compiling the
// backend in, because there is an ABI here: the transform reaches the pipeline
// through `sweeppp_fft_backend_vtable_t`, and "the facet registers under the
// right name" is a promise about the built binary, not about the source.
//
// Nothing here needs hardware, so every case runs but the benchmark, which is
// skipped because it measures rather than checks.
//
// Tolerances rather than bit-exactness throughout: vDSP.h states its routines
// are not expected to conform to IEEE 754 or to be correctly rounded.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdlib>
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
                 std::format("sweeppp-accelerate-test-{}", ++s_counter)) {
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

/// Sets SWEEPPP_ACCELERATE_FORCE_SPLIT for as long as it is alive.
///
/// The backend reads the variable inside `createPlan` rather than caching it,
/// which is what lets one test binary exercise both plan strategies. Without
/// this the split path is dead code on any macOS 12 machine.
class ScopedForceSplit {
public:
    ScopedForceSplit() { ::setenv("SWEEPPP_ACCELERATE_FORCE_SPLIT", "1", 1); }
    ~ScopedForceSplit() { ::unsetenv("SWEEPPP_ACCELERATE_FORCE_SPLIT"); }

    ScopedForceSplit(const ScopedForceSplit&) = delete;
    ScopedForceSplit& operator=(const ScopedForceSplit&) = delete;
};

/// Discovers the directory this plugin was built into. That directory holds
/// every built plugin -- `discover` scans it and does not filter by id -- which
/// is exactly what the benchmark below needs to reach FFTW as well.
const PluginInfo* discover() {
    const std::array<fs::path, 1> directories{fs::path{SWEEPPP_PLUGIN_BINARY_DIR}};
    PluginManager::instance().discover(directories);

    const std::vector<PluginInfo> plugins = PluginManager::instance().enumerate();
    const auto found = std::ranges::find_if(plugins, [](const PluginInfo& plugin) {
        return plugin.id == "org.sweeppp.fft-accelerate";
    });
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
    auto acquired = FftBackendManager::instance().acquire("accelerate");
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

void checkAgainstReferenceDft(IFftBackend& fft) {
    for (const std::size_t size : {16U, 64U, 256U}) {
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

/// One plan, every thread, separate buffers -- which is exactly what the
/// pipeline does with the plan it builds per FFT size.
///
/// This is the only place the `threadSafeExecute` promise is actually
/// exercised: the pipeline's own suite runs against the in-tree `reference`
/// backend, so nothing else in the repository ever executes a *plugin* plan
/// from several threads at once. Under TSan it is what turns a sentence in
/// vDSP.h into evidence; in an ordinary build it still catches a plan that
/// kept scratch of its own, because two threads sharing that scratch produce
/// each other's spectra.
void checkConcurrentExecute(IFftBackend& fft) {
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
    // produce the same bytes whatever thread ran it. Anything else is a race,
    // not a rounding difference.
    for (std::size_t t = 0; t < kThreads; ++t) {
        CAPTURE(t);
        CHECK(outputs[t] == expected[t]);
    }
}

} // namespace

TEST_CASE("the module loads at the host's own ABI version") {
    const ScopedConfigRoot root;

    const PluginInfo* plugin = discover();
    REQUIRE(plugin != nullptr);
    CHECK(plugin->loaded);
    CHECK(plugin->failureReason.empty());
    CHECK(plugin->abiVersion == SWEEPPP_PLUGIN_ABI_VERSION);
    CHECK(plugin->name == "Accelerate");
    CHECK_FALSE(plugin->version.empty());
}

TEST_CASE("the facet id is exactly \"accelerate\"") {
    // Its own test case, because this string is not a label.
    //
    // A saved profile persists the backend name, and the CLI takes the same
    // word as `--fft-backend accelerate`. If the facet ever registered under
    // anything else, every profile naming it would fail to resolve with a "no
    // FFT backend named 'accelerate'" that points nowhere near the change that
    // caused it.
    const ScopedConfigRoot root;

    const PluginInfo* plugin = discover();
    REQUIRE(plugin != nullptr);

    REQUIRE(plugin->facets.size() == 1);
    CHECK(plugin->facets[0].kind == PluginFacetKind::FftBackend);
    CHECK(plugin->facets[0].id == "accelerate");
}

TEST_CASE("the backend reaches the registry with its capabilities intact") {
    const ScopedConfigRoot root;
    REQUIRE(discover() != nullptr);

    const auto info = FftBackendManager::instance().info("accelerate");
    REQUIRE(info.has_value());
    CHECK(info->available);
    CHECK(info->displayName == "Accelerate / vDSP");
    CHECK_FALSE(info->description.empty());

    // `threadSafeExecute` is the one the pipeline acts on: it is what lets
    // several workers share one setup instead of each building its own, and
    // vDSP.h is where the promise comes from.
    CHECK(info->capabilities.threadSafeExecute);
    CHECK_FALSE(info->capabilities.threadSafePlanning);
    CHECK_FALSE(info->capabilities.supportsBatch);
    CHECK(info->capabilities.sizeConstraint == FftSizeConstraint::PowerOfTwo);
    CHECK(info->capabilities.minSize == 8);
}

TEST_CASE("FFT matches a reference DFT") {
    const ScopedConfigRoot root;
    checkAgainstReferenceDft(backend());
}

TEST_CASE("FFT matches a reference DFT on the split-complex fallback") {
    // The same check with the fallback forced, because otherwise nothing on a
    // macOS 12 or later machine ever executes it -- the interleaved setup
    // accepts every size this case asks for.
    const ScopedConfigRoot root;
    const ScopedForceSplit split;
    checkAgainstReferenceDft(backend());
}

TEST_CASE("one plan executes concurrently from every worker") {
    const ScopedConfigRoot root;
    checkConcurrentExecute(backend());
}

TEST_CASE("one plan executes concurrently on the split-complex fallback") {
    // The split path is where a shared plan could actually race: it needs
    // scratch, and the scratch is thread-local precisely so that it does not.
    // Moving those four arrays onto the plan would still pass every other case
    // in this file and fail here.
    const ScopedConfigRoot root;
    const ScopedForceSplit split;
    checkConcurrentExecute(backend());
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

    // vDSP's modern complex-to-complex DFT applies no scale in either
    // direction, so the round trip scales by exactly N -- the same convention
    // FFTW uses, and the reason `dsp::magnitudeToDbfs` needs no per-backend
    // adjustment. The legacy `vDSP_fft_zip` would have scaled its inverse by
    // 1/N and made this factor 1.
    const auto scale = 1.0F / static_cast<float>(kSize);
    for (std::size_t i = 0; i < kSize; ++i) {
        CAPTURE(i);
        CHECK(restored[i].real() * scale == doctest::Approx(original[i].real()).epsilon(1e-4));
        CHECK(restored[i].imag() * scale == doctest::Approx(original[i].imag()).epsilon(1e-4));
    }
}

TEST_CASE("a size past the interleaved ceiling still plans and round-trips") {
    // The automatic fallback, with nothing forced.
    //
    // vDSP.h describes the interleaved setup as accepting f * 2^n for n >= 2,
    // but the shipped library declines everything above 4096 and returns NULL
    // with no error code. Every large sweep therefore lands on the split path
    // in ordinary use -- it is not an old-macOS curiosity -- and this is the
    // case that fails if the fallback is ever dropped as dead code.
    const ScopedConfigRoot root;
    IFftBackend& fft = backend();
    constexpr std::size_t kSize = 65536;

    auto forward = fft.createPlan({.size = kSize, .quality = FftPlanQuality::Fast});
    auto inverse =
        fft.createPlan({.size = kSize, .inverse = true, .quality = FftPlanQuality::Fast});
    REQUIRE(forward.has_value());
    REQUIRE(inverse.has_value());

    std::vector<std::complex<float>> original(kSize);
    for (std::size_t i = 0; i < kSize; ++i) {
        const double t = static_cast<double>(i);
        original[i] = {static_cast<float>(std::sin(0.0007 * t)),
                       static_cast<float>(std::cos(0.0013 * t))};
    }

    std::vector<std::complex<float>> spectrum(kSize);
    std::vector<std::complex<float>> restored(kSize);
    (*forward)->execute(original.data(), spectrum.data());
    (*inverse)->execute(spectrum.data(), restored.data());

    const auto scale = 1.0F / static_cast<float>(kSize);
    for (std::size_t i = 0; i < kSize; ++i) {
        CAPTURE(i);
        CHECK(restored[i].real() * scale == doctest::Approx(original[i].real()).epsilon(1e-3));
        CHECK(restored[i].imag() * scale == doctest::Approx(original[i].imag()).epsilon(1e-3));
    }
}

TEST_CASE("a synthetic tone lands in the right bin at the right level") {
    // The scaling check, and the only one here that would catch a silent
    // constant dB offset: a backend applying a 1/N or a 2x scale somewhere
    // still matches its own inverse and still peaks in the right bin. Only the
    // absolute level moves, and every reading in the application with it.
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
    // The backend declares `supportsBatch = false` -- batching exists only in
    // the legacy vDSP_fftm_* family -- so `executeBatch` loops over `execute`.
    // It is still on the interface and the host may still call it, so it is
    // still checked.
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

    for (std::size_t i = 0; i < input.size(); ++i) {
        CAPTURE(i);
        CHECK(batched[i].real() == doctest::Approx(individually[i].real()).epsilon(1e-5));
        CHECK(batched[i].imag() == doctest::Approx(individually[i].imag()).epsilon(1e-5));
    }
}

TEST_CASE("a non-power-of-two size is refused rather than rounded") {
    const ScopedConfigRoot root;
    IFftBackend& fft = backend();

    CHECK(fft.supportsSize(4096));
    CHECK_FALSE(fft.supportsSize(3000));
    CHECK_FALSE(fft.supportsSize(4));

    // Refused at `createPlan` too, not merely reported unsupported. A plan
    // that quietly rounded would give the pipeline a transform of a different
    // length than the one it computed its bin frequencies from.
    CHECK_FALSE(fft.createPlan({.size = 3000}).has_value());
    CHECK_FALSE(fft.createPlan({.size = 0}).has_value());
    CHECK_FALSE(fft.createPlan({.size = 4096, .batchCount = 0}).has_value());
    CHECK_FALSE(fft.createPlan({.size = fft.capabilities().maxSize * 2}).has_value());

    // And `snapSize` rounds up, which is what the sweep planner calls on
    // sampleRate / RBW before planning.
    CHECK(fft.snapSize(4096) == 4096);
    CHECK(fft.snapSize(3000) == 4096);
    CHECK(fft.snapSize(1) == 8);
}

// ------------------------------------------------------------ benchmark

// No comma in the name: doctest splits `-tc=` filter values on commas, so a
// case with one in its title cannot be selected by name at all.
TEST_CASE("vDSP against FFTW per transform" * doctest::skip()) {
    // Skipped by default: this measures rather than checks, and a timing
    // assertion on a shared machine is a flaky test. Run it with
    //     sweeppp-plugin-fft-accelerate-tests --no-skip \
    //         -tc="vDSP against FFTW per transform"
    // and again with SWEEPPP_ACCELERATE_FORCE_SPLIT=1 for the third column.
    //
    // This is the isolated transform figure. The end-to-end one is
    //     sweeppp-cli sweep --device synthetic --sample-rate 100e6 --duration 30 \
    //         --throttle all-samples --stats --fft-backend {accelerate,fftw}
    // where `--throttle all-samples` is load-bearing: the default skips blocks
    // once the queue passes 75%, so at any rate both backends sustain, FFTs/s
    // comes out identical and only the busy percentage moves.
    // Runs `benchmarkFftBackends` -- the same call the FFT panel's Benchmark
    // button makes -- rather than timing loops of its own. One implementation,
    // so a table printed here and a table drawn in the GUI cannot disagree,
    // and this case doubles as the check that it works against real plugins
    // loaded across the ABI rather than against the in-tree reference backend.
    const ScopedConfigRoot root;
    REQUIRE(discover() != nullptr);

    // Read the columns knowing where the paths actually differ: vDSP's
    // interleaved setup declines everything above 4096, so at 16384 and above
    // the ordinary run and the forced-split run execute identical code and
    // must produce identical numbers. Only 1024 and 4096 measure the
    // interleave tax; equal figures higher up are the paths being the same,
    // not the copy being free.
    const bool forcedSplit = std::getenv("SWEEPPP_ACCELERATE_FORCE_SPLIT") != nullptr;
    MESSAGE(std::format("accelerate plan strategy: {}",
                        forcedSplit ? "split (forced everywhere)"
                                    : "interleaved up to 4096, split above"));

    // Measured at one thread and at the pipeline's worker count, because the
    // two do not agree: on Apple silicon vDSP wins alone at every size and
    // loses to FFTW at the large ones once eight workers share a plan. A
    // benchmark that only reported the first column recommended a backend the
    // pipeline then contradicted.
    FftBenchmarkConfig config = defaultFftBenchmarkConfig();
    config.threadCounts = {1, defaultWorkerCount()};
    // Up to the sizes a fine resolution bandwidth actually needs: at 20 MS/s a
    // 1 kHz RBW is ~30k points and at 100 MS/s ~150k, both above the default
    // ladder's top. This is also where FFTW's measuring planner gets
    // expensive, which the plan column then shows.
    config.sizes = {1024, 4096, 16384, 65536, 262144, 1048576};

    const std::vector<FftBenchmarkEntry> results = benchmarkFftBackends(config);
    REQUIRE_FALSE(results.empty());

    // A -DSWEEPPP_WITH_FFTW=OFF build has no FFTW to compare against. Report
    // what there is rather than failing: the case is a measurement, and half a
    // measurement is still worth printing.
    const auto accelerate = std::ranges::find_if(
        results, [](const FftBenchmarkEntry& e) { return e.backend == "accelerate"; });
    REQUIRE(accelerate != results.end());
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
