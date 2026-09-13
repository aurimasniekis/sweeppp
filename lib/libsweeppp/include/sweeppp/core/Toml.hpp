// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <sweeps/Text.hpp>
#include <toml++/toml.hpp>
#include <vector>

namespace sweeppp::toml_util {

/// Reads and parses a TOML file, turning toml++'s exception into a Result.
[[nodiscard]] Result<::toml::table> load(const std::filesystem::path& path);

/// Parses TOML from memory. `origin` names the source in error messages.
[[nodiscard]] Result<::toml::table> parse(std::string_view text, std::string_view origin);

/// Writes atomically: serialise to `path.tmp`, then rename over `path`.
///
/// A profile or theme half-written by a crash would be worse than no file at
/// all -- the user would lose the config *and* not know why it stopped
/// loading. Rename is atomic on every platform we target.
[[nodiscard]] Status save(const std::filesystem::path& path, const ::toml::table& table,
                          std::string_view header = {});

/// Serialises to a string, with the same header handling as save().
[[nodiscard]] std::string toString(const ::toml::table& table, std::string_view header = {});

// ---------------------------------------------------------------------------
// Typed accessors.
//
// Two flavours throughout: `get*` returns a default for anything missing or
// mistyped, `require*` fails with a message naming the key. Config the user
// hand-edits gets `require*` where a wrong value would silently change
// behaviour, and `get*` where a sensible default is genuinely fine.
// ---------------------------------------------------------------------------

[[nodiscard]] bool getBool(const ::toml::node_view<const ::toml::node>& node, bool fallback);
[[nodiscard]] std::int64_t getInt(const ::toml::node_view<const ::toml::node>& node,
                                  std::int64_t fallback);
[[nodiscard]] double getDouble(const ::toml::node_view<const ::toml::node>& node, double fallback);
[[nodiscard]] std::string getString(const ::toml::node_view<const ::toml::node>& node,
                                    std::string_view fallback);

[[nodiscard]] bool getBool(const ::toml::table& table, std::string_view key, bool fallback);
[[nodiscard]] std::int64_t getInt(const ::toml::table& table, std::string_view key,
                                  std::int64_t fallback);
[[nodiscard]] double getDouble(const ::toml::table& table, std::string_view key, double fallback);

/// Float overload. Exists so the many float-typed UI settings do not each need
/// a cast at the call site, which would bury a real precision mistake among a
/// dozen harmless ones.
[[nodiscard]] float getFloat(const ::toml::table& table, std::string_view key, float fallback);
[[nodiscard]] std::string getString(const ::toml::table& table, std::string_view key,
                                    std::string_view fallback);

[[nodiscard]] Result<bool> requireBool(const ::toml::table& table, std::string_view key);
[[nodiscard]] Result<std::int64_t> requireInt(const ::toml::table& table, std::string_view key);
[[nodiscard]] Result<double> requireDouble(const ::toml::table& table, std::string_view key);
[[nodiscard]] Result<std::string> requireString(const ::toml::table& table, std::string_view key);

[[nodiscard]] std::vector<double> getDoubleArray(const ::toml::table& table, std::string_view key);
[[nodiscard]] std::vector<std::string> getStringArray(const ::toml::table& table,
                                                      std::string_view key);

/// Dotted-path lookup: `at(root, "theme.spectrum.trace_live")`. Returns an
/// empty view when any segment is missing, so callers can chain without
/// checking each level.
[[nodiscard]] ::toml::node_view<const ::toml::node> at(const ::toml::table& table,
                                                       std::string_view dottedPath);

/// Creates (or finds) a nested table for a dotted path, for serialisation.
[[nodiscard]] ::toml::table& ensureTable(::toml::table& table, std::string_view dottedPath);

// ---------------------------------------------------------------------------
// Frequency and duration parsing.
//
// Config files are hand-written, so "2.4 GHz" and "100e6" must both work.
// Doing this centrally is what keeps every file format consistent.
// ---------------------------------------------------------------------------

/// Accepts a bare number in Hz, or a suffixed string: "2.4G", "100M",
/// "868.3 MHz", "1_000_000". Case-insensitive; a trailing "Hz" is optional.
[[nodiscard]] Result<double> parseFrequency(std::string_view text);

/// Accepts "250ms", "1.5s", "2 min", or a bare number in seconds.
[[nodiscard]] Result<double> parseDuration(std::string_view text);

/// Reads a frequency from a node that may be either a number or a suffixed
/// string, which is what makes `start = 2.4e9` and `start = "2.4 GHz"`
/// interchangeable in a sweep plan.
[[nodiscard]] Result<double> frequencyFrom(const ::toml::node_view<const ::toml::node>& node,
                                           std::string_view key);

/// "2.400 000 GHz" -- what gets written back out and shown in the UI. Chooses
/// the unit from the magnitude.
[[nodiscard]] std::string formatFrequency(double hz, int decimals = 6);

/// Compact form for axis labels and chips: "2.4 GHz", "868.3 MHz".
///
/// Forwards to libsweepsfile, which owns the definition: this string is what
/// builds a session segment's `reason`, and `reason` is written into the file.
/// A second implementation here would put file content at the mercy of which
/// one a call site reached.
[[nodiscard]] inline std::string formatFrequencyShort(double hz) {
    return sweeps::formatFrequencyShort(hz);
}

/// "1.7 MB/s", "421 kB/s" -- link bandwidth and file growth readouts.
///
/// Stays here: decimal units, and nothing in a session file uses it.
[[nodiscard]] std::string formatByteRate(double bytesPerSecond);

/// "12.4 GiB" -- session file sizes and pool budgets.
[[nodiscard]] inline std::string formatBytes(std::uint64_t bytes) {
    return sweeps::formatBytes(bytes);
}

} // namespace sweeppp::toml_util
