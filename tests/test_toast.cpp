// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>
#include <format>
#include <string>
#include <sweeppp/ui/ToastCenter.hpp>

using namespace sweeppp;
using namespace sweeppp::ui;

namespace {

constexpr std::uint64_t kSecond = 1'000'000'000;

/// Whether a card with this id is still up.
bool holds(const ToastCenter& toasts, ToastId id) {
    for (const Toast& toast : toasts.visible()) {
        if (toast.id == id) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("information expires on its deadline and failures do not") {
    ToastCenter toasts;

    const ToastId note = toasts.info("plugin 'bandplan' enabled", 0);
    const ToastId saved = toasts.success("profile 'field' saved", 0);
    const ToastId warned = toasts.warning("'field' loaded, but the radio has moved on", 0);
    const ToastId failed = toasts.error("device is not available", 0);

    CHECK(toasts.visible().size() == 4);

    // A whisker before the deadline everything is still up: expiry is not a
    // rounding-down of the dwell.
    toasts.update(ToastCenter::kDefaultDwellNs - 1);
    CHECK(toasts.visible().size() == 4);

    toasts.update(ToastCenter::kDefaultDwellNs);
    CHECK_FALSE(holds(toasts, note));
    CHECK_FALSE(holds(toasts, saved));

    // The two severities worth not missing are the two that wait.
    toasts.update(kSecond * 3600);
    CHECK(toasts.visible().size() == 2);
    CHECK(holds(toasts, warned));
    CHECK(holds(toasts, failed));
}

TEST_CASE("a hovered stack holds its timers") {
    ToastCenter toasts;

    const ToastId note = toasts.info("serial copied to the clipboard", 0);

    toasts.update(kSecond, false);
    CHECK(holds(toasts, note));

    // Two seconds under the pointer, which the card gets back: without the
    // hold it would be gone by the fourth.
    toasts.update(kSecond * 2, true);
    toasts.update(kSecond * 3, true);
    toasts.update(kSecond * 5, false);
    CHECK(holds(toasts, note));

    toasts.update(kSecond * 6, false);
    CHECK_FALSE(holds(toasts, note));
}

TEST_CASE("the first update has no interval to hold") {
    ToastCenter toasts;

    // Hovered from the very first frame, with the clock already well past the
    // dwell -- there is no earlier update to measure the hold against, so the
    // card must expire rather than be carried forward by an invented interval.
    const ToastId note = toasts.info("theme written to /tmp/x.toml", 0);
    toasts.update(kSecond * 30, true);
    CHECK_FALSE(holds(toasts, note));
}

TEST_CASE("identical messages coalesce and restart the timer") {
    ToastCenter toasts;

    const ToastId first = toasts.error("sample rate rejected", 0);
    const ToastId again = toasts.error("sample rate rejected", kSecond);

    CHECK(again == first);
    REQUIRE(toasts.visible().size() == 1);
    CHECK(toasts.visible()[0].count == 2);
    CHECK(toasts.visible()[0].raisedNs == kSecond);

    // Same words, different severity: not the same message.
    toasts.info("sample rate rejected", kSecond);
    CHECK(toasts.visible().size() == 2);

    // A repeat resets the deadline rather than letting the original one run
    // out under a message that is still arriving.
    ToastCenter transient;
    const ToastId note = transient.info("copied https://example.invalid", 0);
    transient.info("copied https://example.invalid", kSecond * 3);
    transient.update(kSecond * 5);
    CHECK(holds(transient, note));
    transient.update(kSecond * 7);
    CHECK_FALSE(holds(transient, note));
}

TEST_CASE("the stack is capped, dropping the oldest") {
    ToastCenter toasts;

    ToastId oldest = 0;
    for (std::size_t i = 0; i < ToastCenter::kMaxVisible + 2; ++i) {
        const ToastId id = toasts.error(std::format("failure {}", i), 0);
        if (i == 0) {
            oldest = id;
        }
    }

    CHECK(toasts.visible().size() == ToastCenter::kMaxVisible);
    CHECK_FALSE(holds(toasts, oldest));

    // Newest is appended, so what survives is the tail in the order it
    // arrived.
    CHECK(toasts.visible().front().text == "failure 2");
    CHECK(toasts.visible().back().text == std::format("failure {}", ToastCenter::kMaxVisible + 1));
}

TEST_CASE("dismissing removes exactly one card") {
    ToastCenter toasts;

    const ToastId first = toasts.error("one", 0);
    const ToastId second = toasts.error("two", 0);

    toasts.dismiss(first);
    CHECK(toasts.visible().size() == 1);
    CHECK(holds(toasts, second));

    // A card dismissed twice, and an id that was never raised, are both
    // no-ops: a caller holding a latched id across frames need not check.
    toasts.dismiss(first);
    toasts.dismiss(0);
    toasts.dismiss(second + 1000);
    CHECK(toasts.visible().size() == 1);

    toasts.dismissAll();
    CHECK(toasts.empty());
}

TEST_CASE("a latched condition raises one card and takes it back") {
    // What AppState's setError/clearError pair does, in the terms this class
    // offers: the radio came back, so "not available" should go rather than
    // linger until somebody clicks it.
    ToastCenter toasts;
    ToastId latched = 0;

    const auto setError = [&](std::string message) {
        toasts.dismiss(latched);
        latched = toasts.error(std::move(message), 0);
    };
    const auto clearError = [&] {
        toasts.dismiss(latched);
        latched = 0;
    };

    setError("hackrf is not available: no such device");
    CHECK(toasts.visible().size() == 1);

    // A second condition replaces the first rather than stacking under it.
    setError("hackrf is not available: claimed by another process");
    CHECK(toasts.visible().size() == 1);
    CHECK(toasts.visible()[0].text == "hackrf is not available: claimed by another process");

    clearError();
    CHECK(toasts.empty());

    // And clearing when nothing is latched leaves the rest of the stack alone.
    toasts.info("plugin 'bandplan' enabled", 0);
    clearError();
    CHECK(toasts.visible().size() == 1);
}

TEST_CASE("a card with something to do about it does not expire") {
    ToastCenter toasts;

    const ToastId id = toasts.post(ToastSeverity::Info, "0.2.0 is available",
                                   {ToastAction{.id = "update.show", .label = "Show"},
                                    ToastAction{.id = "update.disable", .label = "Stop checking"}},
                                   1'000);

    REQUIRE(toasts.visible().size() == 1);
    REQUIRE(toasts.visible().front().actions.size() == 2);
    CHECK(toasts.visible().front().actions.front().id == "update.show");

    // An Info card is normally gone four seconds later. An offer must not be:
    // the operator saw that there was one and has no way to get it back.
    toasts.update(1'000 + ToastCenter::kDefaultDwellNs * 10);
    REQUIRE(toasts.visible().size() == 1);
    CHECK(toasts.visible().front().id == id);

    // It still goes when it is answered or dismissed.
    toasts.dismiss(id);
    CHECK(toasts.empty());
}
