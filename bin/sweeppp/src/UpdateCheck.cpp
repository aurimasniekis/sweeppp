// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "UpdateCheck.hpp"

#include <charconv>
#include <string_view>
#include <sweeppp/core/Log.hpp>
#include <sweeppp/core/Version.hpp>
#include <vector>

#ifdef SWEEPPP_WITH_UPDATE_CHECK
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#endif

namespace sweeppp::ui {
namespace {

/// One dotted version as numbers, ignoring anything that is not a digit run.
///
/// A tag may be "v0.2.0", a build string "0.1.0+abc12345", and a pre-release
/// "0.2.0-rc1". Only the numeric prefix decides which is newer; the rest is
/// deliberately not interpreted, because guessing at rc ordering is worse than
/// treating a pre-release as its own version.
[[nodiscard]] std::vector<int> components(std::string_view text) {
    std::vector<int> parts;
    std::size_t i = 0;

    while (i < text.size() && (text[i] == 'v' || text[i] == 'V')) {
        ++i;
    }

    while (i < text.size()) {
        if (std::isdigit(static_cast<unsigned char>(text[i])) == 0) {
            break;
        }
        int value = 0;
        const auto result = std::from_chars(text.data() + i, text.data() + text.size(), value);
        if (result.ec != std::errc{}) {
            break;
        }
        parts.push_back(value);
        i = static_cast<std::size_t>(result.ptr - text.data());
        if (i < text.size() && text[i] == '.') {
            ++i;
            continue;
        }
        break;
    }
    return parts;
}

#ifdef SWEEPPP_WITH_UPDATE_CHECK

std::size_t appendBody(char* data, std::size_t size, std::size_t count, void* context) {
    const std::size_t bytes = size * count;
    static_cast<std::string*>(context)->append(data, bytes);
    return bytes;
}

/// The releases endpoint and the page a person would open, from one place.
constexpr const char* kApiUrl = SWEEPPP_RELEASES_API;

/// Anything larger is not the answer to this question, and reading it would be
/// somebody else's decision about how much memory this process spends.
constexpr std::size_t kMaxBodyBytes = 1024 * 1024;

#endif

} // namespace

bool UpdateCheck::isNewer(std::string_view candidate, std::string_view current) {
    const std::vector<int> a = components(candidate);
    const std::vector<int> b = components(current);
    if (a.empty()) {
        return false;
    }

    for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        const int left = i < a.size() ? a[i] : 0;
        const int right = i < b.size() ? b[i] : 0;
        if (left != right) {
            return left > right;
        }
    }
    return false;
}

#ifdef SWEEPPP_WITH_UPDATE_CHECK

bool UpdateCheck::supported() noexcept {
    return true;
}

void UpdateCheck::start() {
    if (m_started) {
        return;
    }
    m_started = true;

    m_query = std::async(std::launch::async, []() -> std::optional<Release> {
        CURL* curl = curl_easy_init();
        if (curl == nullptr) {
            return std::nullopt;
        }

        std::string body;
        curl_easy_setopt(curl, CURLOPT_URL, kApiUrl);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendBody);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 4L);
        // Bounded, and both halves matter: a connection that never completes
        // and a response that trickles for ever are different failures and
        // this thread must survive either. Nothing waits on it, but a thread
        // held open for the life of the process is still a leak.
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        // GitHub refuses a request with no user agent.
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "sweeppp-update-check");
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

        const CURLcode code = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);

        if (code != CURLE_OK) {
            // Debug, not warn. Being offline is the normal state of a machine
            // on a bench, and an operator who never asked about updates should
            // not be told the network is down.
            logDebug("update", "check failed: {}", curl_easy_strerror(code));
            return std::nullopt;
        }
        if (status != 200 || body.empty() || body.size() > kMaxBodyBytes) {
            logDebug("update", "check returned HTTP {} ({} bytes)", status, body.size());
            return std::nullopt;
        }

        // Every field is optional as far as this code is concerned: the answer
        // comes from a server, and a parse that assumed a shape would turn a
        // changed API into a crash on start-up.
        const nlohmann::json document = nlohmann::json::parse(body, nullptr, false);
        if (document.is_discarded() || !document.is_object()) {
            logDebug("update", "check returned something that is not a release");
            return std::nullopt;
        }
        if (document.value("draft", false) || document.value("prerelease", false)) {
            return std::nullopt;
        }

        Release release;
        release.version = document.value("tag_name", std::string{});
        release.url = document.value("html_url", std::string(SWEEPPP_RELEASES_PAGE));
        if (release.version.empty()) {
            return std::nullopt;
        }

        // Against versionString(), not buildString(): the commit suffix is not
        // a version and would make every nightly look newer or older than
        // itself depending on how the hash sorted.
        if (!isNewer(release.version, versionString())) {
            return std::nullopt;
        }

        if (!release.version.empty() && (release.version[0] == 'v' || release.version[0] == 'V')) {
            release.version.erase(0, 1);
        }
        logInfo("update", "{} is available", release.version);
        return release;
    });
}

#else

bool UpdateCheck::supported() noexcept {
    return false;
}

void UpdateCheck::start() {
    m_started = true;
}

#endif

std::optional<UpdateCheck::Release> UpdateCheck::poll() {
    if (!m_started || m_taken || !m_query.valid()) {
        return std::nullopt;
    }
    if (m_query.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return std::nullopt;
    }

    m_taken = true;
    return m_query.get();
}

} // namespace sweeppp::ui
