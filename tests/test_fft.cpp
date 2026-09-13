// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ReferenceFft.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <doctest/doctest.h>
#include <memory>
#include <numbers>
#include <string>
#include <sweeppp/fft/FftBackendManager.hpp>
#include <sweeppp/fft/FftBenchmark.hpp>
#include <sweeppp/fft/Window.hpp>
#include <thread>
#include <vector>

using namespace sweeppp;

namespace {

constexpr double kPi = std::numbers::pi;

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

IFftBackend& backend() {
    registerReferenceFftBackend();
    auto acquired = FftBackendManager::instance().acquire("reference");
    REQUIRE(acquired.has_value());
    return **acquired;
}

/// An entry that is listed but cannot be used, so the manager's handling of one
/// can be checked without depending on a backend that happens to be missing.
void registerUnusableBackend() {
    FftBackendManager::instance().registerUnavailable(
        "test.unusable", "Unusable backend", "Registered by the suite, and never available.",
        "no such device on this machine");
}

} // namespace

TEST_CASE("backend manager lists unavailable backends with a reason") {
    registerReferenceFftBackend();
    registerUnusableBackend();

    const std::vector<FftBackendInfo> backends = FftBackendManager::instance().enumerate();
    REQUIRE(backends.size() > 1);

    // The available ones sort first.
    CHECK(backends.front().available);

    // Every unavailable entry must say why. A backend that is simply missing
    // from the list teaches the operator nothing.
    bool sawUnavailable = false;
    for (const FftBackendInfo& info : backends) {
        if (!info.available) {
            sawUnavailable = true;
            CAPTURE(info.name);
            CHECK_FALSE(info.unavailableReason.empty());
            CHECK_FALSE(info.displayName.empty());
        }
    }
    CHECK(sawUnavailable);

    CHECK(FftBackendManager::instance().isAvailable("reference"));
    CHECK_FALSE(FftBackendManager::instance().isAvailable("test.unusable"));
    CHECK_FALSE(FftBackendManager::instance().suggestedDefault().empty());
}

TEST_CASE("acquiring an unavailable backend explains itself") {
    registerUnusableBackend();

    const auto acquired = FftBackendManager::instance().acquire("test.unusable");
    REQUIRE_FALSE(acquired.has_value());
    CHECK(acquired.error().code() == ErrorCode::Unsupported);
    CHECK(acquired.error().message().find("no such device") != std::string::npos);

    const auto missing = FftBackendManager::instance().acquire("nonexistent");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code() == ErrorCode::NotFound);
}

TEST_CASE("FFT matches a reference DFT") {
    IFftBackend& fft = backend();

    // 24 and 100 are not powers of two, which is the path the sweep planner
    // actually takes: it derives a size from RBW and snaps it, and a backend
    // advertising `Any` has to mean it.
    for (const std::size_t size : {16U, 24U, 64U, 100U, 256U}) {
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

TEST_CASE("a synthetic tone lands in the right bin at the right level") {
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

    // Find the peak.
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

    // Amplitude correction: magnitude / (N * coherentGain) must recover the
    // tone's amplitude. This is the check that catches a missing or wrong
    // window-gain factor, which would put every dBm reading out by ~6 dB.
    const double recovered = peakMagnitude * static_cast<double>(window->amplitudeScale());
    CHECK(recovered == doctest::Approx(kAmplitude).epsilon(1e-3));

    // ...and in dBm, into a 50 ohm reference, for a full-scale-1.0 convention.
    const auto toDbm = [](double amplitude) {
        return 10.0 * std::log10(amplitude * amplitude / 2.0) + 30.0 - 10.0 * std::log10(50.0);
    };
    CHECK(toDbm(recovered) == doctest::Approx(toDbm(kAmplitude)).epsilon(1e-3));
}

TEST_CASE("invalid plan requests are rejected rather than clamped") {
    IFftBackend& fft = backend();

    CHECK_FALSE(fft.createPlan({.size = 0}).has_value());
    CHECK_FALSE(fft.createPlan({.size = 4096, .batchCount = 0}).has_value());
    CHECK_FALSE(fft.createPlan({.size = fft.capabilities().maxSize * 2}).has_value());
}

// ------------------------------------------------------------- benchmark

TEST_CASE("the benchmark times every requested size") {
    IFftBackend& fft = backend();

    // Deliberately tiny: this checks the shape of the answer, not the speed of
    // the machine, and a suite that took a second to say so would be paid for
    // on every run.
    const FftBenchmarkConfig config{.sizes = {64, 256},
                                    .threadCounts = {1, 3},
                                    .quality = FftPlanQuality::Fast,
                                    .secondsPerSample = 0.0,
                                    .minRuns = 4,
                                    .maxRuns = 64,
                                    .warmupRuns = 1};

    const FftBenchmarkEntry entry = benchmarkFftBackend(fft, config);

    CHECK(entry.backend == "reference");
    CHECK(entry.error.empty());

    // One sample per size per thread count, sizes outermost.
    REQUIRE(entry.samples.size() == 4);
    CHECK(entry.samples[0].size == 64);
    CHECK(entry.samples[0].threads == 1);
    CHECK(entry.samples[1].size == 64);
    CHECK(entry.samples[1].threads == 3);
    CHECK(entry.samples[2].size == 256);
    CHECK(entry.samples[3].size == 256);

    for (const FftBenchmarkSample& sample : entry.samples) {
        CAPTURE(sample.size);
        CAPTURE(sample.threads);
        CHECK(sample.skipped.empty());
        // A zero would mean the clock never moved, which on any real timer
        // means the transform was optimised away rather than that it was fast.
        CHECK(sample.p50Seconds > 0.0);
        CHECK(sample.p99Seconds >= sample.p50Seconds);
        CHECK(sample.maxSeconds >= sample.p99Seconds);
        CHECK(sample.throughputPerSecond > 0.0);
        CHECK(sample.planSeconds >= 0.0);
        // Every thread runs at least `minRuns`, so the total scales with them.
        CHECK(sample.runs >= config.minRuns * sample.threads);
    }

    // The whole reason the thread axis exists: the same plan must survive
    // being executed by several threads at once, and the pipeline shares one
    // exactly this way.
    CHECK(entry.samples[1].runs > entry.samples[0].runs);
}

TEST_CASE("a size the backend refuses is reported, not skipped silently") {
    // The reference backend takes any size, so the refusal has to come from
    // the range check: a blank cell in the panel must always carry a reason,
    // otherwise it reads as a measurement that failed rather than as a
    // backend that declined.
    IFftBackend& fft = backend();
    const FftBenchmarkConfig config{.sizes = {fft.capabilities().maxSize * 2},
                                    .threadCounts = {1},
                                    .quality = FftPlanQuality::Fast,
                                    .secondsPerSample = 0.0,
                                    .minRuns = 1,
                                    .maxRuns = 1,
                                    .warmupRuns = 0};

    const FftBenchmarkEntry entry = benchmarkFftBackend(fft, config);

    REQUIRE(entry.samples.size() == 1);
    CHECK_FALSE(entry.samples[0].skipped.empty());
    CHECK(entry.samples[0].p50Seconds == 0.0);
    CHECK(entry.samples[0].runs == 0);
}

TEST_CASE("planning is serialised for a backend that says it must be") {
    // The reference backend declares planning thread-safe, so it would take
    // the uncontended path and prove nothing. This one declares the opposite
    // and reports for itself whether the promise was kept -- which is the only
    // way to test a lock whose whole effect is that something does not happen.
    //
    // What it stands in for is FFTW, whose planner mutates library-global
    // tables. Two threads inside its planner is not a test failure there, it
    // is corruption in the field.
    class PlannerWitness final : public IFftBackend {
    public:
        [[nodiscard]] std::string_view name() const noexcept override { return "test.witness"; }
        [[nodiscard]] std::string displayName() const override { return "Planner witness"; }
        [[nodiscard]] const FftCapabilities& capabilities() const noexcept override {
            return m_capabilities;
        }

        [[nodiscard]] Result<std::unique_ptr<IFftPlan>>
        createPlan(const FftPlanConfig& config) override {
            if (m_inside.fetch_add(1) != 0) {
                m_overlapped.store(true);
            }
            // Long enough that unserialised callers would reliably overlap;
            // without it every thread could slip through one at a time by
            // luck and the test would pass whatever the lock did.
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            m_inside.fetch_sub(1);
            return std::make_unique<NullPlan>(config.size);
        }

        [[nodiscard]] bool overlapped() const noexcept { return m_overlapped.load(); }

    private:
        class NullPlan final : public IFftPlan {
        public:
            explicit NullPlan(std::size_t size) noexcept : m_size(size) {}
            void execute(const std::complex<float>*, std::complex<float>*) noexcept override {}
            void executeBatch(const std::complex<float>*, std::complex<float>*,
                              std::size_t) noexcept override {}
            [[nodiscard]] std::size_t size() const noexcept override { return m_size; }
            [[nodiscard]] bool inverse() const noexcept override { return false; }

        private:
            std::size_t m_size;
        };

        FftCapabilities m_capabilities{.threadSafeExecute = true, .threadSafePlanning = false};
        std::atomic<int> m_inside{0};
        std::atomic<bool> m_overlapped{false};
    };

    PlannerWitness witness;
    constexpr std::size_t kThreads = 6;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (std::size_t t = 0; t < kThreads; ++t) {
        workers.emplace_back([&witness] {
            for (int i = 0; i < 4; ++i) {
                auto plan = createPlanSerialised(witness, {.size = 1024});
                CHECK(plan.has_value());
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    CHECK_FALSE(witness.overlapped());
}

TEST_CASE("the benchmark abandons the run when progress returns false") {
    // What cancellation is built on: the UI's Cancel button is this callback
    // answering false, so a run that ignored it would leave a thread the
    // window cannot join.
    IFftBackend& fft = backend();
    const FftBenchmarkConfig config{.sizes = {64, 256, 1024},
                                    .threadCounts = {1},
                                    .quality = FftPlanQuality::Fast,
                                    .secondsPerSample = 0.0,
                                    .minRuns = 1,
                                    .maxRuns = 1,
                                    .warmupRuns = 0};

    int calls = 0;
    const FftBenchmarkEntry entry =
        benchmarkFftBackend(fft, config, [&calls](std::string_view, std::size_t, std::size_t) {
            ++calls;
            return calls < 2;
        });

    CHECK(calls == 2);
    CHECK(entry.samples.size() == 1);
}
