// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sweeppp::remote {

/// Maps the server's monotonic clock onto the client's.
///
/// Each ping gives one estimate of the offset between the two, good to half
/// its round trip. The estimate kept is the one from the fastest round trip
/// among the recent ones: queueing only ever adds delay, so the fastest is
/// the least distorted, and a window rather than all time lets the two
/// clocks drift apart without the map going stale.
class ClockMap {
public:
    static constexpr std::size_t kWindow = 32;

    /// A ping sent at `clientSentNs`, answered at the server's `serverNs`,
    /// whose answer arrived at `clientReceivedNs`.
    void observe(std::uint64_t clientSentNs, std::uint64_t serverNs,
                 std::uint64_t clientReceivedNs) noexcept;

    [[nodiscard]] bool calibrated() const noexcept { return m_count > 0; }

    /// The client time `serverNs` corresponds to, given the client's `nowNs`.
    ///
    /// Never later than `nowNs`, since nothing the server did can have
    /// happened in the client's future, and never earlier than the time this
    /// returned before -- so events and frames mapped in the order they
    /// arrived stay in that order. `nowNs` itself before the first ping.
    [[nodiscard]] std::uint64_t toClient(std::uint64_t serverNs, std::uint64_t nowNs) noexcept;

    /// Round trip of the latest ping, and of the fastest in the window.
    [[nodiscard]] std::uint64_t lastRoundTripNs() const noexcept { return m_lastRoundTripNs; }
    [[nodiscard]] std::uint64_t bestRoundTripNs() const noexcept;

    void reset() noexcept;

private:
    struct Sample {
        std::uint64_t roundTripNs = 0;
        std::int64_t offsetNs = 0; ///< client minus server
    };

    [[nodiscard]] const Sample& best() const noexcept;

    std::array<Sample, kWindow> m_samples{};
    std::size_t m_next = 0;
    std::size_t m_count = 0;
    std::uint64_t m_lastRoundTripNs = 0;
    std::uint64_t m_lastMappedNs = 0;
};

} // namespace sweeppp::remote
