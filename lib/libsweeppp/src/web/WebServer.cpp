// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/web/WebServer.hpp"

#include "sweeppp/core/Log.hpp"
#include "sweeppp/core/Toml.hpp"
#include "sweeppp/crypto/Sha256.hpp"
#include "sweeppp/net/Http.hpp"
#include "sweeppp/net/Socket.hpp"
#include "sweeppp/net/WebSocket.hpp"
#include "sweeppp/plugin/PluginHost.hpp"
#include "sweeppp/sweep/SweepPreset.hpp"
#include "sweeppp/ui/ColorMap.hpp"
#include "sweeppp/ui/Theme.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

namespace sweeppp::web {
namespace {

using Clock = std::chrono::steady_clock;
using json = nlohmann::json;
namespace http = net::http;

constexpr auto kAcceptSlice = std::chrono::milliseconds(200);
constexpr std::string_view kCookie = "sweeppp";
constexpr std::size_t kDownloadChunk = std::size_t{256} * 1024;

/// What a page of this UI may load and talk to: itself.
constexpr std::string_view kContentPolicy =
    "default-src 'self'; img-src 'self' data: blob:; style-src 'self' 'unsafe-inline'; "
    "connect-src 'self' ws: wss:; frame-ancestors 'none'; base-uri 'none'";

std::string addressOnly(std::string_view peer) {
    if (peer.starts_with('[')) {
        return std::string(peer.substr(1, peer.find(']') - 1));
    }
    return std::string(peer.substr(0, peer.rfind(':')));
}

bool isSafeRelative(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos) {
        return false;
    }
    std::string_view rest = path;
    while (!rest.empty()) {
        const std::size_t slash = rest.find('/');
        const std::string_view segment = rest.substr(0, slash);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
    }
    return true;
}

std::string hex(const ui::Color& color) {
    return color.toHex(color.a < 1.0F);
}

json themeJson(const ui::Theme& theme) {
    const ui::ChromeTheme& c = theme.chrome();
    const ui::SpectrumTheme& s = theme.spectrum();
    return json{
        {"name", theme.name()},
        {"dark", c.dark},
        {"chrome",
         {{"windowBackground", hex(c.windowBackground)},
          {"panelBackground", hex(c.panelBackground)},
          {"headerBackground", hex(c.headerBackground)},
          {"text", hex(c.text)},
          {"textDim", hex(c.textDim)},
          {"accent", hex(c.accent)},
          {"accentHover", hex(c.accentHover)},
          {"border", hex(c.border)},
          {"separator", hex(c.separator)},
          {"buttonBackground", hex(c.buttonBackground)},
          {"buttonHover", hex(c.buttonHover)},
          {"buttonActive", hex(c.buttonActive)},
          {"inputBackground", hex(c.inputBackground)},
          {"inputHover", hex(c.inputHover)},
          {"inputActive", hex(c.inputActive)},
          {"start", hex(c.start)},
          {"stop", hex(c.stop)},
          {"record", hex(c.record)},
          {"ok", hex(c.ok)},
          {"warning", hex(c.warning)},
          {"danger", hex(c.danger)},
          {"rounding", c.rounding},
          {"borderSize", c.borderSize}}},
        {"spectrum",
         {{"background", hex(s.background)},
          {"grid", hex(s.grid)},
          {"axisText", hex(s.axisText)},
          {"traceLive", hex(s.traceLive)},
          {"traceMaxHold", hex(s.traceMaxHold)},
          {"traceMinHold", hex(s.traceMinHold)},
          {"traceAverage", hex(s.traceAverage)},
          {"cursor", hex(s.cursor)},
          {"marker", hex(s.marker)},
          {"markerText", hex(s.markerText)},
          {"selection", hex(s.selection)},
          {"contributionAlpha", s.contributionAlpha},
          {"fillColorMap", s.fillColorMap},
          {"fillAlpha", s.fillAlpha},
          {"traceThickness", s.traceThickness}}},
        {"waterfall", {{"colorMap", theme.waterfall().colorMap}}},
    };
}

json colorMapJson(const ui::ColorMap& map) {
    json stops = json::array();
    for (const ui::ColorStop& stop : map.stops()) {
        stops.push_back(json{{"position", stop.position}, {"color", stop.color.toHex()}});
    }
    return json{{"name", map.name()}, {"stops", std::move(stops)}};
}

} // namespace

std::string_view mimeTypeFor(std::string_view path) noexcept {
    const std::size_t dot = path.rfind('.');
    const std::string_view ext =
        dot == std::string_view::npos ? std::string_view{} : path.substr(dot);
    if (ext == ".html") {
        return "text/html; charset=utf-8";
    }
    if (ext == ".js" || ext == ".mjs") {
        return "text/javascript; charset=utf-8";
    }
    if (ext == ".css") {
        return "text/css; charset=utf-8";
    }
    if (ext == ".json" || ext == ".map") {
        return "application/json";
    }
    if (ext == ".svg") {
        return "image/svg+xml";
    }
    if (ext == ".png") {
        return "image/png";
    }
    if (ext == ".ico") {
        return "image/x-icon";
    }
    if (ext == ".woff2") {
        return "font/woff2";
    }
    if (ext == ".txt") {
        return "text/plain; charset=utf-8";
    }
    return "application/octet-stream";
}

struct WebServer::Impl {
    Impl(remote::RemoteServer& server, WebServerConfig config)
        : server(server), config(std::move(config)) {}

    struct Connection {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    // ---- sessions ----------------------------------------------------------------

    [[nodiscard]] bool authRequired() const { return !config.token.empty(); }

    [[nodiscard]] bool authenticated(const http::Request& request) {
        if (!authRequired()) {
            return true;
        }
        const std::string cookie = request.cookie(kCookie);
        if (cookie.empty()) {
            return false;
        }
        const std::lock_guard lock(sessionMutex);
        const auto found = sessions.find(cookie);
        if (found == sessions.end()) {
            return false;
        }
        const auto now = Clock::now();
        if (now - found->second > config.sessionIdle) {
            sessions.erase(found);
            return false;
        }
        found->second = now;
        return true;
    }

    /// The token compared by its hash, so neither its length nor where it
    /// first differs shows in the time taken.
    [[nodiscard]] bool tokenMatches(std::string_view given) const {
        const crypto::Sha256Digest want = crypto::Sha256::of(config.token);
        const crypto::Sha256Digest got = crypto::Sha256::of(given);
        return crypto::constantTimeEqual(want, got);
    }

    void login(net::TcpSocket& socket, const http::Request& request) {
        const std::string address = addressOnly(request.peer);
        {
            const std::lock_guard lock(sessionMutex);
            const auto now = Clock::now();
            auto& last = lastAttempt[address];
            if (last != Clock::time_point{} && now - last < config.loginInterval) {
                (void)http::sendResponse(socket, 429, "text/plain",
                                         "Too many attempts; wait a "
                                         "second.\n");
                return;
            }
            last = now;
            std::erase_if(lastAttempt, [&](const auto& entry) {
                return now - entry.second > std::chrono::minutes(10);
            });
        }

        const std::optional<std::string> token = http::formValue(request.body, "token");
        if (!authRequired() || !token || !tokenMatches(*token)) {
            logWarn("web", "{}: wrong token", request.peer);
            (void)http::sendResponse(socket, 401, "text/plain", "That is not the token.\n");
            return;
        }

        std::array<std::uint8_t, 32> random{};
        if (auto filled = crypto::fillRandom(random); !filled) {
            (void)http::sendResponse(socket, 500, "text/plain", "No randomness for a session.\n");
            return;
        }
        const std::string id = crypto::toHex(random);
        {
            const std::lock_guard lock(sessionMutex);
            const auto now = Clock::now();
            std::erase_if(sessions, [&](const auto& entry) {
                return now - entry.second > config.sessionIdle;
            });
            sessions[id] = now;
        }
        // Secure when the proxy in front says the browser came over HTTPS; a
        // cookie marked Secure on plain HTTP would never come back.
        const bool secure = request.header("x-forwarded-proto") == "https";
        logInfo("web", "{} logged in", request.peer);
        (void)http::sendResponse(
            socket, 204, {}, {},
            {{"Set-Cookie", std::format("{}={}; HttpOnly; SameSite=Strict; Path=/{}", kCookie, id,
                                        secure ? "; Secure" : "")}});
    }

    void logout(net::TcpSocket& socket, const http::Request& request) {
        {
            const std::lock_guard lock(sessionMutex);
            sessions.erase(request.cookie(kCookie));
        }
        (void)http::sendResponse(
            socket, 204, {}, {},
            {{"Set-Cookie",
              std::format("{}=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0", kCookie)}});
    }

    // ---- static files --------------------------------------------------------------

    void serveFile(net::TcpSocket& socket, const http::Request& request, std::string_view path) {
        if (!isSafeRelative(path)) {
            (void)http::sendResponse(socket, 404, "text/plain", "Not found.\n");
            return;
        }
        if (!config.webRoot.empty()) {
            const std::filesystem::path file = config.webRoot / std::filesystem::path(path);
            std::error_code ec;
            if (std::filesystem::is_regular_file(file, ec)) {
                std::ifstream in(file, std::ios::binary);
                const std::string body((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
                (void)http::sendResponse(
                    socket, 200, mimeTypeFor(path), body,
                    {{"Content-Security-Policy", std::string(kContentPolicy)}});
                return;
            }
        }
        const auto found = std::ranges::find(config.assets, path, &EmbeddedFile::path);
        if (found == config.assets.end()) {
            (void)http::sendResponse(socket, 404, "text/plain", "Not found.\n");
            return;
        }
        const std::string etag = std::format("\"{}\"", found->etag);
        http::Headers headers{{"Content-Type", std::string(found->mime)},
                              {"ETag", etag},
                              {"Cache-Control", "no-cache"},
                              {"X-Content-Type-Options", "nosniff"},
                              {"Content-Security-Policy", std::string(kContentPolicy)}};
        if (request.header("if-none-match") == etag) {
            const std::string head = http::responseHead(304, headers, std::nullopt);
            (void)socket.sendAll({reinterpret_cast<const std::uint8_t*>(head.data()), head.size()});
            return;
        }
        const std::string head = http::responseHead(200, headers, found->bytes.size());
        if (socket.sendAll({reinterpret_cast<const std::uint8_t*>(head.data()), head.size()})) {
            (void)socket.sendAll(found->bytes);
        }
    }

    // ---- the API -----------------------------------------------------------------

    static void sendJson(net::TcpSocket& socket, const json& body) {
        (void)http::sendResponse(socket, 200, "application/json", body.dump());
    }

    void overlays(net::TcpSocket& socket, const http::Request& request) {
        const auto number = [&](std::string_view key) {
            const std::optional<std::string> text = request.queryValue(key);
            const Result<double> value =
                text ? toml_util::parseFrequency(*text) : Result<double>(std::nan(""));
            return value ? *value : std::nan("");
        };
        const double from = number("from");
        const double to = number("to");
        if (!std::isfinite(from) || !std::isfinite(to) || to <= from) {
            (void)http::sendResponse(socket, 400, "text/plain", "from and to, in Hz, please.\n");
            return;
        }
        // What the desktop draws: a contributor the operator hid still answers
        // what is at a frequency, but is not painted.
        PluginManager& plugins = PluginManager::instance();
        std::vector<std::string> hidden;
        for (const std::string& id : plugins.contributorOrder()) {
            if (!plugins.contributorShown(id)) {
                hidden.push_back(id);
            }
        }
        json list = json::array();
        for (const Contribution& entry : plugins.contributionsIn(from, to)) {
            if (!entry.hostRendered || std::ranges::find(hidden, entry.pluginId) != hidden.end()) {
                continue;
            }
            list.push_back(json{{"plugin", entry.pluginId},
                                {"pluginName", entry.pluginName},
                                {"type", toString(entry.type)},
                                {"name", entry.name},
                                {"description", entry.description},
                                {"category", entry.category},
                                {"startHz", entry.startHz},
                                {"stopHz", entry.stopHz},
                                {"color", entry.color}});
        }
        sendJson(socket, list);
    }

    /// The server's data contributors in rank order: whether each is shown,
    /// the datasets it offers, and its tick tree.
    void contributors(net::TcpSocket& socket) {
        PluginManager& plugins = PluginManager::instance();
        json list = json::array();
        for (const std::string& id : plugins.contributorOrder()) {
            const Result<PluginInfo> info = plugins.info(id);
            std::string caption;
            if (info) {
                for (const PluginFacetInfo& facet : info->facets) {
                    if (facet.kind == PluginFacetKind::Contributor && !facet.description.empty()) {
                        caption = facet.description;
                        break;
                    }
                }
                if (caption.empty()) {
                    caption = info->description;
                }
            }
            json datasets = json::array();
            for (const auto& [name, description] : plugins.datasets(id)) {
                datasets.push_back(json{{"name", name}, {"description", description}});
            }
            json tree = json::array();
            for (const ContributorTreeRow& row : plugins.contributorTree(id)) {
                tree.push_back(json{{"depth", row.depth},
                                    {"key", row.key},
                                    {"name", row.name},
                                    {"detail", row.detail},
                                    {"description", row.description},
                                    {"color", row.color},
                                    {"anyOn", row.anyOn},
                                    {"allOn", row.allOn}});
            }
            list.push_back(json{{"id", id},
                                {"name", info ? info->name : id},
                                {"caption", caption},
                                {"shown", plugins.contributorShown(id)},
                                {"datasets", std::move(datasets)},
                                {"activeDataset", plugins.activeDataset(id)},
                                {"tree", std::move(tree)}});
        }
        sendJson(socket, json{{"generation", plugins.contributionsGeneration()},
                              {"contributors", std::move(list)}});
    }

    /// The ranges every install ships with; a browser keeps its own beside them.
    void presets(net::TcpSocket& socket) {
        json list = json::array();
        for (const SweepPreset& preset : SweepPresetStore::builtins()) {
            json segments = json::array();
            for (const SweepSegment& segment : preset.segments) {
                segments.push_back(json{{"startHz", segment.startHz}, {"stopHz", segment.stopHz}});
            }
            list.push_back(json{{"name", preset.name}, {"segments", std::move(segments)}});
        }
        sendJson(socket, list);
    }

    void download(net::TcpSocket& socket, const std::string& name) {
        auto path = server.recordingFile(name);
        if (!path) {
            const int status = path.error().code() == ErrorCode::Unavailable ? 409 : 404;
            (void)http::sendResponse(socket, status, "text/plain", path.error().message() + "\n");
            return;
        }
        std::ifstream in(*path, std::ios::binary);
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(*path, ec);
        if (!in || ec) {
            (void)http::sendResponse(socket, 500, "text/plain", "Could not read it.\n");
            return;
        }
        const std::string head = http::responseHead(
            200,
            {{"Content-Type", "application/octet-stream"},
             {"Content-Disposition", std::format("attachment; filename=\"{}\"", name)},
             {"Cache-Control", "no-store"},
             {"X-Content-Type-Options", "nosniff"}},
            static_cast<std::size_t>(size));
        if (!socket.sendAll({reinterpret_cast<const std::uint8_t*>(head.data()), head.size()})) {
            return;
        }
        std::vector<char> chunk(kDownloadChunk);
        while (in && !stopping.load()) {
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const auto got = static_cast<std::size_t>(in.gcount());
            if (got == 0 ||
                !socket.sendAll({reinterpret_cast<const std::uint8_t*>(chunk.data()), got})) {
                return;
            }
        }
    }

    /// The upgrade, and the socket handed to the radio's server. True when
    /// the socket went with it.
    bool upgrade(net::TcpSocket& socket, const http::Request& request) {
        if (!net::ws::sameOrigin(request)) {
            logWarn("web", "{}: a WebSocket from {}, another site", request.peer,
                    request.header("origin"));
            (void)http::sendResponse(socket, 403, "text/plain", "Not from this page.\n");
            return false;
        }
        auto accept = net::ws::checkUpgrade(request);
        if (!accept) {
            (void)http::sendResponse(socket, 426, "text/plain", accept.error().message() + "\n",
                                     {{"Sec-WebSocket-Version", "13"}});
            return false;
        }
        const std::string head = http::responseHead(101,
                                                    {{"Upgrade", "websocket"},
                                                     {"Connection", "Upgrade"},
                                                     {"Sec-WebSocket-Accept", *accept}},
                                                    std::nullopt);
        if (!socket.sendAll({reinterpret_cast<const std::uint8_t*>(head.data()), head.size()})) {
            return false;
        }
        (void)socket.setNoDelay(true);
        server.adopt(std::make_unique<net::ws::WebSocketChannel>(std::move(socket)));
        return true;
    }

    void handle(net::TcpSocket socket) {
        auto request = http::readRequest(socket, http::kMaxBodyBytes,
                                         Clock::now() + config.requestTimeout, stopping);
        if (!request) {
            if (request.error().code() == ErrorCode::ProtocolError) {
                (void)http::sendResponse(socket, 400, "text/plain", "Bad request.\n");
            }
            return;
        }
        const std::string& path = request->path;
        const std::string& method = request->method;
        // Only the login carries a body.
        if (!request->body.empty() && path != "/login") {
            (void)http::sendResponse(socket, 413, "text/plain", "No body here.\n");
            return;
        }

        if (path == "/login" && method == "POST") {
            login(socket, *request);
            return;
        }
        if (path == "/logout" && method == "POST") {
            logout(socket, *request);
            return;
        }
        if (method != "GET") {
            (void)http::sendResponse(socket, 405, "text/plain", "Not here.\n");
            return;
        }

        if (path == "/api/auth") {
            sendJson(socket, json{{"required", authRequired()},
                                  {"authenticated", authenticated(*request)}});
            return;
        }
        if (path == "/api/themes") {
            json list = json::array();
            for (const ui::Theme& theme : ui::discoverThemes()) {
                list.push_back(themeJson(theme));
            }
            sendJson(socket, list);
            return;
        }
        if (path == "/api/colormaps") {
            json list = json::array();
            for (const ui::ColorMap& map : ui::discoverColorMaps()) {
                list.push_back(colorMapJson(map));
            }
            sendJson(socket, list);
            return;
        }

        const bool needsSession = path == "/ws" || path == "/api/overlays" ||
                                  path == "/api/contributors" || path == "/api/presets" ||
                                  path.starts_with("/api/recordings/");
        if (needsSession && !authenticated(*request)) {
            (void)http::sendResponse(socket, 401, "text/plain", "Log in first.\n");
            return;
        }
        if (path == "/ws") {
            (void)upgrade(socket, *request);
            return;
        }
        if (path == "/api/overlays") {
            overlays(socket, *request);
            return;
        }
        if (path == "/api/contributors") {
            contributors(socket);
            return;
        }
        if (path == "/api/presets") {
            presets(socket);
            return;
        }
        if (path.starts_with("/api/recordings/")) {
            download(socket, path.substr(std::string_view("/api/recordings/").size()));
            return;
        }
        if (path.starts_with("/api/")) {
            (void)http::sendResponse(socket, 404, "text/plain", "Not found.\n");
            return;
        }
        // The UI routes everything else to its one page.
        if (path.starts_with("/assets/") || path == "/favicon.svg") {
            serveFile(socket, *request, std::string_view(path).substr(1));
            return;
        }
        serveFile(socket, *request, "index.html");
    }

    void acceptLoop() {
        while (!stopping.load()) {
            std::erase_if(connections, [](Connection& connection) {
                if (!connection.done->load()) {
                    return false;
                }
                connection.thread.join();
                return true;
            });

            auto accepted = listener.accept(kAcceptSlice);
            if (!accepted) {
                logWarn("web", "{}", accepted.error().describe());
                std::this_thread::sleep_for(kAcceptSlice);
                continue;
            }
            if (!*accepted) {
                continue;
            }
            if (connections.size() >= config.maxConnections) {
                (void)http::sendResponse(**accepted, 503, "text/plain", "Busy; try again.\n");
                (*accepted)->close();
                continue;
            }
            auto done = std::make_shared<std::atomic<bool>>(false);
            connections.push_back(Connection{
                .thread = std::thread([this, socket = std::move(**accepted), done]() mutable {
                    handle(std::move(socket));
                    done->store(true);
                }),
                .done = done});
        }
        for (Connection& connection : connections) {
            connection.thread.join();
        }
        connections.clear();
    }

    remote::RemoteServer& server;
    WebServerConfig config;
    net::TcpListener listener;
    std::atomic<bool> stopping{false};
    bool started = false;
    std::thread acceptThread;
    std::vector<Connection> connections; ///< The accept thread's alone

    std::mutex sessionMutex;
    std::map<std::string, Clock::time_point, std::less<>> sessions;
    std::map<std::string, Clock::time_point, std::less<>> lastAttempt;
};

WebServer::WebServer(remote::RemoteServer& server, WebServerConfig config)
    : m_impl(std::make_unique<Impl>(server, std::move(config))) {
}

WebServer::~WebServer() {
    stop();
}

Status WebServer::start() {
    Impl& impl = *m_impl;
    if (impl.started) {
        return ok();
    }
    if (impl.config.token.empty() && !net::isLoopbackAddress(impl.config.listenAddress)) {
        return fail(ErrorCode::PermissionDenied,
                    "the web UI on {} without a token would hand the radio to anyone who can "
                    "reach it; give --token, or listen on 127.0.0.1",
                    impl.config.listenAddress);
    }
    auto listener = net::TcpListener::listen(impl.config.listenAddress, impl.config.port, 16);
    if (!listener) {
        return std::unexpected(std::move(listener).error());
    }
    impl.listener = std::move(*listener);
    impl.stopping.store(false);
    impl.acceptThread = std::thread([&impl] { impl.acceptLoop(); });
    impl.started = true;
    logInfo("web", "the browser UI is on http://{}:{}{}", impl.config.listenAddress,
            impl.listener.port(), impl.config.token.empty() ? " (no login)" : "");
    return ok();
}

void WebServer::stop() {
    Impl& impl = *m_impl;
    if (!impl.started) {
        return;
    }
    impl.stopping.store(true);
    if (impl.acceptThread.joinable()) {
        impl.acceptThread.join();
    }
    impl.listener.close();
    impl.started = false;
}

std::uint16_t WebServer::port() const noexcept {
    return m_impl->listener.port();
}

} // namespace sweeppp::web
