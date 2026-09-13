// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <future>
#include <optional>
#include <string>

namespace sweeppp::ui {

/// Asks GitHub whether there is a newer release, once, on a thread of its own.
///
/// Off the UI thread because it is a network request: DNS, a TLS handshake and
/// a response, any of which can take the full timeout behind a captive portal
/// or a corporate proxy. On the render thread that is a frozen window, and the
/// answer is not worth one frame of stutter, let alone ten seconds.
class UpdateCheck {
public:
    struct Release {
        std::string version; ///< "0.2.0", with any leading v removed.
        std::string url;     ///< The release page, for the operator to open.
    };

    /// Whether this build can check at all. False without libcurl, in which
    /// case the setting is hidden rather than offered and ignored.
    [[nodiscard]] static bool supported() noexcept;

    /// Starts the query. Does nothing if one is already in flight or has
    /// already answered -- this is a start-up check, not a poll.
    void start();

    /// The answer, once, when it arrives. `std::nullopt` while the query is
    /// still running, and after it has been taken.
    ///
    /// A release *newer* than this build, or nothing: "you are up to date" is
    /// not news, and a card saying it every start-up would train the operator
    /// to dismiss the cards that matter.
    [[nodiscard]] std::optional<Release> poll();

    /// Compares two dotted version strings numerically.
    ///
    /// Not a string compare, which puts "0.10.0" before "0.9.0", and not a
    /// float parse, which reads "0.1.0" as malformed. Exposed because it is
    /// the part worth testing.
    [[nodiscard]] static bool isNewer(std::string_view candidate, std::string_view current);

private:
    std::future<std::optional<Release>> m_query;
    bool m_started = false;
    bool m_taken = false;
};

} // namespace sweeppp::ui
