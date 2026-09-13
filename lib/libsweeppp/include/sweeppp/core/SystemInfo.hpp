// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace sweeppp {

/// What the machine has, for sizing decisions the operator makes.
///
/// Deliberately only what can be answered exactly. There is no portable way to
/// ask OpenGL how much video memory exists -- the vendor extensions that answer
/// it are not present on every driver, and inventing a number would be worse
/// than admitting the gap, because it would be used to size a buffer.
struct SystemInfo {
    /// Total physical RAM. Zero when the platform could not be asked.
    ///
    /// On a unified-memory machine this is also the pool textures come from,
    /// which is why it is the right budget to show beside a texture size there.
    std::uint64_t totalMemoryBytes = 0;

    /// True where the GPU allocates from the same pool as the CPU, so
    /// totalMemoryBytes bounds texture allocation too. On a discrete card it
    /// does not, and the real limit is video memory we cannot portably read.
    bool unifiedMemory = false;
};

[[nodiscard]] SystemInfo systemInfo();

} // namespace sweeppp
