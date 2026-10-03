// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The panels: how they are arranged, the row over each, the windows they are
// torn off into, and in Spans mode the segments they are bound to and the
// strip that shows them all. Split from MainWindow.cpp because none of it is
// about any one plot; the plots themselves are in MainWindowPlots.cpp.
#include "BarChrome.hpp"
#include "ContributionOverlay.hpp"
#include "Icons.hpp"
#include "MainWindow.hpp"
#include "PlotGeometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <imgui.h>
#include <imgui_internal.h>
#include <sweeppp/core/Toml.hpp>

namespace sweeppp::ui {
namespace {

ImU32 packed(const Color& color) {
    return color.packed();
}

ImVec4 toImVec4(const Color& color) {
    return {color.r, color.g, color.b, color.a};
}

float headerHeight() {
    return ImGui::GetTextLineHeight() + (bar::padding() * 2.0F);
}

bool sameSpan(const FrequencySpan& span, const SweepSegment& segment) {
    return std::abs(span.startHz - segment.startHz) < 1.0 &&
           std::abs(span.stopHz - segment.stopHz) < 1.0;
}

/// Whether another panel shows the same segment -- a clone made by picking a
/// layout with more slots. Closing one of those closes only the panel.
bool segmentShared(const PanelLayout& layout, const PanelView& view) {
    return std::ranges::any_of(layout.panels, [&view](const PanelView& other) {
        return other.id != view.id && other.segment.valid() &&
               std::abs(other.segment.startHz - view.segment.startHz) < 1.0 &&
               std::abs(other.segment.stopHz - view.segment.stopHz) < 1.0;
    });
}

bool sameSegments(const std::vector<SweepSegment>& a, const std::vector<SweepSegment>& b) {
    return std::ranges::equal(a, b, [](const SweepSegment& x, const SweepSegment& y) {
        return x.startHz == y.startHz && x.stopHz == y.stopHz;
    });
}

/// A small button in the header row, sized to the text line so the row keeps
/// its height.
bool headerButton(const char* id, const char* glyph, const char* fallback, const char* tooltip,
                  bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::SmallButton(icon::glyphOr(glyph, fallback).append(id).c_str());
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return pressed;
}

/// The draggable bar between two arranged panels, styled like the pane
/// splitter: invisible until pointed at.
///
/// Returns the travel this frame along its axis.
float layoutSplitter(const char* id, const ImVec2& min, const ImVec2& size, bool vertical,
                     const ChromeTheme& chrome) {
    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton(id, ImVec2(std::max(size.x, 1.0F), std::max(size.y, 1.0F)));

    const bool active = ImGui::IsItemActive();
    if (active || ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
        const ImVec2 max(min.x + size.x, min.y + size.y);
        const ImU32 color = packed(active ? chrome.accent : chrome.border);
        if (vertical) {
            const float x = (min.x + max.x) * 0.5F;
            ImGui::GetWindowDrawList()->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), color, 2.0F);
        } else {
            const float y = (min.y + max.y) * 0.5F;
            ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), color, 2.0F);
        }
    }

    if (!active) {
        return 0.0F;
    }
    return vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
}

} // namespace

// ---- runtime halves ------------------------------------------------------

void MainWindow::syncPanels() {
    const PanelLayout& layout = m_state.view().layout;

    std::erase_if(m_panels,
                  [&layout](const ViewPanel& panel) { return layout.find(panel.id) == nullptr; });
    for (const PanelView& view : layout.panels) {
        if (std::ranges::find(m_panels, view.id, &ViewPanel::id) == m_panels.end()) {
            m_panels.push_back(ViewPanel{.id = view.id});
        }
    }
}

ViewPanel& MainWindow::runtimeFor(const PanelView& view) {
    const auto found = std::ranges::find(m_panels, view.id, &ViewPanel::id);
    if (found != m_panels.end()) {
        return *found;
    }
    m_panels.push_back(ViewPanel{.id = view.id});
    return m_panels.back();
}

void MainWindow::applyWaterfallColorMap() {
    for (ViewPanel& panel : m_panels) {
        if (panel.waterfall) {
            panel.waterfall->setColorMap(m_state.theme().waterfallColorMap());
        }
    }
}

// ---- windows -------------------------------------------------------------

ViewLimits MainWindow::panelLimits(const PanelView& view) const {
    if (&view == &m_historyView) {
        if (m_history.isOpen()) {
            const session::SessionSummary& summary = m_history.reader()->summary();
            return {summary.lowestHz, summary.highestHz};
        }
        return m_state.viewLimits();
    }
    if (m_state.view().layout.mode == PanelMode::Spans && view.segment.valid()) {
        return {view.segment.startHz, view.segment.stopHz};
    }
    return m_state.viewLimits();
}

FrequencySpan MainWindow::panelFit(const PanelView& view) const {
    if (&view == &m_historyView) {
        if (m_history.isOpen()) {
            const session::SessionSummary& summary = m_history.reader()->summary();
            return {summary.lowestHz, summary.highestHz};
        }
        return m_state.fitRange();
    }
    if (m_state.view().layout.mode == PanelMode::Spans && view.segment.valid()) {
        return view.segment;
    }
    return m_state.fitRange();
}

FrequencySpan MainWindow::panelRange(const PanelView& view) const {
    return resolveView(view, panelFit(view));
}

void MainWindow::setPanelRange(PanelView& view, double fromHz, double toHz) {
    if (const std::optional<FrequencySpan> clamped = clampView(fromHz, toHz, panelLimits(view))) {
        view.viewStartHz = clamped->startHz;
        view.viewStopHz = clamped->stopHz;
    }
}

void MainWindow::refreshMarkers() {
    MarkerSet& markers = m_state.markers();
    if (markers.items.empty()) {
        return;
    }

    // The reach a peak lock searches is a share of the window the marker is
    // being looked at in. Focused panel first, so the one the operator is
    // working in decides when several show the marker.
    std::vector<FrequencySpan> windows;
    if (m_historyViewerMode) {
        windows.push_back(panelRange(m_historyView));
    } else {
        const PanelLayout& layout = m_state.view().layout;
        windows.push_back(panelRange(layout.focused()));
        for (const PanelView& view : layout.panels) {
            if (view.id != layout.focusedId) {
                windows.push_back(panelRange(view));
            }
        }
    }

    for (Marker& marker : markers.items) {
        const auto shown = std::ranges::find_if(
            windows, [&marker](const FrequencySpan& w) { return w.contains(marker.frequencyHz); });
        const FrequencySpan& window = shown != windows.end() ? *shown : windows.front();
        refreshMarkerLevel(marker, m_state.traces(), window.width() * 0.02);
    }
}

void MainWindow::followPlan() {
    PanelLayout& layout = m_state.view().layout;

    const std::uint64_t generation = m_state.viewResetGeneration();
    if (!m_viewResetSeen) {
        // The first frame adopts whatever the saved layout brought. A plan
        // applied at start-up is the saved one coming back, not a move.
        m_viewResetSeen = generation;
    } else if (*m_viewResetSeen != generation) {
        m_viewResetSeen = generation;
        for (PanelView& view : layout.panels) {
            view.viewStartHz = 0.0;
            view.viewStopHz = 0.0;
        }
    }

    if (layout.mode != PanelMode::Spans) {
        m_segmentsBound = false;
        return;
    }
    const std::vector<SweepSegment>& segments = m_state.sweepPlan().segments;
    if (!m_segmentsBound || !sameSegments(segments, m_boundSegments)) {
        rebindSpans();
        m_boundSegments = segments;
        m_segmentsBound = true;
    }
}

// ---- arrangement ---------------------------------------------------------

void MainWindow::setArrangement(PanelArrangement arrangement) {
    PanelLayout& layout = m_state.view().layout;
    const std::size_t wanted = slotCount(arrangement);

    if (arrangement == PanelArrangement::Rows) {
        layout.rowsForTwo = true;
    } else if (arrangement == PanelArrangement::Columns) {
        layout.rowsForTwo = false;
    }

    // Trailing panels go first, so the ones the operator set up at the start
    // of the list survive a step down and back up.
    while (layout.attachedCount() > wanted) {
        const auto last = std::ranges::find_if(layout.panels.rbegin(), layout.panels.rend(),
                                               [](const PanelView& v) { return !v.detached; });
        if (last == layout.panels.rend() || !layout.remove(last->id)) {
            break;
        }
    }

    // A torn-off panel comes back before a new one is made.
    for (PanelView& view : layout.panels) {
        if (layout.attachedCount() >= wanted) {
            break;
        }
        view.detached = false;
    }
    while (layout.attachedCount() < wanted) {
        const PanelView source = layout.focused();
        if (layout.add(source) == nullptr) {
            break;
        }
    }
}

void MainWindow::setPanelMode(PanelMode mode) {
    PanelLayout& layout = m_state.view().layout;
    if (layout.mode == mode) {
        return;
    }
    layout.mode = mode;

    if (mode == PanelMode::Mirror) {
        // The arrangement stays; every view goes back to the whole data.
        for (PanelView& view : layout.panels) {
            view.segment = {};
            view.viewStartHz = 0.0;
            view.viewStopHz = 0.0;
        }
        m_segmentsBound = false;
        return;
    }

    // One panel per segment, lowest first, each keeping the levels of the
    // panel that was in its place.
    std::vector<SweepSegment> segments = m_state.sweepPlan().segments;
    std::ranges::sort(segments, {}, &SweepSegment::startHz);
    const std::size_t count = std::clamp<std::size_t>(segments.size(), 1, kMaxPanels);

    while (layout.panels.size() > count && layout.remove(layout.panels.back().id)) {
    }
    while (layout.panels.size() < count) {
        const PanelView source = layout.focused();
        if (layout.add(source) == nullptr) {
            break;
        }
    }
    for (std::size_t i = 0; i < layout.panels.size(); ++i) {
        PanelView& view = layout.panels[i];
        view.segment = i < segments.size() ? FrequencySpan{segments[i].startHz, segments[i].stopHz}
                                           : FrequencySpan{};
        view.viewStartHz = 0.0;
        view.viewStopHz = 0.0;
    }
    if (layout.attachedCount() == 0) {
        layout.panels.front().detached = false;
    }

    m_boundSegments = m_state.sweepPlan().segments;
    m_segmentsBound = true;
}

void MainWindow::rebindSpans() {
    PanelLayout& layout = m_state.view().layout;
    const std::vector<SweepSegment>& planned = m_state.sweepPlan().segments;
    if (planned.empty()) {
        return;
    }

    std::vector<FrequencySpan> bound;
    bound.reserve(layout.panels.size());
    for (const PanelView& view : layout.panels) {
        bound.push_back(view.segment);
    }
    std::vector<FrequencySpan> segments;
    segments.reserve(planned.size());
    for (const SweepSegment& segment : planned) {
        segments.push_back({segment.startHz, segment.stopHz});
    }

    const SegmentBinding binding = rebindSegments(bound, segments);

    std::vector<PanelView> kept;
    std::size_t dropped = 0;
    for (std::size_t i = 0; i < layout.panels.size(); ++i) {
        const int index = binding.panelSegment[i];
        if (index < 0) {
            ++dropped;
            continue;
        }
        PanelView view = layout.panels[i];
        const FrequencySpan& segment = segments[static_cast<std::size_t>(index)];
        const bool moved = std::abs(view.segment.startHz - segment.startHz) > 1.0 ||
                           std::abs(view.segment.stopHz - segment.stopHz) > 1.0;
        view.segment = segment;
        if (moved) {
            // Kept where it was looking when that is still inside, so editing
            // one edge of a segment does not throw away a zoom on the other.
            const std::optional<FrequencySpan> inside =
                view.viewStopHz > view.viewStartHz
                    ? clampView(view.viewStartHz, view.viewStopHz,
                                ViewLimits{segment.startHz, segment.stopHz})
                    : std::nullopt;
            view.viewStartHz = inside ? inside->startHz : 0.0;
            view.viewStopHz = inside ? inside->stopHz : 0.0;
        }
        kept.push_back(view);
    }

    if (!kept.empty()) {
        layout.panels = std::move(kept);
    }
    for (const std::size_t index : binding.unclaimed) {
        const PanelView source = layout.focused();
        PanelView* added = layout.add(source);
        if (added == nullptr) {
            // Still swept, and listed on the strip, where a click binds the
            // focused panel to it.
            break;
        }
        added->segment = segments[index];
        added->viewStartHz = 0.0;
        added->viewStopHz = 0.0;
    }

    if (layout.find(layout.focusedId) == nullptr) {
        layout.focusedId = layout.panels.front().id;
    }
    if (layout.attachedCount() == 0) {
        layout.panels.front().detached = false;
    }
    if (dropped > 0) {
        toast(ToastSeverity::Info, dropped == 1
                                       ? std::string("Segments merged; one panel closed")
                                       : std::format("Segments merged; {} panels closed", dropped));
    }
}

void MainWindow::applySegments(const std::vector<SweepSegment>& segments) {
    SweepPlan plan = m_state.sweepPlan();
    plan.segments.clear();
    for (const SweepSegment& segment : segments) {
        if (segment.valid()) {
            plan.addSegment(segment);
        }
    }
    if (plan.segments.empty()) {
        return;
    }
    if (auto applied = m_state.sweepRange(plan); !applied) {
        toast(ToastSeverity::Error, applied.error().describe());
    }
}

// ---- drawing -------------------------------------------------------------

void MainWindow::drawPanels() {
    PanelLayout& layout = m_state.view().layout;
    const ChromeTheme& chrome = m_state.theme().chrome();

    // Again here, because a toolbar control drawn earlier this frame may have
    // added a panel since the frame began.
    syncPanels();

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    float top = origin.y;

    if (layout.mode == PanelMode::Spans && layout.overview) {
        const float strip = bar::scaled(44.0F);
        drawOverviewStrip(strip);
        top += strip + ImGui::GetStyle().ItemSpacing.y;
    }

    // Whole pixels throughout. A panel ending on a fraction leaves its last
    // column of pixels half-covered, and whatever is behind shows through.
    const PanelRect area{std::floor(origin.x), std::floor(top), std::floor(available.x),
                         std::floor(std::max(origin.y + available.y - top, 1.0F))};

    std::vector<int> attached;
    for (const PanelView& view : layout.panels) {
        if (!view.detached) {
            attached.push_back(view.id);
        }
    }

    const PanelArrangement arrangement = arrangementFor(attached.size(), layout.rowsForTwo);
    const float gap = attached.size() > 1 ? std::round(bar::splitterThickness()) : 0.0F;
    const std::vector<PanelRect> rects = arrangePanels(arrangement, area, layout.splits, gap);

    // Panels are found by id on every turn, never held across one: a header
    // button can change the list, and a reference into it would dangle.
    //
    // With more than one panel each is inset by a pixel, and the focused one's
    // border is drawn in that pixel. Drawn under the panel instead, it showed
    // through every rounded corner and along any edge a child did not quite
    // cover. Square corners for the same reason: a grid of rounded panels
    // leaves slivers of whatever is behind at each corner.
    const bool several = layout.panels.size() > 1;
    const float inset = several ? 1.0F : 0.0F;
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0F);
    for (std::size_t i = 0; i < attached.size() && i < rects.size(); ++i) {
        PanelView* view = layout.find(attached[i]);
        if (view == nullptr) {
            continue;
        }
        const PanelRect& rect = rects[i];
        ViewPanel& panel = runtimeFor(*view);

        ImGui::SetCursorScreenPos(ImVec2(rect.x + inset, rect.y + inset));
        ImGui::PushID(view->id);
        ImGui::BeginChild("##panel",
                          ImVec2(rect.width - (inset * 2.0F), rect.height - (inset * 2.0F)),
                          ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawPanelBody(*view, panel);
        ImGui::EndChild();
        ImGui::PopID();

        // The panel the toolbar acts on, when there is more than one to mean.
        if (several && layout.focusedId == attached[i]) {
            ImGui::GetWindowDrawList()->AddRect(ImVec2(rect.x, rect.y),
                                                ImVec2(rect.x + rect.width, rect.y + rect.height),
                                                packed(chrome.accent), 0.0F, 1.0F);
        }
    }
    ImGui::PopStyleVar();

    if (attached.size() > 1) {
        drawLayoutSplitters(arrangement, rects, area, gap);
    }
}

void MainWindow::drawLayoutSplitters(PanelArrangement arrangement,
                                     const std::vector<PanelRect>& rects, const PanelRect& area,
                                     float gap) {
    PanelLayout& layout = m_state.view().layout;
    const ChromeTheme& chrome = m_state.theme().chrome();
    if (rects.size() < 2) {
        return;
    }

    const std::size_t columns = columnCount(arrangement);
    const std::size_t rows = rowCount(arrangement);

    // A drag moves its divider by the share of the axis the pointer travelled,
    // measured against what is left once the gaps are taken out -- the same
    // length the fractions are fractions of.
    const auto share = [gap](float travel, float length, std::size_t parts) {
        const float inner = length - (gap * static_cast<float>(parts - 1));
        return travel / std::max(inner, 1.0F);
    };

    PanelSplits splits = layout.splits;

    // Between columns, full height. Three's divider is the two-way one.
    const std::size_t verticalParts = arrangement == PanelArrangement::Three ? 2 : columns;
    for (std::size_t d = 0; d + 1 < verticalParts; ++d) {
        const PanelRect& before = rects[d];
        const std::string id = std::format("##splitx{}", d);
        const float travel = layoutSplitter(id.c_str(), ImVec2(before.x + before.width, area.y),
                                            ImVec2(gap, area.height), true, chrome);
        if (verticalParts == 2) {
            splits.x += share(travel, area.width, 2);
        } else {
            splits.thirdsX[d] += share(travel, area.width, 3);
        }
    }

    // Between rows, full width -- except in Three, where only the right-hand
    // column is split.
    if (arrangement == PanelArrangement::Three) {
        const PanelRect& upper = rects[1];
        splits.y += share(layoutSplitter("##splity0", ImVec2(upper.x, upper.y + upper.height),
                                         ImVec2(upper.width, gap), false, chrome),
                          area.height, 2);
    } else {
        for (std::size_t d = 0; d + 1 < rows; ++d) {
            const PanelRect& above = rects[d * columns];
            const std::string id = std::format("##splity{}", d);
            const float travel = layoutSplitter(id.c_str(), ImVec2(area.x, above.y + above.height),
                                                ImVec2(area.width, gap), false, chrome);
            if (rows == 2) {
                splits.y += share(travel, area.height, 2);
            } else {
                splits.thirdsY[d] += share(travel, area.height, 3);
            }
        }
    }

    layout.splits = clampSplits(splits);
}

void MainWindow::drawPanelBody(PanelView& view, ViewPanel& panel) {
    PanelLayout& layout = m_state.view().layout;

    // A click or a wheel inside a panel makes it the one the toolbar acts on.
    // The gesture itself still lands where it was aimed whichever is focused.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
         ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::GetIO().MouseWheel != 0.0F)) {
        layout.focusedId = view.id;
    }

    const ImVec2 position = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    panel.rect = PanelRect{position.x, position.y, size.x, size.y};

    drawPanelHeader(view, panel);

    const float available = ImGui::GetContentRegionAvail().y;
    const float spacing = ImGui::GetStyle().ItemSpacing.y;

    // Both panes keep a floor, so the split can never be dragged to the point
    // where one of them has no usable height and the other cannot be dragged
    // back.
    constexpr float kMinPaneHeight = 90.0F;
    const float budget = available - bar::splitterThickness() - spacing * 2.0F;
    const float floor = std::min(kMinPaneHeight, budget * 0.5F);
    const float waterfallHeight = std::clamp(budget * view.waterfallFraction, std::max(floor, 0.0F),
                                             std::max(budget - floor, 0.0F));
    const float spectrumHeight = budget - waterfallHeight;

    ImGui::BeginChild("##spectrum", ImVec2(0, spectrumHeight));
    drawSpectrum(view, panel);
    ImGui::EndChild();

    drawPaneSplitter(budget, view.waterfallFraction);

    ImGui::BeginChild("##waterfall", ImVec2(0, 0));
    drawWaterfall(view, panel);
    ImGui::EndChild();
}

void MainWindow::drawPanelHeader(PanelView& view, ViewPanel& panel) {
    PanelLayout& layout = m_state.view().layout;
    const ChromeTheme& chrome = m_state.theme().chrome();
    const FrequencySpan range = panelRange(view);
    const bool spans = layout.mode == PanelMode::Spans && view.segment.valid();

    ImGui::BeginChild("##header", ImVec2(0, headerHeight()), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(bar::scaled(4.0F), 0.0F));
    ImGui::SetCursorPos(ImVec2(bar::padding(), bar::padding()));

    const auto position = static_cast<std::size_t>(
        std::ranges::find(layout.panels, view.id, &PanelView::id) - layout.panels.begin());
    const bool several = layout.panels.size() > 1;

    // Everything below sits on this one row. Taken before the chip, whose
    // Dummy moves the cursor down to the next line.
    const float y = ImGui::GetCursorScreenPos().y;

    // The panel's number, which is also what its floating window and its
    // segment on the strip are called.
    float left = ImGui::GetCursorScreenPos().x;
    if (several) {
        const std::string number = std::format("{}", position + 1);
        const ImVec2 text = ImGui::CalcTextSize(number.c_str());
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const float padX = bar::scaled(5.0F);
        const ImVec2 max(min.x + text.x + padX * 2.0F, min.y + text.y);
        const bool focused = layout.focusedId == view.id;
        const Color& fill = focused ? chrome.accent : chrome.border;
        ImGui::GetWindowDrawList()->AddRectFilled(min, max, packed(fill), 3.0F);
        ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + padX, min.y), chipTextColor(fill),
                                            number.c_str());
        ImGui::Dummy(ImVec2(max.x - min.x, text.y));
        left = max.x + bar::scaled(10.0F);
    }

    // Buttons on the right, reserved first so the fields know where to stop.
    const float spacingX = ImGui::GetStyle().ItemSpacing.x;
    const float button = ImGui::CalcTextSize(icon::glyphOr(icon::kClose, "x").c_str()).x +
                         ImGui::GetStyle().FramePadding.x * 2.0F;
    // Only with something to choose between. A lone panel cannot be closed or
    // torn off, and its pause is the Waterfall popover's.
    const bool buttons = several && &view != &m_historyView;
    const float buttonsWidth = buttons ? (button * 3.0F) + (spacingX * 2.0F) : 0.0F;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - bar::padding() -
                        buttonsWidth - (buttons ? bar::scaled(10.0F) : 0.0F);

    struct Field {
        const char* label;
        std::string value;
        int edit = 0; ///< 0 text, 1 the segment's start, 2 its stop.
    };
    // In Spans the row is the segment, which it edits; the axis below already
    // says where inside it the view is.
    const FrequencySpan described = spans ? view.segment : range;
    const std::array<Field, 4> all{{
        {"Start", toml_util::formatFrequency(described.startHz, 3), spans ? 1 : 0},
        {"Center", toml_util::formatFrequency(described.centre(), 3)},
        {"Span", toml_util::formatFrequency(described.width(), 3)},
        {"Stop", toml_util::formatFrequency(described.stopHz, 3), spans ? 2 : 0},
    }};

    constexpr float kLabelGap = 4.0F;
    constexpr float kFieldGap = 22.0F;

    // Every value gets the same reserved width, taken from the widest one that
    // can occur rather than from the text currently in it, so the fields hold
    // still while only the digits move.
    const float valueWidth = ImGui::CalcTextSize("8888.888 MHz").x;
    const auto widthOf = [&](std::span<const Field* const> fields) {
        float total = 0.0F;
        for (const Field* entry : fields) {
            total += ImGui::CalcTextSize(entry->label).x + kLabelGap + valueWidth;
        }
        return total + kFieldGap * static_cast<float>(fields.size() - 1);
    };

    // What fits: all four, then the two edges, then only the width.
    const std::array<const Field*, 4> full{&all[0], &all[1], &all[2], &all[3]};
    const std::array<const Field*, 2> edges{&all[0], &all[3]};
    const std::array<const Field*, 1> width{&all[2]};
    std::span<const Field* const> shown = full;
    if (widthOf(shown) > right - left) {
        shown = edges;
    }
    if (widthOf(shown) > right - left) {
        shown = width;
    }
    const float totalWidth = widthOf(shown);

    // Centred on the plot's data area, not on the panel: the row describes the
    // spectrum, so it should sit over it. The rect is last frame's, which is
    // imperceptible except for one frame after a resize.
    float x = left;
    if (panel.plotWidth > 16.0F) {
        x = std::clamp(panel.plotX + (panel.plotWidth - totalWidth) * 0.5F, left,
                       std::max(left, right - totalWidth));
    }
    if (widthOf(shown) <= right - left) {
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        for (std::size_t i = 0; i < shown.size(); ++i) {
            const Field& entry = *shown[i];
            const float fieldStart = ImGui::GetCursorPosX();

            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", entry.label);
            ImGui::SameLine(0.0F, kLabelGap);

            if (entry.edit == 0) {
                ImGui::TextUnformatted(entry.value.c_str());
            } else {
                // Typed in MHz and applied when the field is left, never per
                // keystroke: every application re-plans the sweep, and the
                // values a number passes through while being typed are
                // mostly nonsense.
                const char* id = entry.edit == 1 ? "##segstart" : "##segstop";
                double& edited = entry.edit == 1 ? panel.editStartMHz : panel.editStopMHz;
                if (ImGui::GetActiveID() != ImGui::GetID(id)) {
                    edited = (entry.edit == 1 ? view.segment.startHz : view.segment.stopHz) / 1e6;
                }
                ImGui::SetNextItemWidth(valueWidth);
                ImGui::InputDouble(id, &edited, 0.0, 0.0, "%.3f MHz");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("This panel's segment, in MHz");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    FrequencySpan wanted = view.segment;
                    (entry.edit == 1 ? wanted.startHz : wanted.stopHz) = edited * 1e6;
                    if (wanted.valid()) {
                        std::vector<SweepSegment> segments = m_state.sweepPlan().segments;
                        const auto own =
                            std::ranges::find_if(segments, [&view](const SweepSegment& segment) {
                                return sameSpan(view.segment, segment);
                            });
                        if (own != segments.end()) {
                            own->startHz = wanted.startHz;
                            own->stopHz = wanted.stopHz;
                            view.segment = wanted;
                            applySegments(segments);
                        }
                    }
                }
            }

            if (i + 1 < shown.size()) {
                ImGui::SameLine(fieldStart + ImGui::CalcTextSize(entry.label).x + kLabelGap +
                                valueWidth + kFieldGap);
            }
        }
    }

    if (buttons) {
        ImGui::SetCursorScreenPos(ImVec2(right + bar::scaled(10.0F), y));

        if (headerButton("##pause", view.waterfallPaused ? icon::kPlay : icon::kPause,
                         view.waterfallPaused ? ">" : "||",
                         view.waterfallPaused ? "Resume this waterfall" : "Pause this waterfall")) {
            view.waterfallPaused = !view.waterfallPaused;
        }

        ImGui::SameLine();
        const bool canDetach = view.detached || layout.attachedCount() > 1;
        if (headerButton("##detach", icon::kDetach, "^",
                         view.detached ? "Back into the main window" : "Into a window of its own",
                         canDetach)) {
            view.detached = !view.detached;
        }

        // In Spans a panel is its segment, so closing one stops sweeping it --
        // unless another panel shows the same one.
        ImGui::SameLine();
        const bool removesSegment = spans && !segmentShared(layout, view);
        const bool lastSegment = removesSegment && m_state.sweepPlan().segments.size() <= 1;
        if (headerButton("##close", icon::kClose, "x",
                         removesSegment ? "Remove this segment from the sweep" : "Close this panel",
                         several && !lastSegment)) {
            m_closePanelId = view.id;
        }
    }

    ImGui::PopStyleVar();
    ImGui::EndChild();
}

void MainWindow::closePanel(int id) {
    PanelLayout& layout = m_state.view().layout;
    const PanelView* view = layout.find(id);
    if (view == nullptr || layout.panels.size() <= 1) {
        return;
    }

    if (layout.mode == PanelMode::Spans && view->segment.valid() && !segmentShared(layout, *view)) {
        std::vector<SweepSegment> segments = m_state.sweepPlan().segments;
        const FrequencySpan segment = view->segment;
        std::erase_if(segments,
                      [&segment](const SweepSegment& entry) { return sameSpan(segment, entry); });
        if (segments.empty()) {
            return;
        }
        layout.remove(id);
        applySegments(segments);
    } else {
        layout.remove(id);
    }

    // The main window must keep something to show when the last panel left
    // in it was the one closed.
    if (layout.attachedCount() == 0) {
        layout.panels.front().detached = false;
    }
}

void MainWindow::drawFloatingPanels() {
    PanelLayout& layout = m_state.view().layout;

    std::vector<int> detached;
    for (const PanelView& view : layout.panels) {
        if (view.detached) {
            detached.push_back(view.id);
        }
    }

    for (const int id : detached) {
        PanelView* view = layout.find(id);
        if (view == nullptr) {
            continue;
        }
        ViewPanel& panel = runtimeFor(*view);
        const auto position = static_cast<std::size_t>(
            std::ranges::find(layout.panels, id, &PanelView::id) - layout.panels.begin());

        // Its own platform window whenever viewports are on, never merged back
        // into the main one by being dragged over it -- that is what the
        // detach button promised.
        ImGuiWindowClass windowClass;
        windowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
        ImGui::SetNextWindowClass(&windowClass);

        // Beside where it was, so tearing one off is visibly the same panel.
        constexpr float kOffset = 24.0F;
        ImGui::SetNextWindowPos(ImVec2(panel.rect.x + kOffset, panel.rect.y + kOffset),
                                ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(std::max(panel.rect.width, bar::scaled(480.0F)),
                                        std::max(panel.rect.height, bar::scaled(320.0F))),
                                 ImGuiCond_Appearing);

        ImGui::PushStyleColor(ImGuiCol_WindowBg, toImVec4(m_state.theme().spectrum().background));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, toImVec4(m_state.theme().spectrum().background));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        // Square: as an OS window of its own, a rounded corner shows the
        // platform window's background through it.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0F);

        bool open = true;
        const std::string title = std::format("Panel {}###panel{}", position + 1, id);
        if (ImGui::Begin(title.c_str(), &open,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse)) {
            ImGui::PushID(id);
            drawPanelBody(*view, panel);
            ImGui::PopID();
        }
        ImGui::End();

        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);

        // Closed from its own button or from the window manager's: either way
        // it goes back where it came from rather than away.
        if (!open) {
            if (PanelView* closed = layout.find(id)) {
                closed->detached = false;
            }
        }
    }
}

// ---- overview strip ------------------------------------------------------

void MainWindow::drawOverviewStrip(float height) {
    PanelLayout& layout = m_state.view().layout;
    const ChromeTheme& chrome = m_state.theme().chrome();
    const SpectrumTheme& colors = m_state.theme().spectrum();
    const std::vector<SweepSegment>& segments = m_state.sweepPlan().segments;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    if (width <= 16.0F || height <= 8.0F) {
        return;
    }

    // The radio's whole reach when there is one; otherwise the plan with a
    // margin either side, so its outer segments are not flush with the edges.
    FrequencySpan full;
    if (const ISdrDevice* device = m_state.device()) {
        full = {std::max(0.0, device->info().minFrequencyHz), device->info().maxFrequencyHz};
    }
    if (!full.valid()) {
        const SweepPlan& plan = m_state.sweepPlan();
        const double margin = std::max((plan.highestHz() - plan.lowestHz()) * 0.05, 1e6);
        full = {std::max(0.0, plan.lowestHz() - margin), plan.highestHz() + margin};
    }
    if (!full.valid()) {
        full = m_state.fitRange();
    }
    if (m_overviewView.valid()) {
        if (const auto inside = clampView(m_overviewView.startHz, m_overviewView.stopHz,
                                          ViewLimits{full.startHz, full.stopHz})) {
            m_overviewView = *inside;
        }
    }
    const FrequencySpan range = m_overviewView.valid() ? m_overviewView : full;

    SpectrumLayout strip;
    strip.origin = origin;
    strip.size = ImVec2(width, height);
    strip.fromHz = range.startHz;
    strip.toHz = range.stopHz;
    strip.minDb = layout.focused().yMinDb;
    strip.maxDb = layout.focused().yMaxDb;

    ImGui::InvisibleButton("##overview", strip.size);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 far(origin.x + width, origin.y + height);
    draw->AddRectFilled(origin, far, packed(colors.background));
    draw->PushClipRect(origin, far, true);

    // The segments under the trace, numbered by the panel showing each. The
    // focused panel's in the accent; one no panel shows, dimmed -- it is still
    // swept, and clicking it gives it the focused panel.
    const auto panelFor = [&layout](const SweepSegment& segment) -> const PanelView* {
        for (const PanelView& view : layout.panels) {
            if (sameSpan(view.segment, segment)) {
                return &view;
            }
        }
        return nullptr;
    };

    int hoveredSegment = -1;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const SweepSegment& segment = segments[i];
        const float x0 = std::max(strip.xForHz(segment.startHz), origin.x);
        const float x1 = std::min(strip.xForHz(segment.stopHz), far.x);
        if (x1 <= x0) {
            continue;
        }
        const PanelView* shown = panelFor(segment);
        const bool focused = shown != nullptr && shown->id == layout.focusedId;
        const Color& tint = focused            ? chrome.accent
                            : shown != nullptr ? chrome.text
                                               : chrome.textDim;

        draw->AddRectFilled(ImVec2(x0, origin.y), ImVec2(std::max(x1, x0 + 2.0F), far.y),
                            packed(tint.withAlpha(shown != nullptr ? 0.18F : 0.08F)));
        draw->AddRect(ImVec2(x0, origin.y), ImVec2(std::max(x1, x0 + 2.0F), far.y),
                      packed(tint.withAlpha(shown != nullptr ? 0.8F : 0.4F)));

        if (shown != nullptr) {
            const auto number = static_cast<std::size_t>(
                std::ranges::find(layout.panels, shown->id, &PanelView::id) -
                layout.panels.begin());
            const std::string label = std::format("{}", number + 1);
            if (x1 - x0 > ImGui::CalcTextSize(label.c_str()).x + 6.0F) {
                draw->AddText(ImVec2(x0 + 3.0F, origin.y + 2.0F), packed(tint), label.c_str());
            }
        }

        const float mouseX = ImGui::GetIO().MousePos.x;
        if (hovered && mouseX >= x0 - 2.0F && mouseX <= std::max(x1, x0 + 2.0F) + 2.0F) {
            hoveredSegment = static_cast<int>(i);
        }
    }

    // A coarse live trace: one column per pixel, from the strip's own cache so
    // it never evicts a panel's.
    {
        const TraceStore& traces = m_state.traces();
        const auto pixels = static_cast<std::size_t>(std::max(width, 1.0F));
        const Envelope& envelope = traces.envelope(TraceKind::Live, range.startHz, range.stopHz,
                                                   pixels, m_overviewEnvelopes);
        const float column = width / static_cast<float>(std::max<std::size_t>(envelope.size(), 1));
        for (std::size_t i = 0; i < envelope.size(); ++i) {
            if (!measured(envelope.maximum[i])) {
                continue;
            }
            const float x = origin.x + (static_cast<float>(i) + 0.5F) * column;
            const float y = std::clamp(strip.yForDb(envelope.maximum[i]), origin.y, far.y);
            draw->AddLine(ImVec2(x, far.y), ImVec2(x, y), packed(colors.traceLive.withAlpha(0.55F)),
                          std::max(column, 1.0F));
        }
    }

    draw->AddRect(origin, far, packed(chrome.border));

    // ---- gestures --------------------------------------------------------

    const ImGuiIO& io = ImGui::GetIO();

    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !io.KeyShift) {
        m_overviewView = {};
    } else if (hovered && io.MouseWheel != 0.0F) {
        const float wheel = std::clamp(io.MouseWheel, -2.5F, 2.5F);
        const FrequencySpan zoomed = zoomAbout(range, strip.hzForX(io.MousePos.x),
                                               std::pow(0.8, static_cast<double>(wheel)));
        if (const auto inside =
                clampView(zoomed.startHz, zoomed.stopHz, ViewLimits{full.startHz, full.stopHz})) {
            m_overviewView = *inside;
        }
    }

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (io.KeyShift) {
            m_overviewSelecting = true;
            m_overviewSelectStartX = io.MousePos.x;
        } else if (hoveredSegment >= 0) {
            // Its panel, focused; or, when none shows it, the focused panel
            // moved onto it.
            const SweepSegment& segment = segments[static_cast<std::size_t>(hoveredSegment)];
            if (const PanelView* shown = panelFor(segment)) {
                layout.focusedId = shown->id;
            } else {
                PanelView& target = layout.focused();
                target.segment = {segment.startHz, segment.stopHz};
                target.viewStartHz = 0.0;
                target.viewStopHz = 0.0;
            }
        }
    }

    if (m_overviewSelecting) {
        const float current = std::clamp(io.MousePos.x, origin.x, far.x);
        const float fromX = std::min(m_overviewSelectStartX, current);
        const float toX = std::max(m_overviewSelectStartX, current);
        draw->AddRectFilled(ImVec2(fromX, origin.y), ImVec2(toX, far.y),
                            packed(chrome.ok.withAlpha(0.25F)));
        draw->AddRect(ImVec2(fromX, origin.y), ImVec2(toX, far.y), packed(chrome.ok));

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            m_overviewSelecting = false;
            if (toX - fromX >= 4.0F) {
                std::vector<SweepSegment> next = segments;
                next.push_back(
                    SweepSegment{.startHz = strip.hzForX(fromX), .stopHz = strip.hzForX(toX)});
                applySegments(next);
            }
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_overviewSelecting = false;
        }
    }

    if (hovered && !m_overviewSelecting) {
        if (hoveredSegment >= 0) {
            const SweepSegment& segment = segments[static_cast<std::size_t>(hoveredSegment)];
            ImGui::SetTooltip("%s - %s\n%s\nClick: %s\nShift+drag: add a segment",
                              toml_util::formatFrequency(segment.startHz, 3).c_str(),
                              toml_util::formatFrequency(segment.stopHz, 3).c_str(),
                              toml_util::formatFrequency(segment.spanHz(), 3).c_str(),
                              panelFor(segment) != nullptr ? "focus its panel"
                                                           : "show it in the focused panel");
        } else {
            ImGui::SetTooltip("%s\nShift+drag: add a segment\nWheel: zoom, double-click: reset",
                              toml_util::formatFrequency(strip.hzForX(io.MousePos.x), 3).c_str());
        }
    }

    draw->PopClipRect();
}

} // namespace sweeppp::ui
