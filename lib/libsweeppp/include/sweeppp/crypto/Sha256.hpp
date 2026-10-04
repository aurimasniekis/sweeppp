// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace sweeppp::crypto {

using Sha256Digest = std::array<std::uint8_t, 32>;

/// SHA-256 (FIPS 180-4), incremental.
///
/// In-tree because the remote handshake needs exactly one keyed hash and a
/// TLS library for it would be the largest dependency in the build.
class Sha256 {
public:
    static constexpr std::size_t kBlockBytes = 64;

    Sha256() noexcept { reset(); }

    void reset() noexcept;
    void update(std::span<const std::uint8_t> data) noexcept;
    void update(std::string_view text) noexcept;

    /// The digest; the hash must be `reset()` before it is used again.
    [[nodiscard]] Sha256Digest finish() noexcept;

    [[nodiscard]] static Sha256Digest of(std::span<const std::uint8_t> data) noexcept;
    [[nodiscard]] static Sha256Digest of(std::string_view text) noexcept;

private:
    void compress(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 8> m_state{};
    std::array<std::uint8_t, kBlockBytes> m_buffer{};
    std::size_t m_buffered = 0;
    std::uint64_t m_totalBytes = 0;
};

/// HMAC-SHA256 (RFC 2104).
[[nodiscard]] Sha256Digest hmacSha256(std::span<const std::uint8_t> key,
                                      std::span<const std::uint8_t> message) noexcept;

/// Equal length and equal bytes, taking the same time wherever they differ.
[[nodiscard]] bool constantTimeEqual(std::span<const std::uint8_t> a,
                                     std::span<const std::uint8_t> b) noexcept;

/// Lower-case hex.
[[nodiscard]] std::string toHex(std::span<const std::uint8_t> bytes);

/// Fills `out` from the operating system's cryptographic generator.
Status fillRandom(std::span<std::uint8_t> out);

/// A string's bytes, for the functions above.
[[nodiscard]] inline std::span<const std::uint8_t> bytesOf(std::string_view text) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

} // namespace sweeppp::crypto
