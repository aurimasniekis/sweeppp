// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "PlotGeometry.hpp"

#include <imgui.h>
#include <span>
#include <string>
#include <sweeppp/plugin/PluginHost.hpp>
#include <sweeppp/ui/Theme.hpp>
#include <vector>

namespace sweeppp::ui {

/// Which contribution the operator has picked out, if any.
///
/// A flag says what a thing is and roughly where; how wide it is exactly is a
/// second question, and one asked about a single channel at a time. Drawing
/// every width at once puts a coloured stripe under every one of six hundred
/// chips, which reads as a texture rather than as a measurement -- so the
/// stripe is what a click buys.
///
/// Held by the window rather than by this file: it is view state, and a static
/// here would be shared by every plot that ever drew contributions.
struct ContributionSelection {
    std::string pluginId;
    std::string name;
    double startHz = 0.0;
    double stopHz = 0.0;

    [[nodiscard]] bool empty() const noexcept { return pluginId.empty() && name.empty(); }

    /// Matched on the span as well as the name, because two contributors may
    /// use one name for two frequencies and the operator picked one of them.
    [[nodiscard]] bool matches(const Contribution& entry) const noexcept {
        return entry.pluginId == pluginId && entry.name == name && entry.startHz == startHz &&
               entry.stopHz == stopHz;
    }

    void select(const Contribution& entry) {
        pluginId = entry.pluginId;
        name = entry.name;
        startHz = entry.startHz;
        stopHz = entry.stopHz;
    }

    void clear() { *this = ContributionSelection{}; }
};

/// The list behind a "…" chip, handed to the panel that draws it.
///
/// Deliberately not a tooltip. Sixty-eight entries is a list to be read and
/// scrolled, and a tooltip can be neither: it cannot take the cursor, and it
/// vanishes the moment the cursor leaves whatever opened it. So the overlay
/// records what is under the cursor and a real window draws it afterwards --
/// which is also the only legal place to open one, since the overlay itself
/// runs inside ImPlot's `BeginPlot`/`EndPlot`.
struct ContributionOverflowList {
    std::vector<Contribution> entries;

    /// The chip's rectangle. The panel opens flush under it, so the cursor can
    /// travel from one to the other without crossing a gap that would close it.
    ImVec2 chipMin{};
    ImVec2 chipMax{};

    /// Where the panel ended up last frame, so "still on it" is answered from
    /// its real rectangle rather than from a flag that is one frame stale.
    ImVec2 panelMin{};
    ImVec2 panelMax{};
    bool panelDrawn = false;

    bool hoveredChip = false;
};

/// What the operator is doing with the flags, carried across frames.
struct ContributionInteraction {
    ContributionSelection picked;
    ContributionOverflowList overflow;

    /// True when a flag took this frame's click. The caller owes this to its
    /// own click handling: picking a channel must not also drop a marker on it.
    bool claimedClick = false;
};

/// How the host paints what contributors hand it, and whether the flags are
/// answering the pointer at all this frame.
struct ContributionStyle {
    /// False when something else has the cursor: a panel open over the plot, a
    /// popup, a drag in another widget.
    ///
    /// Every hit test below is a raw rectangle against the mouse position,
    /// which knows nothing about what is drawn in front of it -- so without
    /// this, a chip under an open panel highlights itself and puts its tooltip
    /// on top of the panel covering it.
    bool hoverable = true;

    /// From the theme, so contributions are as loud as the rest of the chrome
    /// and get quieter with it.
    float alpha = 0.18F;

    /// Top inset of the host's own RBW/FFT readout row. Labels start below it,
    /// or the two render over each other and neither can be read.
    float textTop = 6.0F;

    /// Which types to paint. Drawing only: what is not painted still answers
    /// the marker's chip.
    bool bands = true;
    bool channels = true;

    /// How many pixels wide an allocation must be before it is drawn at all.
    ///
    /// An allocation is drawn as a bar the width of the allocation, so at a
    /// 6 GHz span most of five hundred of them are a pixel each: a row of
    /// coloured dashes that says nothing and hides what does. Below this they
    /// wait for the operator to zoom into them -- and the marker's chip still
    /// names them meanwhile, so nothing is unreachable.
    float minBandWidth = 7.0F;
};

/// Paints every contribution the host was given, by type.
///
/// The drawing every contributor used to write for itself, once: spans,
/// edges, label collision and the inset that clears the readout row. A plugin
/// that wants something else declines with
/// `SWEEPPP_CONTRIBUTION_RENDER_NONE` and draws through a UI facet instead --
/// and still answers the ranked chip, because contributing data and drawing it
/// are now separate questions.
///
/// The spectrum only. The waterfall shares the frequency axis and could carry
/// the same spans, but its vertical axis is time: a tint that never changes
/// down the image is a wash over the measurement rather than context for it,
/// and the spectrum directly above already says where every band is.
///
/// `ranked` is in the host's rank order, which decides which label wins a
/// contested slot. What it does NOT decide is paint order: fills go
/// widest-first whatever the rank, or a 20 MHz channel disappears under the
/// 83.5 MHz allocation that contains it.
///
/// `state` is read and written: clicking a flag selects it, clicking it again
/// or clicking away clears it, and hovering a "…" fills the overflow list for
/// `drawContributionOverflow` to draw once the plot is closed.
void drawContributions(ImDrawList* draw, const SpectrumLayout& layout,
                       std::span<const Contribution> ranked, const ContributionStyle& style,
                       ContributionInteraction& state);

/// The "…" panel, if one is open. Call outside every plot and popup, beside
/// the application's own floating windows.
///
/// It stays up while the cursor is on the chip that opened it or on the panel
/// itself, so it can be moved onto and scrolled -- which is the whole reason
/// it is a window and not a tooltip. A row clicked in it picks that
/// contribution out on the plot, exactly as clicking its flag would.
void drawContributionOverflow(ContributionInteraction& state);

/// Black or white, whichever can be read on top of `color`.
///
/// A chip is filled with something the host did not choose -- a contributor's
/// own colour, or the theme's marker colour -- so the text has to be picked
/// from the fill rather than themed alongside it, or a yellow channel gets
/// white-on-yellow.
[[nodiscard]] ImU32 chipTextColor(const Color& color) noexcept;

/// The status-bar chip naming what is at `queryHz`.
///
/// The top-ranked contribution in its own colour, `-` when nothing answers.
/// Hovering lists every contribution at that frequency, so the allocation and
/// the channel inside it are both readable rather than one hiding the other.
///
/// Shared by the instrument's status bar and the history viewer's, which is
/// the point: two bars asking the same question must not be able to give
/// different answers.
void drawContributionChip(double queryHz, const ChromeTheme& chrome);

} // namespace sweeppp::ui
