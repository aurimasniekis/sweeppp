// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/crypto/Noise.hpp"

#include "sweeppp/crypto/Sha256.hpp"

#include <algorithm>
#include <limits>
#include <monocypher.h>

namespace sweeppp::crypto {
namespace {

constexpr std::string_view kProtocolName = "Noise_NNpsk0_25519_ChaChaPoly_BLAKE2b";
constexpr std::size_t kBlockBytes = 128;
constexpr std::size_t kDhBytes = 32;

std::span<const std::uint8_t> bytes(std::string_view text) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

HandshakeHash blake2b(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b = {}) {
    HandshakeHash out{};
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, out.size());
    crypto_blake2b_update(&ctx, a.data(), a.size());
    crypto_blake2b_update(&ctx, b.data(), b.size());
    crypto_blake2b_final(&ctx, out.data());
    return out;
}

/// HKDF as Noise defines it (section 4.3): two or three outputs.
struct Derived {
    HandshakeHash first{};
    HandshakeHash second{};
    HandshakeHash third{};
};

Derived hkdf(const HandshakeHash& chainingKey, std::span<const std::uint8_t> material,
             int outputs) {
    HandshakeHash tempKey = hmacBlake2b(chainingKey, material);
    Derived derived;
    const std::uint8_t one = 0x01;
    derived.first = hmacBlake2b(tempKey, {&one, 1});

    std::array<std::uint8_t, 65> chained{};
    std::ranges::copy(derived.first, chained.begin());
    chained[64] = 0x02;
    derived.second = hmacBlake2b(tempKey, chained);

    if (outputs == 3) {
        std::ranges::copy(derived.second, chained.begin());
        chained[64] = 0x03;
        derived.third = hmacBlake2b(tempKey, chained);
    }
    crypto_wipe(tempKey.data(), tempKey.size());
    crypto_wipe(chained.data(), chained.size());
    return derived;
}

Key truncated(const HandshakeHash& hash) noexcept {
    Key key{};
    std::copy_n(hash.begin(), key.size(), key.begin());
    return key;
}

/// The nonce Noise's ChaChaPoly asks for: four zero bytes, then the counter
/// little-endian.
std::array<std::uint8_t, 12> ietfNonce(std::uint64_t counter) noexcept {
    std::array<std::uint8_t, 12> nonce{};
    for (std::size_t i = 0; i < 8; ++i) {
        nonce[4 + i] = static_cast<std::uint8_t>(counter >> (8 * i));
    }
    return nonce;
}

} // namespace

HandshakeHash hmacBlake2b(std::span<const std::uint8_t> key,
                          std::span<const std::uint8_t> message) {
    std::array<std::uint8_t, kBlockBytes> block{};
    if (key.size() > block.size()) {
        const HandshakeHash hashed = blake2b(key);
        std::ranges::copy(hashed, block.begin());
    } else {
        std::ranges::copy(key, block.begin());
    }

    std::array<std::uint8_t, kBlockBytes> pad{};
    for (std::size_t i = 0; i < block.size(); ++i) {
        pad[i] = block[i] ^ 0x36;
    }
    const HandshakeHash inner = blake2b(pad, message);
    for (std::size_t i = 0; i < block.size(); ++i) {
        pad[i] = block[i] ^ 0x5c;
    }
    const HandshakeHash outer = blake2b(pad, inner);
    crypto_wipe(block.data(), block.size());
    crypto_wipe(pad.data(), pad.size());
    return outer;
}

Key pskFromToken(std::string_view token) {
    Key psk{};
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, psk.size());
    const std::span<const std::uint8_t> label = bytes("sweeppp-remote-psk");
    crypto_blake2b_update(&ctx, label.data(), label.size());
    crypto_blake2b_update(&ctx, bytes(token).data(), token.size());
    crypto_blake2b_final(&ctx, psk.data());
    return psk;
}

// ---------------------------------------------------------------- CipherState

CipherState::CipherState(const Key& key) noexcept : m_key(key), m_hasKey(true) {
}

CipherState::~CipherState() {
    crypto_wipe(m_key.data(), m_key.size());
}

CipherState::CipherState(CipherState&& other) noexcept
    : m_key(other.m_key), m_nonce(other.m_nonce), m_hasKey(other.m_hasKey) {
    crypto_wipe(other.m_key.data(), other.m_key.size());
    other.m_hasKey = false;
    other.m_nonce = 0;
}

CipherState& CipherState::operator=(CipherState&& other) noexcept {
    if (this != &other) {
        m_key = other.m_key;
        m_nonce = other.m_nonce;
        m_hasKey = other.m_hasKey;
        crypto_wipe(other.m_key.data(), other.m_key.size());
        other.m_hasKey = false;
        other.m_nonce = 0;
    }
    return *this;
}

Status CipherState::encrypt(std::span<const std::uint8_t> ad,
                            std::span<const std::uint8_t> plaintext,
                            std::vector<std::uint8_t>& out) {
    if (!m_hasKey) {
        out.insert(out.end(), plaintext.begin(), plaintext.end());
        return ok();
    }
    // 2^64 - 1 is reserved (section 5.1); a channel this old is long dead.
    if (m_nonce == std::numeric_limits<std::uint64_t>::max()) {
        return fail(ErrorCode::OutOfRange, "the channel has used every nonce");
    }

    const std::size_t at = out.size();
    out.resize(at + plaintext.size() + kNoiseTagBytes);
    const std::array<std::uint8_t, 12> nonce = ietfNonce(m_nonce);

    // A context per message: Monocypher's streaming context re-keys after
    // each one, which is its own construction rather than Noise's. A fresh
    // context's first message is exactly RFC 8439.
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, m_key.data(), nonce.data());
    crypto_aead_write(&ctx, out.data() + at, out.data() + at + plaintext.size(), ad.data(),
                      ad.size(), plaintext.data(), plaintext.size());
    crypto_wipe(&ctx, sizeof(ctx));
    ++m_nonce;
    return ok();
}

Status CipherState::decrypt(std::span<const std::uint8_t> ad,
                            std::span<const std::uint8_t> ciphertext,
                            std::vector<std::uint8_t>& out) {
    if (!m_hasKey) {
        out.insert(out.end(), ciphertext.begin(), ciphertext.end());
        return ok();
    }
    if (ciphertext.size() < kNoiseTagBytes) {
        return fail(ErrorCode::Corrupt, "a message shorter than its tag");
    }
    if (m_nonce == std::numeric_limits<std::uint64_t>::max()) {
        return fail(ErrorCode::OutOfRange, "the channel has used every nonce");
    }

    const std::size_t textBytes = ciphertext.size() - kNoiseTagBytes;
    const std::size_t at = out.size();
    out.resize(at + textBytes);
    const std::array<std::uint8_t, 12> nonce = ietfNonce(m_nonce);

    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, m_key.data(), nonce.data());
    const int mismatch = crypto_aead_read(&ctx, out.data() + at, ciphertext.data() + textBytes,
                                          ad.data(), ad.size(), ciphertext.data(), textBytes);
    crypto_wipe(&ctx, sizeof(ctx));
    if (mismatch != 0) {
        out.resize(at);
        return fail(ErrorCode::Corrupt, "a message failed authentication");
    }
    ++m_nonce;
    return ok();
}

// -------------------------------------------------------------- the handshake

NoiseHandshake::NoiseHandshake(Role role, const Key& psk, std::span<const std::uint8_t> prologue,
                               std::optional<Key> ephemeral)
    : m_role(role), m_psk(psk) {
    std::ranges::copy(bytes(kProtocolName), m_h.begin());
    m_ck = m_h;
    mixHash(prologue);

    if (ephemeral) {
        m_ephemeralSecret = *ephemeral;
    } else if (!fillRandom(m_ephemeralSecret)) {
        // Without the system generator there is no safe key to make; an
        // all-zero one produces a handshake the other side will reject.
        m_ephemeralSecret.fill(0);
    }
    crypto_x25519_public_key(m_ephemeralPublic.data(), m_ephemeralSecret.data());
}

NoiseHandshake::~NoiseHandshake() {
    crypto_wipe(m_psk.data(), m_psk.size());
    crypto_wipe(m_ck.data(), m_ck.size());
    crypto_wipe(m_ephemeralSecret.data(), m_ephemeralSecret.size());
}

void NoiseHandshake::mixHash(std::span<const std::uint8_t> data) {
    m_h = blake2b(m_h, data);
}

void NoiseHandshake::mixKey(std::span<const std::uint8_t> material) {
    Derived derived = hkdf(m_ck, material, 2);
    m_ck = derived.first;
    m_cipher = CipherState(truncated(derived.second));
    crypto_wipe(&derived, sizeof(derived));
}

void NoiseHandshake::mixKeyAndHash(std::span<const std::uint8_t> material) {
    Derived derived = hkdf(m_ck, material, 3);
    m_ck = derived.first;
    mixHash(derived.second);
    m_cipher = CipherState(truncated(derived.third));
    crypto_wipe(&derived, sizeof(derived));
}

Status NoiseHandshake::encryptAndHash(std::span<const std::uint8_t> plaintext,
                                      std::vector<std::uint8_t>& out) {
    const std::size_t at = out.size();
    const HandshakeHash ad = m_h;
    if (auto encrypted = m_cipher.encrypt(ad, plaintext, out); !encrypted) {
        return encrypted;
    }
    mixHash({out.data() + at, out.size() - at});
    return ok();
}

Status NoiseHandshake::decryptAndHash(std::span<const std::uint8_t> ciphertext,
                                      std::vector<std::uint8_t>& out) {
    const HandshakeHash ad = m_h;
    if (auto decrypted = m_cipher.decrypt(ad, ciphertext, out); !decrypted) {
        return decrypted;
    }
    mixHash(ciphertext);
    return ok();
}

Result<std::vector<std::uint8_t>>
NoiseHandshake::writeMessage(std::span<const std::uint8_t> payload) {
    const bool initiatorTurn = m_message == 0;
    if (complete() || initiatorTurn != (m_role == Role::Initiator)) {
        return fail<std::vector<std::uint8_t>>(ErrorCode::ProtocolError,
                                               "not this side's turn to write");
    }

    std::vector<std::uint8_t> message;
    if (m_message == 0) {
        // -> psk, e
        mixKeyAndHash(m_psk);
        message.insert(message.end(), m_ephemeralPublic.begin(), m_ephemeralPublic.end());
        mixHash(m_ephemeralPublic);
        mixKey(m_ephemeralPublic);
    } else {
        // <- e, ee
        message.insert(message.end(), m_ephemeralPublic.begin(), m_ephemeralPublic.end());
        mixHash(m_ephemeralPublic);
        mixKey(m_ephemeralPublic);
        Key shared{};
        crypto_x25519(shared.data(), m_ephemeralSecret.data(), m_remoteEphemeral.data());
        mixKey(shared);
        crypto_wipe(shared.data(), shared.size());
    }
    if (auto encrypted = encryptAndHash(payload, message); !encrypted) {
        return std::unexpected(std::move(encrypted).error());
    }
    if (message.size() > kNoiseMaxMessageBytes) {
        return fail<std::vector<std::uint8_t>>(ErrorCode::OutOfRange,
                                               "a handshake payload too large for one message");
    }
    ++m_message;
    return message;
}

Result<std::vector<std::uint8_t>>
NoiseHandshake::readMessage(std::span<const std::uint8_t> message) {
    const bool initiatorTurn = m_message == 0;
    if (complete() || initiatorTurn == (m_role == Role::Initiator)) {
        return fail<std::vector<std::uint8_t>>(ErrorCode::ProtocolError,
                                               "not this side's turn to read");
    }
    if (message.size() < kDhBytes + kNoiseTagBytes) {
        return fail<std::vector<std::uint8_t>>(ErrorCode::ProtocolError,
                                               "a handshake message too short to be one");
    }

    if (m_message == 0) {
        mixKeyAndHash(m_psk);
    }
    std::copy_n(message.begin(), kDhBytes, m_remoteEphemeral.begin());
    mixHash(m_remoteEphemeral);
    mixKey(m_remoteEphemeral);
    if (m_message == 1) {
        Key shared{};
        crypto_x25519(shared.data(), m_ephemeralSecret.data(), m_remoteEphemeral.data());
        mixKey(shared);
        crypto_wipe(shared.data(), shared.size());
    }

    std::vector<std::uint8_t> payload;
    if (auto decrypted = decryptAndHash(message.subspan(kDhBytes), payload); !decrypted) {
        return fail<std::vector<std::uint8_t>>(ErrorCode::PermissionDenied,
                                               "the handshake did not authenticate");
    }
    ++m_message;
    return payload;
}

std::pair<CipherState, CipherState> NoiseHandshake::split() {
    Derived derived = hkdf(m_ck, {}, 2);
    std::pair<CipherState, CipherState> ciphers{CipherState(truncated(derived.first)),
                                                CipherState(truncated(derived.second))};
    crypto_wipe(&derived, sizeof(derived));
    return ciphers;
}

} // namespace sweeppp::crypto
