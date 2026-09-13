// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <imgui.h>

namespace sweeppp::ui {

/// Geometry of the spectrum plot, shared by everything drawn over it.
///
/// One arithmetic, in one place. The plot, the overlay a contributor's spans
/// are painted into and the context handed to a plugin all convert between
/// frequency and pixels; a second copy of this is how two of them come to
/// disagree by a pixel and a half at the right-hand edge.
///
/// The waterfall fills in the frequency half and leaves the dB half at its
/// defaults: it shares the frequency axis and has no amplitude axis of its
/// own, which is exactly why one struct serves both.
struct SpectrumLayout {
    ImVec2 origin;
    ImVec2 size;
    double fromHz = 0.0;
    double toHz = 0.0;
    float minDb = -110.0F;
    float maxDb = -10.0F;

    [[nodiscard]] float xForHz(double hz) const {
        const double span = toHz - fromHz;
        const double t = span > 0.0 ? (hz - fromHz) / span : 0.0;
        return origin.x + static_cast<float>(t) * size.x;
    }

    [[nodiscard]] double hzForX(float x) const {
        const float t = size.x > 0.0F ? (x - origin.x) / size.x : 0.0F;
        return fromHz + static_cast<double>(t) * (toHz - fromHz);
    }

    [[nodiscard]] float yForDb(float db) const {
        const float span = maxDb - minDb;
        const float t = span > 0.0F ? (db - minDb) / span : 0.0F;
        return origin.y + (1.0F - t) * size.y;
    }

    [[nodiscard]] float dbForY(float y) const {
        const float t = size.y > 0.0F ? (y - origin.y) / size.y : 0.0F;
        return maxDb - t * (maxDb - minDb);
    }

    [[nodiscard]] float left() const { return origin.x; }
    [[nodiscard]] float right() const { return origin.x + size.x; }
    [[nodiscard]] float top() const { return origin.y; }
    [[nodiscard]] float bottom() const { return origin.y + size.y; }
};

} // namespace sweeppp::ui
