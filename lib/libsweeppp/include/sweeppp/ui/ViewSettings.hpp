// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/ui/Marker.hpp"
#include "sweeppp/ui/PanelLayout.hpp"

#include <string>

namespace sweeppp::ui {

/// Bounds every dBFS level in the UI is held within.
///
/// 0 dBFS is a full-scale sinusoid, so a little headroom above it covers the
/// window and scaling artefacts that can push a bin slightly past full scale;
/// there is nothing to see further up. The floor sits well below any of these
/// radios' noise floor (a HackRF at full gain lands near -110 dBFS), so it
/// never gets in the way of a real measurement while still stopping a scale
/// from running away to a value that would take a lot of dragging to undo.
///
/// Shared by the drag handles and the numeric controls so they cannot
/// disagree about what is reachable.
inline constexpr float kScaleFloorDbfs = -150.0F;
inline constexpr float kScaleCeilingDbfs = 10.0F;

/// Smallest span a scale may be squeezed to, so its two handles never meet and
/// leave the plot un-draggable.
inline constexpr float kMinScaleSpanDb = 5.0F;

/// Display-side settings, all live-adjustable.
///
/// What belongs to one plot -- its window, Y range, gradient, split and pause
/// -- is per panel, in `layout`. Everything here applies to every panel.
struct ViewSettings {
    /// The panels and how they are arranged.
    PanelLayout layout;

    /// Display points across the plot. `autoPoints` follows the widget width,
    /// which is what makes VBW = span / points meaningful.
    bool autoPoints = true;
    int displayPoints = 1024;

    float smoothing = 0.0F;

    bool showMaxHold = false;

    /// dB per second the max-hold bleeds back down. Zero holds forever.
    float maxHoldDecayDbPerSec = 0.0F;
    bool showMinHold = false;
    bool showAverage = false;
    int averageWindow = 16;

    bool showHeatmapFill = true;

    /// 0 gradient, 1 solid, 2 outline only.
    ///
    /// Solid by default: a translucent wash in the trace's own colour reads as
    /// belonging to that trace, which matters as soon as two are shown at
    /// once. Colouring the fill by amplitude instead puts a second, louder
    /// encoding of the same numbers directly under the line -- the waterfall
    /// already carries that, and here it mostly competes with the shape the
    /// eye is trying to follow.
    int fillStyle = 1;

    bool showGrid = true;

    /// Which kinds of contribution the spectrum paints: the wide allocations,
    /// and the named channels and beacons.
    ///
    /// By contribution TYPE rather than by plugin, which is what keeps this
    /// here rather than in a plugin's own settings. "The allocations" and "the
    /// named channels" are two readings of the same axis and which one is
    /// wanted changes minute to minute -- so each is a keystroke, B and C --
    /// and keying on the type means a second channel list is covered by the
    /// same key without the host ever learning its name.
    ///
    /// Drawing only. A hidden contribution still answers the marker's chip,
    /// the same as the per-contributor tick in Data contributors.
    bool showBandContributions = true;
    bool showChannelContributions = true;

    /// Shade the spectrum across what the assigned antennas can hear.
    ///
    /// Full-height bands rather than chips, because this is not an annotation
    /// about a frequency -- it is a statement about which parts of the plot are
    /// a measurement and which are whatever the antenna does off its own band.
    /// Off by default: it is a check an operator makes deliberately, and it
    /// paints over the trace while it is on.
    ///
    /// Host drawing keyed on the host's own data, so it belongs here beside
    /// the contribution flags rather than in a plugin's settings.
    bool showAntennaRanges = false;

    // The band plan's toggle and its plan name used to live here. They belong
    // to the plugin that draws it now, in <configDir>/plugins/<id>.toml -- and
    // that is not a cosmetic move. `settings.toml` and every named profile are
    // this same struct, so a plugin's state living here would mean loading a
    // profile silently swapping the plugin's configuration.
    //
    // What is above is not that: it is the host's own drawing, keyed on a type
    // the ABI defines rather than on any plugin's identity.

    /// Peak detector for the waterfall, rather than average.
    ///
    /// Off by default. Once several bins share a pixel -- which on a wide span
    /// is most of the time -- a peak detector reports the loudest of them, so
    /// the noise floor both lifts and flickers by several dB per line. The
    /// spectrum's own min/max envelope still shows anything narrow.
    bool waterfallPeakDetect = false;

    /// Where the waterfall's time axis is drawn: 0 none, 1 left, 2 right.
    ///
    /// Overlaid on the pane rather than beside it. The waterfall is aligned to
    /// the spectrum's plot rectangle so a signal sits at the same x in both,
    /// and taking width for a gutter would break that alignment for the sake
    /// of a label.
    int waterfallTimeAxis = 0;

    /// Horizontal rules across the waterfall at the labelled times.
    bool waterfallTimeLines = false;

    /// Where the marker readout card sits over the waterfall: 0 off, 1 top-left,
    /// 2 top-centre, 3 top-right, 4 middle-left, 5 middle-right, 6 bottom-left,
    /// 7 bottom-centre, 8 bottom-right.
    int markerReadout = 8;

    /// The markers the operator has placed.
    ///
    /// Here rather than on the application state so they travel in a profile:
    /// a set of cursors built up over a session is exactly the kind of thing an
    /// operator expects to find again after a restart.
    MarkerSet markers;

    /// History depth in lines. Raised to at least a screenful and a half at
    /// runtime, since a ring smaller than the pane leaves nothing to scroll
    /// back to.
    int waterfallLines = 4096;

    std::string themeName = "Dark";
};

} // namespace sweeppp::ui
