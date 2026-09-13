// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

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

    const Envelope& envelope = store.envelope(TraceKind::Live, fromHz, toHz, kPixels);
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

    const Envelope& envelope = store.envelope(
        TraceKind::Live, kStartHz, kStartHz + kBinWidthHz * static_cast<double>(kBins), 900);

    // The outermost column at each end is exempt: bins map to columns by their
    // centre, so the leading and trailing half-bin falls outside the first and
    // last column that any bin lands in. That is a rounding artefact of the
    // mapping, symmetric at both ends, and a pixel wide.
    for (std::size_t i = 1; i + 1 < envelope.size(); ++i) {
        CAPTURE(i);
        CHECK(envelope.maximum[i] == doctest::Approx(-90.0F));
    }
}
