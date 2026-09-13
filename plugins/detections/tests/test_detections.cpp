// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Detector.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <vector>

namespace {

using namespace detections;

constexpr std::uint64_t kSecond = 1'000'000'000ULL;

/// A synthetic spectrum, so a test says what it means rather than filling an
/// array by hand.
///
/// The grid is the one a 20 MHz fixed tune lands on: 800 bins of 25 kHz from
/// 88 MHz, which puts a bin edge on a round number and keeps every expected
/// frequency in this file exact in double.
struct Span {
    double startHz = 88'000'000.0;
    double binWidthHz = 25'000.0;
    std::vector<float> bins;

    explicit Span(std::size_t count = 800, float noiseDbfs = -95.0F) : bins(count, noiseDbfs) {}

    void tone(std::size_t firstBin, std::size_t lastBin, float levelDbfs) {
        for (std::size_t i = firstBin; i <= lastBin; ++i) {
            bins[i] = levelDbfs;
        }
    }

    void unmeasured(std::size_t bin) { bins[bin] = -200.0F; }

    [[nodiscard]] double edge(std::size_t bin) const {
        return startHz + binWidthHz * static_cast<double>(bin);
    }

    [[nodiscard]] double center(std::size_t bin) const {
        return startHz + binWidthHz * (static_cast<double>(bin) + 0.5);
    }
};

/// One frame, with no completed pass -- so every frame is scanned, which is
/// what the cadence gate does outside a sweep. The gate itself has a case of
/// its own below.
void feed(Detector& detector, const Span& span, std::uint64_t atNs) {
    detector.onFrame(atNs, span.startHz, span.binWidthHz, span.bins, false);
}

[[nodiscard]] DetectorConfig absolute(float thresholdDbfs) {
    DetectorConfig config;
    config.mode = ThresholdMode::Absolute;
    config.absoluteDbfs = thresholdDbfs;
    return config;
}

} // namespace

TEST_CASE("an absolute threshold finds a run with its span, width and peak") {
    Span span;
    span.tone(100, 107, -60.0F);
    span.bins[104] = -52.0F;

    Detector detector;
    detector.setConfig(absolute(-70.0F));
    feed(detector, span, kSecond);

    REQUIRE(detector.detections().size() == 1);
    const Detection& found = detector.detections().front();

    // Bin edges, so the width is exactly the eight bins the tone occupies.
    CHECK(found.startHz == doctest::Approx(span.edge(100)));
    CHECK(found.stopHz == doctest::Approx(span.edge(108)));
    CHECK(found.stopHz - found.startHz == doctest::Approx(8 * span.binWidthHz));

    // The loudest bin's centre, which is the frequency an operator reads.
    CHECK(found.centerHz == doctest::Approx(span.center(104)));
    CHECK(found.peakDbfs == doctest::Approx(-52.0F));
    CHECK(found.strongestDbfs == doctest::Approx(-52.0F));
    CHECK(found.hits == 1);
    CHECK(found.appearances == 1);
    CHECK(found.active);
}

TEST_CASE("a threshold above everything present finds nothing") {
    Span span;
    span.tone(100, 107, -60.0F);

    Detector detector;
    detector.setConfig(absolute(-50.0F));
    feed(detector, span, kSecond);

    CHECK(detector.detections().empty());
}

TEST_CASE("unmeasured bins never detect and break a run") {
    SUBCASE("a span of nothing but unmeasured bins detects nothing") {
        Span span(800, -200.0F);

        Detector detector;
        // Below -200, so only the `measured` test can be what rejects these.
        detector.setConfig(absolute(-210.0F));
        feed(detector, span, kSecond);

        CHECK(detector.detections().empty());
    }

    SUBCASE("one unmeasured bin through the middle splits the run in two") {
        Span span;
        span.tone(100, 107, -60.0F);
        span.unmeasured(104);

        Detector detector;
        detector.setConfig(absolute(-70.0F));
        feed(detector, span, kSecond);

        // Two, even though the default merge gap is eight bins wide: a gap
        // nothing was measured across is not a gap the two sides are known to
        // span.
        REQUIRE(detector.detections().size() == 2);
        CHECK(detector.detections()[0].stopHz == doctest::Approx(span.edge(104)));
        CHECK(detector.detections()[1].startHz == doctest::Approx(span.edge(105)));
    }
}

TEST_CASE("an ignore range suppresses what is inside it and splits what straddles it") {
    SUBCASE("a signal wholly inside an ignored range is not reported") {
        Span span;
        span.tone(100, 107, -60.0F);

        Detector detector;
        detector.setConfig(absolute(-70.0F));
        detector.setIgnored(
            {IgnoreRange{.startHz = span.edge(98), .stopHz = span.edge(110), .note = "known"}});
        feed(detector, span, kSecond);

        CHECK(detector.detections().empty());
    }

    SUBCASE("a range through the middle of one signal makes it two") {
        Span span;
        span.tone(100, 107, -60.0F);

        Detector detector;
        detector.setConfig(absolute(-70.0F));
        detector.setIgnored({IgnoreRange{.startHz = span.center(103), .stopHz = span.center(104)}});
        feed(detector, span, kSecond);

        // Two, and the merge gap does not bridge them: an ignore range that
        // could be joined back across by the setting meant to join a signal to
        // itself would not be an ignore range at all.
        REQUIRE(detector.detections().size() == 2);
        CHECK(detector.detections()[0].stopHz == doctest::Approx(span.edge(103)));
        CHECK(detector.detections()[1].startHz == doctest::Approx(span.edge(105)));
    }

    SUBCASE("a range given the wrong way round still means the span between its ends") {
        Span span;
        span.tone(100, 107, -60.0F);

        Detector detector;
        detector.setConfig(absolute(-70.0F));
        detector.setIgnored({IgnoreRange{.startHz = span.edge(110), .stopHz = span.edge(98)}});
        feed(detector, span, kSecond);

        CHECK(detector.detections().empty());
    }
}

TEST_CASE("a merge gap joins two runs and a minimum width drops a one-bin spur") {
    SUBCASE("two runs a bin apart are one signal, and two once merging is switched off") {
        Span span;
        span.tone(100, 103, -60.0F);
        span.tone(105, 108, -60.0F);

        Detector merging;
        merging.setConfig(absolute(-70.0F));
        feed(merging, span, kSecond);

        REQUIRE(merging.detections().size() == 1);
        CHECK(merging.detections().front().startHz == doctest::Approx(span.edge(100)));
        CHECK(merging.detections().front().stopHz == doctest::Approx(span.edge(109)));

        DetectorConfig strict = absolute(-70.0F);
        strict.mergeGapBins = 0;

        Detector separate;
        separate.setConfig(strict);
        feed(separate, span, kSecond);

        CHECK(separate.detections().size() == 2);
    }

    SUBCASE("a gap wider than the setting allows still splits") {
        Span span;
        span.tone(100, 103, -60.0F);
        span.tone(120, 123, -60.0F);

        Detector detector;
        detector.setConfig(absolute(-70.0F));
        feed(detector, span, kSecond);

        CHECK(detector.detections().size() == 2);
    }

    SUBCASE("a one-bin spur is dropped by the default minimum width") {
        Span span;
        span.tone(100, 100, -60.0F);
        span.tone(200, 207, -60.0F);

        Detector detector;
        detector.setConfig(absolute(-70.0F));
        feed(detector, span, kSecond);

        REQUIRE(detector.detections().size() == 1);
        CHECK(detector.detections().front().startHz == doctest::Approx(span.edge(200)));
    }

    SUBCASE("a minimum width of one bin keeps it") {
        Span span;
        span.tone(100, 100, -60.0F);

        DetectorConfig config = absolute(-70.0F);
        config.minWidthBins = 1;

        Detector detector;
        detector.setConfig(config);
        feed(detector, span, kSecond);

        CHECK(detector.detections().size() == 1);
    }
}

TEST_CASE("hysteresis holds a run together across a dip below the threshold") {
    // What an OFDM carrier does between adjacent bins, and the reason a bare
    // comparison against one threshold reports a Wi-Fi channel as a dozen
    // separate signals.
    Span span;
    span.tone(100, 139, -60.0F);
    span.tone(115, 118, -73.0F);

    DetectorConfig config = absolute(-70.0F);
    config.hysteresisDb = 6.0F;
    config.mergeGapBins = 0;

    Detector detector;
    detector.setConfig(config);
    feed(detector, span, kSecond);

    REQUIRE(detector.detections().size() == 1);
    CHECK(detector.detections().front().startHz == doctest::Approx(span.edge(100)));
    CHECK(detector.detections().front().stopHz == doctest::Approx(span.edge(140)));

    SUBCASE("a dip past the closing level does split it") {
        config.hysteresisDb = 1.0F;
        Detector brittle;
        brittle.setConfig(config);
        feed(brittle, span, kSecond);

        CHECK(brittle.detections().size() == 2);
    }
}

TEST_CASE("a run's two edges are measured the same way") {
    // Without the backward walk the leading shoulder is cut at the opening
    // threshold and the trailing one runs on to the closing level, so one
    // symmetrical signal reports an asymmetrical span.
    Span span;
    span.tone(100, 119, -60.0F);
    span.bins[99] = -73.0F;
    span.bins[120] = -73.0F;

    DetectorConfig config = absolute(-70.0F);
    config.hysteresisDb = 6.0F;

    Detector detector;
    detector.setConfig(config);
    feed(detector, span, kSecond);

    REQUIRE(detector.detections().size() == 1);
    const Detection& found = detector.detections().front();
    CHECK(found.startHz == doctest::Approx(span.edge(99)));
    CHECK(found.stopHz == doctest::Approx(span.edge(121)));
}

TEST_CASE("above-noise mode thresholds against the measured floor") {
    // A flat -95 dBFS span reads as the centre of the -95 bucket.
    constexpr float kExpectedFloor = -94.5F;

    Span span;
    span.tone(100, 107, -80.0F);
    span.tone(300, 307, -90.0F);

    DetectorConfig config;
    config.mode = ThresholdMode::AboveNoise;
    config.marginDb = 10.0F;

    Detector detector;
    detector.setConfig(config);
    feed(detector, span, kSecond);

    CHECK(detector.noiseFloorDbfs() == doctest::Approx(kExpectedFloor));
    CHECK(detector.thresholdDbfs() == doctest::Approx(kExpectedFloor + 10.0F));

    // -80 clears the floor by 14.5 dB, -90 by 4.5 -- so only one of the two
    // tones is over a 10 dB margin.
    REQUIRE(detector.detections().size() == 1);
    CHECK(detector.detections().front().startHz == doctest::Approx(span.edge(100)));

    SUBCASE("a bigger margin than the signal has leaves nothing") {
        config.marginDb = 20.0F;
        Detector strict;
        strict.setConfig(config);
        feed(strict, span, kSecond);

        CHECK(strict.detections().empty());
    }
}

TEST_CASE("a signal drifting a bin per frame stays one detection") {
    Detector detector;
    detector.setConfig(absolute(-70.0F));

    for (std::size_t step = 0; step < 20; ++step) {
        Span span;
        span.tone(100 + step, 107 + step, -60.0F);
        feed(detector, span, (step + 1) * kSecond);
    }

    REQUIRE(detector.detections().size() == 1);
    const Detection& found = detector.detections().front();
    CHECK(found.id == 1);
    CHECK(found.hits == 20);
    CHECK(found.appearances == 1);

    const Span last;
    CHECK(found.startHz == doctest::Approx(last.edge(119)));
}

TEST_CASE("a signal that jumps clear of where it was becomes a second detection") {
    Detector detector;
    detector.setConfig(absolute(-70.0F));

    Span first;
    first.tone(100, 107, -60.0F);
    feed(detector, first, kSecond);

    Span moved;
    moved.tone(400, 407, -60.0F);
    feed(detector, moved, 2 * kSecond);

    REQUIRE(detector.detections().size() == 2);
    CHECK(detector.detections()[0].id == 1);
    CHECK(detector.detections()[1].id == 2);
}

TEST_CASE("a detection ages into history once it has not been seen for long enough") {
    DetectorConfig config = absolute(-70.0F);
    config.dropAfterSeconds = 5.0;

    Detector detector;
    detector.setConfig(config);

    Span present;
    present.tone(100, 107, -60.0F);
    feed(detector, present, kSecond);
    REQUIRE(detector.detections().size() == 1);
    CHECK(detector.detections().front().active);

    const Span empty;
    feed(detector, empty, 5 * kSecond);
    CHECK(detector.detections().front().active);

    feed(detector, empty, 7 * kSecond);
    REQUIRE(detector.detections().size() == 1);
    CHECK_FALSE(detector.detections().front().active);
    CHECK(detector.detections().front().lastSeenNs == kSecond);
}

TEST_CASE("a signal that comes back counts a second appearance and reports the interval") {
    DetectorConfig config = absolute(-70.0F);
    config.dropAfterSeconds = 5.0;

    Detector detector;
    detector.setConfig(config);

    Span present;
    present.tone(100, 107, -60.0F);
    const Span empty;

    feed(detector, present, 10 * kSecond);
    feed(detector, empty, 20 * kSecond);
    feed(detector, present, 30 * kSecond);

    REQUIRE(detector.detections().size() == 1);
    const Detection& found = detector.detections().front();
    CHECK(found.id == 1);
    CHECK(found.appearances == 2);
    CHECK(found.hits == 2);
    CHECK(found.active);
    CHECK(found.meanIntervalSeconds == doctest::Approx(20.0));
    CHECK(found.firstSeenNs == 10 * kSecond);
    CHECK(found.lastSeenNs == 30 * kSecond);
}

TEST_CASE("a signal present in every scan reports continuous rather than an interval of zero") {
    Detector detector;
    detector.setConfig(absolute(-70.0F));

    Span present;
    present.tone(100, 107, -60.0F);

    for (std::size_t step = 1; step <= 10; ++step) {
        feed(detector, present, step * kSecond);
    }

    REQUIRE(detector.detections().size() == 1);
    const Detection& found = detector.detections().front();
    CHECK(found.appearances == 1);
    CHECK(found.hits == 10);

    // Zero is the model's way of saying there is no interval, which is a
    // different statement from "it appears every zero seconds".
    CHECK(found.meanIntervalSeconds == doctest::Approx(0.0));
}

TEST_CASE("a grid change does not duplicate a detection") {
    Detector detector;
    detector.setConfig(absolute(-70.0F));

    Span coarse;
    coarse.tone(100, 107, -60.0F);
    feed(detector, coarse, kSecond);
    REQUIRE(detector.detections().size() == 1);

    // The same span re-planned at twice the resolution: a different start, a
    // different bin width and twice the bins, over the same frequencies.
    Span fine(1600);
    fine.binWidthHz = 12'500.0;
    fine.tone(200, 215, -60.0F);
    feed(detector, fine, 2 * kSecond);

    REQUIRE(detector.detections().size() == 1);
    CHECK(detector.detections().front().id == 1);
    CHECK(detector.detections().front().hits == 2);
    CHECK(detector.detections().front().startHz == doctest::Approx(coarse.edge(100)));
}

TEST_CASE("maxTracked drops the least recently seen first") {
    DetectorConfig config = absolute(-70.0F);
    config.maxTracked = 2;

    Detector detector;
    detector.setConfig(config);

    Span first;
    first.tone(100, 107, -60.0F);
    feed(detector, first, kSecond);

    Span second;
    second.tone(300, 307, -60.0F);
    feed(detector, second, 2 * kSecond);

    Span third;
    third.tone(500, 507, -60.0F);
    feed(detector, third, 3 * kSecond);

    REQUIRE(detector.detections().size() == 2);
    CHECK(detector.detections()[0].id == 2);
    CHECK(detector.detections()[1].id == 3);
}

TEST_CASE("only completed passes are scanned while the radio is sweeping") {
    Detector detector;
    detector.setConfig(absolute(-70.0F));

    Span present;
    present.tone(100, 107, -60.0F);

    // A sweep publishes the whole grid every time a step lands. Counting those
    // as separate sightings would make "how often it appears" a measurement of
    // the sweep rate rather than of the signal.
    detector.onFrame(kSecond, present.startHz, present.binWidthHz, present.bins, true);
    for (std::size_t step = 1; step <= 30; ++step) {
        detector.onFrame(kSecond + step * 10'000'000ULL, present.startHz, present.binWidthHz,
                         present.bins, false);
    }
    detector.onFrame(2 * kSecond, present.startHz, present.binWidthHz, present.bins, true);

    REQUIRE(detector.detections().size() == 1);
    CHECK(detector.detections().front().hits == 2);

    SUBCASE("with no pass ever completed, every frame is scanned") {
        Detector fixed;
        fixed.setConfig(absolute(-70.0F));
        for (std::size_t step = 1; step <= 5; ++step) {
            fixed.onFrame(step * kSecond, present.startHz, present.binWidthHz, present.bins, false);
        }

        REQUIRE(fixed.detections().size() == 1);
        CHECK(fixed.detections().front().hits == 5);
    }
}

TEST_CASE("a tick folds the scans since the last one into one sample per signal") {
    Detector detector;
    detector.setConfig(absolute(-70.0F));

    Span present;
    present.tone(100, 107, -60.0F);
    const Span empty;

    feed(detector, present, kSecond);
    feed(detector, empty, 2 * kSecond);
    feed(detector, present, 3 * kSecond);
    feed(detector, empty, 4 * kSecond);

    const std::vector<TickSample> samples = detector.tick(4 * kSecond);
    REQUIRE(samples.size() == 1);
    CHECK(samples.front().id == 1);
    CHECK(samples.front().presentFraction == doctest::Approx(0.5F));
    CHECK(samples.front().hits == 2);
    CHECK(samples.front().appearances == 2);

    SUBCASE("a window with no scan in it carries the last scan's answer forward") {
        // A wide sweep takes longer than a tick to complete a pass. Reporting
        // zero there would punch a hole in the graph of a signal that never
        // went away.
        feed(detector, present, 5 * kSecond);
        CHECK(detector.tick(5 * kSecond).front().presentFraction == doctest::Approx(1.0F));
        CHECK(detector.tick(5 * kSecond).front().presentFraction == doctest::Approx(1.0F));
    }
}

TEST_CASE("transitions are handed over once and then cleared") {
    DetectorConfig config = absolute(-70.0F);
    config.dropAfterSeconds = 5.0;

    Detector detector;
    detector.setConfig(config);

    Span present;
    present.tone(100, 107, -60.0F);
    const Span empty;

    feed(detector, present, kSecond);
    {
        const std::vector<Transition> transitions = detector.takeTransitions();
        REQUIRE(transitions.size() == 1);
        CHECK(transitions.front().kind == Transition::Kind::Appeared);
        CHECK(transitions.front().detection.id == 1);
        CHECK(transitions.front().thresholdDbfs == doctest::Approx(-70.0F));
    }
    CHECK(detector.takeTransitions().empty());

    feed(detector, empty, 10 * kSecond);
    {
        const std::vector<Transition> transitions = detector.takeTransitions();
        REQUIRE(transitions.size() == 1);
        CHECK(transitions.front().kind == Transition::Kind::Gone);
        CHECK(transitions.front().detection.hits == 1);
    }
}

TEST_CASE("a gap in the frame sequence is counted as dropped frames") {
    Detector detector;
    detector.setConfig(absolute(-70.0F));

    Span present;
    present.tone(100, 107, -60.0F);

    detector.onFrame(kSecond, 10, present.startHz, present.binWidthHz, present.bins, false);
    CHECK(detector.droppedFrames() == 0);

    detector.onFrame(2 * kSecond, 14, present.startHz, present.binWidthHz, present.bins, false);
    CHECK(detector.droppedFrames() == 3);
}

TEST_CASE("forget drops one detection and clearHistory drops what has aged out") {
    DetectorConfig config = absolute(-70.0F);
    config.dropAfterSeconds = 5.0;

    Detector detector;
    detector.setConfig(config);

    Span both;
    both.tone(100, 107, -60.0F);
    both.tone(300, 307, -60.0F);
    feed(detector, both, kSecond);
    REQUIRE(detector.detections().size() == 2);

    Span onlySecond;
    onlySecond.tone(300, 307, -60.0F);
    feed(detector, onlySecond, 10 * kSecond);

    REQUIRE(detector.detections().size() == 2);
    CHECK_FALSE(detector.detections()[0].active);
    CHECK(detector.detections()[1].active);

    detector.clearHistory();
    REQUIRE(detector.detections().size() == 1);
    CHECK(detector.detections().front().id == 2);

    detector.forget(2);
    CHECK(detector.detections().empty());
}
