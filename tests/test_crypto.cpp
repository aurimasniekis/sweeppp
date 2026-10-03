// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <sweeppp/crypto/Sha256.hpp>
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
