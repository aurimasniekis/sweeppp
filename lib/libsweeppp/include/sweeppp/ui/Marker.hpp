// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

namespace sweeppp::ui {

/// A measurement cursor the operator placed.
struct Marker {
    /// Stable name; never renumbered while the marker exists.
    ///
    /// The number is a *name*, not a position: it goes into `MarkerEvent::label`,
    /// from there into the `.sweeps` event stream, and from there into the
    /// channels plugin's label-keyed map. Renumbering the survivors on delete
    /// would silently re-point every one of those at a different frequency.
    int id = 0;
    double frequencyHz = 0.0;
    float levelDb = 0.0F; ///< Re-read from the live trace every frame.

    /// Whether the marker is drawn, and so whether it can be clicked or read.
    ///
    /// Not "does this marker exist" -- membership of `MarkerSet::items` is what
    /// answers that. The two used to be one flag, which left no way to keep a
    /// marker and stop drawing it.
    ///
    /// Unlike a hidden *contribution*, which still answers the status bar's
    /// chip, a hidden marker stops answering the readout: the question the
    /// readout asks is "what is under the marker you are looking at".
    bool visible = true;

    /// Keeps the marker on the loudest bin near it rather than where it was put.
    bool peakLocked = false;
};

/// The markers the operator has placed, and which one the gestures act on.
///
/// A plain aggregate with one vector in it, deliberately: this travels in a
/// `Profile`, and a profile snapshot is built several times a second for the
/// plugins to read.
struct MarkerSet {
    std::vector<Marker> items;
    int activeId = 0; ///< 0 = nothing selected.

    /// One past the highest id in `items`, kept there as the set changes.
    ///
    /// The counter follows the set rather than the session: clearing the list
    /// and placing another marker gets M1 back, not M7. A name is only ever
    /// handed out again once nothing on screen carries it, which is what keeps
    /// the numbers small enough to say out loud -- the whole reason they are
    /// names and not addresses.
    int nextId = 1;

    [[nodiscard]] Marker* active() noexcept;
    [[nodiscard]] const Marker* active() const noexcept;

    /// Appends a marker at `hz` and selects it.
    Marker& add(double hz);

    /// Removes a marker by id, moving the selection to a neighbour.
    ///
    /// A neighbour rather than nothing: deleting is how a list gets pruned, and
    /// having to re-pick a marker after every delete makes pruning several of
    /// them a click longer each time.
    bool remove(int id);

    /// Removes every marker and takes the numbering back to M1.
    void clear() noexcept;

    /// Puts `nextId` one past the highest id present.
    ///
    /// Called whenever `items` is changed from outside -- by a profile being
    /// loaded, or by a file someone edited by hand -- so a marker placed
    /// afterwards cannot collide with one already in the set.
    void resetNextId() noexcept;

    /// The visible marker nearest `hz`, or null if the nearest is further away
    /// than `toleranceHz`.
    [[nodiscard]] Marker* nearest(double hz, double toleranceHz) noexcept;
};

} // namespace sweeppp::ui
