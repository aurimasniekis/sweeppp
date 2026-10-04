// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/crypto/Sha1.hpp"

#include <bit>
#include <vector>

namespace sweeppp::crypto {

Sha1Digest sha1(std::span<const std::uint8_t> data) {
    std::array<std::uint32_t, 5> h{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

    // The message, the 0x80 marker, zeros to 56 mod 64, and the bit length.
    std::vector<std::uint8_t> padded(data.begin(), data.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8U;
    padded.push_back(0x80);
    while (padded.size() % 64 != 56) {
        padded.push_back(0);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<std::uint8_t>(bits >> static_cast<unsigned>(shift)));
    }

    std::array<std::uint32_t, 80> w{};
    for (std::size_t block = 0; block < padded.size(); block += 64) {
        for (std::size_t i = 0; i < 16; ++i) {
            const std::uint8_t* p = padded.data() + block + (i * 4);
            w[i] = (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
                   (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
        }
        for (std::size_t i = 16; i < 80; ++i) {
            w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        std::uint32_t a = h[0];
        std::uint32_t b = h[1];
        std::uint32_t c = h[2];
        std::uint32_t d = h[3];
        std::uint32_t e = h[4];
        for (std::size_t i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const std::uint32_t next = std::rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = std::rotl(b, 30);
            b = a;
            a = next;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    Sha1Digest digest{};
    for (std::size_t i = 0; i < h.size(); ++i) {
        digest[(i * 4) + 0] = static_cast<std::uint8_t>(h[i] >> 24);
        digest[(i * 4) + 1] = static_cast<std::uint8_t>(h[i] >> 16);
        digest[(i * 4) + 2] = static_cast<std::uint8_t>(h[i] >> 8);
        digest[(i * 4) + 3] = static_cast<std::uint8_t>(h[i]);
    }
    return digest;
}

Sha1Digest sha1(std::string_view text) {
    return sha1({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

} // namespace sweeppp::crypto
