// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <doctest/doctest.h>
#include <limits>
#include <sweeppp/ui/PanelLayout.hpp>
#include <vector>

using namespace sweeppp::ui;

namespace {

PanelSplits splitAt(float x, float y) {
    PanelSplits splits;
    splits.x = x;
    splits.y = y;
    return splits;
}

bool covers(const PanelRect& rect, float x, float y) {
    return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

} // namespace

// ---------------------------------------------------------------- arrangement

TEST_CASE("the arrangement follows the number of attached panels") {
    CHECK(arrangementFor(0, false) == PanelArrangement::Single);
    CHECK(arrangementFor(1, false) == PanelArrangement::Single);
    CHECK(arrangementFor(1, true) == PanelArrangement::Single);
    CHECK(arrangementFor(2, false) == PanelArrangement::Columns);
    CHECK(arrangementFor(2, true) == PanelArrangement::Rows);
    CHECK(arrangementFor(3, false) == PanelArrangement::Three);
    CHECK(arrangementFor(3, true) == PanelArrangement::Three);
    CHECK(arrangementFor(4, false) == PanelArrangement::Grid);
    CHECK(arrangementFor(4, true) == PanelArrangement::Grid);

    // Between two arrangements the larger, with its last slots empty.
    CHECK(arrangementFor(5, false) == PanelArrangement::Six);
    CHECK(arrangementFor(6, true) == PanelArrangement::Six);
    CHECK(arrangementFor(7, false) == PanelArrangement::Nine);
    CHECK(arrangementFor(9, false) == PanelArrangement::Nine);
    CHECK(arrangementFor(kMaxPanels + 5, false) == PanelArrangement::Nine);

    for (const PanelArrangement arrangement :
         {PanelArrangement::Single, PanelArrangement::Columns, PanelArrangement::Rows,
          PanelArrangement::Three, PanelArrangement::Grid, PanelArrangement::Six,
          PanelArrangement::Nine}) {
        CAPTURE(static_cast<int>(arrangement));
        CHECK(arrangePanels(arrangement, {0, 0, 800, 600}, PanelSplits{}, 4.0F).size() ==
              slotCount(arrangement));
        CHECK(slotCount(arrangement) >= columnCount(arrangement) * rowCount(arrangement));
    }
}

TEST_CASE("arranged panels tile the area without overlapping") {
    const PanelRect area{10.0F, 20.0F, 1000.0F, 600.0F};
    constexpr float kGap = 6.0F;

    PanelSplits splits = splitAt(0.4F, 0.6F);
    splits.thirdsX = {0.2F, 0.7F};
    splits.thirdsY = {0.3F, 0.5F};

    for (const PanelArrangement arrangement :
         {PanelArrangement::Columns, PanelArrangement::Rows, PanelArrangement::Three,
          PanelArrangement::Grid, PanelArrangement::Six, PanelArrangement::Nine}) {
        CAPTURE(static_cast<int>(arrangement));
        const std::vector<PanelRect> rects = arrangePanels(arrangement, area, splits, kGap);

        // Every point of the area is in at most one panel, and every panel is
        // inside the area.
        for (float x = area.x + 0.5F; x < area.x + area.width; x += 7.0F) {
            for (float y = area.y + 0.5F; y < area.y + area.height; y += 7.0F) {
                int hits = 0;
                for (const PanelRect& rect : rects) {
                    hits += covers(rect, x, y) ? 1 : 0;
                }
                CHECK(hits <= 1);
            }
        }
        for (const PanelRect& rect : rects) {
            CHECK(rect.x >= area.x);
            CHECK(rect.y >= area.y);
            CHECK(rect.x + rect.width <= area.x + area.width + 0.01F);
            CHECK(rect.y + rect.height <= area.y + area.height + 0.01F);
            CHECK(rect.width > 0.0F);
            CHECK(rect.height > 0.0F);
        }
    }

    // The split decides the share: 40% of what is left after the gap.
    const std::vector<PanelRect> columns =
        arrangePanels(PanelArrangement::Columns, area, splitAt(0.4F, 0.5F), kGap);
    CHECK(columns[0].width == doctest::Approx((area.width - kGap) * 0.4F).epsilon(0.01));
    CHECK(columns[1].x == doctest::Approx(columns[0].x + columns[0].width + kGap));

    // Three: the wide panel takes the full height on the left, and the right
    // column is split by the vertical fraction.
    const std::vector<PanelRect> three =
        arrangePanels(PanelArrangement::Three, area, splitAt(0.6F, 0.25F), kGap);
    CHECK(three[0].height == doctest::Approx(area.height));
    CHECK(three[1].x == doctest::Approx(three[2].x));
    CHECK(three[1].height < three[2].height);

    // Nine: three columns at the thirds, the same in every row, and the rows
    // at their own thirds.
    const std::vector<PanelRect> nine = arrangePanels(PanelArrangement::Nine, area, splits, kGap);
    const float innerWidth = area.width - (kGap * 2.0F);
    CHECK(nine[0].width == doctest::Approx(innerWidth * 0.2F).epsilon(0.01));
    CHECK(nine[1].x == doctest::Approx(nine[0].x + nine[0].width + kGap));
    CHECK(nine[2].x + nine[2].width == doctest::Approx(area.x + area.width));
    CHECK(nine[4].x == doctest::Approx(nine[1].x));
    CHECK(nine[8].y + nine[8].height == doctest::Approx(area.y + area.height));
    CHECK(nine[3].y == doctest::Approx(nine[0].y + nine[0].height + kGap));

    // Six: three columns, two rows split by the two-way fraction.
    const std::vector<PanelRect> six = arrangePanels(PanelArrangement::Six, area, splits, kGap);
    CHECK(six[0].width == doctest::Approx(nine[0].width));
    CHECK(six[0].height == doctest::Approx((area.height - kGap) * 0.6F).epsilon(0.01));
}

TEST_CASE("a split dragged to an edge is held off it") {
    const PanelRect area{0.0F, 0.0F, 1000.0F, 1000.0F};
    const std::vector<PanelRect> squeezed =
        arrangePanels(PanelArrangement::Grid, area, splitAt(0.0F, 2.0F), 0.0F);
    CHECK(squeezed[0].width == doctest::Approx(1000.0F * kMinSplit).epsilon(0.01));
    CHECK(squeezed[0].height == doctest::Approx(1000.0F * kMaxSplit).epsilon(0.01));
}

TEST_CASE("three-way splits keep every part at least its minimum") {
    PanelSplits splits;
    splits.thirdsX = {0.9F, 0.2F}; // crossed
    splits.thirdsY = {-1.0F, 5.0F};
    splits.x = std::numeric_limits<float>::quiet_NaN();

    const PanelSplits clamped = clampSplits(splits);
    for (const std::array<float, 2>& cuts : {clamped.thirdsX, clamped.thirdsY}) {
        CHECK(cuts[0] >= kMinThird);
        CHECK(cuts[1] - cuts[0] >= kMinThird - 1e-6F);
        CHECK(1.0F - cuts[1] >= kMinThird - 1e-6F);
    }
    CHECK(clamped.x == doctest::Approx(PanelSplits{}.x));

    const std::vector<PanelRect> nine =
        arrangePanels(PanelArrangement::Nine, {0.0F, 0.0F, 1000.0F, 1000.0F}, splits, 0.0F);
    for (const PanelRect& rect : nine) {
        CHECK(rect.width >= 1000.0F * kMinThird - 1.0F);
        CHECK(rect.height >= 1000.0F * kMinThird - 1.0F);
    }
}

// ---------------------------------------------------------------- windows

TEST_CASE("a view slides back inside its limits and keeps its width") {
    const ViewLimits limits{100.0, 200.0};

    const auto left = clampView(80.0, 110.0, limits);
    REQUIRE(left.has_value());
    CHECK(left->startHz == doctest::Approx(100.0));
    CHECK(left->stopHz == doctest::Approx(130.0));

    const auto right = clampView(190.0, 220.0, limits);
    REQUIRE(right.has_value());
    CHECK(right->startHz == doctest::Approx(170.0));
    CHECK(right->stopHz == doctest::Approx(200.0));

    // Wider than the limits: cut down to them.
    const auto wide = clampView(50.0, 300.0, limits);
    REQUIRE(wide.has_value());
    CHECK(wide->startHz == doctest::Approx(100.0));
    CHECK(wide->stopHz == doctest::Approx(200.0));

    const auto inside = clampView(120.0, 130.0, limits);
    REQUIRE(inside.has_value());
    CHECK(inside->startHz == doctest::Approx(120.0));
    CHECK(inside->stopHz == doctest::Approx(130.0));
}

TEST_CASE("an inverted or empty view is refused") {
    CHECK_FALSE(clampView(200.0, 100.0, ViewLimits{0.0, 1000.0}).has_value());
    CHECK_FALSE(clampView(100.0, 100.0, ViewLimits{0.0, 1000.0}).has_value());

    // Unbounded, the only rule is that a frequency is not negative.
    const auto unbounded = clampView(-50.0, 100.0, ViewLimits{});
    REQUIRE(unbounded.has_value());
    CHECK(unbounded->startHz == doctest::Approx(0.0));
    CHECK(unbounded->stopHz == doctest::Approx(100.0));
    CHECK_FALSE(clampView(-50.0, -10.0, ViewLimits{}).has_value());
}

TEST_CASE("zooming keeps the anchor where it was") {
    const FrequencySpan view{100.0, 200.0};
    const double anchor = 130.0;
    const double before = (anchor - view.startHz) / view.width();

    for (const double factor : {0.5, 0.8, 1.25, 3.0}) {
        CAPTURE(factor);
        const FrequencySpan zoomed = zoomAbout(view, anchor, factor);
        CHECK(zoomed.width() == doctest::Approx(view.width() * factor));
        CHECK((anchor - zoomed.startHz) / zoomed.width() == doctest::Approx(before));
    }
}

TEST_CASE("a panel with no window of its own shows the fit range") {
    PanelView panel;
    const FrequencySpan fit{88e6, 108e6};

    FrequencySpan resolved = resolveView(panel, fit);
    CHECK(resolved.startHz == doctest::Approx(88e6));
    CHECK(resolved.stopHz == doctest::Approx(108e6));

    panel.viewStartHz = 95e6;
    panel.viewStopHz = 96e6;
    resolved = resolveView(panel, fit);
    CHECK(resolved.startHz == doctest::Approx(95e6));
    CHECK(resolved.stopHz == doctest::Approx(96e6));

    // Inverted counts as none.
    panel.viewStopHz = 90e6;
    CHECK(resolveView(panel, fit).startHz == doctest::Approx(88e6));
}

// ---------------------------------------------------------------- bin slices

TEST_CASE("a segment's bins are the ones whose centres it covers") {
    // Grid: 100 bins of 1 kHz from 1 MHz.
    constexpr double kStart = 1e6;
    constexpr double kWidth = 1e3;
    constexpr std::size_t kCount = 100;

    SUBCASE("on bin edges") {
        const BinSlice slice = segmentBins(kStart, kWidth, kCount, {1.010e6, 1.020e6});
        CHECK(slice.first == 10);
        CHECK(slice.count == 10);
        CHECK(slice.startHz == doctest::Approx(1.010e6));
        CHECK(slice.stopHz == doctest::Approx(1.020e6));
    }

    SUBCASE("the whole grid") {
        const BinSlice slice = segmentBins(kStart, kWidth, kCount, {kStart, kStart + 100e3});
        CHECK(slice.first == 0);
        CHECK(slice.count == kCount);
    }

    SUBCASE("half outside the grid") {
        const BinSlice slice = segmentBins(kStart, kWidth, kCount, {0.9e6, 1.005e6});
        CHECK(slice.first == 0);
        CHECK(slice.count == 5);
        CHECK(slice.startHz == doctest::Approx(kStart));

        const BinSlice high = segmentBins(kStart, kWidth, kCount, {1.095e6, 2e6});
        CHECK(high.first == 95);
        CHECK(high.count == 5);
        CHECK(high.stopHz == doctest::Approx(1.1e6));
    }

    SUBCASE("entirely outside") {
        CHECK(segmentBins(kStart, kWidth, kCount, {2e6, 3e6}).empty());
        CHECK(segmentBins(kStart, kWidth, kCount, {0.1e6, 0.5e6}).empty());
    }

    SUBCASE("a bin's centre decides it, half a bin either way") {
        // Starts exactly on bin 10's centre: bin 10 is in.
        const BinSlice onCentre = segmentBins(kStart, kWidth, kCount, {1.0105e6, 1.0205e6});
        CHECK(onCentre.first == 10);
        CHECK(onCentre.count == 10);

        // Just past it: bin 10 is out.
        const BinSlice past = segmentBins(kStart, kWidth, kCount, {1.01051e6, 1.0205e6});
        CHECK(past.first == 11);
        CHECK(past.count == 9);
    }

    SUBCASE("nothing to cut") {
        CHECK(segmentBins(kStart, 0.0, kCount, {1.01e6, 1.02e6}).empty());
        CHECK(segmentBins(kStart, kWidth, 0, {1.01e6, 1.02e6}).empty());
        CHECK(segmentBins(kStart, kWidth, kCount, {1.02e6, 1.01e6}).empty());
    }
}

// ---------------------------------------------------------------- rebinding

TEST_CASE("panels follow their segments by overlap, not by position") {
    const std::vector<FrequencySpan> panels{{88e6, 108e6}, {430e6, 440e6}};

    SUBCASE("unchanged") {
        const SegmentBinding binding = rebindSegments(panels, panels);
        CHECK(binding.panelSegment == std::vector<int>{0, 1});
        CHECK(binding.unclaimed.empty());
    }

    SUBCASE("reordered") {
        const std::vector<FrequencySpan> segments{{430e6, 440e6}, {88e6, 108e6}};
        const SegmentBinding binding = rebindSegments(panels, segments);
        CHECK(binding.panelSegment == std::vector<int>{1, 0});
    }

    SUBCASE("one edge edited") {
        const std::vector<FrequencySpan> segments{{90e6, 108e6}, {430e6, 445e6}};
        const SegmentBinding binding = rebindSegments(panels, segments);
        CHECK(binding.panelSegment == std::vector<int>{0, 1});
    }

    SUBCASE("merged into one") {
        const std::vector<FrequencySpan> segments{{88e6, 440e6}};
        const SegmentBinding binding = rebindSegments(panels, segments);
        CHECK(binding.panelSegment == std::vector<int>{0, -1});
        CHECK(binding.unclaimed.empty());
    }

    SUBCASE("one removed") {
        const std::vector<FrequencySpan> segments{{430e6, 440e6}};
        const SegmentBinding binding = rebindSegments(panels, segments);
        CHECK(binding.panelSegment == std::vector<int>{-1, 0});
    }

    SUBCASE("a new one added") {
        const std::vector<FrequencySpan> segments{{88e6, 108e6}, {2.4e9, 2.5e9}, {430e6, 440e6}};
        const SegmentBinding binding = rebindSegments(panels, segments);
        CHECK(binding.panelSegment == std::vector<int>{0, 2});
        CHECK(binding.unclaimed == std::vector<std::size_t>{1});
    }

    SUBCASE("replaced by something unrelated") {
        // A range from history: nothing overlaps, so the panels are reused in
        // order rather than dropped and made again.
        const std::vector<FrequencySpan> segments{{2.4e9, 2.5e9}, {5.1e9, 5.9e9}, {1e9, 1.1e9}};
        const SegmentBinding binding = rebindSegments(panels, segments);
        CHECK(binding.panelSegment == std::vector<int>{0, 1});
        CHECK(binding.unclaimed == std::vector<std::size_t>{2});
    }

    SUBCASE("a panel with nothing bound yet takes a free segment") {
        const std::vector<FrequencySpan> fresh{{88e6, 108e6}, {}};
        const std::vector<FrequencySpan> segments{{88e6, 108e6}, {430e6, 440e6}};
        const SegmentBinding binding = rebindSegments(fresh, segments);
        CHECK(binding.panelSegment == std::vector<int>{0, 1});
    }
}

// ---------------------------------------------------------------- the layout

TEST_CASE("a layout adds, removes and keeps a focus") {
    PanelLayout layout;
    REQUIRE(layout.panels.size() == 1);
    CHECK(layout.focused().id == 1);

    layout.panels.front().yMinDb = -90.0F;
    layout.panels.front().detached = true;
    layout.panels.front().waterfallPaused = true;

    PanelView* second = layout.add(layout.panels.front());
    REQUIRE(second != nullptr);
    CHECK(second->id == 2);
    // A copy of the levels, but attached and running.
    CHECK(second->yMinDb == doctest::Approx(-90.0F));
    CHECK_FALSE(second->detached);
    CHECK_FALSE(second->waterfallPaused);

    while (layout.panels.size() < kMaxPanels) {
        REQUIRE(layout.add(layout.panels.front()) != nullptr);
    }
    CHECK(layout.panels.size() == kMaxPanels);
    CHECK(layout.add(layout.panels.front()) == nullptr);
    CHECK(layout.attachedCount() == kMaxPanels - 1);

    layout.focusedId = 3;
    CHECK(layout.remove(3));
    CHECK(layout.find(3) == nullptr);
    // The neighbour that slid into its place.
    CHECK(layout.focusedId == 4);

    // A removed id is not handed out again while the session runs.
    PanelView* again = layout.add(layout.panels.front());
    REQUIRE(again != nullptr);
    CHECK(again->id == static_cast<int>(kMaxPanels) + 1);

    CHECK_FALSE(layout.remove(42));
    while (layout.panels.size() > 1) {
        REQUIRE(layout.remove(layout.panels.back().id));
    }
    CHECK_FALSE(layout.remove(layout.panels.front().id));
    CHECK(layout.panels.size() == 1);
}

TEST_CASE("a focus naming no panel falls back to the first") {
    PanelLayout layout;
    layout.focusedId = 99;
    CHECK(&layout.focused() == &layout.panels.front());
}
