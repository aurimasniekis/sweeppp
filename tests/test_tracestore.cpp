// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <doctest/doctest.h>
#include <sweeppp/pipeline/SpectrumFrame.hpp>
#include <sweeppp/ui/TraceStore.hpp>

using namespace sweeppp;
using namespace sweeppp::ui;

namespace {

constexpr float kUnmeasured = -200.0F;

/// A frame whose bins are measured only between `firstMeasured` and
/// `lastMeasured`, which is what a sweep in progress -- or one whose usable
/// bandwidth is narrower than its plan -- actually looks like.
SpectrumFrame partiallyMeasured(double startHz, double binWidthHz, std::size_t bins,
                                std::size_t firstMeasured, std::size_t lastMeasured) {
    SpectrumFrame frame;
    frame.sequence = 1;
    frame.startHz = startHz;
    frame.binWidthHz = binWidthHz;
    frame.binsDbfs.assign(bins, kUnmeasured);

    for (std::size_t i = firstMeasured; i <= lastMeasured && i < bins; ++i) {
        frame.binsDbfs[i] = -90.0F;
    }

    return frame;
}

} // namespace

TEST_CASE("the envelope does not extend the last measured bin to the edge of the view") {
    // Zoomed in far enough that a pixel is narrower than a bin, the envelope
    // interpolates across empty columns so the trace stays continuous. Without
    // a right-hand bound that carried the final measured value all the way to
    // the edge of the plot -- a flat line at the noise floor across a band the
    // radio had never looked at, which reads as signal rather than as absence.
    constexpr double kStartHz = 393.0e6;
    constexpr double kBinWidthHz = 650.0;
    constexpr std::size_t kBins = 600;

    TraceStore store;
    store.update(partiallyMeasured(kStartHz, kBinWidthHz, kBins, 100, 300));

    // A window wider than the measured part, at a zoom where pixels are finer
    // than bins -- the condition that triggers interpolation at all.
    const double fromHz = kStartHz;
    const double toHz = kStartHz + kBinWidthHz * static_cast<double>(kBins);
    constexpr std::size_t kPixels = 1500;

    EnvelopeCache cache;
    const Envelope& envelope = store.envelope(TraceKind::Live, fromHz, toHz, kPixels, cache);
    REQUIRE(envelope.size() == kPixels);

    const double perPixel = (toHz - fromHz) / static_cast<double>(kPixels);
    REQUIRE(perPixel < kBinWidthHz);

    const auto columnOf = [&](std::size_t bin) {
        const double hz = kStartHz + kBinWidthHz * (static_cast<double>(bin) + 0.5);
        return static_cast<std::size_t>((hz - fromHz) / perPixel);
    };

    // Past the last measured bin there is nothing to draw, in either direction.
    for (std::size_t i = columnOf(300) + 2; i < kPixels; ++i) {
        CAPTURE(i);
        REQUIRE(envelope.maximum[i] <= kUnmeasured);
    }
    for (std::size_t i = 0; i + 2 < columnOf(100); ++i) {
        CAPTURE(i);
        REQUIRE(envelope.maximum[i] <= kUnmeasured);
    }

    // The middle is still filled in, which is what the interpolation is for:
    // bounding it must not punch holes in the measured span.
    for (std::size_t i = columnOf(100) + 2; i + 2 < columnOf(300); ++i) {
        CAPTURE(i);
        REQUIRE(envelope.maximum[i] == doctest::Approx(-90.0F));
    }
}

TEST_CASE("a fully measured span fills the whole envelope") {
    constexpr double kStartHz = 100.0e6;
    constexpr double kBinWidthHz = 1000.0;
    constexpr std::size_t kBins = 400;

    TraceStore store;
    store.update(partiallyMeasured(kStartHz, kBinWidthHz, kBins, 0, kBins - 1));

    EnvelopeCache cache;
    const Envelope& envelope = store.envelope(
        TraceKind::Live, kStartHz, kStartHz + kBinWidthHz * static_cast<double>(kBins), 900, cache);

    // The outermost column at each end is exempt: bins map to columns by their
    // centre, so the leading and trailing half-bin falls outside the first and
    // last column that any bin lands in. That is a rounding artefact of the
    // mapping, symmetric at both ends, and a pixel wide.
    for (std::size_t i = 1; i + 1 < envelope.size(); ++i) {
        CAPTURE(i);
        CHECK(envelope.maximum[i] == doctest::Approx(-90.0F));
    }
}

TEST_CASE("two caches drawing different windows each get what a fresh one would") {
    // Two plots over the same traces at different zooms, alternating frame by
    // frame -- what a pair of Mirror panels does. A cache that served one
    // window's answer to the other would draw the wrong trace in both.
    constexpr double kStartHz = 100.0e6;
    constexpr double kBinWidthHz = 1000.0;
    constexpr std::size_t kBins = 4000;

    TraceStore store;
    SpectrumFrame frame = partiallyMeasured(kStartHz, kBinWidthHz, kBins, 0, kBins - 1);
    for (std::size_t i = 0; i < kBins; ++i) {
        frame.binsDbfs[i] = -100.0F + static_cast<float>(i % 37);
    }
    store.trace(TraceKind::MaxHold).visible = true;
    store.update(frame);

    const double wideTo = kStartHz + kBinWidthHz * static_cast<double>(kBins);
    const double narrowFrom = kStartHz + 1.0e6;
    const double narrowTo = kStartHz + 1.5e6;

    EnvelopeCache wide;
    EnvelopeCache narrow;
    for (int round = 0; round < 3; ++round) {
        for (const TraceKind kind : {TraceKind::Live, TraceKind::MaxHold}) {
            const Envelope a = store.envelope(kind, kStartHz, wideTo, 300, wide);
            const Envelope b = store.envelope(kind, narrowFrom, narrowTo, 200, narrow);

            EnvelopeCache fresh;
            const Envelope expectA = store.envelope(kind, kStartHz, wideTo, 300, fresh);
            EnvelopeCache freshNarrow;
            const Envelope expectB = store.envelope(kind, narrowFrom, narrowTo, 200, freshNarrow);

            CAPTURE(round);
            CHECK(a.maximum == expectA.maximum);
            CHECK(a.minimum == expectA.minimum);
            CHECK(b.maximum == expectB.maximum);
            CHECK(b.minimum == expectB.minimum);
        }
    }
}

TEST_CASE("a cached envelope is recomputed once the traces change") {
    constexpr double kStartHz = 100.0e6;
    constexpr double kBinWidthHz = 1000.0;
    constexpr std::size_t kBins = 100;

    TraceStore store;
    store.update(partiallyMeasured(kStartHz, kBinWidthHz, kBins, 0, kBins - 1));

    EnvelopeCache cache;
    const double toHz = kStartHz + kBinWidthHz * static_cast<double>(kBins);
    CHECK(store.envelope(TraceKind::Live, kStartHz, toHz, 50, cache).maximum[10] ==
          doctest::Approx(-90.0F));

    SpectrumFrame louder = partiallyMeasured(kStartHz, kBinWidthHz, kBins, 0, kBins - 1);
    std::ranges::fill(louder.binsDbfs, -40.0F);
    store.update(louder);
    CHECK(store.envelope(TraceKind::Live, kStartHz, toHz, 50, cache).maximum[10] ==
          doctest::Approx(-40.0F));

    store.clear();
    CHECK(store.envelope(TraceKind::Live, kStartHz, toHz, 50, cache).empty());
}

TEST_CASE("a zoomed-out envelope still shows a one-bin carrier and keeps its gaps") {
    // Far enough out that the envelope reads the shared pyramid rather than the
    // bins: the property that matters is the one the bin walk had -- nothing
    // narrow is lost, and nothing unmeasured is filled in.
    constexpr double kStartHz = 70.0e6;
    constexpr double kBinWidthHz = 5.0e3;
    constexpr std::size_t kBins = 400'000;
    constexpr std::size_t kCarrier = 123'457;
    constexpr std::size_t kPixels = 500;

    SpectrumFrame frame = partiallyMeasured(kStartHz, kBinWidthHz, kBins, 0, kBins - 1);
    for (std::size_t i = 0; i < kBins; ++i) {
        frame.binsDbfs[i] = -100.0F + static_cast<float>((i * 7919) % 20);
    }
    frame.binsDbfs[kCarrier] = -20.0F;
    for (std::size_t i = 200'000; i < 260'000; ++i) {
        frame.binsDbfs[i] = kUnmeasured;
    }

    TraceStore store;
    store.update(frame);

    const double toHz = kStartHz + kBinWidthHz * static_cast<double>(kBins);
    EnvelopeCache cache;
    const Envelope& envelope = store.envelope(TraceKind::Live, kStartHz, toHz, kPixels, cache);
    REQUIRE(envelope.size() == kPixels);

    const double perPixel = (toHz - kStartHz) / static_cast<double>(kPixels);
    const auto columnOf = [&](std::size_t bin) {
        return static_cast<std::size_t>((kBinWidthHz * (static_cast<double>(bin) + 0.5)) /
                                        perPixel);
    };

    // The carrier, in its own column or the one beside it -- an entry is
    // placed by its centre, which is at most a quarter of a column away.
    const std::size_t carrier = columnOf(kCarrier);
    float nearCarrier = kUnmeasured;
    for (std::size_t c = carrier - 1; c <= carrier + 1; ++c) {
        nearCarrier = std::max(nearCarrier, envelope.maximum[c]);
    }
    CHECK(nearCarrier == doctest::Approx(-20.0F));
    CHECK(*std::ranges::max_element(envelope.maximum) == doctest::Approx(-20.0F));

    // The quietest reading survives as the floor of the band.
    float lowest = 0.0F;
    for (std::size_t c = 0; c < kPixels; ++c) {
        if (envelope.maximum[c] > kUnmeasured) {
            lowest = std::min(lowest, envelope.minimum[c]);
        }
    }
    CHECK(lowest == doctest::Approx(-100.0F));

    // Columns wholly inside the gap stay empty.
    for (std::size_t c = columnOf(200'000) + 1; c + 1 < columnOf(260'000); ++c) {
        CAPTURE(c);
        CHECK(envelope.maximum[c] <= kUnmeasured);
    }
}
