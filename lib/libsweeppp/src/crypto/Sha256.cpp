// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/crypto/Sha256.hpp"

#if defined(_WIN32)
#include <windows.h>
// windows.h first: bcrypt.h uses its types without including it.
#include <bcrypt.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <stdlib.h> // NOLINT(modernize-deprecated-headers): arc4random_buf is not in <cstdlib>
#else
#include <cerrno>
#include <sys/random.h>
#endif

#include <algorithm>
#include <bit>
#include <cstring>

namespace sweeppp::crypto {
namespace {

constexpr std::array<std::uint32_t, 64> kRound{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::array<std::uint32_t, 8> kInitial{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

std::uint32_t loadBigEndian(const std::uint8_t* p) noexcept {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) |
           std::uint32_t{p[3]};
}

} // namespace

void Sha256::reset() noexcept {
    m_state = kInitial;
    m_buffered = 0;
    m_totalBytes = 0;
}

void Sha256::compress(const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = loadBigEndian(block + (i * 4));
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 =
            std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    auto [a, b, c, d, e, f, g, h] = m_state;
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + choose + kRound[i] + w[i];
        const std::uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void Sha256::update(std::span<const std::uint8_t> data) noexcept {
    m_totalBytes += data.size();
    const std::uint8_t* p = data.data();
    std::size_t left = data.size();

    if (m_buffered > 0) {
        const std::size_t take = std::min(left, kBlockBytes - m_buffered);
        std::memcpy(m_buffer.data() + m_buffered, p, take);
        m_buffered += take;
        p += take;
        left -= take;
        if (m_buffered < kBlockBytes) {
            return;
        }
        compress(m_buffer.data());
        m_buffered = 0;
    }
    while (left >= kBlockBytes) {
        compress(p);
        p += kBlockBytes;
        left -= kBlockBytes;
    }
    if (left > 0) {
        std::memcpy(m_buffer.data(), p, left);
        m_buffered = left;
    }
}

void Sha256::update(std::string_view text) noexcept {
    update(bytesOf(text));
}

Sha256Digest Sha256::finish() noexcept {
    const std::uint64_t bits = m_totalBytes * 8;

    // 0x80, zeros to 56 mod 64, then the length in bits, big-endian.
    std::array<std::uint8_t, kBlockBytes + 8> tail{};
    tail[0] = 0x80;
    const std::size_t padding =
        (m_buffered < 56 ? 56 - m_buffered : (kBlockBytes + 56) - m_buffered);
    for (std::size_t i = 0; i < 8; ++i) {
        tail[padding + i] = static_cast<std::uint8_t>(bits >> (56 - (i * 8)));
    }
    update(std::span<const std::uint8_t>(tail.data(), padding + 8));

    Sha256Digest digest{};
    for (std::size_t i = 0; i < m_state.size(); ++i) {
        digest[(i * 4) + 0] = static_cast<std::uint8_t>(m_state[i] >> 24);
        digest[(i * 4) + 1] = static_cast<std::uint8_t>(m_state[i] >> 16);
        digest[(i * 4) + 2] = static_cast<std::uint8_t>(m_state[i] >> 8);
        digest[(i * 4) + 3] = static_cast<std::uint8_t>(m_state[i]);
    }
    return digest;
}

Sha256Digest Sha256::of(std::span<const std::uint8_t> data) noexcept {
    Sha256 hash;
    hash.update(data);
    return hash.finish();
}

Sha256Digest Sha256::of(std::string_view text) noexcept {
    return of(bytesOf(text));
}

Sha256Digest hmacSha256(std::span<const std::uint8_t> key,
                        std::span<const std::uint8_t> message) noexcept {
    std::array<std::uint8_t, Sha256::kBlockBytes> block{};
    if (key.size() > block.size()) {
        const Sha256Digest hashed = Sha256::of(key);
        std::ranges::copy(hashed, block.begin());
    } else {
        std::ranges::copy(key, block.begin());
    }

    std::array<std::uint8_t, Sha256::kBlockBytes> pad{};
    for (std::size_t i = 0; i < block.size(); ++i) {
        pad[i] = block[i] ^ 0x36;
    }
    Sha256 inner;
    inner.update(pad);
    inner.update(message);
    const Sha256Digest innerDigest = inner.finish();

    for (std::size_t i = 0; i < block.size(); ++i) {
        pad[i] = block[i] ^ 0x5c;
    }
    Sha256 outer;
    outer.update(pad);
    outer.update(innerDigest);
    return outer.finish();
}

bool constantTimeEqual(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        difference |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    }
    return difference == 0;
}

std::string toHex(std::span<const std::uint8_t> bytes) {
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        text.push_back(kDigits[byte >> 4]);
        text.push_back(kDigits[byte & 0x0f]);
    }
    return text;
}

Status fillRandom(std::span<std::uint8_t> out) {
#if defined(_WIN32)
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        return fail(ErrorCode::IoError, "the system random generator failed");
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(out.data(), out.size());
#else
    std::size_t filled = 0;
    while (filled < out.size()) {
        const ssize_t got = getrandom(out.data() + filled, out.size() - filled, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(ErrorCode::IoError, "the system random generator failed");
        }
        filled += static_cast<std::size_t>(got);
    }
#endif
    return ok();
}

} // namespace sweeppp::crypto
