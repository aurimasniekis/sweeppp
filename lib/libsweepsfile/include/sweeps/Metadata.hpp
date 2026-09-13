// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#pragma once

#include "sweeps/Result.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sweeps {

/// Declared in `sweeps/FileFormat.hpp`, which includes this header. Named here
/// rather than included so that the dependency runs one way only.
class ByteReader;

class Metadata;

/// One typed metadata value.
///
/// Typed rather than a text document, so that reading a session's name needs no
/// parser for any other language: the type tag is one byte and every body is a
/// scalar this format already encodes.
class Value {
public:
    /// Wire tag. The numbering is part of the format -- see §4.2 of the
    /// specification -- so values are never renumbered.
    enum class Type : std::uint8_t {
        String = 1,
        Int = 2,
        Float = 3,
        Bool = 4,
        Bytes = 5,
        Hash = 6,
        Array = 7,
    };

    Value();

    /// Out-of-line, every one of them: `m_hash` is a `unique_ptr` to a type that
    /// is incomplete here, and an implicitly defined destructor or copy would be
    /// generated at the first use of the class, where it still is.
    ~Value();
    Value(const Value& other);
    Value(Value&& other) noexcept;
    Value& operator=(const Value& other);
    Value& operator=(Value&& other) noexcept;

    [[nodiscard]] static Value ofString(std::string value);
    [[nodiscard]] static Value ofInt(std::int64_t value);
    [[nodiscard]] static Value ofFloat(double value);
    [[nodiscard]] static Value ofBool(bool value);
    [[nodiscard]] static Value ofBytes(std::vector<std::byte> value);
    [[nodiscard]] static Value ofHash(Metadata value);

    /// An array, whose element type is taken from its first element.
    ///
    /// Arrays are **homogeneous**: the element type is stated once on the wire
    /// and the elements carry no tag of their own. An element of another type
    /// therefore could not be written back, so it is replaced here by that
    /// type's default -- which keeps what is in memory equal to what will be
    /// encoded. An empty array built this way has element type `String`; use the
    /// overload below to say otherwise.
    [[nodiscard]] static Value ofArray(std::vector<Value> elements);
    [[nodiscard]] static Value ofArray(Type elementType, std::vector<Value> elements);

    /// A default-constructed value of a given type: the empty string, zero,
    /// false, no bytes, an empty hash, an empty array of strings.
    [[nodiscard]] static Value defaultOf(Type type);

    [[nodiscard]] Type type() const noexcept { return m_type; }

    /// The element type of an `Array`. Meaningless for every other type.
    [[nodiscard]] Type elementType() const noexcept { return m_elementType; }

    // Each accessor answers for its own type only and returns the fallback
    // otherwise. Deliberately no coercion: asking for the wrong type gets the
    // stated default rather than a plausible-looking reading of other data.
    [[nodiscard]] std::string asString(std::string_view fallback = {}) const;
    [[nodiscard]] std::int64_t asInt(std::int64_t fallback = 0) const;
    [[nodiscard]] double asFloat(double fallback = 0.0) const;
    [[nodiscard]] bool asBool(bool fallback = false) const;

    /// The string itself, or null when this value is not one.
    ///
    /// `asString` above returns a copy, which is what nearly every caller
    /// wants. This exists for the callers that cannot take one: a C consumer
    /// borrowing a pointer across the ABI boundary, where a copy would have
    /// nowhere to live. It also draws the same absent/empty line the other
    /// reference accessors do -- an empty string and a value that is not a
    /// string are different answers.
    [[nodiscard]] const std::string* asStringRef() const;

    /// Null on a type mismatch. A `Bytes` of length zero and a value that is not
    /// `Bytes` at all are different things, and this is what distinguishes them.
    [[nodiscard]] const std::vector<std::byte>* asBytes() const;
    [[nodiscard]] const Metadata* asHash() const;
    [[nodiscard]] const std::vector<Value>* asArray() const;

    /// Type tag followed by the body.
    void encode(std::vector<std::byte>& out) const;
    [[nodiscard]] static Result<Value> decode(ByteReader& in, unsigned depthBudget);

    /// The body alone: what an array's elements carry, having no tag of their own.
    void encodeBody(std::vector<std::byte>& out) const;
    [[nodiscard]] static Result<Value> decodeBody(ByteReader& in, Type type, unsigned depthBudget);

private:
    Type m_type = Type::String;
    Type m_elementType = Type::String;

    // One member per type rather than a union or a variant. A `Metadata` value
    // holds a handful of entries, so the few dozen idle bytes cost nothing worth
    // the copy constructor a variant of an incomplete type would need.
    std::string m_string;
    std::int64_t m_int = 0;
    double m_float = 0.0;
    bool m_bool = false;
    std::vector<std::byte> m_bytes;
    std::unique_ptr<Metadata> m_hash;
    std::vector<Value> m_array;
};

[[nodiscard]] std::string_view toString(Value::Type type) noexcept;

/// A typed key/value object: the session manifest, a plugin event's fields, and
/// anything else this format needs to carry as metadata rather than structure.
///
/// Keys are emitted in ascending byte order, which is what keeps a byte-frozen
/// golden file possible. Values nest: a value may itself be a hash or an array,
/// and the depth cap below is what keeps that from being a way to crash a reader
/// with a sixty-byte file.
class Metadata {
public:
    using Map = std::map<std::string, Value, std::less<>>;

    /// Nesting deeper than this is refused rather than descended.
    ///
    /// Not decoration. Without it, a payload that nests a hash inside a hash
    /// half a million deep -- which fits in about sixty bytes, since an empty
    /// hash is four -- blows the reader's stack. That is a crash caused by a
    /// file, which is the exact class §12 of the specification exists to
    /// prevent.
    static constexpr unsigned kMaxDepth = 32;

    void setString(std::string key, std::string value);
    void setInt(std::string key, std::int64_t value);
    void setFloat(std::string key, double value);
    void setBool(std::string key, bool value);
    void setBytes(std::string key, std::vector<std::byte> value);
    void setHash(std::string key, Metadata value);
    void setArray(std::string key, std::vector<Value> value);
    void set(std::string key, Value value);

    /// Null when the key is absent. **This is the only "absent".** A `Bytes` of
    /// length zero, an empty string and an empty hash are all present values.
    [[nodiscard]] const Value* find(std::string_view key) const;

    [[nodiscard]] std::string getString(std::string_view key, std::string_view fallback = {}) const;
    [[nodiscard]] std::int64_t getInt(std::string_view key, std::int64_t fallback = 0) const;
    [[nodiscard]] double getFloat(std::string_view key, double fallback = 0.0) const;
    [[nodiscard]] bool getBool(std::string_view key, bool fallback = false) const;

    [[nodiscard]] bool contains(std::string_view key) const;
    [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return m_entries.size(); }

    [[nodiscard]] Map::const_iterator begin() const noexcept { return m_entries.begin(); }
    [[nodiscard]] Map::const_iterator end() const noexcept { return m_entries.end(); }

    void encode(std::vector<std::byte>& out) const;
    [[nodiscard]] static Result<Metadata> decode(ByteReader& in, unsigned depthBudget = kMaxDepth);

    /// JSON, for a human or for `jq`. Write-only by design: there is no parser
    /// here and no round trip through text, so there is no quoting matrix to get
    /// wrong. An `indent` of zero or less emits the compact form.
    [[nodiscard]] std::string toJson(int indent = 2) const;

private:
    Map m_entries;
};

} // namespace sweeps
