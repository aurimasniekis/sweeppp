// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/net/Base64.hpp"

#include <array>

namespace sweeppp::net {
namespace {

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int valueOf(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

} // namespace

std::string base64Encode(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t group =
            (std::uint32_t{bytes[i]} << 16) | (std::uint32_t{bytes[i + 1]} << 8) | bytes[i + 2];
        out += kAlphabet[(group >> 18) & 63U];
        out += kAlphabet[(group >> 12) & 63U];
        out += kAlphabet[(group >> 6) & 63U];
        out += kAlphabet[group & 63U];
    }
    const std::size_t rest = bytes.size() - i;
    if (rest > 0) {
        std::uint32_t group = std::uint32_t{bytes[i]} << 16;
        if (rest == 2) {
            group |= std::uint32_t{bytes[i + 1]} << 8;
        }
        out += kAlphabet[(group >> 18) & 63U];
        out += kAlphabet[(group >> 12) & 63U];
        out += rest == 2 ? kAlphabet[(group >> 6) & 63U] : '=';
        out += '=';
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> base64Decode(std::string_view text) {
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> out;
    out.reserve((text.size() / 4) * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        std::array<int, 4> v{};
        int padding = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const char c = text[i + j];
            if (c == '=' && last && j >= 2) {
                v[j] = 0;
                ++padding;
                continue;
            }
            if (padding > 0) {
                return std::nullopt;
            }
            v[j] = valueOf(c);
            if (v[j] < 0) {
                return std::nullopt;
            }
        }
        const auto group =
            (static_cast<std::uint32_t>(v[0]) << 18) | (static_cast<std::uint32_t>(v[1]) << 12) |
            (static_cast<std::uint32_t>(v[2]) << 6) | static_cast<std::uint32_t>(v[3]);
        out.push_back(static_cast<std::uint8_t>(group >> 16));
        if (padding < 2) {
            out.push_back(static_cast<std::uint8_t>(group >> 8));
        }
        if (padding < 1) {
            out.push_back(static_cast<std::uint8_t>(group));
        }
    }
    return out;
}

} // namespace sweeppp::net
