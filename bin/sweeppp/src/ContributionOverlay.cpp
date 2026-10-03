// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// Host-side drawing for what contributor facets hand over.
//
// This used to live in the band-plan plugin, which linked its own ImGui,
// adopted the host's, hand-wrote the spans and labels, reimplemented label
// collision, and hardcoded its own copy of the readout row's inset because the
// ABI had no way to publish it. Every future contributor would have copied all
// of it. Here it is written once, and a Wi-Fi or FPV channel list is a TOML
// file plus a loader.
#include "ContributionOverlay.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <sweeppp/core/Toml.hpp>
#include <sweeppp/ui/ColorMap.hpp>
#include <vector>

namespace sweeppp::ui {
namespace {

Color colorOf(const Contribution& entry) noexcept {
    return Color{entry.color[0], entry.color[1], entry.color[2], entry.color[3]};
}

ImVec4 toImVec4(const Contribution& entry) noexcept {
    return {entry.color[0], entry.color[1], entry.color[2], entry.color[3]};
}

/// Horizontal extent one label or chip has claimed on its row.
struct LabelSlot {
    float start = 0.0F;
    float end = 0.0F;
};

[[nodiscard]] bool fits(const std::vector<LabelSlot>& row, float start, float end, float gap) {
    return std::ranges::none_of(row, [start, end, gap](const LabelSlot& slot) {
        return start < slot.end + gap && slot.start - gap < end;
    });
}

/// Chips carry their own background, so they need only enough space not to
/// touch.
constexpr float kChipGap = 2.0F;
constexpr float kChipPadX = 3.0F;
constexpr float kChipPadY = 1.0F;

/// Height of the coloured stripe a channel's width is drawn as, under its own
/// chip.
constexpr float kWidthBar = 3.0F;

/// Enough for a chip's width stripe and a hair of air under it, so a row's
/// stripe never reads as the next row's underline.
constexpr float kChipRowGap = kWidthBar + 3.0F;

/// How tall the stack of chips is allowed to get.
///
/// Eight bands of eight FPV channels each is sixty-four names at one x, and no
/// number of rows makes that readable -- past this the chips are dropped
/// rather than allowed to swallow the trace they are annotating. The whole
/// point of the group tree is that this is the operator's dial, not ours.
constexpr std::size_t kMaxChipRows = 8;
constexpr float kMaxChipFraction = 0.45F;

/// Where the first row of chips starts.
///
/// One text line below `textTop`, which is where the host's own RBW/VBW/FPS
/// readout is drawn -- starting the stack at `textTop` puts the first row
/// straight over it and neither can be read. This is the same inset the band
/// labels have always had, and it is the reason `textTop` is in the style at
/// all.
[[nodiscard]] float firstChipRow(const SpectrumLayout& layout, const ContributionStyle& style) {
    return layout.top() + style.textTop + ImGui::GetTextLineHeight() + 4.0F;
}

/// True when the operator is asking to dismiss whatever is under the cursor.
///
/// Ctrl or Command, because macOS turns a ctrl-click into a right click in
/// some paths and an operator there reaches for Command anyway.
[[nodiscard]] bool dismissClick() {
    const ImGuiIO& io = ImGui::GetIO();
    return ImGui::IsMouseClicked(ImGuiMouseButton_Left) && (io.KeyCtrl || io.KeySuper);
}

/// A contribution's extent, as it reads in a status bar.
[[nodiscard]] std::string spanText(const Contribution& entry) {
    if (entry.type == ContributionType::Spot || entry.stopHz <= entry.startHz) {
        return toml_util::formatFrequencyShort(entry.startHz);
    }
    return std::format("{} - {}", toml_util::formatFrequencyShort(entry.startHz),
                       toml_util::formatFrequencyShort(entry.stopHz));
}

/// One contribution written out: what it is, where it is, and who says so.
///
/// The same body the status-bar chip's hover list uses, so pointing at a flag
/// on the plot and pointing at the marker's chip describe a thing the same
/// way rather than in two vocabularies.
void describe(const Contribution& entry) {
    ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(entry));
    ImGui::TextUnformatted(entry.name.c_str());
    ImGui::PopStyleColor();

    ImGui::TextUnformatted(spanText(entry).c_str());
    if (entry.type != ContributionType::Spot && entry.stopHz > entry.startHz) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s wide)", toml_util::formatFrequencyShort(entry.widthHz()).c_str());
    }

    ImGui::TextDisabled("%s", std::format("{} · {}{}{}", entry.pluginName, toString(entry.type),
                                          entry.category.empty() ? "" : " · ", entry.category)
                                  .c_str());

    if (!entry.description.empty()) {
        ImGui::TextUnformatted(entry.description.c_str());
    }
}

/// What a click on a bar or a chip did, so the caller can keep the marker off it.
struct ChipResult {
    bool placed = true; ///< False when the stack was full and it overflowed.
    bool hovered = false;
    bool claimed = false; ///< This one took the click.
};

void paint(ImDrawList* draw, const SpectrumLayout& layout, const Contribution& entry, float alpha,
           const ContributionSelection& picked) {
    const Color color = colorOf(entry);
    const float top = layout.top();
    const float bottom = layout.bottom();

    // A channel and a spot are flags, not allocations.
    //
    // They used to be filled spans like a band, at a louder alpha so they
    // would show up inside one. That works for the handful a band plan
    // carries and falls apart at the scale a channel list has: eighty 12 MHz
    // FPV channels painted floor to ceiling is a wash of colour with the trace
    // somewhere behind it. What an operator wants from a channel is *where it
    // is and what it is called*, so it gets a stripe for its width, a hairline
    // for its centre and a named chip at the top -- and eighty of them read as
    // eighty flags rather than as one stain.
    if (entry.type != ContributionType::Band) {
        // Nothing at all until it is picked, and then everything: the line
        // down to the trace and, from the chip, the width.
        //
        // The line used to be drawn for all of them, which at the scale a
        // channel list works at is several hundred verticals over the
        // measurement -- the same mistake as painting each one as a filled
        // span, in one pixel instead of forty. A flag names a thing; asking
        // exactly where it sits is a question about one of them.
        if (!picked.matches(entry)) {
            return;
        }
        const float x = layout.xForHz((entry.startHz + entry.stopHz) * 0.5);
        if (x >= layout.left() && x <= layout.right()) {
            draw->AddLine(ImVec2(x, top), ImVec2(x, bottom), color.withAlpha(0.65F).packed(), 1.0F);
        }
        return;
    }

    // An allocation is its bar, drawn below with its name inside it. Over the
    // trace it shows only once picked -- five hundred filled spans floor to
    // ceiling was a wash of colour with the measurement somewhere behind it,
    // which is the whole reason this stopped being how they are drawn.
    if (!picked.matches(entry)) {
        return;
    }

    const float x0 = std::max(layout.xForHz(entry.startHz), layout.left());
    const float x1 = std::min(layout.xForHz(entry.stopHz), layout.right());
    if (x1 <= x0) {
        return;
    }

    draw->AddRectFilled(ImVec2(x0, top), ImVec2(x1, bottom), color.withAlpha(alpha).packed());

    // Edges only where they are actually in view, so something running off the
    // side does not grow a boundary it does not have.
    if (entry.startHz > layout.fromHz) {
        draw->AddLine(ImVec2(x0, top), ImVec2(x0, bottom), color.withAlpha(0.65F).packed(), 1.0F);
    }
    if (entry.stopHz < layout.toHz) {
        draw->AddLine(ImVec2(x1, top), ImVec2(x1, bottom), color.withAlpha(0.65F).packed(), 1.0F);
    }
}

/// One allocation, as a bar the width of the allocation with its name inside.
///
/// The band answer to the channel's chip: a channel is a point and gets a
/// label-sized flag, an allocation is an extent and gets a bar that *is* the
/// extent. Stacked the same way, so an allocation nested inside another --
/// 2.4 GHz Wi-Fi inside the 13 cm amateur band -- reads as two bars rather
/// than as one hiding the other.
[[nodiscard]] ChipResult bar(ImDrawList* draw, const SpectrumLayout& layout,
                             const Contribution& entry, const ContributionStyle& style,
                             std::vector<std::vector<LabelSlot>>& rows, std::size_t maxRows,
                             float topY, ContributionSelection& picked) {
    const float x0 = std::max(layout.xForHz(entry.startHz), layout.left());
    const float x1 = std::min(layout.xForHz(entry.stopHz), layout.right());

    // Too narrow to be anything but a dash at this span. Not overflow: it is
    // not competing for room, it is waiting to be zoomed into, and a "…" here
    // would promise a list that zooming makes unnecessary.
    if (x1 - x0 < style.minBandWidth) {
        return ChipResult{};
    }

    // No gap between bars, unlike between chips.
    //
    // An allocation table is contiguous by construction -- one allocation ends
    // exactly where the next begins -- so a bar that demanded clear air either
    // side would push every second allocation onto a second row and the whole
    // table would come out staggered. Only allocations that genuinely overlap,
    // one nested inside another, take a row of their own.
    std::size_t row = 0;
    for (; row < rows.size(); ++row) {
        if (fits(rows[row], x0, x1, 0.0F)) {
            break;
        }
    }
    if (row == rows.size()) {
        if (rows.size() >= maxRows) {
            return ChipResult{.placed = false};
        }
        rows.emplace_back();
    }
    rows[row].push_back(LabelSlot{.start = x0, .end = x1});

    const Color color = colorOf(entry);
    const float height = ImGui::GetTextLineHeight() + (kChipPadY * 2.0F);
    const float y = topY + (static_cast<float>(row) * (height + kChipRowGap));

    // Half a pixel in on each side, so two allocations that share an edge read
    // as two bars rather than as one long one.
    const ImVec2 min(x0 + 0.5F, y);
    const ImVec2 max(x1 - 0.5F, y + height);
    const bool hovered = style.hoverable && ImGui::IsMouseHoveringRect(min, max);
    const bool selected = picked.matches(entry);

    draw->AddRectFilled(min, max, color.withAlpha(hovered || selected ? 1.0F : 0.92F).packed(),
                        2.0F);
    if (hovered || selected) {
        draw->AddRect(min, max, IM_COL32(255, 255, 255, selected ? 255 : 160), 2.0F, 1.0F);
    }

    // Centred, and clipped to the bar rather than shrunk or dropped: a name
    // wider than its allocation still says which allocation it is, and half a
    // word in the right place beats a bar with nothing in it.
    if (!entry.name.empty()) {
        const ImVec2 size = ImGui::CalcTextSize(entry.name.c_str());
        draw->PushClipRect(ImVec2(min.x + 1.0F, min.y), ImVec2(max.x - 1.0F, max.y), true);
        draw->AddText(ImVec2(((x0 + x1) * 0.5F) - (size.x * 0.5F), y + kChipPadY),
                      chipTextColor(color), entry.name.c_str());
        draw->PopClipRect();
    }

    ChipResult result{.placed = true, .hovered = hovered, .claimed = false};
    if (!hovered) {
        return result;
    }

    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0F);
    describe(entry);
    ImGui::Separator();
    ImGui::TextDisabled("click to mark its extent · ctrl-click to hide");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();

    if (dismissClick()) {
        (void)PluginManager::instance().hideContribution(entry);
        result.claimed = true;
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (selected) {
            picked.clear();
        } else {
            picked.select(entry);
        }
        result.claimed = true;
    }
    return result;
}

/// One named chip, on the first row it fits.
///
/// Stacked rather than dropped, which is the difference between a channel list
/// and a band plan: two channels whose names overlap on screen are the normal
/// case here, and dropping one of them loses exactly the information the
/// operator ticked the group to see.
[[nodiscard]] ChipResult chip(ImDrawList* draw, const SpectrumLayout& layout,
                              const Contribution& entry, const ContributionStyle& style,
                              std::vector<std::vector<LabelSlot>>& rows, std::size_t maxRows,
                              ContributionSelection& picked) {
    // The visible part of the entry, so something running off the left edge is
    // still named at the edge rather than vanishing with its centre.
    const float lo = std::max(layout.xForHz(entry.startHz), layout.left());
    const float hi = std::min(layout.xForHz(entry.stopHz), layout.right());
    if (entry.name.empty() || hi < lo) {
        // Off screen or unnamed is not overflow: there is nothing an "…" here
        // would be promising the operator.
        return ChipResult{};
    }
    const float x = std::clamp(layout.xForHz((entry.startHz + entry.stopHz) * 0.5), lo, hi);

    const ImVec2 size = ImGui::CalcTextSize(entry.name.c_str());
    const float half = (size.x * 0.5F) + kChipPadX;
    if (half * 2.0F > layout.size.x) {
        // A plot narrower than one chip. Nothing fits, and nothing is missing
        // that a marker could point at.
        return ChipResult{};
    }

    const float start = std::clamp(x - half, layout.left(), layout.right() - (half * 2.0F));
    const float end = start + (half * 2.0F);

    std::size_t row = 0;
    for (; row < rows.size(); ++row) {
        if (fits(rows[row], start, end, kChipGap)) {
            break;
        }
    }
    if (row == rows.size()) {
        if (rows.size() >= maxRows) {
            return ChipResult{.placed = false};
        }
        rows.emplace_back();
    }
    rows[row].push_back(LabelSlot{.start = start, .end = end});

    const Color color = colorOf(entry);
    const float height = size.y + (kChipPadY * 2.0F);
    const float y =
        firstChipRow(layout, style) + (static_cast<float>(row) * (height + kChipRowGap));

    const ImVec2 min(start, y);
    const ImVec2 max(end, y + height);
    const bool hovered = style.hoverable && ImGui::IsMouseHoveringRect(min, max);
    const bool selected = picked.matches(entry);

    // Where it actually is, while the cursor is on it. A chip four rows down
    // and clamped away from the plot edge has no other way of saying which
    // frequency it belongs to, and having to click to find out would make the
    // stack a puzzle rather than a legend.
    if (hovered && !selected) {
        draw->AddLine(ImVec2(x, max.y), ImVec2(x, layout.bottom()), color.withAlpha(0.45F).packed(),
                      1.0F);
    }

    draw->AddRectFilled(min, max, color.withAlpha(hovered || selected ? 1.0F : 0.95F).packed(),
                        2.0F);
    if (hovered || selected) {
        draw->AddRect(min, max, IM_COL32(255, 255, 255, selected ? 255 : 160), 2.0F, 1.0F);
    }
    draw->AddText(ImVec2(start + kChipPadX, y + kChipPadY), chipTextColor(color),
                  entry.name.c_str());

    // The width, immediately under its own chip -- and only for the one flag
    // the operator has clicked. Under all of them at once it is a stripe below
    // every chip on screen, which reads as a texture rather than as a
    // measurement; under one it answers the question that was asked.
    if (selected && entry.stopHz > entry.startHz) {
        const float stripe = max.y + 1.0F;
        const float w0 = std::max(layout.xForHz(entry.startHz), layout.left());
        const float w1 = std::min(layout.xForHz(entry.stopHz), layout.right());
        draw->AddRectFilled(ImVec2(w0, stripe), ImVec2(std::max(w1, w0 + 1.0F), stripe + kWidthBar),
                            color.withAlpha(0.95F).packed());
    }

    ChipResult result{.placed = true, .hovered = hovered, .claimed = false};
    if (!hovered) {
        return result;
    }

    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0F);
    describe(entry);
    ImGui::Separator();
    ImGui::TextDisabled("click for its width · ctrl-click to hide");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();

    // Straight to the plugin that owns it, so the tree that lists this channel
    // comes back with it unticked rather than the host quietly keeping a
    // second opinion about what is visible.
    if (dismissClick()) {
        (void)PluginManager::instance().hideContribution(entry);
        result.claimed = true;
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (selected) {
            picked.clear();
        } else {
            picked.select(entry);
        }
        result.claimed = true;
    }
    return result;
}

/// What did not fit, gathered into one "…" chip per cluster.
///
/// Dropping the remainder silently would be the worst of both: the operator
/// ticked the group, the plot says nothing, and there is no way to tell "there
/// is nothing here" from "there are nine things here and no room". A marker
/// they can hover says which of the two it is and then names all nine.
void drawOverflow(ImDrawList* draw, const SpectrumLayout& layout, const ContributionStyle& style,
                  const std::vector<const Contribution*>& overflow, std::size_t row,
                  ContributionOverflowList& list) {
    if (overflow.empty()) {
        return;
    }

    const char* kMore = "…";
    const ImVec2 size = ImGui::CalcTextSize(kMore);
    const float half = (size.x * 0.5F) + kChipPadX;
    const float height = size.y + (kChipPadY * 2.0F);
    if (half * 2.0F > layout.size.x) {
        return;
    }
    const float y =
        firstChipRow(layout, style) + (static_cast<float>(row) * (height + kChipRowGap));

    std::vector<const Contribution*> byX = overflow;
    std::ranges::stable_sort(byX, [&layout](const Contribution* a, const Contribution* b) {
        return layout.xForHz((a->startHz + a->stopHz) * 0.5) <
               layout.xForHz((b->startHz + b->stopHz) * 0.5);
    });

    std::size_t first = 0;
    while (first < byX.size()) {
        const float startX = layout.xForHz((byX[first]->startHz + byX[first]->stopHz) * 0.5);

        // One marker per cluster of things whose markers would have collided,
        // so a dense band gets one "…" rather than a row of them.
        std::size_t last = first;
        while (last + 1 < byX.size()) {
            const float nextX =
                layout.xForHz((byX[last + 1]->startHz + byX[last + 1]->stopHz) * 0.5);
            if (nextX - startX > (half * 2.0F) + kChipGap) {
                break;
            }
            ++last;
        }

        const float endX = layout.xForHz((byX[last]->startHz + byX[last]->stopHz) * 0.5);
        const float centre =
            std::clamp((startX + endX) * 0.5F, layout.left() + half, layout.right() - half);

        const ImVec2 min(centre - half, y);
        const ImVec2 max(centre + half, y + height);
        const Color color = colorOf(*byX[first]);
        draw->AddRectFilled(min, max, color.withAlpha(0.95F).packed(), 2.0F);
        draw->AddText(ImVec2(min.x + kChipPadX, min.y + kChipPadY), chipTextColor(color), kMore);

        // Recorded rather than drawn: the panel is a window, and a window
        // cannot be opened from inside a plot. It goes up after the plot
        // closes, from what this hands over.
        if (style.hoverable && ImGui::IsMouseHoveringRect(min, max)) {
            list.hoveredChip = true;
            list.chipMin = min;
            list.chipMax = max;
            list.entries.clear();
            list.entries.reserve((last - first) + 1);
            for (std::size_t i = first; i <= last; ++i) {
                list.entries.push_back(*byX[i]);
            }
        }

        first = last + 1;
    }
}

} // namespace

void beginContributionFrame(ContributionInteraction& state) {
    state.overflow.hoveredChip = false;
}

bool drawContributions(ImDrawList* draw, const SpectrumLayout& layout,
                       std::span<const Contribution> ranked, const ContributionStyle& style,
                       ContributionInteraction& state) {
    ContributionSelection& picked = state.picked;

    if (draw == nullptr || ranked.empty()) {
        return false;
    }

    const float alpha = style.alpha > 0.0F ? style.alpha : 0.18F;

    // The type decides whether it is painted at all. Filtered here rather than
    // asked for differently, because the chip still answers from the full set:
    // hiding is about the picture, not about what is known.
    const auto shown = [&style](const Contribution& entry) {
        return entry.type == ContributionType::Band ? style.bands : style.channels;
    };

    // Widest first, whatever the rank: a 20 MHz channel painted before the
    // 83.5 MHz allocation containing it would be buried by it, and the more
    // specific claim is the one worth seeing.
    std::vector<const Contribution*> byWidth;
    byWidth.reserve(ranked.size());
    for (const Contribution& entry : ranked) {
        if (shown(entry)) {
            byWidth.push_back(&entry);
        }
    }
    std::ranges::stable_sort(byWidth, [](const Contribution* a, const Contribution* b) {
        return a->widthHz() > b->widthHz();
    });

    for (const Contribution* entry : byWidth) {
        paint(draw, layout, *entry, alpha, picked);
    }

    // How many rows of chips this plot can carry without the annotation
    // becoming the picture. Derived from the height rather than fixed, so a
    // tall spectrum names more and a pane dragged down to a sliver names less.
    const float chipHeight = ImGui::GetTextLineHeight() + (kChipPadY * 2.0F) + kChipRowGap;
    const float budget = (layout.bottom() * kMaxChipFraction) +
                         (layout.top() * (1.0F - kMaxChipFraction)) - firstChipRow(layout, style);
    const auto byHeight = static_cast<std::size_t>(std::max(budget, chipHeight) / chipHeight);
    const std::size_t maxRows = std::min(kMaxChipRows, byHeight);

    // Chips in rank order, so the highest-priority contributor takes the top
    // row and whatever is left over is the lower-ranked answer.
    // Rank order, except that the picked one is placed first whatever its
    // rank. Picking is how an operator answers "which of these sixty is it",
    // and an answer that then loses its own chip to the stack limit -- which is
    // exactly what happens to anything picked out of the "…" list -- would be
    // no answer at all.
    std::vector<const Contribution*> byRank;
    byRank.reserve(ranked.size());
    for (const Contribution& entry : ranked) {
        if (entry.type != ContributionType::Band && shown(entry)) {
            byRank.push_back(&entry);
        }
    }
    std::ranges::stable_partition(
        byRank, [&picked](const Contribution* entry) { return picked.matches(*entry); });

    std::vector<std::vector<LabelSlot>> chipRows;
    std::vector<const Contribution*> overflow;
    bool claimed = false;
    bool anyHovered = false;
    for (const Contribution* held : byRank) {
        const Contribution& entry = *held;
        const ChipResult result = chip(draw, layout, entry, style, chipRows, maxRows, picked);
        if (!result.placed) {
            overflow.push_back(&entry);
        }
        anyHovered = anyHovered || result.hovered;
        claimed = claimed || result.claimed;
    }
    drawOverflow(draw, layout, style, overflow, chipRows.size(), state.overflow);

    // The allocations below the stack, and below the overflow row when there
    // is one: an allocation is the context for the channels inside it, so it
    // reads under them rather than over them.
    const float bandY =
        firstChipRow(layout, style) +
        (static_cast<float>(chipRows.size() + (overflow.empty() ? 0 : 1)) * chipHeight);

    // Widest first, so an allocation nested inside another takes the row below
    // it rather than the row it wanted -- 2.4 GHz Wi-Fi under the 13 cm band,
    // the way the two actually sit.
    std::vector<std::vector<LabelSlot>> bandRows;
    for (const Contribution* entry : byWidth) {
        if (entry->type != ContributionType::Band) {
            continue;
        }
        const ChipResult result =
            bar(draw, layout, *entry, style, bandRows, kMaxChipRows, bandY, picked);
        anyHovered = anyHovered || result.hovered;
        claimed = claimed || result.claimed;
    }

    // Clicking the plot rather than a flag or a bar puts the selection away
    // again. Last, after every hit test: run before the bars, a click on one
    // would read as a click on empty plot and clear the very thing it picked.
    // Kept inside the plot rectangle, so a click on the toolbar is not a
    // gesture about something on the spectrum.
    if (style.hoverable && !anyHovered && !picked.empty() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        ImGui::IsMouseHoveringRect(ImVec2(layout.left(), layout.top()),
                                   ImVec2(layout.right(), layout.bottom()))) {
        picked.clear();
    }

    return claimed;
}

void drawContributionOverflow(ContributionInteraction& state) {
    ContributionOverflowList& list = state.overflow;

    // Still on it counts the panel's own rectangle from last frame, not a flag
    // set while drawing it. The two are flush, so the cursor is over one or the
    // other at every instant -- and a one-frame stale flag would blink the
    // panel out exactly as the cursor crossed from the chip onto it.
    const bool overPanel =
        list.panelDrawn && ImGui::IsMouseHoveringRect(list.panelMin, list.panelMax, false);

    if (list.entries.empty() || (!list.hoveredChip && !overPanel)) {
        list.panelDrawn = false;
        return;
    }

    // Flush under the chip, and left-aligned to it, so moving straight down
    // from the "…" lands inside the panel.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float maxHeight = viewport->WorkSize.y * 0.6F;

    // Three lines and a separator per entry, which is what `describe` draws.
    // An estimate is enough: it decides how much of the panel is empty or
    // whether a scrollbar appears, and never what can be reached.
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const float perEntry = (line * 3.0F) + ImGui::GetStyle().ItemSpacing.y + 2.0F;
    const float wanted = (ImGui::GetStyle().WindowPadding.y * 2.0F) + line + 4.0F +
                         (static_cast<float>(list.entries.size()) * perEntry);

    ImGui::SetNextWindowPos(ImVec2(list.chipMin.x, list.chipMax.y));
    ImGui::SetNextWindowSize(ImVec2(360.0F, std::min(wanted, maxHeight)));

    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking;

    if (ImGui::Begin("##contributions_more", nullptr, kFlags)) {
        ImGui::TextDisabled("%zu here · click one to pick it out", list.entries.size());
        ImGui::Separator();

        // Row highlights go behind the text they belong to, which means they
        // are drawn after the row's height is known and then pushed under it.
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImDrawListSplitter splitter;
        splitter.Split(draw, 2);
        splitter.SetCurrentChannel(draw, 1);

        struct RowHighlight {
            ImVec2 min;
            ImVec2 max;
            ImU32 color;
        };
        const bool windowHovered = ImGui::IsWindowHovered();
        std::vector<RowHighlight> highlights;

        ImGui::PushTextWrapPos(0.0F);
        for (std::size_t i = 0; i < list.entries.size(); ++i) {
            const Contribution& entry = list.entries[i];
            ImGui::PushID(static_cast<int>(i));

            if (i > 0) {
                ImGui::Separator();
            }

            const ImVec2 rowMin(ImGui::GetWindowPos().x, ImGui::GetCursorScreenPos().y);
            ImGui::BeginGroup();
            describe(entry);
            ImGui::EndGroup();
            const ImVec2 rowMax(rowMin.x + ImGui::GetWindowSize().x, ImGui::GetItemRectMax().y);

            const bool rowHovered = windowHovered && ImGui::IsMouseHoveringRect(rowMin, rowMax);
            const bool selected = state.picked.matches(entry);
            if (selected || rowHovered) {
                highlights.push_back(
                    RowHighlight{.min = rowMin,
                                 .max = rowMax,
                                 .color = IM_COL32(255, 255, 255, selected ? 34 : 18)});
            }

            if (rowHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (dismissClick()) {
                    (void)PluginManager::instance().hideContribution(entry);
                } else if (selected) {
                    state.picked.clear();
                } else {
                    // Picked out on the plot, where it now gets a flag of its
                    // own: a contribution that landed in this list had no room
                    // for one, and selecting it from here without giving it one
                    // would answer with nothing.
                    state.picked.select(entry);
                }
            }

            ImGui::PopID();
        }
        ImGui::PopTextWrapPos();

        splitter.SetCurrentChannel(draw, 0);
        for (const RowHighlight& row : highlights) {
            draw->AddRectFilled(row.min, row.max, row.color);
        }
        splitter.Merge(draw);
    }

    list.panelMin = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    list.panelMax = ImVec2(list.panelMin.x + size.x, list.panelMin.y + size.y);
    list.panelDrawn = true;

    ImGui::End();
}

ImU32 chipTextColor(const Color& color) noexcept {
    return color.luminance() > 0.6F ? IM_COL32(16, 16, 16, 255) : IM_COL32(255, 255, 255, 255);
}

void drawContributionChip(double queryHz, const ChromeTheme& chrome) {
    const std::vector<Contribution> found = PluginManager::instance().contributionsAt(queryHz);

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("At");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();

    if (found.empty()) {
        ImGui::TextDisabled("-");
        return;
    }

    // The first entry titles the chip. Which one that is comes from the
    // operator's contributor order, not from whichever span happens to be
    // narrower -- the rest are a hover away.
    const Contribution& top = found.front();
    ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(top));
    ImGui::TextUnformatted(top.name.c_str());
    ImGui::PopStyleColor();

    if (!ImGui::IsItemHovered()) {
        return;
    }

    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0F);

    for (std::size_t i = 0; i < found.size(); ++i) {
        const Contribution& entry = found[i];
        if (i > 0) {
            ImGui::Separator();
        }

        ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(entry));
        ImGui::TextUnformatted(entry.name.c_str());
        ImGui::PopStyleColor();

        ImGui::TextUnformatted(spanText(entry).c_str());

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(chrome.textDim.r, chrome.textDim.g,
                                                    chrome.textDim.b, chrome.textDim.a));
        ImGui::TextUnformatted(std::format("{} · {}{}{}", entry.pluginName, toString(entry.type),
                                           entry.category.empty() ? "" : " · ", entry.category)
                                   .c_str());
        ImGui::PopStyleColor();

        if (!entry.description.empty()) {
            ImGui::TextUnformatted(entry.description.c_str());
        }
    }

    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

} // namespace sweeppp::ui
