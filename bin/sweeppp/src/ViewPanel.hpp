// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "render/WaterfallRenderer.hpp"

#include <cstdint>
#include <memory>
#include <sweeppp/ui/PanelLayout.hpp>
#include <sweeppp/ui/TraceStore.hpp>

namespace sweeppp::ui {

/// Which pane a gesture is happening in.
///
/// The two share one frequency axis, so a drag started in either means the
/// same thing -- but only the pane it started in may act on the release, or a
/// selection would be applied twice.
enum class FrequencyPane : std::uint8_t { Spectrum, Waterfall };

/// Which handle the operator is currently dragging, so a drag that leaves the
/// widget still tracks.
enum class DragTarget : std::uint8_t { None, YMin, YMax, GradientMin, GradientMax };

/// Shift-drag band selection: the same gesture aimed at the display or at the
/// radio. Decided when the drag starts and held for its duration, so releasing
/// the modifier mid-drag cannot change what happens on release.
enum class SelectionMode : std::uint8_t { ZoomView, SweepRange, AddSweepRange };

/// One panel's gestures in progress, carried across frames.
struct PanelGestures {
    DragTarget dragging = DragTarget::None;

    /// Where a level drag started: the handle's value, and the cursor position
    /// at the moment it was grabbed.
    ///
    /// Levels move by how far the cursor has travelled since the grab, not to
    /// wherever it happens to be. Setting them from the absolute position
    /// makes the handle jump to meet the cursor the instant it is touched --
    /// and since a hit box is deliberately larger than the handle it draws
    /// (and the gradient bar's covers the whole bar), a click that lands even
    /// slightly off throws the level across the scale before the drag begins.
    float dragAnchorDb = 0.0F;
    float dragAnchorY = 0.0F;

    bool selecting = false;
    SelectionMode selectionMode = SelectionMode::ZoomView;
    float selectionStartX = 0.0F;
    FrequencyPane selectionPane = FrequencyPane::Spectrum;

    /// Left-drag panning, anchored to the window the drag started from.
    ///
    /// The whole gesture is computed from where the cursor is now against where
    /// it went down, never integrated from MouseDelta. The view is clamped to
    /// its limits, so an incremental pan pushed against either end loses the
    /// excess every frame and dragging back lands somewhere else than it
    /// started.
    bool panning = false;
    FrequencyPane panPane = FrequencyPane::Spectrum;
    float panAnchorX = 0.0F;
    double panStartFromHz = 0.0;
    double panStartToHz = 0.0;

    /// Whether the right button went down on a pane, so holding it walks the
    /// active marker along rather than any right-drag from elsewhere doing so.
    bool markerDrag = false;
};

/// What a panel needs at runtime, matched by id to a `PanelView` in the
/// layout. The view is the saved half; this is the half that holds a GL
/// texture and a gesture in progress.
struct ViewPanel {
    int id = 0;

    /// Created on the first frame the panel draws a waterfall.
    std::unique_ptr<WaterfallRenderer> waterfall;

    /// Screen-space X and width of the spectrum's plot area, last frame.
    ///
    /// The waterfall is drawn to exactly this rectangle rather than to its own
    /// child's full width, or the two panes would use different horizontal
    /// extents -- the spectrum's is inset by its Y-axis labels and the gradient
    /// bar -- and a signal would appear at a different x in each.
    float plotX = 0.0F;
    float plotWidth = 0.0F;

    /// Where the panel was drawn last frame, in screen coordinates. A panel
    /// torn off opens just beside it.
    PanelRect rect;

    PanelGestures gestures;
    EnvelopeCache envelopes;

    /// Whether a contribution flag took this frame's click in this panel.
    bool claimedClick = false;

    /// The header's segment edges while they are being typed, in MHz.
    double editStartMHz = 0.0;
    double editStopMHz = 0.0;
};

} // namespace sweeppp::ui
