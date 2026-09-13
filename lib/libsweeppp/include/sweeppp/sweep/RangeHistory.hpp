// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/sweep/SweepPlan.hpp"

#include <cstdint>
#include <vector>

namespace sweeppp {

/// Where the radio has been pointed, as a back/forward stack.
///
/// Narrowing onto a signal is a one-way gesture -- a drag or a preset replaces
/// the range outright -- so returning to the wider view otherwise means
/// retyping numbers that were on screen a moment earlier.
///
/// Only ranges are tracked. Resolution and mode are settings an operator
/// adjusts and leaves; the range is the thing that is *navigated*, and mixing
/// the two would make "back" mean something different depending on what was
/// touched last.
///
/// The clock is a parameter rather than read internally, so the coalescing
/// behaviour can be tested without waiting for real time to pass.
class SweepRangeHistory {
public:
    /// Entries kept before the oldest is dropped.
    static constexpr std::size_t kMaxEntries = 64;

    /// Changes closer together than this collapse into one entry.
    static constexpr std::uint64_t kCoalesceNs = 900'000'000;

    /// Records a range as somewhere the operator has been.
    ///
    /// Ignored when it matches where they already are, so re-applying the same
    /// range -- which the UI does freely, on every edit -- cannot fill the
    /// history with duplicates of one place.
    void record(std::vector<SweepSegment> segments, std::uint64_t nowNs);

    /// Establishes a starting point, discarding anything already recorded.
    ///
    /// The range a session opens on is set directly rather than applied, so it
    /// never passes through record() -- and without it the first change has
    /// nothing behind it and "back" does nothing, which reads as the history
    /// being broken rather than empty.
    ///
    /// Deliberately leaves the coalescing clock cleared: a starting point is a
    /// place in its own right, and the next change must push a new entry
    /// rather than overwrite it however quickly it follows.
    void reset(std::vector<SweepSegment> segments);

    [[nodiscard]] bool canGoBack() const noexcept { return m_cursor > 0; }
    [[nodiscard]] bool canGoForward() const noexcept {
        return !m_entries.empty() && m_cursor + 1 < m_entries.size();
    }

    /// Steps the cursor. Returns the range now current; empty at either end.
    [[nodiscard]] std::vector<SweepSegment> goBack();
    [[nodiscard]] std::vector<SweepSegment> goForward();

    [[nodiscard]] std::size_t size() const noexcept { return m_entries.size(); }
    [[nodiscard]] std::size_t cursor() const noexcept { return m_cursor; }

    /// The range the cursor is on, or empty when nothing has been recorded.
    [[nodiscard]] const std::vector<SweepSegment>& current() const;

private:
    std::vector<std::vector<SweepSegment>> m_entries;
    std::size_t m_cursor = 0;
    std::uint64_t m_lastRecordNs = 0;
};

} // namespace sweeppp
