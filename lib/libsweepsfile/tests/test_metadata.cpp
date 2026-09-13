// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <limits>
#include <string>
#include <sweeps/FileFormat.hpp>
#include <sweeps/Metadata.hpp>
#include <vector>

using namespace sweeps;

namespace {

[[nodiscard]] std::vector<std::byte> encoded(const Metadata& metadata) {
    std::vector<std::byte> out;
    metadata.encode(out);
    return out;
}

[[nodiscard]] Result<Metadata> roundTrip(const Metadata& metadata) {
    const std::vector<std::byte> bytes = encoded(metadata);
    ByteReader reader(bytes.data(), bytes.size());
    return Metadata::decode(reader);
}

[[nodiscard]] std::vector<std::byte> bytesOf(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (const unsigned value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

} // namespace

TEST_CASE("every value type round-trips through the encoding") {
    Metadata metadata;
    metadata.setString("name", "Bob's rooftop");
    metadata.setInt("count", -42);
    metadata.setFloat("db_per_step", 0.5);
    metadata.setBool("calibrated", true);
    metadata.setBytes("blob", bytesOf({0x00, 0xDE, 0xAD, 0xFF}));

    auto decoded = roundTrip(metadata);
    REQUIRE(decoded.has_value());

    CHECK(decoded->getString("name") == "Bob's rooftop");
    CHECK(decoded->getInt("count") == -42);
    CHECK(decoded->getFloat("db_per_step") == doctest::Approx(0.5));
    CHECK(decoded->getBool("calibrated"));
    REQUIRE(decoded->find("blob") != nullptr);
    REQUIRE(decoded->find("blob")->asBytes() != nullptr);
    CHECK(*decoded->find("blob")->asBytes() == bytesOf({0x00, 0xDE, 0xAD, 0xFF}));
}

TEST_CASE("an i64 travels as two's complement rather than as a magnitude") {
    // The trap for an independent reader: `i64` is not `u64`, and guessing
    // sign-magnitude turns the most negative value into something else entirely.
    Metadata metadata;
    metadata.setInt("min", std::numeric_limits<std::int64_t>::min());
    metadata.setInt("max", std::numeric_limits<std::int64_t>::max());
    metadata.setInt("minus_one", -1);

    auto decoded = roundTrip(metadata);
    REQUIRE(decoded.has_value());
    CHECK(decoded->getInt("min") == std::numeric_limits<std::int64_t>::min());
    CHECK(decoded->getInt("max") == std::numeric_limits<std::int64_t>::max());
    CHECK(decoded->getInt("minus_one") == -1);

    // Appendix A.2's i64 vectors, which is where an independent implementer
    // guesses wrong -- and guesses silently, until a negative value appears.
    const auto bodyOf = [](std::int64_t value) {
        Metadata one;
        one.setInt("k", value);
        const std::vector<std::byte> bytes = encoded(one);
        REQUIRE(bytes.size() == 4 + 4 + 1 + 1 + 8);
        return std::vector<std::byte>(bytes.end() - 8, bytes.end());
    };

    CHECK(bodyOf(-1) == bytesOf({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    CHECK(bodyOf(std::numeric_limits<std::int64_t>::min()) ==
          bytesOf({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80}));
    CHECK(bodyOf(std::numeric_limits<std::int64_t>::max()) ==
          bytesOf({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F}));
}

TEST_CASE("hashes nest and arrays are homogeneous") {
    Metadata inner;
    inner.setString("id", "org.sweeppp.bandplan");
    inner.setInt("matches", 3);

    Metadata deeper;
    deeper.setHash("plugin", inner);

    std::vector<Value> rows;
    rows.push_back(Value::ofHash(inner));
    rows.push_back(Value::ofHash(Metadata{}));

    Metadata metadata;
    metadata.setHash("nested", deeper);
    metadata.setArray("rows", rows);
    metadata.setArray("levels", {Value::ofFloat(-90.5), Value::ofFloat(-12.0)});

    auto decoded = roundTrip(metadata);
    REQUIRE(decoded.has_value());

    const Value* nested = decoded->find("nested");
    REQUIRE(nested != nullptr);
    REQUIRE(nested->asHash() != nullptr);
    const Metadata* plugin = nested->asHash()->find("plugin")->asHash();
    REQUIRE(plugin != nullptr);
    CHECK(plugin->getString("id") == "org.sweeppp.bandplan");
    CHECK(plugin->getInt("matches") == 3);

    const Value* array = decoded->find("rows");
    REQUIRE(array != nullptr);
    REQUIRE(array->asArray() != nullptr);
    REQUIRE(array->asArray()->size() == 2);
    CHECK(array->elementType() == Value::Type::Hash);
    CHECK((*array->asArray())[0].asHash()->getInt("matches") == 3);
    CHECK((*array->asArray())[1].asHash()->empty());

    const Value* levels = decoded->find("levels");
    REQUIRE(levels != nullptr);
    REQUIRE(levels->asArray() != nullptr);
    CHECK(levels->elementType() == Value::Type::Float);
    CHECK((*levels->asArray())[1].asFloat() == doctest::Approx(-12.0));
}

TEST_CASE("an array's element type is stated once, so a stray element is normalised") {
    // The element type appears once on the wire and the elements carry no tag,
    // so an element of another type could not be written back. Normalising it on
    // construction keeps what is in memory equal to what will be encoded.
    Metadata metadata;
    metadata.setArray("mixed", {Value::ofInt(7), Value::ofString("not an int")});

    const Value* array = metadata.find("mixed");
    REQUIRE(array != nullptr);
    CHECK(array->elementType() == Value::Type::Int);
    REQUIRE(array->asArray()->size() == 2);
    CHECK((*array->asArray())[1].asInt() == 0);

    auto decoded = roundTrip(metadata);
    REQUIRE(decoded.has_value());
    CHECK((*decoded->find("mixed")->asArray())[1].asInt() == 0);
}

TEST_CASE("keys are emitted in ascending byte order whatever order they arrive in") {
    // What keeps the encoding a function of the content alone, and therefore
    // what makes a byte-frozen golden file possible.
    Metadata forwards;
    forwards.setString("a", "1");
    forwards.setString("b", "2");
    forwards.setString("c", "3");

    Metadata backwards;
    backwards.setString("c", "3");
    backwards.setString("b", "2");
    backwards.setString("a", "1");

    CHECK(encoded(forwards) == encoded(backwards));

    // And the first key in the stream really is the smallest.
    const std::vector<std::byte> bytes = encoded(forwards);
    REQUIRE(bytes.size() > 9);
    CHECK(static_cast<char>(bytes[8]) == 'a');
}

TEST_CASE("setting a key twice replaces it rather than accumulating") {
    Metadata metadata;
    metadata.setString("name", "first");
    metadata.setInt("name", 2);

    CHECK(metadata.size() == 1);
    CHECK(metadata.getInt("name") == 2);
    CHECK(metadata.getString("name", "fallback") == "fallback");
}

TEST_CASE("absent and empty are different") {
    // find() returning null is the only "absent": a zero-length Bytes, an empty
    // string and an empty hash are all present values.
    Metadata metadata;
    metadata.setBytes("empty", {});
    metadata.setString("blank", "");

    CHECK(metadata.contains("empty"));
    REQUIRE(metadata.find("empty") != nullptr);
    REQUIRE(metadata.find("empty")->asBytes() != nullptr);
    CHECK(metadata.find("empty")->asBytes()->empty());

    CHECK(metadata.find("missing") == nullptr);
    CHECK_FALSE(metadata.contains("missing"));
    CHECK(metadata.getString("missing", "fallback") == "fallback");

    auto decoded = roundTrip(metadata);
    REQUIRE(decoded.has_value());
    CHECK(decoded->find("empty") != nullptr);
    CHECK(decoded->find("missing") == nullptr);
}

TEST_CASE("an accessor of the wrong type yields the fallback, never a conversion") {
    Metadata metadata;
    metadata.setString("text", "12");

    CHECK(metadata.getInt("text", -1) == -1);
    CHECK(metadata.getFloat("text", -1.0) == doctest::Approx(-1.0));
    CHECK(metadata.getBool("text", true));
    CHECK(metadata.find("text")->asBytes() == nullptr);
    CHECK(metadata.find("text")->asHash() == nullptr);
    CHECK(metadata.find("text")->asArray() == nullptr);
}

TEST_CASE("a depth bomb is rejected rather than crashing the reader") {
    // A hash nested inside a hash half a million deep fits in about sixty bytes,
    // because an empty hash is four. Without the depth cap this is a stack
    // overflow triggered by a file, which is the class of failure §12 exists to
    // prevent.
    constexpr std::uint32_t kDepth = 100'000;

    std::vector<std::byte> bytes;
    for (std::uint32_t i = 0; i < kDepth; ++i) {
        writeU32(bytes, 1);     // one entry
        writeString(bytes, ""); // with an empty key
        writeU8(bytes, static_cast<std::uint8_t>(Value::Type::Hash));
    }
    writeU32(bytes, 0); // and finally an empty hash at the bottom

    ByteReader reader(bytes.data(), bytes.size());
    const auto decoded = Metadata::decode(reader);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code() == ErrorCode::Corrupt);
    CHECK(decoded.error().message().find("nested deeper") != std::string::npos);
}

TEST_CASE("nesting up to the cap still decodes") {
    // The cap has to refuse the bomb without refusing anything a writer would
    // plausibly produce.
    constexpr unsigned kDepth = Metadata::kMaxDepth - 1;

    std::vector<std::byte> bytes;
    for (unsigned i = 0; i < kDepth; ++i) {
        writeU32(bytes, 1);
        writeString(bytes, "down");
        writeU8(bytes, static_cast<std::uint8_t>(Value::Type::Hash));
    }
    writeU32(bytes, 1);
    writeString(bytes, "leaf");
    writeU8(bytes, static_cast<std::uint8_t>(Value::Type::String));
    writeString(bytes, "bottom");

    ByteReader reader(bytes.data(), bytes.size());
    auto decoded = Metadata::decode(reader);
    REQUIRE(decoded.has_value());

    const Metadata* level = &*decoded;
    for (unsigned i = 0; i < kDepth; ++i) {
        REQUIRE(level->find("down") != nullptr);
        level = level->find("down")->asHash();
        REQUIRE(level != nullptr);
    }
    CHECK(level->getString("leaf") == "bottom");
}

TEST_CASE("a count larger than the payload is rejected rather than reserved") {
    // Four billion entries in a four-byte payload. A reader that reserves first
    // and validates later is one allocation away from being killed by a file.
    std::vector<std::byte> bytes;
    writeU32(bytes, 0xFFFF'FFFFU);

    ByteReader reader(bytes.data(), bytes.size());
    const auto decoded = Metadata::decode(reader);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code() == ErrorCode::Corrupt);

    // The same for an array's count.
    std::vector<std::byte> array;
    writeU32(array, 1);
    writeString(array, "k");
    writeU8(array, static_cast<std::uint8_t>(Value::Type::Array));
    writeU8(array, static_cast<std::uint8_t>(Value::Type::Float));
    writeU32(array, 0xFFFF'FFFFU);

    ByteReader arrayReader(array.data(), array.size());
    const auto arrayDecoded = Metadata::decode(arrayReader);
    REQUIRE_FALSE(arrayDecoded.has_value());
    CHECK(arrayDecoded.error().code() == ErrorCode::Corrupt);
}

TEST_CASE("the count check does not refuse a payload that genuinely fits") {
    // The bound has to be the true minimum entry size. A conservative
    // over-estimate would reject this: a hundred booleans encode to six bytes
    // each, and any per-entry figure above six refuses a legitimate file.
    Metadata metadata;
    for (int i = 0; i < 100; ++i) {
        metadata.setBool(std::string(1, static_cast<char>('!' + i)), i % 2 == 0);
    }

    auto decoded = roundTrip(metadata);
    REQUIRE(decoded.has_value());
    CHECK(decoded->size() == 100);
    CHECK(decoded->getBool("!"));
    CHECK_FALSE(decoded->getBool("\""));
}

TEST_CASE("an undefined type tag is refused") {
    std::vector<std::byte> bytes;
    writeU32(bytes, 1);
    writeString(bytes, "k");
    writeU8(bytes, 99);
    writeU32(bytes, 0);

    ByteReader reader(bytes.data(), bytes.size());
    const auto decoded = Metadata::decode(reader);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().message().find("not defined") != std::string::npos);
}

TEST_CASE("a bool reads as true for any non-zero byte") {
    std::vector<std::byte> bytes;
    writeU32(bytes, 1);
    writeString(bytes, "flag");
    writeU8(bytes, static_cast<std::uint8_t>(Value::Type::Bool));
    writeU8(bytes, 0x7F);

    ByteReader reader(bytes.data(), bytes.size());
    auto decoded = Metadata::decode(reader);
    REQUIRE(decoded.has_value());
    CHECK(decoded->getBool("flag"));
}

TEST_CASE("JSON rendering escapes what it must and hexes what it cannot") {
    Metadata inner;
    inner.setInt("depth", 2);

    Metadata metadata;
    metadata.setString("name", "quote\" back\\ tab\t newline\n");
    metadata.setString("control", std::string("nul\0here", 8));
    metadata.setBytes("blob", bytesOf({0x00, 0x0F, 0xDE, 0xAD}));
    metadata.setBool("on", true);
    metadata.setInt("count", -7);
    metadata.setFloat("half", 0.5);
    metadata.setHash("inner", inner);
    metadata.setArray("list", {Value::ofInt(1), Value::ofInt(2)});

    const std::string json = metadata.toJson(0);

    CHECK(json.find("\"name\":\"quote\\\" back\\\\ tab\\t newline\\n\"") != std::string::npos);
    CHECK(json.find("\"control\":\"nul\\u0000here\"") != std::string::npos);
    CHECK(json.find("\"blob\":\"000fdead\"") != std::string::npos);
    CHECK(json.find("\"on\":true") != std::string::npos);
    CHECK(json.find("\"count\":-7") != std::string::npos);
    CHECK(json.find("\"half\":0.5") != std::string::npos);
    CHECK(json.find("\"inner\":{\"depth\":2}") != std::string::npos);
    CHECK(json.find("\"list\":[1,2]") != std::string::npos);

    // Compact means no newlines at all, which is what one-object-per-line output
    // depends on.
    CHECK(json.find('\n') == std::string::npos);

    // Keys come out sorted, so the rendering is as deterministic as the encoding.
    CHECK(json.rfind("{\"blob\"", 0) == 0);
}

TEST_CASE("JSON floats keep enough digits to round-trip and never read as words") {
    Metadata metadata;
    metadata.setFloat("tenth", 0.1);
    metadata.setFloat("big", 90e6);
    metadata.setFloat("infinite", std::numeric_limits<double>::infinity());

    const std::string json = metadata.toJson(0);
    CHECK(json.find("\"tenth\":0.10000000000000001") != std::string::npos);
    CHECK(json.find("\"big\":90000000") != std::string::npos);
    // JSON has no infinity. `null` is the only thing a parser will accept.
    CHECK(json.find("\"infinite\":null") != std::string::npos);
}

TEST_CASE("indented JSON nests and an empty object is still an object") {
    Metadata metadata;
    metadata.setString("a", "1");
    metadata.setHash("b", Metadata{});

    const std::string json = metadata.toJson(2);
    CHECK(json == "{\n  \"a\": \"1\",\n  \"b\": {}\n}");
    CHECK(Metadata{}.toJson(2) == "{}");
}

TEST_CASE("a copied value deep-copies its nested hash") {
    Metadata inner;
    inner.setString("who", "original");

    Value value = Value::ofHash(inner);
    Value copy = value;

    inner.setString("who", "changed");
    REQUIRE(copy.asHash() != nullptr);
    CHECK(copy.asHash()->getString("who") == "original");
    CHECK(value.asHash() != copy.asHash());

    // And assignment, which is the other half of the rule of five here.
    Value assigned;
    assigned = copy;
    REQUIRE(assigned.asHash() != nullptr);
    CHECK(assigned.asHash()->getString("who") == "original");
    CHECK(assigned.asHash() != copy.asHash());
}

TEST_CASE("the encoding matches the specification's metadata test vectors") {
    // Appendix A.6. An independent implementation is checked against these, so
    // they are part of the contract rather than an illustration.
    const auto hex = [](const std::vector<std::byte>& buffer) {
        std::string out;
        for (const std::byte value : buffer) {
            static const char* kDigits = "0123456789ABCDEF";
            const auto byte = static_cast<unsigned char>(value);
            out.push_back(kDigits[byte >> 4U]);
            out.push_back(kDigits[byte & 0x0FU]);
        }
        return out;
    };

    Metadata scalars;
    scalars.setString("str", "hi");
    scalars.setInt("neg", -1);
    scalars.setBool("flg", true);
    scalars.setBytes("raw", bytesOf({0xDE, 0xAD}));

    CHECK(hex(encoded(scalars)) == "04000000"
                                   "03000000"
                                   "666C67"
                                   "04"
                                   "01"
                                   "03000000"
                                   "6E6567"
                                   "02"
                                   "FFFFFFFFFFFFFFFF"
                                   "03000000"
                                   "726177"
                                   "05"
                                   "02000000"
                                   "DEAD"
                                   "03000000"
                                   "737472"
                                   "01"
                                   "02000000"
                                   "6869");

    std::vector<std::byte> emptyFloats;
    Value::ofArray(Value::Type::Float, {}).encode(emptyFloats);
    CHECK(hex(emptyFloats) == "07"
                              "03"
                              "00000000");

    Metadata first;
    first.setString("a", "");

    std::vector<std::byte> hashes;
    Value::ofArray(Value::Type::Hash, {Value::ofHash(first), Value::ofHash(Metadata{})})
        .encode(hashes);
    CHECK(hex(hashes) == "07"
                         "06"
                         "02000000"
                         "01000000"
                         "01000000"
                         "61"
                         "01"
                         "00000000"
                         "00000000");
}

TEST_CASE("an empty metadata object encodes to four zero bytes") {
    const Metadata metadata;
    CHECK(encoded(metadata) == bytesOf({0x00, 0x00, 0x00, 0x00}));

    auto decoded = roundTrip(metadata);
    REQUIRE(decoded.has_value());
    CHECK(decoded->empty());
}
