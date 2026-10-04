// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/net/Socket.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// As much of HTTP/1.1 as the web UI needs: one request a connection, small
/// bodies, no chunking. Every limit is a refusal, not a truncation.
namespace sweeppp::net::http {

/// The request line and every header together.
inline constexpr std::size_t kMaxHeadBytes = std::size_t{16} * 1024;
inline constexpr std::size_t kMaxHeaders = 100;
/// The largest body any request may carry; most may carry none.
inline constexpr std::size_t kMaxBodyBytes = std::size_t{4} * 1024;

struct Request {
    std::string method;
    std::string path;  ///< Percent-decoded, without the query
    std::string query; ///< Raw, after the '?'
    /// Names lower-cased; repeats joined with ", ".
    std::map<std::string, std::string, std::less<>> headers;
    std::string body;
    std::string peer; ///< "192.168.1.20:51234"

    /// Empty when absent.
    [[nodiscard]] std::string_view header(std::string_view name) const;
    /// One value of the query string, decoded; nothing when absent.
    [[nodiscard]] std::optional<std::string> queryValue(std::string_view key) const;
    /// One cookie's value; empty when absent.
    [[nodiscard]] std::string cookie(std::string_view name) const;
};

/// The request line and headers, the blank line excluded. ProtocolError for
/// anything that is not a well-formed request a browser would send.
[[nodiscard]] Result<Request> parseHead(std::string_view head);

/// One request off `socket`, body included when it has one of at most
/// `maxBody` bytes. ProtocolError for a malformed or oversized request,
/// TimedOut when it is not all there by `deadline`.
[[nodiscard]] Result<Request> readRequest(TcpSocket& socket, std::size_t maxBody,
                                          std::chrono::steady_clock::time_point deadline,
                                          const std::atomic<bool>& stopping);

using Headers = std::vector<std::pair<std::string, std::string>>;

/// A status line and headers, with Connection: close, ready to send. The
/// body's length is given when it is known.
[[nodiscard]] std::string responseHead(int status, const Headers& headers,
                                       std::optional<std::size_t> contentLength);

/// A whole response.
Status sendResponse(TcpSocket& socket, int status, std::string_view contentType,
                    std::string_view body, const Headers& extra = {});

[[nodiscard]] std::string_view reasonPhrase(int status) noexcept;

/// `%XX` and, in a query, `+` as a space. Nothing for a malformed escape or
/// one that decodes to NUL.
[[nodiscard]] std::optional<std::string> percentDecode(std::string_view text, bool plusIsSpace);

/// One field of an application/x-www-form-urlencoded body.
[[nodiscard]] std::optional<std::string> formValue(std::string_view body, std::string_view key);

} // namespace sweeppp::net::http
