// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

/// Noise_NNpsk0_25519_ChaChaPoly_BLAKE2b, from the Noise Protocol Framework
/// (revision 34), on Monocypher's primitives.
///
/// The one pattern the remote link needs: neither side has a long-term key,
/// and both prove they know the same pre-shared one -- the server's token --
/// before anything else is said. Kept to that pattern rather than a general
/// Noise library, so there is no pattern table to get wrong.
namespace sweeppp::crypto {

using Key = std::array<std::uint8_t, 32>;
using HandshakeHash = std::array<std::uint8_t, 64>;

inline constexpr std::size_t kNoiseTagBytes = 16;

/// Noise's 65535-byte limit on any message, tag included.
inline constexpr std::size_t kNoiseMaxMessageBytes = 65535;

/// One direction of an established channel: ChaCha20-Poly1305 with a counter
/// nonce. Wiped when destroyed.
class CipherState {
public:
    CipherState() = default;
    explicit CipherState(const Key& key) noexcept;
    ~CipherState();

    CipherState(const CipherState&) = delete;
    CipherState& operator=(const CipherState&) = delete;
    CipherState(CipherState&& other) noexcept;
    CipherState& operator=(CipherState&& other) noexcept;

    [[nodiscard]] bool hasKey() const noexcept { return m_hasKey; }

    /// Appends the ciphertext and its tag to `out`. With no key, the plaintext
    /// as it is -- which is what Noise asks of a handshake not yet keyed.
    Status encrypt(std::span<const std::uint8_t> ad, std::span<const std::uint8_t> plaintext,
                   std::vector<std::uint8_t>& out);

    /// Appends the plaintext to `out`. A wrong tag is Corrupt, and leaves the
    /// nonce where it was.
    Status decrypt(std::span<const std::uint8_t> ad, std::span<const std::uint8_t> ciphertext,
                   std::vector<std::uint8_t>& out);

private:
    Key m_key{};
    std::uint64_t m_nonce = 0;
    bool m_hasKey = false;
};

/// The handshake, initiator or responder: two messages, then `split()`.
class NoiseHandshake {
public:
    enum class Role : std::uint8_t { Initiator, Responder };

    /// `ephemeral` fixes the ephemeral secret key, for the published test
    /// vectors; left empty, one is drawn from the system generator.
    NoiseHandshake(Role role, const Key& psk, std::span<const std::uint8_t> prologue,
                   std::optional<Key> ephemeral = std::nullopt);
    ~NoiseHandshake();

    NoiseHandshake(const NoiseHandshake&) = delete;
    NoiseHandshake& operator=(const NoiseHandshake&) = delete;
    NoiseHandshake(NoiseHandshake&&) = delete;
    NoiseHandshake& operator=(NoiseHandshake&&) = delete;

    /// This side's next message, carrying `payload`.
    [[nodiscard]] Result<std::vector<std::uint8_t>>
    writeMessage(std::span<const std::uint8_t> payload);

    /// The payload of the other side's next message. PermissionDenied when it
    /// does not decrypt -- which, for the first message, means the two sides
    /// were given different keys.
    [[nodiscard]] Result<std::vector<std::uint8_t>>
    readMessage(std::span<const std::uint8_t> message);

    [[nodiscard]] bool complete() const noexcept { return m_message >= 2; }

    /// Initiator-to-responder, then responder-to-initiator. Once, when
    /// complete.
    [[nodiscard]] std::pair<CipherState, CipherState> split();

    [[nodiscard]] const HandshakeHash& handshakeHash() const noexcept { return m_h; }

private:
    void mixHash(std::span<const std::uint8_t> data);
    void mixKey(std::span<const std::uint8_t> material);
    void mixKeyAndHash(std::span<const std::uint8_t> material);
    Status encryptAndHash(std::span<const std::uint8_t> plaintext, std::vector<std::uint8_t>& out);
    Status decryptAndHash(std::span<const std::uint8_t> ciphertext, std::vector<std::uint8_t>& out);

    Role m_role;
    Key m_psk{};
    HandshakeHash m_ck{};
    HandshakeHash m_h{};
    CipherState m_cipher;
    Key m_ephemeralSecret{};
    Key m_ephemeralPublic{};
    Key m_remoteEphemeral{};
    int m_message = 0;
};

/// The pre-shared key a token stands for: BLAKE2b-256 of a label and the
/// token, so a key is always 32 bytes whatever the token's length.
[[nodiscard]] Key pskFromToken(std::string_view token);

/// HMAC-BLAKE2b (RFC 2104, 128-byte block), as Noise's HKDF uses it.
[[nodiscard]] HandshakeHash hmacBlake2b(std::span<const std::uint8_t> key,
                                        std::span<const std::uint8_t> message);

} // namespace sweeppp::crypto
