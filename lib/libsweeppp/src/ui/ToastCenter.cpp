// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>
#include <sweeppp/ui/ToastCenter.hpp>
#include <utility>

namespace sweeppp::ui {
namespace {

std::uint64_t dwellNs(ToastSeverity severity) noexcept {
    switch (severity) {
    case ToastSeverity::Warning:
    case ToastSeverity::Error:
        return 0;
    case ToastSeverity::Info:
    case ToastSeverity::Success:
        break;
    }
    return ToastCenter::kDefaultDwellNs;
}

std::uint64_t deadline(std::uint64_t nowNs, std::uint64_t dwell) noexcept {
    return dwell == 0 ? 0 : nowNs + dwell;
}

} // namespace

ToastId ToastCenter::post(ToastSeverity severity, std::string text,
                          std::vector<ToastAction> actions, std::uint64_t nowNs) {
    const ToastId id = post(severity, std::move(text), nowNs);

    const std::lock_guard lock(m_mutex);
    for (Toast& toast : m_toasts) {
        if (toast.id == id) {
            toast.actions = std::move(actions);
            // No deadline: see the declaration. Coalescing onto an existing
            // card takes its actions too, which is what a repeat of the same
            // offer should do.
            toast.expiresNs = 0;
            break;
        }
    }
    return id;
}

ToastId ToastCenter::post(ToastSeverity severity, std::string text, std::uint64_t nowNs) {
    const std::lock_guard lock(m_mutex);

    const std::uint64_t dwell = dwellNs(severity);

    // Coalescing is what makes a persistent error survivable. A failing
    // parameter write inside a slider drag raises the same sentence every
    // frame; appending each one fills the corner in a second and buries the
    // message under copies of itself.
    for (Toast& existing : m_toasts) {
        if (existing.severity == severity && existing.text == text) {
            ++existing.count;
            existing.raisedNs = nowNs;
            existing.expiresNs = deadline(nowNs, dwell);
            return existing.id;
        }
    }

    const ToastId id = ++m_nextId;
    m_toasts.push_back(Toast{.id = id,
                             .severity = severity,
                             .text = std::move(text),
                             .count = 1,
                             .raisedNs = nowNs,
                             .expiresNs = deadline(nowNs, dwell)});

    if (m_toasts.size() > kMaxVisible) {
        const auto excess = static_cast<std::ptrdiff_t>(m_toasts.size() - kMaxVisible);
        m_toasts.erase(m_toasts.begin(), m_toasts.begin() + excess);
    }

    return id;
}

void ToastCenter::dismiss(ToastId id) {
    if (id == 0) {
        return;
    }

    const std::lock_guard lock(m_mutex);
    std::erase_if(m_toasts, [id](const Toast& toast) { return toast.id == id; });
}

void ToastCenter::dismissAll() {
    const std::lock_guard lock(m_mutex);
    m_toasts.clear();
}

void ToastCenter::update(std::uint64_t nowNs, bool hovered) {
    const std::lock_guard lock(m_mutex);

    // Held rather than paused-and-resumed: every deadline is pushed out by the
    // interval just spent under the pointer, so a card that had a second left
    // when it was hovered still has a second left when the pointer leaves.
    if (hovered && m_haveLastUpdate && nowNs > m_lastUpdateNs) {
        const std::uint64_t held = nowNs - m_lastUpdateNs;
        for (Toast& toast : m_toasts) {
            if (toast.expiresNs != 0) {
                toast.expiresNs += held;
            }
        }
    }

    m_lastUpdateNs = nowNs;
    m_haveLastUpdate = true;

    std::erase_if(m_toasts, [nowNs](const Toast& toast) {
        return toast.expiresNs != 0 && toast.expiresNs <= nowNs;
    });
}

} // namespace sweeppp::ui
