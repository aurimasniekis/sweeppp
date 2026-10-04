// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <sweeppp/crypto/Noise.hpp>
#include <sweeppp/crypto/Sha256.hpp>
#include <utility>
#include <vector>

using namespace sweeppp::crypto;

namespace {

std::string hexOf(const Sha256Digest& digest) {
    return toHex(digest);
}

std::vector<std::uint8_t> repeated(std::uint8_t byte, std::size_t count) {
    return std::vector<std::uint8_t>(count, byte);
}

} // namespace

TEST_CASE("SHA-256 matches the FIPS 180-4 examples") {
    CHECK(hexOf(Sha256::of("")) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hexOf(Sha256::of("abc")) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 56 bytes: the length no longer fits in the block, so padding takes two.
    CHECK(hexOf(Sha256::of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(hexOf(Sha256::of(repeated('a', 1'000'000))) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("SHA-256 gives the same digest however the input is split") {
    std::vector<std::uint8_t> message(200);
    for (std::size_t i = 0; i < message.size(); ++i) {
        message[i] = static_cast<std::uint8_t>((i * 31) + 7);
    }
    const Sha256Digest whole = Sha256::of(message);

    for (std::size_t split = 0; split <= message.size(); ++split) {
        Sha256 hash;
        hash.update(std::span<const std::uint8_t>(message).first(split));
        hash.update(std::span<const std::uint8_t>(message).subspan(split));
        CHECK(hash.finish() == whole);
    }

    Sha256 bytewise;
    for (const std::uint8_t byte : message) {
        bytewise.update(std::span<const std::uint8_t>(&byte, 1));
    }
    CHECK(bytewise.finish() == whole);

    Sha256 reused;
    reused.update("something else");
    (void)reused.finish();
    reused.reset();
    reused.update(message);
    CHECK(reused.finish() == whole);
}

TEST_CASE("HMAC-SHA256 matches the RFC 4231 test cases") {
    SUBCASE("1: a short key") {
        CHECK(hexOf(hmacSha256(repeated(0x0b, 20), bytesOf("Hi There"))) ==
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    }
    SUBCASE("2: a key shorter than the output") {
        CHECK(hexOf(hmacSha256(bytesOf("Jefe"), bytesOf("what do ya want for nothing?"))) ==
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    }
    SUBCASE("3: key and data of repeated bytes") {
        CHECK(hexOf(hmacSha256(repeated(0xaa, 20), repeated(0xdd, 50))) ==
              "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
    }
    SUBCASE("6: a key longer than the block is hashed first") {
        CHECK(
            hexOf(hmacSha256(repeated(0xaa, 131),
                             bytesOf("Test Using Larger Than Block-Size Key - Hash Key First"))) ==
            "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    }
    SUBCASE("7: a long key and long data") {
        CHECK(hexOf(hmacSha256(
                  repeated(0xaa, 131),
                  bytesOf("This is a test using a larger than block-size key and a larger than "
                          "block-size data. The key needs to be hashed before being used by the "
                          "HMAC algorithm."))) ==
              "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
    }
}

TEST_CASE("digests compare in constant time, and only at equal length") {
    const Sha256Digest a = Sha256::of("a");
    Sha256Digest b = a;
    CHECK(constantTimeEqual(a, b));
    b[31] ^= 0x01;
    CHECK_FALSE(constantTimeEqual(a, b));
    CHECK_FALSE(constantTimeEqual(std::span<const std::uint8_t>(a).first(16), a));
    CHECK(constantTimeEqual({}, {}));
}

TEST_CASE("the system generator fills what it is given") {
    std::array<std::uint8_t, 32> first{};
    std::array<std::uint8_t, 32> second{};
    REQUIRE(fillRandom(first).has_value());
    REQUIRE(fillRandom(second).has_value());
    // 2^-256 of a false failure.
    CHECK(first != second);
    CHECK(fillRandom({}).has_value());
}

namespace {

std::vector<std::uint8_t> fromHex(std::string_view hex) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(
            static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    }
    return out;
}

Key keyFromHex(std::string_view hex) {
    const std::vector<std::uint8_t> bytes = fromHex(hex);
    Key key{};
    std::copy(bytes.begin(), bytes.end(), key.begin());
    return key;
}

} // namespace

TEST_CASE("the Noise handshake matches the published NNpsk0 BLAKE2b vector") {
    // cacophony's vector for Noise_NNpsk0_25519_ChaChaPoly_BLAKE2b.
    const std::vector<std::uint8_t> prologue = fromHex("4a6f686e2047616c74");
    const Key psk = keyFromHex("54686973206973206d7920417573747269616e20706572737065637469766521");
    NoiseHandshake initiator(
        NoiseHandshake::Role::Initiator, psk, prologue,
        keyFromHex("893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a"));
    NoiseHandshake responder(
        NoiseHandshake::Role::Responder, psk, prologue,
        keyFromHex("bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b"));

    const std::vector<std::pair<std::string_view, std::string_view>> messages{
        {"4c756477696720766f6e204d69736573",
         "ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c7944ed63df2f5a12aee1185ee9c50"
         "305f2ecf12421dcb53c047a63b784cf7c54a105"},
        {"4d757272617920526f746862617264",
         "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f1448088430e0fe7d5c0c9b92f7478716c8"
         "852f1b4f389edb75e3ebe546fafafd5b6f0f7"},
        {"462e20412e20486179656b", "0fe04b26dcfbb69f8a6a94c61f1a26ef88769218d32f5d17c068a6"},
        {"4361726c204d656e676572", "2239e0c540c01f09eb1f4cc5258fd5acfefff18a5a3773f21b7bfe"},
        {"4a65616e2d426170746973746520536179",
         "f216cb03d30fb8af1e561fe02a07552d09b416e27a75e62c77d193718b0bdaae9f"},
        {"457567656e2042f6686d20766f6e2042617765726b",
         "6e8baf98b944184dd7dc5ef14d8108cbeca626de118a5460f3308e294d61e17238bce29bd9"},
    };

    auto first = initiator.writeMessage(fromHex(messages[0].first));
    REQUIRE(first.has_value());
    CHECK(toHex(*first) == messages[0].second);
    auto firstPayload = responder.readMessage(*first);
    REQUIRE(firstPayload.has_value());
    CHECK(toHex(*firstPayload) == messages[0].first);

    auto second = responder.writeMessage(fromHex(messages[1].first));
    REQUIRE(second.has_value());
    CHECK(toHex(*second) == messages[1].second);
    auto secondPayload = initiator.readMessage(*second);
    REQUIRE(secondPayload.has_value());
    CHECK(toHex(*secondPayload) == messages[1].first);

    REQUIRE(initiator.complete());
    REQUIRE(responder.complete());
    CHECK(toHex(initiator.handshakeHash()) ==
          "ed5e9692d0ab507b6c2beec3f584fd5b127817a9d20b26cd50aa72c507260fa31aa7d88dd3723316338af37"
          "ce0b4cfb2923aeb848bbf2b934911306f01ffc963");
    CHECK(initiator.handshakeHash() == responder.handshakeHash());

    auto [initiatorSend, initiatorReceive] = initiator.split();
    auto [responderReceive, responderSend] = responder.split();
    for (std::size_t i = 2; i < messages.size(); ++i) {
        const bool fromInitiator = i % 2 == 0;
        CipherState& sender = fromInitiator ? initiatorSend : responderSend;
        CipherState& receiver = fromInitiator ? responderReceive : initiatorReceive;
        std::vector<std::uint8_t> ciphertext;
        REQUIRE(sender.encrypt({}, fromHex(messages[i].first), ciphertext).has_value());
        CHECK(toHex(ciphertext) == messages[i].second);
        std::vector<std::uint8_t> plaintext;
        REQUIRE(receiver.decrypt({}, ciphertext, plaintext).has_value());
        CHECK(toHex(plaintext) == messages[i].first);
    }
}

TEST_CASE("a Noise channel refuses a wrong key, a tampered byte and a replay") {
    const std::vector<std::uint8_t> prologue{'p'};

    SUBCASE("different keys fail the first message") {
        NoiseHandshake initiator(NoiseHandshake::Role::Initiator, pskFromToken("right"), prologue);
        NoiseHandshake responder(NoiseHandshake::Role::Responder, pskFromToken("wrong"), prologue);
        auto first = initiator.writeMessage(bytesOf("hello"));
        REQUIRE(first.has_value());
        auto read = responder.readMessage(*first);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().code() == sweeppp::ErrorCode::PermissionDenied);
    }

    SUBCASE("established, then attacked") {
        NoiseHandshake initiator(NoiseHandshake::Role::Initiator, pskFromToken("t"), prologue);
        NoiseHandshake responder(NoiseHandshake::Role::Responder, pskFromToken("t"), prologue);
        REQUIRE(responder.readMessage(*initiator.writeMessage({})).has_value());
        REQUIRE(initiator.readMessage(*responder.writeMessage({})).has_value());
        auto [send, unusedReceive] = initiator.split();
        auto [receive, unusedSend] = responder.split();

        std::vector<std::uint8_t> one;
        std::vector<std::uint8_t> two;
        REQUIRE(send.encrypt({}, bytesOf("first"), one).has_value());
        REQUIRE(send.encrypt({}, bytesOf("second"), two).has_value());

        std::vector<std::uint8_t> tampered = one;
        tampered[2] ^= 0x01;
        std::vector<std::uint8_t> out;
        CHECK_FALSE(receive.decrypt({}, tampered, out).has_value());
        CHECK(out.empty());

        // Out of order: the second under the first's nonce.
        CHECK_FALSE(receive.decrypt({}, two, out).has_value());

        REQUIRE(receive.decrypt({}, one, out).has_value());
        CHECK(std::string(out.begin(), out.end()) == "first");
        // Replayed.
        out.clear();
        CHECK_FALSE(receive.decrypt({}, one, out).has_value());
        REQUIRE(receive.decrypt({}, two, out).has_value());
        CHECK(std::string(out.begin(), out.end()) == "second");
    }

    SUBCASE("turns are kept") {
        NoiseHandshake responder(NoiseHandshake::Role::Responder, pskFromToken("t"), prologue);
        CHECK_FALSE(responder.writeMessage({}).has_value());
        CHECK_FALSE(responder.readMessage(std::vector<std::uint8_t>(10)).has_value());
    }
}
