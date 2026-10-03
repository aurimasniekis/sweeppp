// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <sweeppp/ui/Marker.hpp>
#include <sweeppp/ui/TraceStore.hpp>

namespace sweeppp::ui {

Marker* MarkerSet::active() noexcept {
    if (activeId == 0) {
        return nullptr;
    }
    const auto found = std::ranges::find(items, activeId, &Marker::id);
    return found != items.end() ? &*found : nullptr;
}

const Marker* MarkerSet::active() const noexcept {
    if (activeId == 0) {
        return nullptr;
    }
    const auto found = std::ranges::find(items, activeId, &Marker::id);
    return found != items.end() ? &*found : nullptr;
}

Marker& MarkerSet::add(double hz) {
    items.push_back(Marker{.id = nextId, .frequencyHz = hz});
    ++nextId;
    activeId = items.back().id;
    return items.back();
}

bool MarkerSet::remove(int id) {
    const auto found = std::ranges::find(items, id, &Marker::id);
    if (found == items.end()) {
        return false;
    }

    const std::size_t index = static_cast<std::size_t>(found - items.begin());
    items.erase(found);

    // The counter comes back down with the set, so a list emptied and started
    // again numbers from M1 rather than carrying on from wherever the session
    // happened to have got to.
    resetNextId();

    if (activeId != id) {
        return true;
    }
    if (items.empty()) {
        activeId = 0;
    } else {
        // The entry that slid into the deleted one's place, or the last one
        // when the deleted marker was at the end.
        activeId = items[std::min(index, items.size() - 1)].id;
    }
    return true;
}

void MarkerSet::clear() noexcept {
    items.clear();
    activeId = 0;
    nextId = 1;
}

void MarkerSet::resetNextId() noexcept {
    nextId = 1;
    for (const Marker& marker : items) {
        nextId = std::max(nextId, marker.id + 1);
    }
}

Marker* MarkerSet::nearest(double hz, double toleranceHz) noexcept {
    Marker* best = nullptr;
    double bestDistance = 0.0;

    for (Marker& marker : items) {
        if (!marker.visible) {
            continue;
        }
        const double distance = std::abs(marker.frequencyHz - hz);
        if (distance > toleranceHz) {
            continue;
        }
        // Strictly closer, so two markers at the same frequency resolve to the
        // one placed first rather than flickering between them.
        if (best == nullptr || distance < bestDistance) {
            best = &marker;
            bestDistance = distance;
        }
    }

    return best;
}

void refreshMarkerLevel(Marker& marker, const TraceStore& traces, double reachHz) {
    if (marker.peakLocked) {
        double peakHz = 0.0;
        float peakDb = 0.0F;
        if (traces.peakIn(marker.frequencyHz - reachHz, marker.frequencyHz + reachHz, peakHz,
                          peakDb)) {
            marker.frequencyHz = peakHz;
            marker.levelDb = peakDb;
        }
        return;
    }
    marker.levelDb = traces.levelAt(marker.frequencyHz);
}

} // namespace sweeppp::ui
