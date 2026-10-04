// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/remote/RemoteInstrument.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace sweeppp::remote {

/// When to try a dropped server again: at once, then backing off to every
/// 30 seconds, for as long as the operator leaves it trying.
///
/// Only the schedule. Making each attempt, and putting things back when one
/// succeeds, is the owner's: this has no clock of its own, so it can be
/// tested against a made-up one.
class Reconnector {
public:
    /// Delays before attempts 2, 3, ... ; the last repeats.
    static constexpr std::array<std::uint64_t, 6> kDelaysNs{
        1'000'000'000, 2'000'000'000, 4'000'000'000, 8'000'000'000, 15'000'000'000, 30'000'000'000,
    };

    /// Starts trying `endpoint` again, the first attempt due now.
    void begin(RemoteEndpoint endpoint, std::string label, std::uint64_t nowNs);
    void cancel() noexcept;

    [[nodiscard]] bool active() const noexcept { return m_active; }

    /// Whether an attempt should start at `nowNs`. True once per attempt:
    /// the attempt is under way until `failed()` or `succeeded()`.
    [[nodiscard]] bool due(std::uint64_t nowNs) noexcept;

    /// The attempt under way failed; the next is scheduled from `nowNs`.
    void failed(std::uint64_t nowNs) noexcept;
    void succeeded() noexcept;

    /// The attempt under way or last made, from 1.
    [[nodiscard]] int attempt() const noexcept { return m_attempt; }
    [[nodiscard]] std::uint64_t nextAtNs() const noexcept { return m_nextAtNs; }
    [[nodiscard]] bool attempting() const noexcept { return m_attempting; }

    [[nodiscard]] const RemoteEndpoint& endpoint() const noexcept { return m_endpoint; }
    [[nodiscard]] const std::string& label() const noexcept { return m_label; }

private:
    RemoteEndpoint m_endpoint;
    std::string m_label;
    bool m_active = false;
    bool m_attempting = false;
    int m_attempt = 0;
    std::uint64_t m_nextAtNs = 0;
};

} // namespace sweeppp::remote
