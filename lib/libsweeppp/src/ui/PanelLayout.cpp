// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/ui/PanelLayout.hpp"

#include <algorithm>
#include <cmath>

namespace sweeppp::ui {

PanelView& PanelLayout::focused() noexcept {
    PanelView* found = find(focusedId);
    return found != nullptr ? *found : panels.front();
}

const PanelView& PanelLayout::focused() const noexcept {
    const PanelView* found = find(focusedId);
    return found != nullptr ? *found : panels.front();
}

PanelView* PanelLayout::find(int id) noexcept {
    const auto found = std::ranges::find(panels, id, &PanelView::id);
    return found != panels.end() ? &*found : nullptr;
}

const PanelView* PanelLayout::find(int id) const noexcept {
    const auto found = std::ranges::find(panels, id, &PanelView::id);
    return found != panels.end() ? &*found : nullptr;
}

PanelView* PanelLayout::add(const PanelView& cloneFrom) {
    if (panels.size() >= kMaxPanels) {
        return nullptr;
    }

    PanelView copy = cloneFrom;
    copy.id = nextId++;
    copy.detached = false;
    copy.waterfallPaused = false;
    panels.push_back(copy);
    return &panels.back();
}

bool PanelLayout::remove(int id) {
    if (panels.size() <= 1) {
        return false;
    }
    const auto found = std::ranges::find(panels, id, &PanelView::id);
    if (found == panels.end()) {
        return false;
    }

    const auto index = static_cast<std::size_t>(found - panels.begin());
    panels.erase(found);

    // The id is not handed back: the window's runtime half is matched by id,
    // and a new panel inheriting a removed one's id would inherit its
    // waterfall history too.
    if (focusedId == id) {
        focusedId = panels[std::min(index, panels.size() - 1)].id;
    }
    return true;
}

std::size_t PanelLayout::attachedCount() const noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(panels, [](const PanelView& panel) { return !panel.detached; }));
}

void PanelLayout::resetNextId() noexcept {
    nextId = 1;
    for (const PanelView& panel : panels) {
        nextId = std::max(nextId, panel.id + 1);
    }
}

PanelSplits clampSplits(const PanelSplits& splits) noexcept {
    // A value that is not a number would pass every clamp unchanged.
    const PanelSplits defaults;
    const auto finite = [](float value, float fallback) {
        return std::isfinite(value) ? value : fallback;
    };

    PanelSplits out;
    out.x = std::clamp(finite(splits.x, defaults.x), kMinSplit, kMaxSplit);
    out.y = std::clamp(finite(splits.y, defaults.y), kMinSplit, kMaxSplit);

    const auto thirds = [&](const std::array<float, 2>& requested,
                            const std::array<float, 2>& fallback) {
        const std::array<float, 2> cuts{finite(requested[0], fallback[0]),
                                        finite(requested[1], fallback[1])};
        const float first = std::clamp(cuts[0], kMinThird, 1.0F - (kMinThird * 2.0F));
        const float second = std::clamp(cuts[1], first + kMinThird, 1.0F - kMinThird);
        return std::array<float, 2>{first, second};
    };
    out.thirdsX = thirds(splits.thirdsX, defaults.thirdsX);
    out.thirdsY = thirds(splits.thirdsY, defaults.thirdsY);
    return out;
}

PanelArrangement arrangementFor(std::size_t attached, bool rowsForTwo) noexcept {
    if (attached <= 1) {
        return PanelArrangement::Single;
    }
    if (attached == 2) {
        return rowsForTwo ? PanelArrangement::Rows : PanelArrangement::Columns;
    }
    if (attached == 3) {
        return PanelArrangement::Three;
    }
    if (attached == 4) {
        return PanelArrangement::Grid;
    }
    return attached <= 6 ? PanelArrangement::Six : PanelArrangement::Nine;
}

std::size_t slotCount(PanelArrangement arrangement) noexcept {
    switch (arrangement) {
    case PanelArrangement::Single:
        return 1;
    case PanelArrangement::Columns:
    case PanelArrangement::Rows:
        return 2;
    case PanelArrangement::Three:
        return 3;
    case PanelArrangement::Grid:
        return 4;
    case PanelArrangement::Six:
        return 6;
    case PanelArrangement::Nine:
        return 9;
    }
    return 1;
}

std::size_t columnCount(PanelArrangement arrangement) noexcept {
    switch (arrangement) {
    case PanelArrangement::Single:
    case PanelArrangement::Rows:
        return 1;
    case PanelArrangement::Columns:
    case PanelArrangement::Three:
    case PanelArrangement::Grid:
        return 2;
    case PanelArrangement::Six:
    case PanelArrangement::Nine:
        return 3;
    }
    return 1;
}

std::size_t rowCount(PanelArrangement arrangement) noexcept {
    switch (arrangement) {
    case PanelArrangement::Single:
    case PanelArrangement::Columns:
    case PanelArrangement::Three:
        return 1;
    case PanelArrangement::Rows:
    case PanelArrangement::Grid:
    case PanelArrangement::Six:
        return 2;
    case PanelArrangement::Nine:
        return 3;
    }
    return 1;
}

namespace {

/// Where the parts of one axis start and how long each is.
struct Cuts {
    std::array<float, 3> start{};
    std::array<float, 3> length{};
};

/// `length` cut into `parts` at `fractions` of what is left once the gaps
/// are taken out. Whole pixels but for the last part, which takes the rest.
Cuts cutAxis(float origin, float length, std::size_t parts, float split,
             const std::array<float, 2>& thirds, float gap) {
    Cuts cuts;
    const float inner = std::max(length - (gap * static_cast<float>(parts - 1)), 0.0F);

    std::array<float, 3> ends{inner, inner, inner};
    if (parts == 2) {
        ends[0] = std::floor(inner * split);
    } else if (parts == 3) {
        ends[0] = std::floor(inner * thirds[0]);
        ends[1] = std::floor(inner * thirds[1]);
    }

    float before = 0.0F;
    for (std::size_t i = 0; i < parts; ++i) {
        cuts.start[i] = origin + before + (gap * static_cast<float>(i));
        cuts.length[i] = ends[i] - before;
        before = ends[i];
    }
    return cuts;
}

} // namespace

std::vector<PanelRect> arrangePanels(PanelArrangement arrangement, const PanelRect& area,
                                     const PanelSplits& requested, float gap) {
    const PanelSplits splits = clampSplits(requested);

    if (arrangement == PanelArrangement::Three) {
        // The wide panel on the left, full height; the right column split.
        const Cuts columns = cutAxis(area.x, area.width, 2, splits.x, splits.thirdsX, gap);
        const Cuts right = cutAxis(area.y, area.height, 2, splits.y, splits.thirdsY, gap);
        return {{columns.start[0], area.y, columns.length[0], area.height},
                {columns.start[1], right.start[0], columns.length[1], right.length[0]},
                {columns.start[1], right.start[1], columns.length[1], right.length[1]}};
    }

    const std::size_t columnParts = columnCount(arrangement);
    const std::size_t rowParts = rowCount(arrangement);
    const Cuts columns = cutAxis(area.x, area.width, columnParts, splits.x, splits.thirdsX, gap);
    const Cuts rows = cutAxis(area.y, area.height, rowParts, splits.y, splits.thirdsY, gap);

    std::vector<PanelRect> rects;
    rects.reserve(columnParts * rowParts);
    for (std::size_t row = 0; row < rowParts; ++row) {
        for (std::size_t column = 0; column < columnParts; ++column) {
            rects.push_back(
                {columns.start[column], rows.start[row], columns.length[column], rows.length[row]});
        }
    }
    return rects;
}

FrequencySpan resolveView(const PanelView& panel, const FrequencySpan& fit) noexcept {
    if (panel.viewStopHz > panel.viewStartHz) {
        return {panel.viewStartHz, panel.viewStopHz};
    }
    return fit;
}

FrequencySpan zoomAbout(const FrequencySpan& view, double anchorHz, double factor) noexcept {
    return {anchorHz - (anchorHz - view.startHz) * factor,
            anchorHz + (view.stopHz - anchorHz) * factor};
}

std::optional<FrequencySpan> clampView(double fromHz, double toHz,
                                       const ViewLimits& limits) noexcept {
    if (!(toHz > fromHz)) {
        return std::nullopt;
    }

    if (limits.bounded()) {
        const double width = std::min(toHz - fromHz, limits.highHz - limits.lowHz);
        if (fromHz < limits.lowHz) {
            fromHz = limits.lowHz;
            toHz = fromHz + width;
        }
        if (toHz > limits.highHz) {
            toHz = limits.highHz;
            fromHz = toHz - width;
        }
        return FrequencySpan{fromHz, toHz};
    }

    fromHz = std::max(fromHz, 0.0);
    if (toHz <= fromHz) {
        return std::nullopt;
    }
    return FrequencySpan{fromHz, toHz};
}

BinSlice segmentBins(double gridStartHz, double binWidthHz, std::size_t binCount,
                     const FrequencySpan& span) noexcept {
    if (binWidthHz <= 0.0 || binCount == 0 || !span.valid()) {
        return {};
    }

    // Bin i is in the slice when its centre, start + (i + 0.5) * width, is in
    // [start, stop): the first such i is ceil(offset - 0.5).
    const auto edge = [&](double hz) {
        const double index = std::ceil((hz - gridStartHz) / binWidthHz - 0.5);
        return static_cast<std::size_t>(std::clamp(index, 0.0, static_cast<double>(binCount)));
    };

    const std::size_t first = edge(span.startHz);
    const std::size_t last = std::max(edge(span.stopHz), first);

    return BinSlice{.first = first,
                    .count = last - first,
                    .startHz = gridStartHz + binWidthHz * static_cast<double>(first),
                    .stopHz = gridStartHz + binWidthHz * static_cast<double>(last)};
}

SegmentBinding rebindSegments(std::span<const FrequencySpan> bound,
                              std::span<const FrequencySpan> segments) {
    SegmentBinding binding;
    binding.panelSegment.assign(bound.size(), -1);
    std::vector<bool> taken(segments.size(), false);

    const auto overlap = [](const FrequencySpan& a, const FrequencySpan& b) {
        return std::min(a.stopHz, b.stopHz) - std::max(a.startHz, b.startHz);
    };

    for (std::size_t p = 0; p < bound.size(); ++p) {
        if (!bound[p].valid()) {
            continue;
        }
        double best = 0.0;
        int choice = -1;
        for (std::size_t s = 0; s < segments.size(); ++s) {
            const double shared = overlap(bound[p], segments[s]);
            if (!taken[s] && shared > best) {
                best = shared;
                choice = static_cast<int>(s);
            }
        }
        if (choice >= 0) {
            binding.panelSegment[p] = choice;
            taken[static_cast<std::size_t>(choice)] = true;
        }
    }

    std::size_t next = 0;
    const auto nextFree = [&]() -> int {
        while (next < segments.size() && taken[next]) {
            ++next;
        }
        return next < segments.size() ? static_cast<int>(next) : -1;
    };

    // A panel whose band went entirely -- a range picked from history, a
    // different preset -- is reused rather than dropped, so its levels carry
    // over to whatever it shows next.
    for (std::size_t p = 0; p < bound.size(); ++p) {
        if (binding.panelSegment[p] >= 0) {
            continue;
        }
        const bool overlapsTaken = std::ranges::any_of(
            segments, [&](const FrequencySpan& s) { return overlap(bound[p], s) > 0.0; });
        if (overlapsTaken && bound[p].valid()) {
            // Its band is still swept, only shown by another panel now.
            continue;
        }
        const int free = nextFree();
        if (free >= 0) {
            binding.panelSegment[p] = free;
            taken[static_cast<std::size_t>(free)] = true;
        }
    }

    for (std::size_t s = 0; s < segments.size(); ++s) {
        if (!taken[s]) {
            binding.unclaimed.push_back(s);
        }
    }
    return binding;
}

} // namespace sweeppp::ui
