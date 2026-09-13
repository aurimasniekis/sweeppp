// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/sweep/RangeHistory.hpp"

#include <algorithm>
#include <cmath>

namespace sweeppp {
namespace {

/// Compared with a tolerance, not exactly: these values come back from the
/// planner and the device having been through several floating-point
/// conversions, and a difference of a fraction of a hertz is the same range.
bool sameRange(const std::vector<SweepSegment>& a, const std::vector<SweepSegment>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::abs(a[i].startHz - b[i].startHz) > 1.0 ||
            std::abs(a[i].stopHz - b[i].stopHz) > 1.0) {
            return false;
        }
    }
    return true;
}

const std::vector<SweepSegment> kEmpty;

} // namespace

const std::vector<SweepSegment>& SweepRangeHistory::current() const {
    return m_entries.empty() ? kEmpty : m_entries[m_cursor];
}

void SweepRangeHistory::reset(std::vector<SweepSegment> segments) {
    m_entries.clear();
    m_cursor = 0;
    m_lastRecordNs = 0;

    if (!segments.empty()) {
        m_entries.push_back(std::move(segments));
    }
}

void SweepRangeHistory::record(std::vector<SweepSegment> segments, std::uint64_t nowNs) {
    if (segments.empty()) {
        return;
    }
    if (!m_entries.empty() && sameRange(m_entries[m_cursor], segments)) {
        return;
    }

    // Anything ahead of the cursor is discarded, exactly as a browser does:
    // going back and then somewhere new makes the old forward path a branch
    // nobody asked to keep.
    if (!m_entries.empty()) {
        m_entries.resize(m_cursor + 1);
    }

    // Edits arrive one keystroke and one nudge-button click at a time, and
    // each is a valid range. Recording all of them would make "back" mean
    // "undo one digit" and bury the range actually came from under fifty
    // intermediate ones. Changes landing close together collapse into the most
    // recent, so a burst of adjustment is one place to return to.
    const bool coalesce =
        !m_entries.empty() && m_lastRecordNs != 0 && nowNs - m_lastRecordNs < kCoalesceNs;

    if (coalesce) {
        m_entries[m_cursor] = std::move(segments);
    } else {
        m_entries.push_back(std::move(segments));
        m_cursor = m_entries.size() - 1;
    }
    m_lastRecordNs = nowNs;

    if (m_entries.size() > kMaxEntries) {
        const std::size_t excess = m_entries.size() - kMaxEntries;
        m_entries.erase(m_entries.begin(), m_entries.begin() + static_cast<std::ptrdiff_t>(excess));
        m_cursor -= std::min(m_cursor, excess);
    }
}

std::vector<SweepSegment> SweepRangeHistory::goBack() {
    if (!canGoBack()) {
        return {};
    }
    --m_cursor;

    // Stepping is not itself a place to come back to, so the coalescing clock
    // is reset: the next real change starts a new entry rather than
    // overwriting the one just navigated to.
    m_lastRecordNs = 0;
    return m_entries[m_cursor];
}

std::vector<SweepSegment> SweepRangeHistory::goForward() {
    if (!canGoForward()) {
        return {};
    }
    ++m_cursor;
    m_lastRecordNs = 0;
    return m_entries[m_cursor];
}

} // namespace sweeppp
