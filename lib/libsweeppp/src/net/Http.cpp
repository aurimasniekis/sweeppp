// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/net/Http.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>

namespace sweeppp::net::http {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kReadSlice = std::chrono::milliseconds(200);

bool isTokenChar(char c) noexcept {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    return std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

/// Visible ASCII, space and tab: what a header value may hold.
bool isValueChar(char c) noexcept {
    const auto byte = static_cast<unsigned char>(c);
    return byte == '\t' || (byte >= 0x20 && byte != 0x7F);
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return out;
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

std::string_view Request::header(std::string_view name) const {
    const auto found = headers.find(name);
    return found != headers.end() ? std::string_view(found->second) : std::string_view{};
}

std::optional<std::string> Request::queryValue(std::string_view key) const {
    return formValue(query, key);
}

std::string Request::cookie(std::string_view name) const {
    std::string_view rest = header("cookie");
    while (!rest.empty()) {
        const std::size_t end = rest.find(';');
        const std::string_view pair = trim(rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        const std::size_t equals = pair.find('=');
        if (equals != std::string_view::npos && pair.substr(0, equals) == name) {
            return std::string(pair.substr(equals + 1));
        }
    }
    return {};
}

std::optional<std::string> percentDecode(std::string_view text, bool plusIsSpace) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '%') {
            if (i + 2 >= text.size()) {
                return std::nullopt;
            }
            const int high = hexValue(text[i + 1]);
            const int low = hexValue(text[i + 2]);
            if (high < 0 || low < 0 || (high == 0 && low == 0)) {
                return std::nullopt;
            }
            out += static_cast<char>((high << 4) | low);
            i += 2;
        } else if (c == '+' && plusIsSpace) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

std::optional<std::string> formValue(std::string_view body, std::string_view key) {
    while (!body.empty()) {
        const std::size_t end = body.find('&');
        const std::string_view pair = body.substr(0, end);
        body = end == std::string_view::npos ? std::string_view{} : body.substr(end + 1);
        const std::size_t equals = pair.find('=');
        const auto name = percentDecode(pair.substr(0, equals), true);
        if (name && *name == key) {
            return equals == std::string_view::npos ? std::optional<std::string>(std::string{})
                                                    : percentDecode(pair.substr(equals + 1), true);
        }
    }
    return std::nullopt;
}

namespace {

Status malformed(std::string_view why) {
    return fail(ErrorCode::ProtocolError, "a malformed request: {}", why);
}

/// METHOD SP target SP HTTP/1.x, single spaces, nothing else.
Status parseRequestLine(std::string_view line, Request& request) {
    const std::size_t firstSpace = line.find(' ');
    const std::size_t secondSpace =
        firstSpace == std::string_view::npos ? firstSpace : line.find(' ', firstSpace + 1);
    if (secondSpace == std::string_view::npos ||
        line.find(' ', secondSpace + 1) != std::string_view::npos) {
        return malformed("the request line");
    }
    const std::string_view method = line.substr(0, firstSpace);
    const std::string_view target = line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    const std::string_view version = line.substr(secondSpace + 1);
    if (method.empty() || method.size() > 16 || !std::ranges::all_of(method, isTokenChar)) {
        return malformed("the method");
    }
    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        return malformed("the version");
    }
    const bool printable = std::ranges::all_of(target, [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte > 0x20 && byte < 0x7F;
    });
    if (target.empty() || target.front() != '/' || target.size() > 4096 || !printable) {
        return malformed("the target");
    }
    request.method = std::string(method);
    const std::size_t question = target.find('?');
    auto path = percentDecode(target.substr(0, question), false);
    if (!path) {
        return malformed("the path");
    }
    request.path = std::move(*path);
    if (question != std::string_view::npos) {
        request.query = std::string(target.substr(question + 1));
    }
    return ok();
}

Status addHeader(std::string_view line, Request& request) {
    const std::size_t colon = line.find(':');
    // A line starting with whitespace is a folded continuation, which RFC
    // 9112 forbids a server to accept; it fails the name check here.
    if (colon == std::string_view::npos || colon == 0 ||
        !std::ranges::all_of(line.substr(0, colon), isTokenChar)) {
        return malformed("a header line");
    }
    const std::string_view value = trim(line.substr(colon + 1));
    if (!std::ranges::all_of(value, isValueChar)) {
        return malformed("a header value");
    }
    std::string name = lower(line.substr(0, colon));
    const bool isCookie = name == "cookie";
    const auto [it, inserted] = request.headers.try_emplace(std::move(name), value);
    if (!inserted) {
        it->second += isCookie ? "; " : ", ";
        it->second += value;
    }
    return ok();
}

/// Bytes off a socket by a deadline, for one request.
struct RequestReader {
    TcpSocket& socket;
    Clock::time_point deadline;
    const std::atomic<bool>& stopping;
    std::string buffer;

    Status readMore() {
        std::array<char, 4096> chunk{};
        while (true) {
            if (stopping.load()) {
                return fail(ErrorCode::Cancelled, "cancelled");
            }
            const auto now = Clock::now();
            if (now >= deadline) {
                return fail(ErrorCode::TimedOut, "the request did not arrive in time");
            }
            auto readable = socket.waitReadable(std::min<std::chrono::milliseconds>(
                kReadSlice, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
            if (!readable) {
                return std::unexpected(std::move(readable).error());
            }
            if (!*readable) {
                continue;
            }
            auto got =
                socket.receive({reinterpret_cast<std::uint8_t*>(chunk.data()), chunk.size()});
            if (!got) {
                return std::unexpected(std::move(got).error());
            }
            if (*got == 0) {
                return fail(ErrorCode::IoError, "the client closed the connection");
            }
            buffer.append(chunk.data(), *got);
            return ok();
        }
    }
};

Result<std::size_t> bodyLength(const Request& request, std::size_t maxBody) {
    if (!request.header("transfer-encoding").empty()) {
        return fail<std::size_t>(ErrorCode::ProtocolError, "a chunked request body");
    }
    std::size_t length = 0;
    if (const std::string_view text = request.header("content-length"); !text.empty()) {
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), length);
        if (error != std::errc{} || end != text.data() + text.size()) {
            return fail<std::size_t>(ErrorCode::ProtocolError, "a Content-Length of '{}'", text);
        }
    }
    if (length > std::min(maxBody, kMaxBodyBytes)) {
        return fail<std::size_t>(ErrorCode::ProtocolError, "a request body of {} bytes", length);
    }
    return length;
}

} // namespace

Result<Request> parseHead(std::string_view head) {
    if (head.size() > kMaxHeadBytes) {
        return std::unexpected(malformed("too long").error());
    }
    Request request;
    std::size_t lineEnd = head.find("\r\n");
    if (auto line = parseRequestLine(head.substr(0, lineEnd), request); !line) {
        return std::unexpected(std::move(line).error());
    }
    head = lineEnd == std::string_view::npos ? std::string_view{} : head.substr(lineEnd + 2);

    std::size_t count = 0;
    while (!head.empty()) {
        lineEnd = head.find("\r\n");
        const std::string_view line = head.substr(0, lineEnd);
        head = lineEnd == std::string_view::npos ? std::string_view{} : head.substr(lineEnd + 2);
        if (line.empty()) {
            break;
        }
        if (++count > kMaxHeaders) {
            return std::unexpected(malformed("too many headers").error());
        }
        if (auto added = addHeader(line, request); !added) {
            return std::unexpected(std::move(added).error());
        }
    }
    return request;
}

Result<Request> readRequest(TcpSocket& socket, std::size_t maxBody, Clock::time_point deadline,
                            const std::atomic<bool>& stopping) {
    RequestReader reader{.socket = socket, .deadline = deadline, .stopping = stopping};
    std::size_t headEnd = std::string::npos;
    while (headEnd == std::string::npos) {
        if (auto more = reader.readMore(); !more) {
            return std::unexpected(std::move(more).error());
        }
        headEnd = reader.buffer.find("\r\n\r\n");
        if (headEnd == std::string::npos && reader.buffer.size() > kMaxHeadBytes) {
            break;
        }
    }
    if (headEnd == std::string::npos || headEnd > kMaxHeadBytes) {
        return fail<Request>(ErrorCode::ProtocolError, "a request head over {} bytes",
                             kMaxHeadBytes);
    }

    auto request = parseHead(std::string_view(reader.buffer).substr(0, headEnd));
    if (!request) {
        return request;
    }
    request->peer = socket.peerAddress();
    auto length = bodyLength(*request, maxBody);
    if (!length) {
        return std::unexpected(std::move(length).error());
    }
    const std::size_t bodyStart = headEnd + 4;
    while (reader.buffer.size() < bodyStart + *length) {
        if (auto more = reader.readMore(); !more) {
            return std::unexpected(std::move(more).error());
        }
    }
    request->body = reader.buffer.substr(bodyStart, *length);
    return request;
}

std::string_view reasonPhrase(int status) noexcept {
    switch (status) {
    case 101:
        return "Switching Protocols";
    case 200:
        return "OK";
    case 204:
        return "No Content";
    case 304:
        return "Not Modified";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 413:
        return "Content Too Large";
    case 426:
        return "Upgrade Required";
    case 429:
        return "Too Many Requests";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "Unknown";
    }
}

std::string responseHead(int status, const Headers& headers,
                         std::optional<std::size_t> contentLength) {
    std::string head = std::format("HTTP/1.1 {} {}\r\n", status, reasonPhrase(status));
    for (const auto& [name, value] : headers) {
        head += std::format("{}: {}\r\n", name, value);
    }
    if (contentLength) {
        head += std::format("Content-Length: {}\r\n", *contentLength);
    }
    if (status != 101) {
        head += "Connection: close\r\n";
    }
    head += "\r\n";
    return head;
}

Status sendResponse(TcpSocket& socket, int status, std::string_view contentType,
                    std::string_view body, const Headers& extra) {
    Headers headers = extra;
    if (!contentType.empty()) {
        headers.emplace_back("Content-Type", std::string(contentType));
    }
    headers.emplace_back("Cache-Control", "no-store");
    headers.emplace_back("X-Content-Type-Options", "nosniff");
    std::string out = responseHead(status, headers, body.size());
    out += body;
    return socket.sendAll({reinterpret_cast<const std::uint8_t*>(out.data()), out.size()});
}

} // namespace sweeppp::net::http
