// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// Spectrum and waterfall drawing. Split from MainWindow.cpp because this is
// the part with real geometry in it, and it is easier to reason about alone.
#include "BarChrome.hpp"
#include "ContributionOverlay.hpp"
#include "MainWindow.hpp"
#include "PlotGeometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/core/Toml.hpp>
#include <sweeppp/plugin/PluginHost.hpp>

namespace sweeppp::ui {
namespace {

constexpr float kHandleSize = 10.0F;
constexpr float kGradientBarWidth = 18.0F;

/// How far the cursor may travel and still count as a click rather than a drag.
///
/// The same few pixels separate a pan from a marker pick and a band selection
/// from a click that happened to have shift held. One number, because from the
/// operator's side it is one thing: how still a hand has to be to have meant a
/// click.
constexpr float kClickSlopPx = 6.0F;

ImU32 packed(const Color& color) {
    return color.packed();
}

/// Top inset of the readout row: RBW / FFT on the left, VBW / points / FPS on
/// the right.
///
/// Anything that labels the plot has to clear this row or both become
/// unreadable. Passed to the contribution overlay rather than duplicated in
/// it, which is what the band-plan plugin used to have to do from outside the
/// application.
constexpr float kOverlayTextTop = 6.0F;

ImVec4 toImVec4(const Color& color) {
    return {color.r, color.g, color.b, color.a};
}

} // namespace

namespace {

/// Contributions across a range, ranked, minus the contributors the operator
/// has hidden.
///
/// The tick is a drawing setting: a hidden contributor is dropped here and
/// still answers the marker's chip, because what is drawn and what is known
/// are separate questions.
std::vector<Contribution> visibleContributions(double fromHz, double toHz) {
    std::vector<Contribution> found = PluginManager::instance().contributionsIn(fromHz, toHz);

    // A contributor that draws its own is left to it. It still answers every
    // other query -- the marker readout, the hover list -- so what it knows is
    // not lost, only its painting is its own.
    std::erase_if(found, [](const Contribution& entry) { return !entry.hostRendered; });

    // Asked once per contributor rather than once per contribution: the answer
    // is the same for every span one plugin returned, and there are hundreds
    // of those on a wide view.
    std::vector<std::string> hidden;
    for (const std::string& id : PluginManager::instance().contributorOrder()) {
        if (!PluginManager::instance().contributorShown(id)) {
            hidden.push_back(id);
        }
    }
    if (!hidden.empty()) {
        std::erase_if(found, [&hidden](const Contribution& entry) {
            return std::ranges::find(hidden, entry.pluginId) != hidden.end();
        });
    }

    return found;
}

/// Where a plugin's overlay is being asked to draw.
///
/// Mirrors `SpectrumLayout` field for field, which is the point: the plugin
/// side reimplements `xForHz`/`hzForX`/`yForDb` from these same numbers rather
/// than calling back once per point, and identical inputs are what stop the
/// two arithmetics drifting.
sweeppp_plot_context_t pluginPlotContext(const SpectrumLayout& layout, ImDrawList* draw,
                                         sweeppp_ui_spot_t spot, sweeppp_ui_layer_t layer,
                                         double markerHz, float overlayAlpha) {
    sweeppp_plot_context_t context{};
    context.struct_size = sizeof(context);
    context.spot = spot;
    context.layer = layer;
    context.draw_list = draw;
    context.origin_x = layout.origin.x;
    context.origin_y = layout.origin.y;
    context.size_x = layout.size.x;
    context.size_y = layout.size.y;
    context.from_hz = layout.fromHz;
    context.to_hz = layout.toHz;
    context.min_db = layout.minDb;
    context.max_db = layout.maxDb;
    context.marker_hz = markerHz;
    context.overlay_alpha = overlayAlpha;
    return context;
}

} // namespace

void MainWindow::drawSpectrum() {
    ViewSettings& view = m_state.view();
    const SpectrumTheme& colors = m_state.theme().spectrum();

    double fromHz = 0.0;
    double toHz = 0.0;
    m_state.visibleRange(fromHz, toHz);

    const ImVec2 available = ImGui::GetContentRegionAvail();
    // Room on the right for the gradient bar.
    const ImVec2 plotSize(available.x - kGradientBarWidth - 8.0F, available.y);
    if (plotSize.x <= 32.0F || plotSize.y <= 32.0F) {
        return;
    }

    ImPlot::PushStyleColor(ImPlotCol_FrameBg, toImVec4(colors.background));
    ImPlot::PushStyleColor(ImPlotCol_PlotBg, toImVec4(colors.background));
    ImPlot::PushStyleColor(ImPlotCol_AxisText, toImVec4(colors.axisText));
    ImPlot::PushStyleColor(ImPlotCol_AxisGrid, toImVec4(colors.grid));

    SpectrumLayout layout;
    layout.fromHz = fromHz;
    layout.toHz = toHz;
    layout.minDb = view.yMinDb;
    layout.maxDb = view.yMaxDb;

    if (ImPlot::BeginPlot("##spectrum", plotSize,
                          ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes("Frequency (MHz)", "dBFS", ImPlotAxisFlags_NoHighlight,
                          ImPlotAxisFlags_NoHighlight);
        ImPlot::SetupAxisLimits(ImAxis_X1, fromHz / 1e6, toHz / 1e6, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, static_cast<double>(view.yMinDb),
                                static_cast<double>(view.yMaxDb), ImPlotCond_Always);
        if (!view.showGrid) {
            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoGridLines,
                              ImPlotAxisFlags_NoGridLines);
        }

        layout.origin = ImPlot::GetPlotPos();
        layout.size = ImPlot::GetPlotSize();

        // Handed to the waterfall so both panes share one frequency axis.
        m_spectrumPlotX = layout.origin.x;
        m_spectrumPlotWidth = layout.size.x;

        // The plot is an ImPlot host for axes, grid and interaction; the trace
        // itself is drawn by hand into the same draw list. Per-pixel min/max
        // envelopes are not something a line series can express, and drawing a
        // million points as a line series would be both slow and wrong.
        drawSpectrumOverlay(layout);

        ImPlot::EndPlot();
    }

    ImPlot::PopStyleColor(4);

    // The vertical gradient bar with two handles, immediately right of the
    // plot. Its handles set the waterfall's colour limits.
    //
    // Positioned from the plot's own rectangle rather than with SameLine.
    // SameLine anchors to the *last item*, and drawSpectrumOverlay() creates
    // the Y-axis handle buttons inside the plot -- so SameLine would put this
    // bar just right of a Y handle, near the plot's left edge.
    if (layout.size.x <= 0.0F || layout.size.y <= 0.0F) {
        return;
    }
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin(layout.origin.x + layout.size.x + 8.0F, layout.origin.y);
        const float height = layout.size.y;
        const float top = layout.origin.y;

        const ColorMap& map = m_state.theme().waterfallColorMap();

        // Drawn as horizontal slices of the LUT, so the bar always shows
        // exactly what the waterfall will use.
        constexpr int kSlices = 64;
        for (int i = 0; i < kSlices; ++i) {
            const float t0 = static_cast<float>(i) / kSlices;
            const float t1 = static_cast<float>(i + 1) / kSlices;
            const ImVec2 a(origin.x, top + height * (1.0F - t1));
            const ImVec2 b(origin.x + kGradientBarWidth, top + height * (1.0F - t0));
            draw->AddRectFilled(a, b, packed(map.sample((t0 + t1) * 0.5F)));
        }
        draw->AddRect(ImVec2(origin.x, top), ImVec2(origin.x + kGradientBarWidth, top + height),
                      packed(m_state.theme().chrome().border));

        // Two handles: gradient min at the bottom, max at the top.
        const auto handleY = [&](float db) {
            const float span = view.yMaxDb - view.yMinDb;
            const float t = span > 0.0F ? (db - view.yMinDb) / span : 0.0F;
            return top + height * (1.0F - std::clamp(t, 0.0F, 1.0F));
        };

        const float maxY = handleY(view.gradientMaxDb);
        const float minY = handleY(view.gradientMinDb);
        const ImU32 handleColor = packed(m_state.theme().chrome().accent);

        draw->AddTriangleFilled(ImVec2(origin.x - 6.0F, maxY), ImVec2(origin.x, maxY - 5.0F),
                                ImVec2(origin.x, maxY + 5.0F), handleColor);
        draw->AddTriangleFilled(ImVec2(origin.x - 6.0F, minY), ImVec2(origin.x, minY - 5.0F),
                                ImVec2(origin.x, minY + 5.0F), handleColor);

        ImGui::SetCursorScreenPos(ImVec2(origin.x - 8.0F, top));
        ImGui::InvisibleButton("##gradientbar", ImVec2(kGradientBarWidth + 8.0F, height));
        // Which handle, and where it started, decided once on the grab frame.
        // The hit box covers the whole bar so either handle can be caught
        // anywhere along it; without pinning the choice and the origin here, a
        // touch would both pick a handle and fling it to the cursor at once.
        if (ImGui::IsItemActivated()) {
            const float mouseY = ImGui::GetIO().MousePos.y;
            const bool grabbedMax = std::abs(mouseY - maxY) < std::abs(mouseY - minY);
            m_dragging = grabbedMax ? DragTarget::GradientMax : DragTarget::GradientMin;
            m_dragAnchorDb = grabbedMax ? view.gradientMaxDb : view.gradientMinDb;
            m_dragAnchorY = mouseY;
        }

        if (ImGui::IsItemActive() &&
            (m_dragging == DragTarget::GradientMin || m_dragging == DragTarget::GradientMax)) {
            const float mouseY = ImGui::GetIO().MousePos.y;
            const float dbPerPixel = height > 0.0F ? (view.yMaxDb - view.yMinDb) / height : 0.0F;
            const float db = m_dragAnchorDb - (mouseY - m_dragAnchorY) * dbPerPixel;

            if (m_dragging == DragTarget::GradientMax) {
                view.gradientMaxDb = std::clamp(db, view.gradientMinDb + 1.0F, view.yMaxDb);
            } else {
                view.gradientMinDb = std::clamp(db, view.yMinDb, view.gradientMaxDb - 1.0F);
            }
        } else if (m_dragging == DragTarget::GradientMin || m_dragging == DragTarget::GradientMax) {
            m_dragging = DragTarget::None;
        }

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Waterfall gradient\n%.1f .. %.1f dBFS\nDrag a handle to change it.",
                              static_cast<double>(view.gradientMinDb),
                              static_cast<double>(view.gradientMaxDb));
        }
    }
}

void MainWindow::handleFrequencyGestures(const SpectrumLayout& layout, bool hovered,
                                         FrequencyPane pane, MarkerClicks markerClicks) {
    const SpectrumTheme& colors = m_state.theme().spectrum();
    const bool placeMarkers = markerClicks == MarkerClicks::Enabled;
    MarkerSet& markers = m_state.markers();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    if (hovered) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;

        // Shift-drag selects a band to zoom into.
        //
        // The fastest way to get from "something is happening around there" to
        // reading it: the wheel needs several notches and a steady hand to
        // land on a narrow signal, while a drag says exactly which frequencies
        // matter in one gesture. Shift keeps it out of the way of the pan,
        // which is the unmodified left drag.
        // Adding a modifier retargets the same gesture at the radio instead of
        // the display: shift alone chooses what to *look* at, shift with ctrl
        // (or command) chooses what to *measure*, and alt on top of that adds
        // to what is measured rather than replacing it. Selecting a band and
        // sweeping only that band is the single biggest thing an operator can
        // do for resolution and sweep rate, and it should not require finding
        // the range editor and typing the numbers that are already on screen.
        const ImGuiIO& modifiers = ImGui::GetIO();

        // Ctrl and Command are read as the same modifier throughout: macOS
        // operators reach for Command, and macOS itself turns a ctrl-click into
        // a right click on some paths.
        const bool addModifier = modifiers.KeyCtrl || modifiers.KeySuper;

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && modifiers.KeyShift &&
            m_dragging == DragTarget::None) {
            m_selecting = true;
            m_selectionStartX = mouse.x;
            m_selectionPane = pane;

            m_selectionMode = !addModifier       ? SelectionMode::ZoomView
                              : modifiers.KeyAlt ? SelectionMode::AddSweepRange
                                                 : SelectionMode::SweepRange;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !addModifier &&
                   !m_contributions.claimedClick && m_dragging == DragTarget::None) {
            // The left button pans, and on release without travel it picks a
            // marker instead. Not with ctrl or command held, which belongs to
            // dismissing a contribution, and not when a flag has already taken
            // the click: moving the view as well would be a side effect of
            // pointing at a label.
            m_panning = true;
            m_panPane = pane;
            m_panAnchorX = mouse.x;
            m_panStartFromHz = layout.fromHz;
            m_panStartToHz = layout.toHz;
        }

        // The right button owns the markers. Not while shift is held, so a
        // shift-drag re-planning the sweep is never interrupted by one.
        //
        // The macOS hazard is now benign: a ctrl+left that the OS rewrites into
        // a right click reads as "add a marker" rather than as anything
        // destructive.
        if (placeMarkers && !modifiers.KeyShift) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                m_markerDrag = true;
                const double hz = layout.hzForX(mouse.x);

                Marker* active = markers.active();
                if (addModifier || active == nullptr) {
                    // With nothing selected the first right click still just
                    // works: there is no separate "create" gesture to find.
                    markers.add(hz);
                } else {
                    active->frequencyHz = hz;
                    active->peakLocked = false;
                }
            } else if (m_markerDrag && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                // Held down, the active marker follows the cursor, so walking
                // one onto a peak is one gesture rather than a series of clicks.
                if (Marker* active = markers.active()) {
                    active->frequencyHz = layout.hzForX(mouse.x);
                    active->peakLocked = false;
                }
            }
        }

        // Wheel zooms about the cursor, which is what makes zooming feel like
        // it is following the signal rather than the centre of the screen.
        // Shift holds the span and slides it instead -- the usual pairing, and
        // the only way to move sideways without a drag once zoomed in far
        // enough that the interesting signal is off-screen.
        const ImGuiIO& io = ImGui::GetIO();
        const double span = layout.toHz - layout.fromHz;

        // The wheel is read by how far it turned, never per event.
        //
        // One notch of a mouse wheel is 1.0 and arrives on its own; a trackpad
        // reports a stream of fractions for as long as the fingers are moving,
        // and GLFW folds macOS's precise deltas into the same units rather than
        // telling us which device they came from. So the only thing that
        // separates the two is magnitude, and it has to be respected.
        //
        // A clamp on top, because a flick can sum to tens of units in one
        // frame and nothing an operator means by "scroll" is a hundredfold
        // zoom -- generous enough that a fast spin of a real wheel still lands
        // inside it.
        constexpr float kMaxWheelPerFrame = 2.5F;
        const float wheelY = std::clamp(io.MouseWheel, -kMaxWheelPerFrame, kMaxWheelPerFrame);
        const float wheelX = std::clamp(io.MouseWheelH, -kMaxWheelPerFrame, kMaxWheelPerFrame);

        // Horizontal wheel first, and it consumes the gesture.
        //
        // A trackpad reports shift+scroll as a horizontal event already, so
        // taking both would pan twice for one flick on macOS and once
        // everywhere else.
        // Ctrl belongs to the waterfall's scrollback, so it is left alone
        // here -- zooming and scrolling back on the same gesture would fight.
        const float panWheel = io.KeyCtrl       ? 0.0F
                               : wheelX != 0.0F ? -wheelX
                               : io.KeyShift    ? wheelY
                                                : 0.0F;

        if (panWheel != 0.0F) {
            // A fifth of the visible span per notch: enough to cross the plot
            // in a few flicks, small enough to land on something.
            const double stepHz = span * 0.2 * -static_cast<double>(panWheel);
            m_state.setVisibleRange(layout.fromHz + stepHz, layout.toHz + stepHz);
        } else if (wheelY != 0.0F && !io.KeyCtrl) {
            // Raised to the delta rather than picked from its sign: a whole
            // notch still zooms by a fifth, a tenth of one by two percent, and
            // the frames of a trackpad scroll compose into exactly the zoom
            // their sum describes. The history gutter's wheel already works
            // this way.
            const double anchor = layout.hzForX(mouse.x);
            const double factor = std::pow(0.8, static_cast<double>(wheelY));
            const double newFrom = anchor - (anchor - layout.fromHz) * factor;
            const double newTo = anchor + (layout.toHz - anchor) * factor;
            m_state.setVisibleRange(newFrom, newTo);
        }
    }

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        m_markerDrag = false;
    }

    // Panning, tracked outside the hovered test for the same reason the
    // selection below is: a drag that leaves the pane still has to finish.
    if (m_panning && m_panPane == pane) {
        const float travel = ImGui::GetIO().MousePos.x - m_panAnchorX;

        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

            // Only once the cursor has actually moved. A window of equal values
            // is not the same thing as no window at all -- zero means "fit the
            // whole span" -- so a button held still must not quietly pin the
            // view to where it happens to be.
            if (travel != 0.0F) {
                // Against the window the drag started from, so the gesture is
                // exactly reversible even after pushing into the radio's limits.
                const double hzPerPx = layout.size.x > 0.0F ? (m_panStartToHz - m_panStartFromHz) /
                                                                  static_cast<double>(layout.size.x)
                                                            : 0.0;
                const double deltaHz = -static_cast<double>(travel) * hzPerPx;
                m_state.setVisibleRange(m_panStartFromHz + deltaHz, m_panStartToHz + deltaHz);
            }
        } else {
            m_panning = false;

            // A gesture that went nowhere was a click, not a pan: it picks the
            // marker under the cursor, and picks nothing when there is none.
            if (placeMarkers && std::abs(travel) < kClickSlopPx) {
                const double toleranceHz = layout.size.x > 0.0F
                                               ? std::abs(static_cast<double>(kClickSlopPx) *
                                                          (layout.toHz - layout.fromHz) /
                                                          static_cast<double>(layout.size.x))
                                               : 0.0;
                const Marker* hit =
                    markers.nearest(layout.hzForX(ImGui::GetIO().MousePos.x), toleranceHz);
                markers.activeId = hit != nullptr ? hit->id : 0;
            }
        }
    }

    // Tracked outside the hovered test on purpose: a drag that overshoots the
    // edge of the plot should still finish, clamped, rather than being
    // abandoned halfway with the band left on screen.
    if (m_selecting) {
        const float currentX =
            std::clamp(ImGui::GetIO().MousePos.x, layout.origin.x, layout.origin.x + layout.size.x);
        const float fromX = std::min(m_selectionStartX, currentX);
        const float toX = std::max(m_selectionStartX, currentX);
        const float top = layout.origin.y;
        const float bottom = layout.origin.y + layout.size.y;

        // Coloured by what the release will do. Re-planning the sweep retunes
        // the radio and starts a new segment in the session; zooming only
        // changes what is drawn. Those are different enough in consequence
        // that the band should not look the same for both.
        const bool adding = m_selectionMode == SelectionMode::AddSweepRange;
        const bool replanning = adding || m_selectionMode == SelectionMode::SweepRange;

        // Adding is coloured apart from replacing: one keeps what is already
        // being swept and one throws it away, and finding out which by trying
        // it costs the operator their plan.
        const Color band = adding       ? m_state.theme().chrome().ok
                           : replanning ? m_state.theme().chrome().record
                                        : colors.selection;

        draw->AddRectFilled(ImVec2(fromX, top), ImVec2(toX, bottom), packed(band.withAlpha(0.20F)));
        draw->AddLine(ImVec2(fromX, top), ImVec2(fromX, bottom), packed(band), 1.0F);
        draw->AddLine(ImVec2(toX, top), ImVec2(toX, bottom), packed(band), 1.0F);

        // The width in Hz, while dragging, and which of the two it is. Choosing
        // a span is the whole point of the gesture, so the number being chosen
        // should be visible during it rather than only in the axis afterwards.
        const double fromHz = layout.hzForX(fromX);
        const double toHz = layout.hzForX(toX);
        const std::string label = std::format("{}{}",
                                              adding       ? "add "
                                              : replanning ? "sweep "
                                                           : "",
                                              toml_util::formatFrequencyShort(toHz - fromHz));
        const ImVec2 size = ImGui::CalcTextSize(label.c_str());
        draw->AddText(ImVec2((fromX + toX - size.x) * 0.5F, top + 24.0F), packed(colors.markerText),
                      label.c_str());

        if (pane != m_selectionPane) {
            // The other pane draws the band so the selection is visible across
            // both, but must not act on it.
            return;
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            m_selecting = false;
            // A few pixels is a click that happened to have shift held, not a
            // selection. Acting on it would leave a span of essentially
            // nothing and no obvious way back.
            if (toX - fromX >= kClickSlopPx) {
                if (replanning) {
                    SweepPlan plan = m_state.sweepPlan();
                    if (adding) {
                        // Merged rather than appended: a range abutting one
                        // already planned is one range, and overlapping
                        // segments are not a plan the stitcher can resolve.
                        plan.addSegment(SweepSegment{.startHz = fromHz, .stopHz = toHz});
                    } else {
                        plan.segments = {SweepSegment{.startHz = fromHz, .stopHz = toHz}};
                    }

                    // Asking the radio to cover a band implies sweeping it,
                    // even if it was sitting on one centre frequency before.
                    if (auto applied = m_state.sweepRange(plan); !applied) {
                        toast(ToastSeverity::Error, applied.error().describe());
                    }
                } else {
                    m_state.setVisibleRange(fromHz, toHz);
                }
            }
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_selecting = false;
        }
    }
}

void MainWindow::drawSpectrumOverlay(const SpectrumLayout& layout) {
    ViewSettings& view = m_state.view();
    const SpectrumTheme& colors = m_state.theme().spectrum();
    const TraceStore& traces = m_state.traces();

    ImDrawList* draw = ImPlot::GetPlotDrawList();

    // Display points: either the widget width, or a fixed count. This is what
    // makes VBW meaningful -- it is span / points, so the operator can trade
    // detail against smoothness explicitly.
    const auto pixels = static_cast<std::size_t>(
        view.autoPoints ? std::max(layout.size.x, 1.0F)
                        : static_cast<float>(std::clamp(view.displayPoints, 64, 16384)));
    if (pixels == 0) {
        return;
    }

    ImPlot::PushPlotClipRect();

    // ---- antenna coverage, under even the contributions --------------------
    //
    // Full height and per leg rather than merged: two antennas both covering
    // 2.4 GHz is a fact worth seeing, and the stacked alpha says it without a
    // second colour. Each band is labelled, which is what carries *which*
    // antenna -- merging would have printed one band nothing could name.
    if (view.showAntennaRanges) {
        const ChromeTheme& chrome = m_state.theme().chrome();
        const float top = layout.origin.y;
        const float bottom = layout.origin.y + layout.size.y;

        for (const RfLeg& leg : m_state.rfPath()) {
            const double fromHz = std::max(leg.route.startHz, layout.fromHz);
            const double toHz = std::min(leg.route.stopHz, layout.toHz);
            if (toHz <= fromHz) {
                continue;
            }

            const float x0 = layout.xForHz(fromHz);
            const float x1 = layout.xForHz(toHz);

            draw->AddRectFilled(ImVec2(x0, top), ImVec2(x1, bottom),
                                packed(chrome.accent.withAlpha(0.10F)));
            // Edges at full strength, because the band's *ends* are the whole
            // point -- a wash with no boundary says nothing about where
            // coverage stops.
            draw->AddLine(ImVec2(x0, top), ImVec2(x0, bottom),
                          packed(chrome.accent.withAlpha(0.55F)), 1.0F);
            draw->AddLine(ImVec2(x1, top), ImVec2(x1, bottom),
                          packed(chrome.accent.withAlpha(0.55F)), 1.0F);

            // Along the bottom, where the readout row and the contribution
            // chips are not. Only when the band is wide enough to hold it --
            // a clipped name is worse than none, because it reads as a
            // different antenna.
            const std::string label = std::format("{} · {}", leg.antenna->name, leg.portLabel);
            const ImVec2 size = ImGui::CalcTextSize(label.c_str());
            if (x1 - x0 > size.x + 10.0F) {
                draw->AddText(ImVec2(x0 + 5.0F, bottom - size.y - 4.0F),
                              packed(chrome.accent.withAlpha(0.85F)), label.c_str());
            }
        }
    }

    // ---- contributions, under everything ----------------------------------
    //
    // Drawn by the host from data, not by whoever contributed it: the spans,
    // the labels and the inset that clears the readout row have one
    // implementation, and a contributor ships no drawing code at all.
    const std::vector<Contribution> contributions =
        visibleContributions(layout.fromHz, layout.toHz);

    // This runs every frame ahead of the gesture handling below, and clears
    // `claimedClick` on the way in, so it is also that flag's reset.
    // `IsPlotHovered` and not a rectangle test: it goes through ImGui's own
    // hover rules, so an open panel, a popup or a drag elsewhere all take the
    // flags out of the pointer's way rather than only the ones this file could
    // think to check for.
    drawContributions(draw, layout, contributions,
                      ContributionStyle{.hoverable = ImPlot::IsPlotHovered(),
                                        .alpha = colors.contributionAlpha,
                                        .textTop = kOverlayTextTop,
                                        .bands = view.showBandContributions,
                                        .channels = view.showChannelContributions},
                      m_contributions);

    // ---- plugin overlays, under the traces --------------------------------
    //
    // For a plugin drawing something the host has no vocabulary for. Inside
    // the clip rect, before the traces, once per frame on this thread.
    // Anything a plugin throws is caught by the dispatcher and disables that
    // facet with the reason -- the same containment `FrameBus::publish` gives
    // frame consumers, and for the same reason: one misbehaving plugin must
    // not take down the instrument.
    const auto pluginOverlay = [&](sweeppp_ui_layer_t layer) {
        const Marker* active = m_state.markers().active();
        PluginManager::instance().drawOverlay(
            pluginPlotContext(layout, draw, SWEEPPP_UI_SPOT_SPECTRUM_OVERLAY, layer,
                              active != nullptr && active->visible ? active->frequencyHz : 0.0,
                              colors.contributionAlpha));
    };

    pluginOverlay(SWEEPPP_UI_LAYER_UNDER);

    // ---- traces ----------------------------------------------------------

    const auto drawTrace = [&](TraceKind kind, const Color& color, bool fill) {
        const Trace& trace = traces.trace(kind);
        if (!trace.visible && kind != TraceKind::Live) {
            return;
        }
        if (trace.values.empty()) {
            return;
        }

        const Envelope& envelope = traces.envelope(kind, layout.fromHz, layout.toHz, pixels);
        if (envelope.empty()) {
            return;
        }

        const float columnWidth = layout.size.x / static_cast<float>(envelope.size());

        if (fill && view.showHeatmapFill && view.fillStyle != 2) {
            // Gradient fill under the trace, coloured by amplitude. Drawn as
            // one quad per column so each column's colour reflects its own
            // level rather than a single flat tint.
            const ColorMap& map = m_state.theme().spectrumFillColorMap();
            const float baseY = layout.origin.y + layout.size.y;

            for (std::size_t i = 0; i < envelope.size(); ++i) {
                const float value = envelope.maximum[i];
                if (!measured(value)) {
                    continue;
                }

                const float x0 = layout.origin.x + static_cast<float>(i) * columnWidth;
                const float x1 = x0 + columnWidth + 1.0F;
                const float y = layout.yForDb(value);

                const Color tint = view.fillStyle == 1 ? color
                                                       : map.sampleDb(value, view.gradientMinDb,
                                                                      view.gradientMaxDb);

                draw->AddRectFilled(ImVec2(x0, y), ImVec2(x1, baseY),
                                    packed(tint.withAlpha(colors.fillAlpha)));
            }
        }

        // The envelope band: min to max within each pixel column. This is what
        // keeps a narrow signal visible however far the view is zoomed out --
        // picking one bin per pixel would make it flicker.
        for (std::size_t i = 0; i < envelope.size(); ++i) {
            const float high = envelope.maximum[i];
            const float low = envelope.minimum[i];
            if (!measured(high)) {
                continue;
            }

            const float x = layout.origin.x + (static_cast<float>(i) + 0.5F) * columnWidth;
            const float yHigh = layout.yForDb(high);
            const float yLow = layout.yForDb(low);

            if (yLow - yHigh > 1.0F) {
                draw->AddLine(ImVec2(x, yHigh), ImVec2(x, yLow), packed(color),
                              colors.traceThickness);
            }

            // Connect to the next column so the trace reads as a continuous
            // line rather than a picket fence.
            if (i + 1 < envelope.size() && measured(envelope.maximum[i + 1])) {
                const float nextX = layout.origin.x + (static_cast<float>(i) + 1.5F) * columnWidth;
                draw->AddLine(ImVec2(x, yHigh),
                              ImVec2(nextX, layout.yForDb(envelope.maximum[i + 1])), packed(color),
                              colors.traceThickness);
            }
        }
    };

    // Order matters: live on top, since it is what the operator is watching.
    drawTrace(TraceKind::MinHold, colors.traceMinHold, false);
    drawTrace(TraceKind::Average, colors.traceAverage, false);
    drawTrace(TraceKind::MaxHold, colors.traceMaxHold, false);
    drawTrace(TraceKind::Live, colors.traceLive, true);

    // ---- markers ---------------------------------------------------------

    MarkerSet& markers = m_state.markers();
    for (Marker& marker : markers.items) {
        // Measured before the visibility test: hiding a marker takes it off the
        // plot, it does not stop it being a measurement, and the panel lists
        // the level of every marker in the set.
        if (marker.peakLocked) {
            // Around this marker rather than across the whole window. With a
            // fixed pair it made no difference; with a list of them, every
            // locked marker searching the whole view would collapse the lot
            // onto the same peak and leave the operator one cursor.
            const double reach = (layout.toHz - layout.fromHz) * 0.02;
            double peakHz = 0.0;
            float peakDb = 0.0F;
            if (traces.peakIn(marker.frequencyHz - reach, marker.frequencyHz + reach, peakHz,
                              peakDb)) {
                marker.frequencyHz = peakHz;
                marker.levelDb = peakDb;
            }
        } else {
            marker.levelDb = traces.levelAt(marker.frequencyHz);
        }

        if (!marker.visible) {
            continue;
        }

        const float x = layout.xForHz(marker.frequencyHz);
        if (x < layout.origin.x || x > layout.origin.x + layout.size.x) {
            continue;
        }

        // The selection draws in the cursor colour and everything else in the
        // marker colour: the highlight means "this is the one the right button
        // moves", which is the only distinction between them now.
        const bool selected = marker.id == markers.activeId;
        const Color& color = selected ? colors.cursor : colors.marker;

        // Every stroke goes down twice: a wider line in the plot's own
        // background colour, then the marker on top of it.
        //
        // A marker is drawn over whatever happens to be there -- a saturated
        // allocation bar, the fill under the trace, another trace -- and no
        // single colour survives all of them. The backing is what makes it
        // readable rather than the hue, which is why it comes from the
        // background: on a dark theme it is a dark outline, on a light one a
        // light one, and either way the line separates from what it crosses.
        const ImU32 backing = packed(colors.background.withAlpha(0.75F));
        const float weight = selected ? 1.8F : 1.2F;

        draw->AddLine(ImVec2(x, layout.origin.y), ImVec2(x, layout.origin.y + layout.size.y),
                      backing, weight + 2.0F);
        draw->AddLine(ImVec2(x, layout.origin.y), ImVec2(x, layout.origin.y + layout.size.y),
                      packed(color), weight);

        const float y = layout.yForDb(marker.levelDb);
        draw->AddCircleFilled(ImVec2(x, y), 5.5F, backing);
        draw->AddCircleFilled(ImVec2(x, y), 4.0F, packed(color));

        // Frequency readout chip on the axis under the cursor, carrying the
        // marker's name so a row in the panel can be found on the plot.
        const std::string label =
            std::format("M{} {}", marker.id, toml_util::formatFrequencyShort(marker.frequencyHz));
        const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
        const ImVec2 chipMin(x - textSize.x * 0.5F - 4.0F,
                             layout.origin.y + layout.size.y - textSize.y - 4.0F);
        const ImVec2 chipMax(x + textSize.x * 0.5F + 4.0F, layout.origin.y + layout.size.y);
        draw->AddRectFilled(chipMin, chipMax, packed(color), 3.0F);
        draw->AddRect(chipMin, chipMax, backing, 3.0F);

        // Picked from the fill rather than from the theme's `marker_text`,
        // which is one colour for a chip that is drawn in two -- and was
        // near-white on a pale yellow cursor chip, the one place the frequency
        // most needs reading.
        draw->AddText(ImVec2(chipMin.x + 4.0F, chipMin.y + 2.0F), chipTextColor(color),
                      label.c_str());
    }

    // The measurements that used to sit in the top-right corner here are now in
    // the readout card over the waterfall, together with what is under the
    // selected marker -- one place answering "what am I looking at" rather than
    // a box in one pane and a chip in the status bar.

    // Over the traces, still inside the clip rect: an annotation a plugin
    // wants read has to sit above the line it is annotating.
    pluginOverlay(SWEEPPP_UI_LAYER_OVER);

    ImPlot::PopPlotClipRect();

    // ---- overlay text ----------------------------------------------------

    const SpectrumFramePtr frame = m_state.latestFrame();
    if (frame) {
        const double span = layout.toHz - layout.fromHz;
        // VBW is span / display points -- the effective bandwidth of one
        // drawn column, which is what the eye is actually integrating over.
        const double vbw = pixels > 0 ? span / static_cast<double>(pixels) : 0.0;

        std::string gains;
        for (const auto& [key, value] : frame->config.gains) {
            gains += std::format("{} {:.0f}  ", key, value);
        }

        const std::string left = std::format("RBW {}   FFT {}   {}",
                                             toml_util::formatFrequencyShort(frame->config.rbwHz),
                                             frame->config.fftSize, gains);
        const std::string right =
            std::format("VBW {}   Points {}", toml_util::formatFrequencyShort(vbw), pixels);

        draw->AddText(ImVec2(layout.origin.x + 8.0F, layout.origin.y + kOverlayTextTop),
                      packed(colors.axisText), left.c_str());

        const ImVec2 rightSize = ImGui::CalcTextSize(right.c_str());
        draw->AddText(ImVec2(layout.origin.x + layout.size.x - rightSize.x - 8.0F,
                             layout.origin.y + kOverlayTextTop),
                      packed(colors.axisText), right.c_str());
    }

    // ---- Y-axis handles --------------------------------------------------

    const ImU32 handleColor = packed(m_state.theme().chrome().accent);
    const float handleX = layout.origin.x + 3.0F;

    const auto yHandle = [&](float db, DragTarget target, const char* id) {
        const float y = layout.yForDb(db);
        draw->AddRectFilled(ImVec2(handleX, y - kHandleSize * 0.5F),
                            ImVec2(handleX + kHandleSize * 1.6F, y + kHandleSize * 0.5F),
                            handleColor, 2.0F);

        ImGui::SetCursorScreenPos(ImVec2(handleX, y - kHandleSize));
        ImGui::InvisibleButton(id, ImVec2(kHandleSize * 2.4F, kHandleSize * 2.0F));

        // The grab point is recorded once, on the frame the drag starts, so
        // the level moves with the cursor instead of jumping to it.
        if (ImGui::IsItemActivated()) {
            m_dragging = target;
            m_dragAnchorDb = db;
            m_dragAnchorY = ImGui::GetIO().MousePos.y;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
    };

    // Top and bottom drag independently, so the operator can stretch either
    // end of the scale without the other moving.
    yHandle(view.yMaxDb, DragTarget::YMax, "##ymax");
    yHandle(view.yMinDb, DragTarget::YMin, "##ymin");

    if (m_dragging == DragTarget::YMax || m_dragging == DragTarget::YMin) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Travelled since the grab, not wherever the cursor is now.
            const float db = m_dragAnchorDb + layout.dbForY(ImGui::GetIO().MousePos.y) -
                             layout.dbForY(m_dragAnchorY);
            if (m_dragging == DragTarget::YMax) {
                view.yMaxDb = std::clamp(db, view.yMinDb + kMinScaleSpanDb, kScaleCeilingDbfs);
            } else {
                view.yMinDb = std::clamp(db, kScaleFloorDbfs, view.yMaxDb - kMinScaleSpanDb);
            }
        } else {
            m_dragging = DragTarget::None;
        }
    }

    // ---- interaction -----------------------------------------------------

    handleFrequencyGestures(layout, ImPlot::IsPlotHovered(), FrequencyPane::Spectrum);
}

void MainWindow::drawMarkerReadout(const ImVec2& origin, const ImVec2& size) {
    const ViewSettings& view = m_state.view();
    const MarkerSet& markers = m_state.markers();
    const ChromeTheme& chrome = m_state.theme().chrome();

    // A hidden marker answers nothing: the question this card asks is what is
    // under the marker being looked at, and there is nothing to look at.
    const Marker* selected = markers.active();
    if (view.markerReadout <= 0 || selected == nullptr || !selected->visible) {
        return;
    }

    const std::string head = std::format("M{}   {}\n{}", selected->id,
                                         toml_util::formatFrequency(selected->frequencyHz, 3),
                                         levelText(selected->levelDb));

    // The same source the status bar's chip reads, so the card and the bar
    // cannot disagree about what is at a frequency.
    std::string contributor = "-";
    Color whatColor = chrome.textDim;
    if (const std::vector<Contribution> found =
            PluginManager::instance().contributionsAt(selected->frequencyHz);
        !found.empty()) {
        contributor = std::format("{} · {}", found.front().name, found.front().pluginName);
        whatColor = Color{.r = found.front().color[0],
                          .g = found.front().color[1],
                          .b = found.front().color[2],
                          .a = found.front().color[3]};
    }

    // Measured against the previous visible marker in the list, wrapping, so
    // the block is there whenever there are two markers to measure between
    // rather than only when the selected one happens not to be the first.
    const Marker* other = nullptr;
    {
        const std::size_t count = markers.items.size();
        const auto found = std::ranges::find(markers.items, selected->id, &Marker::id);
        const auto index = static_cast<std::size_t>(found - markers.items.begin());
        for (std::size_t step = 1; step < count; ++step) {
            const Marker& candidate = markers.items[(index + count - step) % count];
            if (candidate.visible) {
                other = &candidate;
                break;
            }
        }
    }

    std::string body;
    if (other != nullptr) {
        const double startHz = std::min(selected->frequencyHz, other->frequencyHz);
        const double stopHz = std::max(selected->frequencyHz, other->frequencyHz);

        double peakHz = 0.0;
        float peakDb = 0.0F;
        const bool havePeak = m_state.traces().peakIn(startHz, stopHz, peakHz, peakDb);

        // The delta only means anything when both ends are readings.
        const std::string delta =
            measured(selected->levelDb) && measured(other->levelDb)
                ? std::format("{:.1f} dB", static_cast<double>(selected->levelDb - other->levelDb))
                : std::string("-");

        body = std::format(
            "vs M{}\nStart  {}\nEnd    {}\nCenter {}\nSpan   {}\nDelta  {}{}", other->id,
            toml_util::formatFrequency(startHz, 3), toml_util::formatFrequency(stopHz, 3),
            toml_util::formatFrequency((startHz + stopHz) * 0.5, 3),
            toml_util::formatFrequency(stopHz - startHz, 3), delta,
            havePeak ? std::format("\nPeak   {} at {}", toml_util::formatFrequency(peakHz, 3),
                                   levelText(peakDb))
                     : std::string{});
    }

    const float padding = bar::scaled(6.0F);
    constexpr float kMargin = 10.0F;
    constexpr float kRuleGap = 5.0F;

    // Measured with every digit at its widest, so the card is the same size
    // whatever the numbers are doing.
    //
    // It is anchored to a corner, so a width taken from the live text moves the
    // opposite edge on every frame a peak crosses a bin or a level gains a
    // digit -- and a readout that will not hold still cannot be read.
    const ImVec2 headSize = ImGui::CalcTextSize(bar::digitMask(head).c_str());
    const ImVec2 bodySize =
        body.empty() ? ImVec2(0.0F, 0.0F) : ImGui::CalcTextSize(bar::digitMask(body).c_str());

    // Floored at the widest line the card can ever hold, so placing a second
    // marker does not resize it either -- and so the contributor below has a
    // box worth reading rather than four characters and an ellipsis.
    const float widest = ImGui::CalcTextSize("Peak   8888.888 MHz at -888.8 dBFS").x;
    const float width = std::max({headSize.x, bodySize.x, widest}) + padding * 2.0F;

    // Cut to the box the numbers set rather than allowed to widen it: the
    // contributor changes with every band the marker is walked across, and the
    // card would resize under the operator's eye all the way.
    const std::string what = bar::ellipsised(contributor, width - padding * 2.0F);
    const ImVec2 whatSize = ImGui::CalcTextSize(what.c_str());
    const float height = headSize.y + whatSize.y +
                         (body.empty() ? 0.0F : bodySize.y + kRuleGap * 2.0F + 1.0F) +
                         padding * 2.0F;

    // 0 is off; the rest run top-left to bottom-right, skipping the middle of
    // the pane -- a card in the centre of a waterfall covers exactly what the
    // operator is watching.
    constexpr std::array<ImVec2, 9> kAnchors{{{0.0F, 0.0F},
                                              {0.0F, 0.0F},
                                              {0.5F, 0.0F},
                                              {1.0F, 0.0F},
                                              {0.0F, 0.5F},
                                              {1.0F, 0.5F},
                                              {0.0F, 1.0F},
                                              {0.5F, 1.0F},
                                              {1.0F, 1.0F}}};
    const ImVec2 anchor = kAnchors[static_cast<std::size_t>(std::min(view.markerReadout, 8))];

    // Clamped to the pane afterwards, so a card taller than a waterfall dragged
    // down to nothing still lands somewhere it can be read.
    const ImVec2 boxMin(
        std::clamp(origin.x + kMargin + (size.x - kMargin * 2.0F - width) * anchor.x, origin.x,
                   std::max(origin.x, origin.x + size.x - width)),
        std::clamp(origin.y + kMargin + (size.y - kMargin * 2.0F - height) * anchor.y, origin.y,
                   std::max(origin.y, origin.y + size.y - height)));
    const ImVec2 boxMax(boxMin.x + width, boxMin.y + height);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(boxMin, boxMax, packed(chrome.panelBackground.withAlpha(0.92F)), 4.0F);
    draw->AddRect(boxMin, boxMax, packed(chrome.border), 4.0F);

    float y = boxMin.y + padding;
    draw->AddText(ImVec2(boxMin.x + padding, y), packed(chrome.text), head.c_str());
    y += headSize.y;
    draw->AddText(ImVec2(boxMin.x + padding, y), packed(whatColor), what.c_str());
    y += whatSize.y;

    if (!body.empty()) {
        y += kRuleGap;
        draw->AddLine(ImVec2(boxMin.x + padding, y), ImVec2(boxMax.x - padding, y),
                      packed(chrome.border), 1.0F);
        y += kRuleGap + 1.0F;
        draw->AddText(ImVec2(boxMin.x + padding, y), packed(chrome.text), body.c_str());
    }
}

namespace {

/// Elapsed time as an operator reads it: seconds close in, minutes further out.
std::string formatElapsed(double seconds) {
    if (seconds < 60.0) {
        return std::format("-{:.1f}s", seconds);
    }
    const auto minutes = static_cast<int>(seconds / 60.0);
    return std::format("-{}:{:02.0f}", minutes, seconds - minutes * 60.0);
}

} // namespace

bool MainWindow::drawWaterfallTimeAxis(float x, float y, float width, float height,
                                       std::uint32_t visibleLines, std::uint32_t maxScroll) {
    const ImVec2 origin(x, y);
    const ImVec2 size(width, height);

    ViewSettings& view = m_state.view();
    if (view.waterfallTimeAxis == 0 || visibleLines == 0) {
        return false;
    }

    const ChromeTheme& chrome = m_state.theme().chrome();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    constexpr float kAxisWidth = 62.0F;
    const bool onLeft = view.waterfallTimeAxis == 1;
    const float axisX = onLeft ? origin.x : origin.x + size.x - kAxisWidth;

    // A backing so labels stay readable over a bright waterfall.
    draw->AddRectFilled(ImVec2(axisX, origin.y), ImVec2(axisX + kAxisWidth, origin.y + size.y),
                        packed(Color{0.0F, 0.0F, 0.0F, 0.55F}));

    // Anchored to the newest line in the ring, not to the top of the view.
    //
    // Anchoring to the view meant the reference moved with the scroll, so every
    // label read the same however far back you went -- the one thing the axis
    // exists to tell you. Against a fixed anchor the numbers grow as you scroll
    // into the past, which is the point.
    //
    // The anchor is the newest *stored* line rather than the wall clock so the
    // labels hold still when the waterfall is paused or the sweep has stopped,
    // instead of counting up against data that is no longer arriving.
    const std::uint64_t newest = m_waterfall.timeAtLinesBack(0);

    // One label roughly every 70 pixels, snapped to whole rows.
    const auto rowsPerLabel = static_cast<std::uint32_t>(
        std::max(1.0F, 70.0F * ImGui::GetIO().DisplayFramebufferScale.y));
    const float pixelsPerRow = size.y / static_cast<float>(visibleLines);

    for (std::uint32_t row = 0; row < visibleLines; row += rowsPerLabel) {
        const float rowY = origin.y + static_cast<float>(row) * pixelsPerRow;
        const std::uint64_t when = m_waterfall.timeAtLinesBack(m_waterfall.scrollLines() + row);
        if (when == 0 || newest == 0) {
            continue;
        }

        if (view.waterfallTimeLines) {
            // Fixed light rather than a theme colour, for the same reason the
            // gutter's backing is fixed dark: these sit over the colormap, not
            // over the application's chrome, and a themed separator disappears
            // against whichever part of the gradient happens to be under it.
            draw->AddLine(ImVec2(origin.x, rowY), ImVec2(origin.x + size.x, rowY),
                          packed(Color{1.0F, 1.0F, 1.0F, 0.55F}), 1.5F);
        }

        const std::string label = formatElapsed(nsToSeconds(newest - when));
        const ImVec2 extent = ImGui::CalcTextSize(label.c_str());
        const float textX = onLeft ? axisX + 6.0F : axisX + kAxisWidth - extent.x - 6.0F;
        draw->AddText(ImVec2(textX, rowY + 2.0F), packed(chrome.textDim), label.c_str());
    }

    // The axis is also the scroll surface.
    //
    // Ctrl+wheel was the obvious binding and is not dependable: macOS claims
    // ctrl+scroll for its own zoom before the application sees it, so the
    // modifier never arrives and the gesture reads as a plain zoom. A surface
    // that exists only to represent time has no such conflict.
    ImGui::SetCursorScreenPos(ImVec2(axisX, origin.y));
    ImGui::InvisibleButton("##waterfalltimeaxis", ImVec2(kAxisWidth, size.y));

    const bool hovered = ImGui::IsItemHovered();
    if (hovered || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }

    float scroll = static_cast<float>(m_waterfall.scrollLines());

    if (hovered && ImGui::GetIO().MouseWheel != 0.0F) {
        scroll -= ImGui::GetIO().MouseWheel *
                  static_cast<float>(std::max<std::uint32_t>(visibleLines / 10U, 1U));
    }
    if (ImGui::IsItemActive()) {
        // Dragging down reveals older lines, matching the direction the
        // waterfall itself flows.
        scroll += ImGui::GetIO().MouseDelta.y / std::max(pixelsPerRow, 0.001F);
    }

    m_waterfall.setScrollLines(
        static_cast<std::uint32_t>(std::clamp(scroll, 0.0F, static_cast<float>(maxScroll))));

    // The caller suppresses its own frequency gestures while this is true --
    // dragging the time axis must not pan the span at the same time.
    return hovered || ImGui::IsItemActive();
}

void MainWindow::drawWaterfall() {
    ViewSettings& view = m_state.view();

    const ImVec2 available = ImGui::GetContentRegionAvail();
    // No footer any more: the sweep rate, line rate and frame rate moved to
    // the status bar, where the rest of the at-a-glance numbers live. The pane
    // takes the height back.
    constexpr float footerHeight = 0.0F;

    // Exactly the spectrum's plot rectangle, so a signal sits at the same x in
    // both panes. The spectrum's data area is inset from its child by the
    // Y-axis labels on the left and the gradient bar on the right; matching
    // the child instead would shift the waterfall by ~70 px and silently
    // misalign every frequency.
    const bool aligned = m_spectrumPlotWidth > 16.0F;
    const ImVec2 plotSize(aligned ? m_spectrumPlotWidth : available.x - kGradientBarWidth - 8.0F,
                          available.y - footerHeight);
    if (plotSize.x <= 16.0F || plotSize.y <= 16.0F) {
        return;
    }

    // The texture is sized to the *acquisition's* bin count, not the widget's
    // width.
    //
    // This is what makes zoom work on the whole history. Storing at display
    // resolution would mean each row was resampled onto whatever window
    // happened to be visible when it arrived, baking the zoom in permanently;
    // rows written before a zoom would keep the old mapping and the waterfall
    // would become a patchwork. At acquisition resolution the rows are
    // zoom-independent and the view is a UV transform in the shader.
    //
    // It also gives real headroom to zoom into: a 4096-point FFT stores ~3x
    // more columns than a 1400 px pane can show. Beyond that the display
    // interpolates -- detail that was never stored cannot be recovered, and
    // the full-resolution answer is the session history store.
    // Before the first frame there is no acquisition grid yet, so the pane's
    // own width is a reasonable placeholder; the texture is resized once real
    // bins arrive.
    //
    // Only before the FIRST one, though. Acquisition restarts whenever the plan
    // changes -- a sample-rate change re-plans the sweep -- and the trace store
    // is cleared for the few frames until the new grid arrives, so `binCount()`
    // reads zero again. Taking the placeholder in that gap would resize the
    // texture down to the pane width and back up moments later, and every one
    // of those round trips runs the whole history through a max-decimation:
    // one-bin carriers come back four bins wide, then sixteen, growing without
    // limit for as long as the operator keeps changing the rate. Holding the
    // width the texture already has costs nothing -- there is nothing to draw
    // during the gap anyway.
    const std::size_t sourceBins = m_state.traces().binCount();
    const std::size_t fallbackBins = m_waterfall.valid() && m_waterfall.bins() > 0
                                         ? static_cast<std::size_t>(m_waterfall.bins())
                                         : static_cast<std::size_t>(plotSize.x);
    const auto bins = static_cast<std::uint32_t>(
        std::clamp<std::size_t>(sourceBins > 0 ? sourceBins : fallbackBins, 256, 8192));
    // The ring has to be larger than the pane, or there is no history behind
    // what is already drawn and scrolling back has nowhere to go.
    //
    // The pane is measured in *framebuffer* rows: 660 points on a 2x display is
    // 1320 rows, so the 1024-line default was already smaller than one
    // screenful. The requested size is treated as a minimum and a screenful is
    // guaranteed on top of it.
    const auto paneRows = static_cast<int>(plotSize.y * ImGui::GetIO().DisplayFramebufferScale.y);
    const int wanted = std::max(view.waterfallLines, paneRows + paneRows / 2);

    // Rounded up to a coarse step rather than tracking the pane exactly.
    //
    // Every change here reallocates the ring and copies it, so following the
    // pane row for row would do that on every frame of a window drag. The step
    // makes it happen at most a handful of times across any resize, and the
    // spare depth is free scrollback.
    constexpr int kDepthStep = 1024;

    // The driver's limit, not a constant: one line is one texture row, so a
    // depth past GL_MAX_TEXTURE_SIZE fails to allocate. Asking the driver keeps
    // this and the settings slider agreeing on one number.
    const std::uint32_t maxTexture = WaterfallRenderer::maxTextureSize();
    const auto ceiling = static_cast<int>(maxTexture > 0 ? maxTexture : 16384U);
    const auto lines = static_cast<std::uint32_t>(
        std::clamp((wanted + kDepthStep - 1) / kDepthStep * kDepthStep, 256, ceiling));

    if (!m_waterfall.valid() || m_waterfall.bins() != bins || m_waterfall.lines() != lines) {
        if (auto resized = m_waterfall.resize(bins, lines); !resized) {
            ImGui::TextDisabled("waterfall unavailable: %s", resized.error().c_str());
            return;
        }
        m_waterfall.setColorMap(m_state.theme().waterfallColorMap());
    }

    m_waterfall.setGradientRange(view.gradientMinDb, view.gradientMaxDb);
    m_waterfall.setPeakDetect(view.waterfallPeakDetect);

    double fromHz = 0.0;
    double toHz = 0.0;
    m_state.visibleRange(fromHz, toHz);

    // Declares what the texture's width means. A retune or a span change
    // clears the history inside setSpan, for the same reason a session opens a
    // new segment: old rows cannot be reinterpreted under a new grid.
    if (sourceBins > 0) {
        m_waterfall.setSpan(m_state.traces().startHz(), m_state.traces().stopHz());
    }

    // Drain the lines the UI thread parked. Done here rather than in the frame
    // callback so no GL call ever happens off the render thread.
    for (const std::vector<float>& line : m_state.takePendingWaterfallLines()) {
        if (line.empty()) {
            continue;
        }

        if (line.size() == bins) {
            // The common case: stored verbatim, at full acquisition resolution.
            m_waterfall.pushLine(line.data(), bins, monotonicNs());
        } else {
            // A sweep grid wider than the texture. Reduce with MAX, never
            // mean: a narrow carrier must survive the reduction, which is the
            // same reasoning as the spectrum's envelope and the session
            // store's LOD pyramid.
            //
            // Unmeasured bins take no part. Their sentinel is below every
            // reading, so a max already ignores them where a column has any
            // measurement -- and a column with none keeps the sentinel and
            // stays a gap.
            std::vector<float> reduced(bins, kUnmeasuredDbfs);
            for (std::size_t i = 0; i < line.size(); ++i) {
                if (!measured(line[i])) {
                    continue;
                }
                const auto column = static_cast<std::size_t>(static_cast<double>(i) * bins /
                                                             static_cast<double>(line.size()));
                if (column < reduced.size()) {
                    reduced[column] = std::max(reduced[column], line[i]);
                }
            }
            m_waterfall.pushLine(reduced.data(), bins, monotonicNs());

            // Scrolled back means anchored to what is being read, not to a
            // fixed distance from the newest line. Without this, every new
            // line would drag the view forward under the operator -- the
            // scrollback equivalent of a terminal that jumps to the bottom
            // while you are reading.
            if (m_waterfall.scrollLines() > 0) {
                m_waterfall.setScrollLines(m_waterfall.scrollLines() + 1);
            }
        }

        m_state.telemetry().render().waterfallLines.fetch_add(1, std::memory_order_relaxed);
    }

    // The GL draw happens after ImGui's own rendering has been set up, so it
    // is queued as a callback rather than issued inline.
    if (aligned) {
        // Screen coordinates are absolute, so the pane can be placed under the
        // spectrum's plot area even though it lives in a different child.
        ImGui::SetCursorScreenPos(ImVec2(m_spectrumPlotX, ImGui::GetCursorScreenPos().y));
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##waterfallsurface", plotSize);
    bool surfaceHovered = ImGui::IsItemHovered();

    // Gestures are handled after the pane has painted, not before: the
    // selection band is drawn into the same list, and the waterfall's own
    // content is a GL callback that would otherwise cover it. The readout card
    // is queued here for the same reason.
    const auto gestures = [&]() {
        SpectrumLayout waterfallLayout;
        waterfallLayout.origin = origin;
        waterfallLayout.size = plotSize;
        waterfallLayout.fromHz = fromHz;
        waterfallLayout.toHz = toHz;

        drawMarkerReadout(origin, plotSize);
        handleFrequencyGestures(waterfallLayout, surfaceHovered, FrequencyPane::Waterfall);
    };

    if (m_waterfall.linesPushed() == 0) {
        // Painted flat rather than left to the shader. An empty texture is all
        // zeros, which maps to the *bottom* of the colour map and would fill
        // the pane with whatever colour that is -- indistinguishable from a
        // real signal sitting at the floor. Empty has to look empty.
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, ImVec2(origin.x + plotSize.x, origin.y + plotSize.y),
                            packed(m_state.theme().spectrum().background));

        gestures();
        return;
    }

    struct DrawContext {
        WaterfallRenderer* renderer;
        float x;
        float y;
        float width;
        float height;
        std::uint32_t visibleLines;
        double viewStartHz;
        double viewStopHz;
        float scaleX;
        float scaleY;
    };

    const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;

    // Relative to the viewport that owns this window, not to the desktop.
    //
    // The renderer scales these straight into glViewport/glScissor, which are
    // framebuffer coordinates of whichever window is being drawn. With
    // multi-viewport enabled ImGui's screen coordinates became desktop-absolute,
    // so passing them raw offset the waterfall by wherever the window happened
    // to sit on screen -- it drew as a thin band well below its pane.
    const ImGuiViewport* hostViewport = ImGui::GetWindowViewport();

    const DrawContext context{.renderer = &m_waterfall,
                              .x = origin.x - hostViewport->Pos.x,
                              .y = origin.y - hostViewport->Pos.y,
                              .width = plotSize.x,
                              .height = plotSize.y,
                              // One texture line per framebuffer pixel row, so a Retina display
                              // shows twice the history rather than a doubled-up smear.
                              .visibleLines =
                                  static_cast<std::uint32_t>(plotSize.y * framebufferScale.y),
                              .viewStartHz = fromHz,
                              .viewStopHz = toHz,
                              .scaleX = framebufferScale.x,
                              .scaleY = framebufferScale.y};

    // The context is copied into the draw list rather than pointed at: the
    // callback runs after this frame's UI code has returned, so anything on
    // the stack would be gone, and a static would break the moment there were
    // two waterfalls.
    ImGui::GetWindowDrawList()->AddCallback(
        [](const ImDrawList*, const ImDrawCmd* cmd) {
            const auto* ctx = static_cast<const DrawContext*>(cmd->UserCallbackData);
            ctx->renderer->draw(ctx->x, ctx->y, ctx->width, ctx->height, ctx->visibleLines,
                                ctx->viewStartHz, ctx->viewStopHz, ctx->scaleX, ctx->scaleY);
        },
        const_cast<DrawContext*>(&context), sizeof(context));

    // Our GL draw leaves its own program and buffers bound; this tells the
    // backend to restore what it expects before the next ImGui command.
    //
    // ImGui 1.92 moved this from the ImDrawCallback_ResetRenderState sentinel
    // to a callback on the platform IO.
    ImGui::GetWindowDrawList()->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState,
                                            nullptr);

    if (view.waterfallPaused) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 badgeMin(origin.x + 8.0F, origin.y + 8.0F);
        const ImVec2 badgeMax(badgeMin.x + 74.0F, badgeMin.y + 22.0F);
        draw->AddRectFilled(badgeMin, badgeMax,
                            packed(m_state.theme().chrome().warning.withAlpha(0.85F)), 4.0F);
        draw->AddText(ImVec2(badgeMin.x + 8.0F, badgeMin.y + 3.0F), 0xFF000000, "PAUSED");
    }

    // Plugin overlays, after the pane for the same reason the time axis is:
    // the waterfall itself is a GL callback that would paint over anything
    // queued before it.
    //
    // Both layers are dispatched here, in order. There is nothing to be
    // *under* on a waterfall -- the image is drawn by the GPU outside this
    // draw list -- so an overlay declaring UNDER still gets called rather than
    // being silently skipped on one of the two panes it asked for.
    {
        SpectrumLayout waterfallLayout;
        waterfallLayout.origin = origin;
        waterfallLayout.size = plotSize;
        waterfallLayout.fromHz = fromHz;
        waterfallLayout.toHz = toHz;

        const Marker* active = m_state.markers().active();
        const double markerHz = active != nullptr && active->visible ? active->frequencyHz : 0.0;
        ImDrawList* draw = ImGui::GetWindowDrawList();

        for (const sweeppp_ui_layer_t layer : {SWEEPPP_UI_LAYER_UNDER, SWEEPPP_UI_LAYER_OVER}) {
            PluginManager::instance().drawOverlay(
                pluginPlotContext(waterfallLayout, draw, SWEEPPP_UI_SPOT_WATERFALL_OVERLAY, layer,
                                  markerHz, m_state.theme().spectrum().contributionAlpha));
        }
    }

    // Drawn after the pane, so it sits over the waterfall rather than under
    // the GL callback that paints it.
    if (drawWaterfallTimeAxis(origin.x, origin.y, plotSize.x, plotSize.y, context.visibleLines,
                              m_waterfall.maxScrollLines(context.visibleLines))) {
        surfaceHovered = false;
    }

    gestures();
}

void MainWindow::drawHistoryWaterfall() {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (available.x <= 1.0F || available.y <= 1.0F) {
        return;
    }

    // Taken from the shared visible range here, immediately before the
    // composite, rather than after both panes have drawn.
    //
    // The spectrum reads that range and its gestures mutate it mid-frame, so a
    // waterfall built from the view's own copy was drawing the window from the
    // frame before: mid-zoom the trace extended past the edge of the data
    // underneath it, and only for as long as the gesture lasted. Syncing here
    // makes both panes draw the same window whichever one the gesture came
    // from -- a gesture on the waterfall lands after this point, so that frame
    // has the spectrum and the waterfall agreeing on the old window, and the
    // next has them agreeing on the new one.
    double fromHz = 0.0;
    double toHz = 0.0;
    m_state.visibleRange(fromHz, toHz);
    m_history.setFrequencyRange(fromHz, toHz);

    // Aligned to the spectrum's plot rectangle for the same reason the live
    // waterfall is: a signal has to sit at the same x in both panes.
    const float x = m_spectrumPlotX > 0.0F ? m_spectrumPlotX : ImGui::GetCursorScreenPos().x;
    const float width = m_spectrumPlotWidth > 0.0F ? m_spectrumPlotWidth : available.x;

    const ImVec2 origin(x, ImGui::GetCursorScreenPos().y);
    const ImVec2 size(width, available.y);

    const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    const std::uint32_t texture = m_history.texture(static_cast<std::uint32_t>(size.x * scale.x),
                                                    static_cast<std::uint32_t>(size.y * scale.y));

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 far(origin.x + size.x, origin.y + size.y);
    draw->AddRectFilled(origin, far, packed(m_state.theme().spectrum().background));

    if (texture != 0) {
        draw->AddImage(static_cast<ImTextureID>(texture), origin, far);
    }

    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##historysurface", size);
    const bool hovered = ImGui::IsItemHovered();

    drawHistoryAxes(origin, size, 0.0F, 0.0F);

    // The time-label gutter is a scroll surface, not decoration: it is the one
    // strip of this pane that means time, and the panes themselves deliberately
    // only handle frequency.
    const float gutter = origin.x - ImGui::GetWindowPos().x;
    if (gutter > 8.0F) {
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x, origin.y));
        ImGui::InvisibleButton("##historytimeaxis", ImVec2(gutter, size.y));

        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Drag or scroll to move through the recording\n"
                              "Shift+scroll for a page, Cmd/Ctrl+scroll to zoom\n"
                              "Double-click to reset the zoom");
        }

        // Checked before the drag, which a double click would otherwise be read
        // as -- its two presses carry no movement, but the pan runs on activity
        // rather than on distance.
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            m_history.resetTimeZoom();
        }

        // Wheel travels along the recording rather than rescaling it: this
        // gutter is the time ruler, and scrolling a ruler means moving down it.
        // It also makes the bare wheel mean the same thing here as it does on
        // the overview strip, which is the other surface that owns time.
        //
        // Zoom stays reachable as the modified gesture, since nothing else in
        // the layout narrows the time window.
        if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0F) {
            const auto wheel = static_cast<double>(ImGui::GetIO().MouseWheel);
            if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) {
                const double anchor = static_cast<double>(std::clamp(
                    (ImGui::GetIO().MousePos.y - origin.y) / std::max(size.y, 1.0F), 0.0F, 1.0F));
                m_history.zoomTime(std::pow(0.9, wheel), anchor);
            } else {
                // The stride the strip uses, so both time surfaces travel at
                // the same rate; shift is the coarse, page-sized jump.
                m_history.panTime(-wheel * (ImGui::GetIO().KeyShift ? 1.0 : 0.25));
            }
        }

        if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.y != 0.0F) {
            m_history.panTime(-static_cast<double>(ImGui::GetIO().MouseDelta.y) /
                              static_cast<double>(std::max(size.y, 1.0F)));
        }
    }

    // The playhead, drawn where the selected instant falls in the window.
    if (m_historySelection) {
        const auto span = static_cast<double>(m_history.viewToNs() - m_history.viewFromNs());
        const double fraction =
            span > 0.0
                ? static_cast<double>(m_historySelection->monotonicNs - m_history.viewFromNs()) /
                      span
                : -1.0;
        if (fraction >= 0.0 && fraction <= 1.0) {
            const float y = origin.y + size.y * static_cast<float>(fraction);
            draw->AddLine(ImVec2(origin.x, y), ImVec2(far.x, y),
                          packed(m_state.theme().chrome().warning), 1.5F);
        }
    }

    // The instrument's frequency gestures -- wheel zoom, left-drag pan,
    // shift+drag band select -- but nothing that touches the markers.
    //
    // Here the vertical axis is time, and a plain click means "plot this line",
    // which is the question this window exists to answer. The pan reads only
    // the horizontal travel, so it leaves that click intact; markers stay on
    // the spectrum above, where the x under the pointer really is a frequency.
    SpectrumLayout layout;
    layout.origin = origin;
    layout.size = size;
    m_state.visibleRange(layout.fromHz, layout.toHz);
    handleFrequencyGestures(layout, hovered, FrequencyPane::Waterfall, MarkerClicks::Disabled);

    // Picked on release rather than on press, and only when the pointer did not
    // travel: the same click also begins a shift+drag band selection, and a
    // release that ended a drag must not additionally move the playhead to
    // wherever the drag happened to stop.
    if (ImGui::IsItemDeactivated() && !ImGui::GetIO().KeyShift &&
        ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y == 0.0F &&
        ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x == 0.0F) {
        const auto fraction = static_cast<double>(std::clamp(
            (ImGui::GetIO().MousePos.y - origin.y) / std::max(size.y, 1.0F), 0.0F, 1.0F));
        seekHistory(m_history.timeAt(fraction));
    }
}

} // namespace sweeppp::ui
