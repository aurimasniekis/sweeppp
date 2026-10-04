// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/remote/Reconnector.hpp"

#include <algorithm>
#include <utility>

namespace sweeppp::remote {

void Reconnector::begin(RemoteEndpoint endpoint, std::string label, std::uint64_t nowNs) {
    m_endpoint = std::move(endpoint);
    m_label = std::move(label);
    m_active = true;
    m_attempting = false;
    m_attempt = 0;
    m_nextAtNs = nowNs;
}

void Reconnector::cancel() noexcept {
    m_active = false;
    m_attempting = false;
}

bool Reconnector::due(std::uint64_t nowNs) noexcept {
    if (!m_active || m_attempting || nowNs < m_nextAtNs) {
        return false;
    }
    m_attempting = true;
    ++m_attempt;
    return true;
}

void Reconnector::failed(std::uint64_t nowNs) noexcept {
    if (!m_active) {
        return;
    }
    m_attempting = false;
    const auto index = static_cast<std::size_t>(std::max(m_attempt - 1, 0));
    m_nextAtNs = nowNs + kDelaysNs[std::min(index, kDelaysNs.size() - 1)];
}

void Reconnector::succeeded() noexcept {
    m_active = false;
    m_attempting = false;
}

} // namespace sweeppp::remote
