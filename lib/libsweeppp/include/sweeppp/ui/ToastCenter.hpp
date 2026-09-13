// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace sweeppp::ui {

/// How loudly a message asks to be read, and -- through that -- how long it
/// stays up.
enum class ToastSeverity : std::uint8_t { Info, Success, Warning, Error };

using ToastId = std::uint64_t;

/// A button on a card.
///
/// An id and a label, not a callback. This class is pure data with no ImGui
/// and no thread affinity in it -- a std::function stored here would be
/// captured wherever the message was raised and invoked on the render thread,
/// which is exactly the coupling keeping it testable is meant to avoid. The
/// renderer reports which id was clicked and the caller decides what that
/// means.
struct ToastAction {
    std::string id;
    std::string label;
};

struct Toast {
    ToastId id = 0;
    ToastSeverity severity = ToastSeverity::Info;
    std::string text;
    std::uint32_t count = 1;     ///< Repeats coalesced into this one.
    std::uint64_t raisedNs = 0;  ///< Or last repeated, which is what the timer runs from.
    std::uint64_t expiresNs = 0; ///< Zero means it stays until dismissed.

    /// Buttons offered on the card, left to right. Usually empty.
    std::vector<ToastAction> actions;
};

/// The application's one message channel: what used to be a single line of
/// text in the status bar, as a stack of cards that expire.
///
/// Pure data and time, with no ImGui in it, for the reason `Theme` and
/// `TraceStore` are: the rules worth getting right here -- when a message
/// expires, when two of them are the same message -- are testable only if
/// nothing has to open a window to reach them.
///
/// `nowNs` is a parameter rather than a `monotonicNs()` call inside, so those
/// rules can be driven by a test at whatever instant it likes instead of by
/// sleeping. The renderer passes the house clock.
class ToastCenter {
public:
    /// How long an Info or Success card stays up.
    ///
    /// Long enough to catch out of the corner of an eye and read; short enough
    /// that a run of confirmations does not silt up the corner. Warnings and
    /// errors get no deadline at all -- a message that can be missed in four
    /// seconds is not where a failure belongs, now that the status bar no
    /// longer keeps one.
    static constexpr std::uint64_t kDefaultDwellNs = 4'000'000'000;

    /// Most cards up at once, oldest dropped past it.
    ///
    /// With identical messages coalescing, six *distinct* live messages means
    /// something is genuinely wrong, and stacking more of them would cover the
    /// plot they are about.
    static constexpr std::size_t kMaxVisible = 6;

    /// Raises a message, or folds it into the identical one already up.
    ///
    /// Returns the card's id either way, so a caller holding a latched
    /// condition can dismiss exactly what it raised.
    ToastId post(ToastSeverity severity, std::string text, std::uint64_t nowNs);

    /// Raises a message that offers something to do about it.
    ///
    /// A card with actions never expires on its own, whatever its severity: an
    /// offer that vanishes after four seconds is worse than no offer, because
    /// the operator saw that there was one and cannot get it back.
    ToastId post(ToastSeverity severity, std::string text, std::vector<ToastAction> actions,
                 std::uint64_t nowNs);

    ToastId info(std::string text, std::uint64_t nowNs) {
        return post(ToastSeverity::Info, std::move(text), nowNs);
    }
    ToastId success(std::string text, std::uint64_t nowNs) {
        return post(ToastSeverity::Success, std::move(text), nowNs);
    }
    ToastId warning(std::string text, std::uint64_t nowNs) {
        return post(ToastSeverity::Warning, std::move(text), nowNs);
    }
    ToastId error(std::string text, std::uint64_t nowNs) {
        return post(ToastSeverity::Error, std::move(text), nowNs);
    }

    /// Removes one card. An id that is no longer up -- expired, dismissed by a
    /// click, or never raised -- is a no-op, so a caller may hold one across
    /// frames without checking.
    void dismiss(ToastId id);
    void dismissAll();

    /// Drops what has expired, and advances the clock the timers run against.
    ///
    /// `hovered` holds every timer for as long as it is true, the way a web
    /// toast pauses while the pointer is on it: the cards are stacked, so
    /// reading the bottom one means the pointer is over the one above it, and
    /// a message must not run out while it is being read.
    void update(std::uint64_t nowNs, bool hovered = false);

    /// Oldest first: newest is appended, so a persistent error settles at the
    /// top of the stack and does not jump when a transient card below it
    /// expires.
    ///
    /// A view into the list rather than a copy, valid until the next call that
    /// mutates it. Rendering reads it and then acts -- dismissing from inside
    /// a loop over this would cut the ground from under the loop.
    [[nodiscard]] std::span<const Toast> visible() const noexcept { return m_toasts; }

    [[nodiscard]] bool empty() const noexcept { return m_toasts.empty(); }

private:
    /// Posting is the UI thread's job today, but `AppState` is the project's
    /// cross-thread hub and this centre hangs off it, so the first message
    /// raised from a worker is a matter of when rather than whether. One lock
    /// against a whole class of later bug.
    mutable std::mutex m_mutex;

    std::vector<Toast> m_toasts;
    ToastId m_nextId = 0;

    /// When `update` last ran, so a hovered frame knows how much time to give
    /// back. Unset before the first one: there is no interval to hold yet.
    std::uint64_t m_lastUpdateNs = 0;
    bool m_haveLastUpdate = false;
};

} // namespace sweeppp::ui
