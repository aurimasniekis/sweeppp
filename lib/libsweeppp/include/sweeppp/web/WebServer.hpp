// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/remote/RemoteServer.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace sweeppp::web {

/// A file of the browser UI, compiled into the binary.
struct EmbeddedFile {
    std::string_view path; ///< "index.html", "assets/index-3f2a.js"
    std::span<const std::uint8_t> bytes;
    std::string_view mime;
    std::string_view etag;
};

struct WebServerConfig {
    std::string listenAddress = "127.0.0.1";
    std::uint16_t port = 8080; ///< 0 binds an ephemeral port
    /// What the login page asks for. Empty means no login, which `start()`
    /// allows on a loopback address only.
    std::string token;
    /// The UI, as built into the binary.
    std::span<const EmbeddedFile> assets;
    /// Files served from here first, for working on the UI; empty for none.
    std::filesystem::path webRoot;

    std::chrono::seconds sessionIdle = std::chrono::hours(12);
    /// Between two login attempts from one address.
    std::chrono::milliseconds loginInterval{1000};
    std::chrono::milliseconds requestTimeout{10'000};
    std::size_t maxConnections = 32;
};

/// The browser UI and the endpoints it boots from, on plain HTTP; TLS is a
/// reverse proxy's job. A WebSocket on /ws carries the record stream to the
/// `RemoteServer`, as one more of its clients.
class WebServer {
public:
    WebServer(remote::RemoteServer& server, WebServerConfig config);
    ~WebServer();

    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;
    WebServer(WebServer&&) = delete;
    WebServer& operator=(WebServer&&) = delete;

    Status start();
    void stop();

    [[nodiscard]] std::uint16_t port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// The MIME type a file of this name is served as.
[[nodiscard]] std::string_view mimeTypeFor(std::string_view path) noexcept;

} // namespace sweeppp::web
