// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/remote/ClockMap.hpp"

#include <algorithm>

namespace sweeppp::remote {

void ClockMap::observe(std::uint64_t clientSentNs, std::uint64_t serverNs,
                       std::uint64_t clientReceivedNs) noexcept {
    if (clientReceivedNs < clientSentNs) {
        return;
    }
    const std::uint64_t roundTrip = clientReceivedNs - clientSentNs;
    const std::uint64_t midpoint = clientSentNs + (roundTrip / 2);
    m_samples[m_next] = Sample{.roundTripNs = roundTrip,
                               .offsetNs = static_cast<std::int64_t>(midpoint - serverNs)};
    m_next = (m_next + 1) % kWindow;
    m_count = std::min(m_count + 1, kWindow);
    m_lastRoundTripNs = roundTrip;
}

const ClockMap::Sample& ClockMap::best() const noexcept {
    const Sample* fastest = m_samples.data();
    for (std::size_t i = 1; i < m_count; ++i) {
        if (m_samples[i].roundTripNs < fastest->roundTripNs) {
            fastest = &m_samples[i];
        }
    }
    return *fastest;
}

std::uint64_t ClockMap::bestRoundTripNs() const noexcept {
    return calibrated() ? best().roundTripNs : 0;
}

std::uint64_t ClockMap::toClient(std::uint64_t serverNs, std::uint64_t nowNs) noexcept {
    std::uint64_t mapped = nowNs;
    if (calibrated()) {
        // Unsigned arithmetic wraps to the right answer for either sign of
        // the offset.
        mapped = std::min(serverNs + static_cast<std::uint64_t>(best().offsetNs), nowNs);
    }
    m_lastMappedNs = std::max(m_lastMappedNs, mapped);
    return m_lastMappedNs;
}

void ClockMap::reset() noexcept {
    *this = ClockMap{};
}

} // namespace sweeppp::remote
