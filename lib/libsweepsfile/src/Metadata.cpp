// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include "sweeps/Metadata.hpp"

#include "sweeps/FileFormat.hpp"

#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

namespace sweeps {

using detail::fail;

namespace {

/// The smallest a body of each type can encode to. Used to reject a count
/// before it becomes a `reserve()`.
[[nodiscard]] std::size_t minimumBodyBytes(Value::Type type) noexcept {
    switch (type) {
    case Value::Type::String:
        return 4; // u32 length, zero bytes of text
    case Value::Type::Int:
        return 8;
    case Value::Type::Float:
        return 8;
    case Value::Type::Bool:
        return 1;
    case Value::Type::Bytes:
        return 4; // u32 length, zero bytes
    case Value::Type::Hash:
        return 4; // u32 count of zero
    case Value::Type::Array:
        return 5; // u8 element type, u32 count of zero
    }
    return 1;
}

/// The smallest one hash entry can encode to: a `u32` key length of zero, no key
/// bytes, a one-byte type tag and the smallest body any type has, which is the
/// single byte of a `Bool`.
///
/// It is deliberately the *true* minimum. A larger, "safely conservative" figure
/// would reject a legitimate file -- a hash of a hundred booleans encodes to six
/// hundred bytes, and any per-entry estimate above six would refuse it.
constexpr std::size_t kMinEntryBytes = 6;

[[nodiscard]] bool isKnownType(std::uint8_t tag) noexcept {
    return tag >= static_cast<std::uint8_t>(Value::Type::String) &&
           tag <= static_cast<std::uint8_t>(Value::Type::Array);
}

// ------------------------------------------------------------------ JSON

void appendJsonString(std::string& out, std::string_view value) {
    out.push_back('"');
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (byte) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20) {
                static const char* kHex = "0123456789ABCDEF";
                out += "\\u00";
                out.push_back(kHex[byte >> 4U]);
                out.push_back(kHex[byte & 0x0FU]);
            } else {
                // Bytes at or above 0x80 pass through unchanged. A `str` is
                // UTF-8 by definition (§2.2), so valid input yields valid JSON;
                // input that lied about being UTF-8 yields output that says so.
                out.push_back(character);
            }
            break;
        }
    }
    out.push_back('"');
}

void appendJsonNumber(std::string& out, double value) {
    if (!std::isfinite(value)) {
        // JSON has no infinity and no NaN. `null` is the only honest rendering,
        // and no key this format defines can take those values anyway.
        out += "null";
        return;
    }

    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    out += stream.str();
}

void appendJsonHex(std::string& out, const std::vector<std::byte>& bytes) {
    static const char* kHex = "0123456789abcdef";
    out.push_back('"');
    for (const std::byte value : bytes) {
        const auto byte = static_cast<unsigned char>(value);
        out.push_back(kHex[byte >> 4U]);
        out.push_back(kHex[byte & 0x0FU]);
    }
    out.push_back('"');
}

void appendNewline(std::string& out, int indent, int depth) {
    if (indent <= 0) {
        return;
    }
    out.push_back('\n');
    out.append(static_cast<std::size_t>(indent) * static_cast<std::size_t>(depth), ' ');
}

void appendJsonValue(std::string& out, const Value& value, int indent, int depth);

void appendJsonHash(std::string& out, const Metadata& hash, int indent, int depth) {
    if (hash.empty()) {
        out += "{}";
        return;
    }

    out.push_back('{');
    bool first = true;
    for (const auto& entry : hash) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        appendNewline(out, indent, depth + 1);
        appendJsonString(out, entry.first);
        out.push_back(':');
        if (indent > 0) {
            out.push_back(' ');
        }
        appendJsonValue(out, entry.second, indent, depth + 1);
    }
    appendNewline(out, indent, depth);
    out.push_back('}');
}

void appendJsonValue(std::string& out, const Value& value, int indent, int depth) {
    switch (value.type()) {
    case Value::Type::String:
        appendJsonString(out, value.asString());
        break;
    case Value::Type::Int:
        out += std::to_string(value.asInt());
        break;
    case Value::Type::Float:
        appendJsonNumber(out, value.asFloat());
        break;
    case Value::Type::Bool:
        out += value.asBool() ? "true" : "false";
        break;
    case Value::Type::Bytes:
        if (const std::vector<std::byte>* bytes = value.asBytes()) {
            appendJsonHex(out, *bytes);
        }
        break;
    case Value::Type::Hash:
        if (const Metadata* hash = value.asHash()) {
            appendJsonHash(out, *hash, indent, depth);
        }
        break;
    case Value::Type::Array: {
        const std::vector<Value>* elements = value.asArray();
        if (elements == nullptr || elements->empty()) {
            out += "[]";
            break;
        }
        out.push_back('[');
        bool first = true;
        for (const Value& element : *elements) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            appendNewline(out, indent, depth + 1);
            appendJsonValue(out, element, indent, depth + 1);
        }
        appendNewline(out, indent, depth);
        out.push_back(']');
        break;
    }
    }
}

} // namespace

// --------------------------------------------------------------------- Value

Value::Value() = default;
Value::~Value() = default;
Value::Value(Value&&) noexcept = default;
Value& Value::operator=(Value&&) noexcept = default;

Value::Value(const Value& other)
    : m_type(other.m_type), m_elementType(other.m_elementType), m_string(other.m_string),
      m_int(other.m_int), m_float(other.m_float), m_bool(other.m_bool), m_bytes(other.m_bytes),
      m_hash(other.m_hash ? std::unique_ptr<Metadata>(new Metadata(*other.m_hash)) : nullptr),
      m_array(other.m_array) {
}

Value& Value::operator=(const Value& other) {
    if (this != &other) {
        Value copy(other);
        *this = std::move(copy);
    }
    return *this;
}

Value Value::ofString(std::string value) {
    Value result;
    result.m_type = Type::String;
    result.m_string = std::move(value);
    return result;
}

Value Value::ofInt(std::int64_t value) {
    Value result;
    result.m_type = Type::Int;
    result.m_int = value;
    return result;
}

Value Value::ofFloat(double value) {
    Value result;
    result.m_type = Type::Float;
    result.m_float = value;
    return result;
}

Value Value::ofBool(bool value) {
    Value result;
    result.m_type = Type::Bool;
    result.m_bool = value;
    return result;
}

Value Value::ofBytes(std::vector<std::byte> value) {
    Value result;
    result.m_type = Type::Bytes;
    result.m_bytes = std::move(value);
    return result;
}

Value Value::ofHash(Metadata value) {
    Value result;
    result.m_type = Type::Hash;
    result.m_hash = std::unique_ptr<Metadata>(new Metadata(std::move(value)));
    return result;
}

Value Value::ofArray(std::vector<Value> elements) {
    const Type elementType = elements.empty() ? Type::String : elements.front().type();
    return ofArray(elementType, std::move(elements));
}

Value Value::ofArray(Type elementType, std::vector<Value> elements) {
    Value result;
    result.m_type = Type::Array;
    result.m_elementType = elementType;
    result.m_array = std::move(elements);

    for (Value& element : result.m_array) {
        if (element.type() != elementType) {
            element = defaultOf(elementType);
        }
    }
    return result;
}

Value Value::defaultOf(Type type) {
    switch (type) {
    case Type::String:
        return ofString({});
    case Type::Int:
        return ofInt(0);
    case Type::Float:
        return ofFloat(0.0);
    case Type::Bool:
        return ofBool(false);
    case Type::Bytes:
        return ofBytes({});
    case Type::Hash:
        return ofHash(Metadata{});
    case Type::Array:
        return ofArray(Type::String, {});
    }
    return ofString({});
}

std::string Value::asString(std::string_view fallback) const {
    return m_type == Type::String ? m_string : std::string(fallback);
}

std::int64_t Value::asInt(std::int64_t fallback) const {
    return m_type == Type::Int ? m_int : fallback;
}

double Value::asFloat(double fallback) const {
    return m_type == Type::Float ? m_float : fallback;
}

bool Value::asBool(bool fallback) const {
    return m_type == Type::Bool ? m_bool : fallback;
}

const std::string* Value::asStringRef() const {
    return m_type == Type::String ? &m_string : nullptr;
}

const std::vector<std::byte>* Value::asBytes() const {
    return m_type == Type::Bytes ? &m_bytes : nullptr;
}

const Metadata* Value::asHash() const {
    return m_type == Type::Hash ? m_hash.get() : nullptr;
}

const std::vector<Value>* Value::asArray() const {
    return m_type == Type::Array ? &m_array : nullptr;
}

void Value::encode(std::vector<std::byte>& out) const {
    writeU8(out, static_cast<std::uint8_t>(m_type));
    encodeBody(out);
}

void Value::encodeBody(std::vector<std::byte>& out) const {
    switch (m_type) {
    case Type::String:
        writeString(out, m_string);
        break;

    case Type::Int: {
        // Two's complement, little-endian: the bit pattern of the `i64` written
        // as a `u64`. Spelled with memcpy rather than a cast so that it is the
        // object representation that travels, not a conversion whose behaviour
        // for out-of-range values is implementation-defined before C++20.
        std::uint64_t bits = 0;
        std::memcpy(&bits, &m_int, sizeof(bits));
        writeU64(out, bits);
        break;
    }

    case Type::Float:
        writeF64(out, m_float);
        break;

    case Type::Bool:
        writeU8(out, m_bool ? 1U : 0U);
        break;

    case Type::Bytes:
        writeU32(out, static_cast<std::uint32_t>(m_bytes.size()));
        writeBytes(out, m_bytes.data(), m_bytes.size());
        break;

    case Type::Hash:
        if (m_hash) {
            m_hash->encode(out);
        } else {
            writeU32(out, 0);
        }
        break;

    case Type::Array:
        // The element type is stated once; the elements carry no tag, which is
        // what `ofArray` keeps true by construction.
        writeU8(out, static_cast<std::uint8_t>(m_elementType));
        writeU32(out, static_cast<std::uint32_t>(m_array.size()));
        for (const Value& element : m_array) {
            element.encodeBody(out);
        }
        break;
    }
}

Result<Value> Value::decode(ByteReader& in, unsigned depthBudget) {
    auto tag = in.readU8();
    if (!tag) {
        return unexpected<Error>(tag.error());
    }
    if (!isKnownType(*tag)) {
        return fail<Value>(ErrorCode::Corrupt, "metadata value type {} is not defined",
                           static_cast<unsigned>(*tag));
    }
    return decodeBody(in, static_cast<Type>(*tag), depthBudget);
}

Result<Value> Value::decodeBody(ByteReader& in, Type type, unsigned depthBudget) {
    switch (type) {
    case Type::String: {
        auto text = in.readString();
        if (!text) {
            return unexpected<Error>(text.error());
        }
        return ofString(std::move(*text));
    }

    case Type::Int: {
        auto bits = in.readU64();
        if (!bits) {
            return unexpected<Error>(bits.error());
        }
        std::int64_t value = 0;
        std::memcpy(&value, &*bits, sizeof(value));
        return ofInt(value);
    }

    case Type::Float: {
        auto value = in.readF64();
        if (!value) {
            return unexpected<Error>(value.error());
        }
        return ofFloat(*value);
    }

    case Type::Bool: {
        auto value = in.readU8();
        if (!value) {
            return unexpected<Error>(value.error());
        }
        return ofBool(*value != 0);
    }

    case Type::Bytes: {
        auto length = in.readU32();
        if (!length) {
            return unexpected<Error>(length.error());
        }
        if (*length > in.remaining()) {
            return fail<Value>(ErrorCode::Corrupt,
                               "metadata bytes of length {} at offset {} exceed the {} "
                               "remaining",
                               *length, in.offset(), in.remaining());
        }
        std::vector<std::byte> bytes(*length);
        if (auto read = in.readBytes(bytes.data(), bytes.size()); !read) {
            return unexpected<Error>(read.error());
        }
        return ofBytes(std::move(bytes));
    }

    case Type::Hash: {
        if (depthBudget == 0) {
            return fail<Value>(ErrorCode::Corrupt, "metadata nested deeper than {} at offset {}",
                               Metadata::kMaxDepth, in.offset());
        }
        auto hash = Metadata::decode(in, depthBudget - 1);
        if (!hash) {
            return unexpected<Error>(hash.error());
        }
        return ofHash(std::move(*hash));
    }

    case Type::Array: {
        if (depthBudget == 0) {
            return fail<Value>(ErrorCode::Corrupt, "metadata nested deeper than {} at offset {}",
                               Metadata::kMaxDepth, in.offset());
        }

        auto tag = in.readU8();
        if (!tag) {
            return unexpected<Error>(tag.error());
        }
        if (!isKnownType(*tag)) {
            return fail<Value>(ErrorCode::Corrupt, "metadata array element type {} is not defined",
                               static_cast<unsigned>(*tag));
        }
        const auto elementType = static_cast<Type>(*tag);

        auto count = in.readU32();
        if (!count) {
            return unexpected<Error>(count.error());
        }
        // Checked before it becomes an allocation, and by division so the
        // product cannot wrap: a count of four billion in a twenty-byte payload
        // must be rejected, not attempted.
        if (*count > in.remaining() / minimumBodyBytes(elementType)) {
            return fail<Value>(ErrorCode::Corrupt,
                               "metadata array of {} elements at offset {} cannot fit in "
                               "the {} bytes remaining",
                               *count, in.offset(), in.remaining());
        }

        std::vector<Value> elements;
        elements.reserve(*count);
        for (std::uint32_t i = 0; i < *count; ++i) {
            auto element = decodeBody(in, elementType, depthBudget - 1);
            if (!element) {
                return unexpected<Error>(element.error());
            }
            elements.push_back(std::move(*element));
        }
        return ofArray(elementType, std::move(elements));
    }
    }

    return fail<Value>(ErrorCode::Corrupt, "metadata value type {} is not defined",
                       static_cast<unsigned>(type));
}

std::string_view toString(Value::Type type) noexcept {
    switch (type) {
    case Value::Type::String:
        return "string";
    case Value::Type::Int:
        return "int";
    case Value::Type::Float:
        return "float";
    case Value::Type::Bool:
        return "bool";
    case Value::Type::Bytes:
        return "bytes";
    case Value::Type::Hash:
        return "hash";
    case Value::Type::Array:
        return "array";
    }
    return "unknown";
}

// ------------------------------------------------------------------ Metadata

void Metadata::setString(std::string key, std::string value) {
    set(std::move(key), Value::ofString(std::move(value)));
}

void Metadata::setInt(std::string key, std::int64_t value) {
    set(std::move(key), Value::ofInt(value));
}

void Metadata::setFloat(std::string key, double value) {
    set(std::move(key), Value::ofFloat(value));
}

void Metadata::setBool(std::string key, bool value) {
    set(std::move(key), Value::ofBool(value));
}

void Metadata::setBytes(std::string key, std::vector<std::byte> value) {
    set(std::move(key), Value::ofBytes(std::move(value)));
}

void Metadata::setHash(std::string key, Metadata value) {
    set(std::move(key), Value::ofHash(std::move(value)));
}

void Metadata::setArray(std::string key, std::vector<Value> value) {
    set(std::move(key), Value::ofArray(std::move(value)));
}

void Metadata::set(std::string key, Value value) {
    // insert_or_assign rather than operator[], which would default-construct a
    // Value and then assign over it.
    m_entries.insert_or_assign(std::move(key), std::move(value));
}

const Value* Metadata::find(std::string_view key) const {
    const auto it = m_entries.find(key);
    return it != m_entries.end() ? &it->second : nullptr;
}

std::string Metadata::getString(std::string_view key, std::string_view fallback) const {
    const Value* value = find(key);
    return value != nullptr ? value->asString(fallback) : std::string(fallback);
}

std::int64_t Metadata::getInt(std::string_view key, std::int64_t fallback) const {
    const Value* value = find(key);
    return value != nullptr ? value->asInt(fallback) : fallback;
}

double Metadata::getFloat(std::string_view key, double fallback) const {
    const Value* value = find(key);
    return value != nullptr ? value->asFloat(fallback) : fallback;
}

bool Metadata::getBool(std::string_view key, bool fallback) const {
    const Value* value = find(key);
    return value != nullptr ? value->asBool(fallback) : fallback;
}

bool Metadata::contains(std::string_view key) const {
    return m_entries.find(key) != m_entries.end();
}

void Metadata::encode(std::vector<std::byte>& out) const {
    writeU32(out, static_cast<std::uint32_t>(m_entries.size()));
    // Iterating a std::map is what puts keys in ascending byte order, which is
    // what makes the encoding a function of the content alone -- and therefore
    // what makes a byte-frozen golden file possible.
    for (const auto& entry : m_entries) {
        writeString(out, entry.first);
        entry.second.encode(out);
    }
}

Result<Metadata> Metadata::decode(ByteReader& in, unsigned depthBudget) {
    if (depthBudget == 0) {
        return fail<Metadata>(ErrorCode::Corrupt, "metadata nested deeper than {} at offset {}",
                              kMaxDepth, in.offset());
    }

    auto count = in.readU32();
    if (!count) {
        return unexpected<Error>(count.error());
    }
    // Division rather than multiplication: `count * kMinEntryBytes` is exactly
    // the overflow a hostile count is written to cause.
    if (*count > in.remaining() / kMinEntryBytes) {
        return fail<Metadata>(ErrorCode::Corrupt,
                              "metadata claims {} entries at offset {}, which cannot fit in "
                              "the {} bytes remaining",
                              *count, in.offset(), in.remaining());
    }

    Metadata metadata;
    for (std::uint32_t i = 0; i < *count; ++i) {
        auto key = in.readString();
        if (!key) {
            return unexpected<Error>(key.error());
        }
        auto value = Value::decode(in, depthBudget);
        if (!value) {
            return unexpected<Error>(value.error());
        }
        metadata.set(std::move(*key), std::move(*value));
    }
    return metadata;
}

std::string Metadata::toJson(int indent) const {
    std::string out;
    appendJsonHash(out, *this, indent, 0);
    return out;
}

} // namespace sweeps
