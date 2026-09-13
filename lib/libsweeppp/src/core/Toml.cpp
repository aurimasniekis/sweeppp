// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/core/Toml.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <locale>
#include <optional>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace sweeppp::toml_util {
namespace {

/// Splits "theme.spectrum.trace_live" into its segments. A dotted path is
/// nicer to read at the call site than three chained lookups, and this is the
/// only place that has to know how it decomposes.
std::vector<std::string_view> splitPath(std::string_view dottedPath) {
    std::vector<std::string_view> segments;
    std::size_t start = 0;
    while (start <= dottedPath.size()) {
        const std::size_t dot = dottedPath.find('.', start);
        if (dot == std::string_view::npos) {
            segments.push_back(dottedPath.substr(start));
            break;
        }
        segments.push_back(dottedPath.substr(start, dot - start));
        start = dot + 1;
    }
    return segments;
}

std::string_view trim(std::string_view text) {
    const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    while (!text.empty() && isSpace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

/// Strict, locale-independent string-to-double.
///
/// Not std::from_chars: libc++ gates the floating-point overload behind
/// macOS 26, so it fails to link on any earlier system. Not strtod/stod
/// either -- both honour the global locale, and a user running under a comma-
/// decimal locale would silently misparse "2.4e9" as 2. An explicitly classic
/// stream is the portable option that cannot be affected by either.
std::optional<double> parseDoubleStrict(std::string_view text) {
    std::istringstream stream{std::string(text)};
    stream.imbue(std::locale::classic());

    double value = 0.0;
    stream >> value;
    if (stream.fail()) {
        return std::nullopt;
    }

    // Reject trailing junk, so "2.4GG" is an error rather than 2.4.
    stream >> std::ws;
    if (!stream.eof()) {
        return std::nullopt;
    }
    return value;
}

std::string commentEachLine(std::string_view header) {
    std::string result;
    std::size_t start = 0;
    while (start < header.size()) {
        const std::size_t end = header.find('\n', start);
        const std::string_view line = header.substr(
            start, end == std::string_view::npos ? header.size() - start : end - start);
        result += line.empty() ? "#\n" : std::format("# {}\n", line);
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return result;
}

} // namespace

Result<::toml::table> load(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return fail<::toml::table>(ErrorCode::NotFound, "no such file: {}", path.string());
    }

    try {
        return ::toml::parse_file(path.string());
    } catch (const ::toml::parse_error& error) {
        // toml++ reports line and column; carrying them through is the
        // difference between "bad theme file" and a fixable message.
        return fail<::toml::table>(ErrorCode::ParseError, "{}:{}:{}: {}", path.string(),
                                   error.source().begin.line, error.source().begin.column,
                                   std::string(error.description()));
    } catch (const std::exception& error) {
        return fail<::toml::table>(ErrorCode::IoError, "{}: {}", path.string(), error.what());
    }
}

Result<::toml::table> parse(std::string_view text, std::string_view origin) {
    try {
        return ::toml::parse(text, std::string(origin));
    } catch (const ::toml::parse_error& error) {
        return fail<::toml::table>(ErrorCode::ParseError, "{}:{}:{}: {}", origin,
                                   error.source().begin.line, error.source().begin.column,
                                   std::string(error.description()));
    }
}

std::string toString(const ::toml::table& table, std::string_view header) {
    std::ostringstream stream;
    if (!header.empty()) {
        stream << commentEachLine(header) << '\n';
    }
    stream << table << '\n';
    return stream.str();
}

Status save(const fs::path& path, const ::toml::table& table, std::string_view header) {
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec && !fs::is_directory(path.parent_path())) {
            return fail(ErrorCode::IoError, "could not create {}: {}", path.parent_path().string(),
                        ec.message());
        }
    }

    // Write-then-rename. A crash mid-write leaves the previous file intact
    // rather than a truncated one that fails to parse on next start-up.
    fs::path temporary = path;
    temporary += ".tmp";

    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return fail(ErrorCode::IoError, "could not open {} for writing", temporary.string());
        }
        out << toString(table, header);
        if (!out) {
            return fail(ErrorCode::IoError, "could not write {}", temporary.string());
        }
    }

    fs::rename(temporary, path, ec);
    if (ec) {
        fs::remove(temporary, ec);
        return fail(ErrorCode::IoError, "could not replace {}: {}", path.string(), ec.message());
    }
    return ok();
}

// ------------------------------------------------------------- accessors

bool getBool(const ::toml::node_view<const ::toml::node>& node, bool fallback) {
    return node.value_or(fallback);
}

std::int64_t getInt(const ::toml::node_view<const ::toml::node>& node, std::int64_t fallback) {
    if (const auto value = node.value<std::int64_t>()) {
        return *value;
    }
    // A user writing `fft_size = 4096.0` means 4096. Accepting it costs
    // nothing and avoids a confusing "expected integer" for a valid intent.
    if (const auto value = node.value<double>()) {
        return static_cast<std::int64_t>(*value);
    }
    return fallback;
}

double getDouble(const ::toml::node_view<const ::toml::node>& node, double fallback) {
    if (const auto value = node.value<double>()) {
        return *value;
    }
    if (const auto value = node.value<std::int64_t>()) {
        return static_cast<double>(*value);
    }
    return fallback;
}

std::string getString(const ::toml::node_view<const ::toml::node>& node,
                      std::string_view fallback) {
    return node.value_or(std::string(fallback));
}

bool getBool(const ::toml::table& table, std::string_view key, bool fallback) {
    return getBool(at(table, key), fallback);
}

std::int64_t getInt(const ::toml::table& table, std::string_view key, std::int64_t fallback) {
    return getInt(at(table, key), fallback);
}

double getDouble(const ::toml::table& table, std::string_view key, double fallback) {
    return getDouble(at(table, key), fallback);
}

float getFloat(const ::toml::table& table, std::string_view key, float fallback) {
    return static_cast<float>(getDouble(at(table, key), static_cast<double>(fallback)));
}

std::string getString(const ::toml::table& table, std::string_view key, std::string_view fallback) {
    return getString(at(table, key), fallback);
}

Result<bool> requireBool(const ::toml::table& table, std::string_view key) {
    const auto node = at(table, key);
    if (const auto value = node.value<bool>()) {
        return *value;
    }
    return fail<bool>(ErrorCode::ParseError, "missing or non-boolean key '{}'", key);
}

Result<std::int64_t> requireInt(const ::toml::table& table, std::string_view key) {
    const auto node = at(table, key);
    if (const auto value = node.value<std::int64_t>()) {
        return *value;
    }
    if (const auto value = node.value<double>()) {
        return static_cast<std::int64_t>(*value);
    }
    return fail<std::int64_t>(ErrorCode::ParseError, "missing or non-integer key '{}'", key);
}

Result<double> requireDouble(const ::toml::table& table, std::string_view key) {
    const auto node = at(table, key);
    if (const auto value = node.value<double>()) {
        return *value;
    }
    if (const auto value = node.value<std::int64_t>()) {
        return static_cast<double>(*value);
    }
    return fail<double>(ErrorCode::ParseError, "missing or non-numeric key '{}'", key);
}

Result<std::string> requireString(const ::toml::table& table, std::string_view key) {
    const auto node = at(table, key);
    if (const auto value = node.value<std::string>()) {
        return *value;
    }
    return fail<std::string>(ErrorCode::ParseError, "missing or non-string key '{}'", key);
}

std::vector<double> getDoubleArray(const ::toml::table& table, std::string_view key) {
    std::vector<double> values;
    if (const ::toml::array* array = at(table, key).as_array()) {
        values.reserve(array->size());
        for (const ::toml::node& element : *array) {
            if (const auto value = element.value<double>()) {
                values.push_back(*value);
            } else if (const auto integer = element.value<std::int64_t>()) {
                values.push_back(static_cast<double>(*integer));
            }
        }
    }
    return values;
}

std::vector<std::string> getStringArray(const ::toml::table& table, std::string_view key) {
    std::vector<std::string> values;
    if (const ::toml::array* array = at(table, key).as_array()) {
        values.reserve(array->size());
        for (const ::toml::node& element : *array) {
            if (const auto value = element.value<std::string>()) {
                values.push_back(*value);
            }
        }
    }
    return values;
}

::toml::node_view<const ::toml::node> at(const ::toml::table& table, std::string_view dottedPath) {
    const std::vector<std::string_view> segments = splitPath(dottedPath);
    ::toml::node_view<const ::toml::node> current{table};

    for (const std::string_view segment : segments) {
        if (!current) {
            return {};
        }
        current = current[segment];
    }
    return current;
}

::toml::table& ensureTable(::toml::table& table, std::string_view dottedPath) {
    const std::vector<std::string_view> segments = splitPath(dottedPath);
    ::toml::table* current = &table;

    for (const std::string_view segment : segments) {
        const std::string key(segment);
        ::toml::node* existing = current->get(key);
        if (existing == nullptr || !existing->is_table()) {
            current->insert_or_assign(key, ::toml::table{});
            existing = current->get(key);
        }
        current = existing->as_table();
    }
    return *current;
}

// ------------------------------------------------------- units and parsing

Result<double> parseFrequency(std::string_view text) {
    std::string_view input = trim(text);
    if (input.empty()) {
        return fail<double>(ErrorCode::ParseError, "empty frequency");
    }

    // Underscores as digit separators: "1_000_000" reads better than 1000000
    // in a hand-edited band plan.
    std::string cleaned;
    cleaned.reserve(input.size());
    for (const char ch : input) {
        if (ch != '_' && std::isspace(static_cast<unsigned char>(ch)) == 0) {
            cleaned.push_back(ch);
        }
    }

    // Optional trailing "Hz", case-insensitive.
    if (cleaned.size() >= 2) {
        const std::string_view tail(cleaned.data() + cleaned.size() - 2, 2);
        if ((tail[0] == 'H' || tail[0] == 'h') && (tail[1] == 'Z' || tail[1] == 'z')) {
            cleaned.resize(cleaned.size() - 2);
        }
    }
    if (cleaned.empty()) {
        return fail<double>(ErrorCode::ParseError, "'{}' has no numeric part", text);
    }

    double multiplier = 1.0;
    switch (cleaned.back()) {
    case 'k':
    case 'K':
        multiplier = 1e3;
        cleaned.pop_back();
        break;
    case 'M':
    case 'm':
        multiplier = 1e6;
        cleaned.pop_back();
        break;
    case 'G':
    case 'g':
        multiplier = 1e9;
        cleaned.pop_back();
        break;
    case 'T':
    case 't':
        multiplier = 1e12;
        cleaned.pop_back();
        break;
    default:
        break;
    }

    // 'm' is milli everywhere else in SI, but nobody writes a millihertz sweep
    // plan and everybody writes "100m" for 100 MHz. Mega wins here on purpose.

    const std::optional<double> value = parseDoubleStrict(cleaned);
    if (!value) {
        return fail<double>(ErrorCode::ParseError, "'{}' is not a frequency", text);
    }

    return *value * multiplier;
}

Result<double> parseDuration(std::string_view text) {
    std::string_view input = trim(text);
    if (input.empty()) {
        return fail<double>(ErrorCode::ParseError, "empty duration");
    }

    std::string cleaned;
    cleaned.reserve(input.size());
    for (const char ch : input) {
        if (std::isspace(static_cast<unsigned char>(ch)) == 0) {
            cleaned.push_back(ch);
        }
    }

    double multiplier = 1.0;
    const auto stripSuffix = [&cleaned](std::string_view suffix) {
        if (cleaned.size() > suffix.size() && std::string_view(cleaned).ends_with(suffix)) {
            cleaned.resize(cleaned.size() - suffix.size());
            return true;
        }
        return false;
    };

    if (stripSuffix("min")) {
        multiplier = 60.0;
    } else if (stripSuffix("ms")) {
        multiplier = 1e-3;
    } else if (stripSuffix("us")) {
        multiplier = 1e-6;
    } else if (stripSuffix("ns")) {
        multiplier = 1e-9;
    } else if (stripSuffix("h")) {
        multiplier = 3600.0;
    } else if (stripSuffix("s")) {
        multiplier = 1.0;
    }

    const std::optional<double> value = parseDoubleStrict(cleaned);
    if (!value) {
        return fail<double>(ErrorCode::ParseError, "'{}' is not a duration", text);
    }

    return *value * multiplier;
}

Result<double> frequencyFrom(const ::toml::node_view<const ::toml::node>& node,
                             std::string_view key) {
    if (const auto value = node.value<double>()) {
        return *value;
    }
    if (const auto value = node.value<std::int64_t>()) {
        return static_cast<double>(*value);
    }
    if (const auto value = node.value<std::string>()) {
        auto parsed = parseFrequency(*value);
        if (!parsed) {
            return std::unexpected(parsed.error().withContext(std::format("key '{}'", key)));
        }
        return *parsed;
    }
    return fail<double>(ErrorCode::ParseError, "missing or unreadable frequency at '{}'", key);
}

std::string formatFrequency(double hz, int decimals) {
    const double magnitude = std::abs(hz);
    if (magnitude >= 1e9) {
        return std::format("{:.{}f} GHz", hz / 1e9, decimals);
    }
    if (magnitude >= 1e6) {
        return std::format("{:.{}f} MHz", hz / 1e6, decimals);
    }
    if (magnitude >= 1e3) {
        return std::format("{:.{}f} kHz", hz / 1e3, decimals);
    }
    return std::format("{:.{}f} Hz", hz, decimals);
}

std::string formatByteRate(double bytesPerSecond) {
    if (bytesPerSecond >= 1e9) {
        return std::format("{:.2f} GB/s", bytesPerSecond / 1e9);
    }
    if (bytesPerSecond >= 1e6) {
        return std::format("{:.2f} MB/s", bytesPerSecond / 1e6);
    }
    if (bytesPerSecond >= 1e3) {
        return std::format("{:.1f} kB/s", bytesPerSecond / 1e3);
    }
    return std::format("{:.0f} B/s", bytesPerSecond);
}

// formatBytes uses binary units and formatByteRate decimal ones. That is not
// an inconsistency: storage is conventionally binary and throughput
// conventionally decimal, and matching each convention is less confusing than
// picking one and being wrong half the time. formatBytes itself now lives in
// libsweepsfile, beside the format whose file sizes it renders.

} // namespace sweeppp::toml_util
