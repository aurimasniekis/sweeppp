// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <random>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/correction/CorrectionLearner.hpp>
#include <sweeppp/correction/Corrections.hpp>
#include <sweeppp/dsp/Convert.hpp>
#include <sweeppp/fft/Window.hpp>
#include <sweeppp/pipeline/SpectrumFrame.hpp>
#include <vector>

using namespace sweeppp;

namespace {

class ScopedFile {
public:
    ScopedFile()
        : m_path(std::filesystem::temp_directory_path() /
                 std::format("sweeppp-calibration-{}.toml", monotonicNs())) {}
    ~ScopedFile() {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }
    ScopedFile(const ScopedFile&) = delete;
    ScopedFile& operator=(const ScopedFile&) = delete;
    ScopedFile(ScopedFile&&) = delete;
    ScopedFile& operator=(ScopedFile&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

/// The receiver's hump: a parabola in dB over the LO offset, in [-1, 1].
float humpDb(double x) {
    return static_cast<float>(-90.0 + (10.0 * x * x));
}

/// The frequency at the centre of bin `i` of a frame of `count` bins.
double binCentreHz(double centerHz, double sampleRate, std::size_t count, std::size_t i) {
    return centerHz - (sampleRate * 0.5) +
           ((static_cast<double>(i) + 0.5) * sampleRate / static_cast<double>(count));
}

float medianOf(std::vector<float> values) {
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

} // namespace

TEST_CASE("block stats report an injected DC offset and convertAndWindow removes it") {
    constexpr std::size_t kFrames = 512;
    constexpr std::int16_t kOffsetI = 1000;
    constexpr std::int16_t kOffsetQ = -500;

    // A zero-mean pattern on top of a constant, so the mean is the constant
    // alone and the pattern is what must survive the subtraction.
    std::vector<std::int16_t> raw(kFrames * 2);
    for (std::size_t i = 0; i < kFrames; ++i) {
        const auto wobble = static_cast<std::int16_t>((i % 2 == 0) ? 300 : -300);
        raw[2 * i] = static_cast<std::int16_t>(kOffsetI + wobble);
        raw[(2 * i) + 1] = static_cast<std::int16_t>(kOffsetQ - wobble);
    }
    const auto* bytes = reinterpret_cast<const std::byte*>(raw.data());

    const dsp::BlockStats stats = dsp::blockStats(bytes, SampleFormat::Cs16, kFrames);
    CHECK(stats.clippedFraction == doctest::Approx(0.0F));

    // The mean must agree with the standalone conversion's scale, not restate
    // it: the two share one set of constants.
    std::vector<std::complex<float>> converted(kFrames);
    dsp::convertToComplexFloat(bytes, SampleFormat::Cs16, converted.data(), kFrames);
    double sumRe = 0.0;
    double sumIm = 0.0;
    for (const auto& sample : converted) {
        sumRe += sample.real();
        sumIm += sample.imag();
    }
    CHECK(stats.mean.real() == doctest::Approx(sumRe / kFrames).epsilon(1e-5));
    CHECK(stats.mean.imag() == doctest::Approx(sumIm / kFrames).epsilon(1e-5));
    CHECK(stats.mean.real() > 0.0F);
    CHECK(stats.mean.imag() < 0.0F);

    const auto window = Window::create(WindowType::Rectangular, kFrames);
    REQUIRE(window.has_value());

    std::vector<std::complex<float>> out(kFrames);
    dsp::convertAndWindow(bytes, SampleFormat::Cs16, window->coefficients(), out.data(), kFrames,
                          stats.mean);

    double outRe = 0.0;
    double outIm = 0.0;
    for (const auto& sample : out) {
        outRe += sample.real();
        outIm += sample.imag();
    }
    CHECK(std::abs(outRe / kFrames) < 1e-6);
    CHECK(std::abs(outIm / kFrames) < 1e-6);

    // The pattern is intact: only the constant went.
    CHECK(out[0].real() == doctest::Approx(300.0F / 32768.0F).epsilon(1e-3));
    CHECK(out[1].real() == doctest::Approx(-300.0F / 32768.0F).epsilon(1e-3));
}

TEST_CASE("a floor shape resamples to any FFT size") {
    // Learned once at 2048 points, applied at whatever transform size is
    // running: both sizes must flatten the same hump to within a fraction of
    // a dB, at the frequencies they share.
    FloorShape shape;
    shape.sampleRate = 20e6;
    shape.levelDb.resize(2048);
    for (std::size_t k = 0; k < shape.levelDb.size(); ++k) {
        const double x = -1.0 + (2.0 * (static_cast<double>(k) + 0.5) / 2048.0);
        shape.levelDb[k] = humpDb(x);
    }

    CorrectionSet set;
    set.floor = shape;
    const float median = shape.medianDb();

    const CorrectionSettings settings{.dcRemoval = true, .flatten = true, .spurMask = false};

    const auto flatten = [&](std::size_t count) {
        std::vector<float> bins(count);
        for (std::size_t i = 0; i < count; ++i) {
            const double x =
                -1.0 + (2.0 * (static_cast<double>(i) + 0.5) / static_cast<double>(count));
            bins[i] = humpDb(x);
        }
        CorrectionScratch scratch;
        applyCorrections(bins, 1e9, 20e6, set, settings, scratch);
        return bins;
    };

    const std::vector<float> small = flatten(1024);
    const std::vector<float> large = flatten(4096);

    // Flat, at the level the shape was flattened to.
    for (std::size_t i = 0; i < small.size(); ++i) {
        CAPTURE(i);
        CHECK(std::abs(small[i] - median) < 0.1F);
        CHECK(std::abs(large[4 * i] - median) < 0.1F);
    }
}

TEST_CASE("a spur is replaced by its neighbours and unmeasured bins are ignored") {
    constexpr std::size_t kBins = 256;
    constexpr double kRate = 2.56e6; // 10 kHz bins
    const double binWidth = kRate / kBins;

    CorrectionSet set;
    // Bin 100's centre is 100.5 bins above the band edge, 27.5 bins below LO.
    set.spurs.push_back(SpurEntry{
        .kind = SpurKind::LoOffset, .hz = (100.5 - 128.0) * binWidth, .widthHz = binWidth});
    const CorrectionSettings settings{.dcRemoval = true, .flatten = false, .spurMask = true};

    SUBCASE("a spike between two quiet neighbours is levelled") {
        std::vector<float> bins(kBins, -90.0F);
        bins[99] = -70.0F;
        bins[100] = -40.0F;
        bins[101] = -70.0F;
        bins[110] = -50.0F; // not under the mask: stays

        CorrectionScratch scratch;
        applyCorrections(bins, 1e9, kRate, set, settings, scratch);

        for (std::size_t i = 99; i <= 101; ++i) {
            CAPTURE(i);
            CHECK(bins[i] == doctest::Approx(-90.0F));
        }
        CHECK(bins[110] == doctest::Approx(-50.0F));
        CHECK(bins[0] == doctest::Approx(-90.0F));
    }

    SUBCASE("a sloping floor is bridged with a line, not a step") {
        std::vector<float> bins(kBins);
        for (std::size_t i = 0; i < kBins; ++i) {
            bins[i] = -100.0F + (static_cast<float>(i) * 0.1F);
        }
        bins[100] = -40.0F;

        CorrectionScratch scratch;
        applyCorrections(bins, 1e9, kRate, set, settings, scratch);
        CHECK(bins[100] == doctest::Approx(-100.0F + (100.0F * 0.1F)).epsilon(1e-3));
    }

    SUBCASE("unmeasured bins are neither read nor written") {
        std::vector<float> bins(kBins, -90.0F);
        bins[100] = -40.0F;
        // Everything above the spur was never measured, so the line can only
        // be anchored on the left, and that side's level is carried across.
        for (std::size_t i = 101; i < kBins; ++i) {
            bins[i] = kUnmeasuredDbfs;
        }
        bins[98] = -80.0F;

        CorrectionScratch scratch;
        applyCorrections(bins, 1e9, kRate, set, settings, scratch);

        CHECK(bins[100] == doctest::Approx(-80.0F));
        CHECK(bins[99] == doctest::Approx(-80.0F));
        for (std::size_t i = 102; i < kBins; ++i) {
            CAPTURE(i);
            CHECK_FALSE(measured(bins[i]));
        }
    }

    SUBCASE("a run with no measured neighbour on either side is left alone") {
        std::vector<float> bins(kBins, kUnmeasuredDbfs);
        bins[100] = -40.0F;

        CorrectionScratch scratch;
        applyCorrections(bins, 1e9, kRate, set, settings, scratch);
        CHECK(bins[100] == doctest::Approx(-40.0F));
    }
}

TEST_CASE("an absolute spur masks a different local bin at each LO") {
    constexpr std::size_t kBins = 1024;
    constexpr double kRate = 20e6;
    constexpr double kSpurHz = 1.0384e9;
    const double binWidth = kRate / kBins;

    CorrectionSet set;
    set.spurs.push_back(
        SpurEntry{.kind = SpurKind::Absolute, .hz = kSpurHz, .widthHz = binWidth * 2.0});
    const CorrectionSettings settings{.dcRemoval = true, .flatten = false, .spurMask = true};
    CorrectionScratch scratch;

    for (const double centerHz : {1.030e9, 1.040e9, 1.046e9}) {
        CAPTURE(centerHz);
        const auto spurBin =
            static_cast<std::size_t>((kSpurHz - (centerHz - (kRate * 0.5))) / binWidth);
        REQUIRE(spurBin < kBins);

        std::vector<float> bins(kBins, -95.0F);
        bins[spurBin] = -30.0F;
        // A second peak well away from the spur must survive at every centre.
        const std::size_t other = (spurBin + 300) % kBins;
        bins[other] = -50.0F;

        applyCorrections(bins, centerHz, kRate, set, settings, scratch);

        CHECK(bins[spurBin] == doctest::Approx(-95.0F));
        CHECK(bins[other] == doctest::Approx(-50.0F));
    }

    // Tuned so the spur is outside the band, nothing is touched.
    std::vector<float> bins(kBins, -95.0F);
    bins[10] = -30.0F;
    applyCorrections(bins, 2.0e9, kRate, set, settings, scratch);
    CHECK(bins[10] == doctest::Approx(-30.0F));
}

TEST_CASE("the learner finds the hump and separates LO-offset from absolute spurs") {
    constexpr std::size_t kBins = 1024;
    constexpr double kRate = 20e6;
    constexpr double kLoSpurOffsetHz = 2e6;
    constexpr double kAbsoluteSpurHz = 1.0384e9;
    const double binWidth = kRate / kBins;

    // A sweep of steps 10 MHz apart. Every step sees the hump and the LO-offset
    // spur in the same local bins, while the absolute spur walks through
    // different ones and one step carries a real signal.
    std::vector<double> centres;
    centres.reserve(12);
    for (int k = 0; k < 12; ++k) {
        centres.push_back(1.0e9 + (10e6 * k));
    }

    CorrectionLearner learner;
    learner.begin(kBins, kRate, true);

    const auto synthesise = [&](double centerHz, bool withSignal) {
        std::vector<float> bins(kBins);
        for (std::size_t i = 0; i < kBins; ++i) {
            const double x = -1.0 + (2.0 * (static_cast<double>(i) + 0.5) / kBins);
            bins[i] = humpDb(x);
            // A little deterministic texture, so the median has something to do.
            bins[i] += static_cast<float>(0.5 * std::sin(static_cast<double>(i) * 0.7));
        }
        const auto loBin = static_cast<std::size_t>((kLoSpurOffsetHz + (kRate * 0.5)) / binWidth);
        bins[loBin] = -40.0F;
        bins[loBin + 1] = -55.0F;

        const double absOffset = kAbsoluteSpurHz - (centerHz - (kRate * 0.5));
        if (absOffset >= 0.0 && absOffset < kRate) {
            bins[static_cast<std::size_t>(absOffset / binWidth)] = -35.0F;
        }
        if (withSignal) {
            for (std::size_t i = 600; i < 620; ++i) {
                bins[i] = -50.0F;
            }
        }
        return bins;
    };

    for (std::size_t k = 0; k < centres.size(); ++k) {
        const std::vector<float> bins = synthesise(centres[k], k == 3);
        learner.addFrame(bins, centres[k]);
    }
    CHECK(learner.frameCount() == centres.size());

    // The floor follows the hump, not the spurs or the one-off signal.
    const FloorShape floor = learner.floorShape(2048);
    REQUIRE(floor.levelDb.size() == 2048);
    CHECK(floor.sampleRate == doctest::Approx(kRate));
    for (std::size_t k = 0; k < floor.levelDb.size(); k += 64) {
        const double x = -1.0 + (2.0 * (static_cast<double>(k) + 0.5) / 2048.0);
        CAPTURE(k);
        CHECK(std::abs(floor.levelDb[k] - humpDb(x)) < 1.0F);
    }

    // The LO-offset spur is found at its offset; the absolute one is not: it
    // was in a different local bin at every step, so no bin's mean rose.
    const std::vector<SpurEntry> lo = learner.loSpurs();
    REQUIRE(lo.size() == 1);
    CHECK(lo[0].kind == SpurKind::LoOffset);
    CHECK(std::abs(lo[0].hz - kLoSpurOffsetHz) < binWidth * 2.0);
    CHECK(lo[0].widthHz > 0.0);
    CHECK(lo[0].widthHz <= binWidth * 5.0);

    // The stitched passes, with the LO-offset spur already masked out, are
    // what a learner of the grid reads: the fixed tone is the one thing left.
    SpectrumFrame stitched;
    stitched.startHz = centres.front() - (kRate * 0.5);
    stitched.binWidthHz = binWidth;
    const auto gridBins =
        static_cast<std::size_t>((centres.back() + (kRate * 0.5) - stitched.startHz) / binWidth);
    stitched.binsDbfs.assign(gridBins, -90.0F);
    for (std::size_t i = 0; i < gridBins; ++i) {
        stitched.binsDbfs[i] += static_cast<float>(0.5 * std::sin(static_cast<double>(i) * 0.7));
    }
    // A gap of unmeasured bins, as a discontinuous plan would leave.
    for (std::size_t i = 5000; i < 5200; ++i) {
        stitched.binsDbfs[i] = kUnmeasuredDbfs;
    }
    const auto absBin = static_cast<std::size_t>((kAbsoluteSpurHz - stitched.startHz) / binWidth);
    stitched.binsDbfs[absBin] = -35.0F;

    CorrectionLearner grid;
    grid.begin(gridBins, binWidth * static_cast<double>(gridBins), false);
    for (int pass = 0; pass < 3; ++pass) {
        grid.addFrame(stitched.binsDbfs, stitched.centerHz());
    }
    const std::vector<SpurEntry> absolute = grid.loSpurs();
    REQUIRE(absolute.size() == 1);
    CHECK(absolute[0].kind == SpurKind::Absolute);
    CHECK(std::abs(absolute[0].hz - kAbsoluteSpurHz) < binWidth * 2.0);
}

TEST_CASE("a noise floor alone yields no spurs, however few the frames") {
    // The failure this guards against: a threshold fixed in dB over a floor
    // whose per-bin scatter has not averaged down flags a twentieth of the
    // bins as spurs, thousands of them on a wide grid.
    constexpr std::size_t kBins = 8192;
    constexpr double kRate = 20e6;
    // A fixed seed is the point: the check must reproduce.
    std::mt19937 rng(0xF100DU); // NOLINT(bugprone-random-generator-seed,cert-msc51-cpp)
    std::exponential_distribution<double> power(1.0);

    for (const std::size_t frames : {std::size_t{1}, std::size_t{4}, std::size_t{50}}) {
        CAPTURE(frames);
        CorrectionLearner learner;
        learner.begin(kBins, kRate, true);
        for (std::size_t f = 0; f < frames; ++f) {
            std::vector<float> bins(kBins);
            for (float& bin : bins) {
                bin = static_cast<float>(-110.0 + (10.0 * std::log10(power(rng) + 1e-12)));
            }
            learner.addFrame(bins, 1e9);
        }
        CHECK(learner.loSpurs().empty());
    }

    // And a real spur, twenty dB up, is still found once the scatter has
    // averaged down.
    CorrectionLearner learner;
    learner.begin(kBins, kRate, true);
    for (std::size_t f = 0; f < 50; ++f) {
        std::vector<float> bins(kBins);
        for (float& bin : bins) {
            bin = static_cast<float>(-110.0 + (10.0 * std::log10(power(rng) + 1e-12)));
        }
        bins[3000] = -90.0F;
        learner.addFrame(bins, 1e9);
    }
    const std::vector<SpurEntry> spurs = learner.loSpurs();
    REQUIRE(spurs.size() == 1);
    CHECK(std::abs(spurs[0].hz - (((3000.5 / kBins) - 0.5) * kRate)) < kRate / kBins);
}

TEST_CASE("at a fixed tune the learner reports spurs as absolute frequencies") {
    constexpr std::size_t kBins = 512;
    constexpr double kRate = 2e6;
    constexpr double kCenterHz = 100e6;
    const double binWidth = kRate / kBins;

    CorrectionLearner learner;
    learner.begin(kBins, kRate, false);
    for (int frame = 0; frame < 20; ++frame) {
        std::vector<float> bins(kBins, -90.0F);
        bins[300] = -40.0F;
        learner.addFrame(bins, kCenterHz);
    }

    const std::vector<SpurEntry> spurs = learner.loSpurs();
    REQUIRE(spurs.size() == 1);
    CHECK(spurs[0].kind == SpurKind::Absolute);
    CHECK(std::abs(spurs[0].hz - binCentreHz(kCenterHz, kRate, kBins, 300)) < binWidth);
}

TEST_CASE("a stale context skips the floor but keeps the spur mask") {
    CalibrationContext const learned{.driver = "bladerf",
                                     .deviceId = "abc",
                                     .dcRemoval = true,
                                     .parameters = {{"gain", "30"}, {"sample_rate", "61440000"}}};

    CHECK(learned.firstDifference(learned).empty());

    CalibrationContext gainChanged = learned;
    gainChanged.parameters[0].second = "40";
    CHECK(learned.firstDifference(gainChanged) == "gain");

    CalibrationContext extraKey = learned;
    extraKey.parameters.emplace_back("bandwidth", "56000000");
    std::ranges::sort(extraKey.parameters);
    CHECK(learned.firstDifference(extraKey) == "bandwidth");

    CalibrationContext dcChanged = learned;
    dcChanged.dcRemoval = false;
    CHECK(learned.firstDifference(dcChanged) == "dc_removal");

    CalibrationContext otherRadio = learned;
    otherRadio.deviceId = "def";
    CHECK(learned.firstDifference(otherRadio) == "device");

    // What the application does with a stale floor: installs the set without
    // it. The mask must still act on the same set.
    constexpr std::size_t kBins = 256;
    constexpr double kRate = 2.56e6;
    const double binWidth = kRate / kBins;

    CorrectionSet set;
    set.context = learned;
    set.floor.sampleRate = kRate;
    set.floor.levelDb.assign(2048, -90.0F);
    set.floor.levelDb[1024] = -60.0F; // would move bin 128 by 30 dB if applied
    set.spurs.push_back(SpurEntry{
        .kind = SpurKind::LoOffset, .hz = (100.5 - 128.0) * binWidth, .widthHz = binWidth});

    CorrectionSet installed = set;
    installed.floor = {};

    std::vector<float> bins(kBins, -90.0F);
    bins[100] = -40.0F;
    CorrectionScratch scratch;
    applyCorrections(bins, 1e9, kRate, installed,
                     CorrectionSettings{.dcRemoval = true, .flatten = true, .spurMask = true},
                     scratch);

    CHECK(bins[100] == doctest::Approx(-90.0F));
    CHECK(bins[128] == doctest::Approx(-90.0F));
}

TEST_CASE("a calibration round-trips through TOML") {
    const ScopedFile file;

    CorrectionSet saved;
    saved.learnedAt = "2026-09-21T10:00:00Z";
    saved.context = {.driver = "bladerf",
                     .deviceId = "abc123",
                     .dcRemoval = false,
                     .parameters = {{"gain", "30"}, {"sample_rate", "61440000"}}};
    saved.floor.sampleRate = 61.44e6;
    saved.floor.levelDb = {-90.0F, -88.5F, -87.25F, -88.5F, -90.0F};
    saved.spurs = {
        SpurEntry{.kind = SpurKind::LoOffset, .hz = 2e6, .widthHz = 45e3, .automatic = false},
        SpurEntry{.kind = SpurKind::Absolute, .hz = 2.3808e9, .widthHz = 30e3, .automatic = false},
        SpurEntry{.kind = SpurKind::LoOffset, .hz = -5e6, .widthHz = 15e3, .automatic = true},
    };

    REQUIRE(saved.save(file.path()).has_value());

    const auto loaded = CorrectionSet::load(file.path());
    REQUIRE(loaded.has_value());

    CHECK(loaded->learnedAt == saved.learnedAt);
    CHECK(loaded->context.driver == "bladerf");
    CHECK(loaded->context.deviceId == "abc123");
    CHECK_FALSE(loaded->context.dcRemoval);
    CHECK(loaded->context.parameters == saved.context.parameters);
    CHECK(loaded->context.firstDifference(saved.context).empty());

    CHECK(loaded->floor.sampleRate == doctest::Approx(61.44e6));
    REQUIRE(loaded->floor.levelDb.size() == 5);
    CHECK(loaded->floor.levelDb[2] == doctest::Approx(-87.25F));

    // The automatic entry was found against whatever was in the air and is
    // not part of the calibration; the learned two are.
    REQUIRE(loaded->spurs.size() == 2);
    CHECK(loaded->spurs[0].kind == SpurKind::LoOffset);
    CHECK(loaded->spurs[0].hz == doctest::Approx(2e6));
    CHECK(loaded->spurs[0].widthHz == doctest::Approx(45e3));
    CHECK(loaded->spurs[1].kind == SpurKind::Absolute);
    CHECK(loaded->spurs[1].hz == doctest::Approx(2.3808e9));
    CHECK_FALSE(loaded->spurs[1].automatic);

    // Median of the shape is what a correction is relative to.
    CHECK(loaded->floor.medianDb() == doctest::Approx(-88.5F));
    CHECK(medianOf(loaded->floor.levelDb) == doctest::Approx(-88.5F));
}

TEST_CASE("correction switches pack into one byte and back") {
    const CorrectionSettings all{
        .dcRemoval = true, .flatten = true, .spurMask = true, .autoSpurs = true};
    const CorrectionSettings none{
        .dcRemoval = false, .flatten = false, .spurMask = false, .autoSpurs = false};
    const CorrectionSettings some{
        .dcRemoval = false, .flatten = true, .spurMask = false, .autoSpurs = true};

    for (const CorrectionSettings& settings : {all, none, some}) {
        const CorrectionSettings back = CorrectionSettings::fromBits(settings.toBits());
        CHECK(back.dcRemoval == settings.dcRemoval);
        CHECK(back.flatten == settings.flatten);
        CHECK(back.spurMask == settings.spurMask);
        CHECK(back.autoSpurs == settings.autoSpurs);
    }
    CHECK(all.toBits() != none.toBits());
}
