// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <sweeps/SessionReader.hpp>

/// The `sweeps` command line, as functions.
///
/// Every entry point takes the streams it writes to rather than printing.
/// That seam is what lets two binaries built at different language standards
/// share one implementation of the output text: the `sweeps` tool here, and
/// Sweep++'s own C++23 CLI, which formats with `std::println` throughout and
/// could not otherwise call into a C++17 library without duplicating every
/// line of it.
namespace sweeps::cli {

struct InfoOptions {
    std::filesystem::path path;
    /// Events listed before the output is abbreviated. 0 means all of them.
    std::size_t maxEvents = 20;
};

struct VerifyOptions {
    std::filesystem::path path;
    /// Report only failures. Exit status still distinguishes the outcomes.
    bool quiet = false;
};

struct ExtractOptions {
    std::filesystem::path input;
    std::filesystem::path output;

    /// Offsets from the session's first stored line, in seconds. A `to` of 0
    /// means "to the end".
    double fromSeconds = 0.0;
    double toSeconds = 0.0;

    /// 0 means "whatever the session covers".
    double startHz = 0.0;
    double stopHz = 0.0;

    /// Recorded in the extract's manifest. Empty means the library's version.
    std::string applicationVersion;
};

struct EventsOptions {
    std::filesystem::path path;
    /// Only events of this kind, in the text spelling from `toString`. Empty
    /// means every kind.
    std::string kind;
};

struct ManifestOptions {
    std::filesystem::path path;
};

struct PluginsOptions {
    std::filesystem::path path;
    /// Only records from this plugin. Empty means every one.
    std::string pluginId;
};

struct DumpOptions {
    std::filesystem::path path;

    /// Which segment to dump. Unset dumps every segment in turn.
    std::optional<std::uint32_t> segmentId;
    /// Which pyramid level. Unset picks the finest level that has tiles.
    std::optional<std::uint32_t> lod;

    double fromSeconds = 0.0;
    double toSeconds = 0.0;
    double startHz = 0.0;
    double stopHz = 0.0;

    /// Cap on emitted rows, so a careless `dump` of a three-hour session does
    /// not fill a terminal with a million lines. 0 means unlimited.
    std::uint64_t maxLines = 0;
};

/// A session's metadata, segments and first events.
[[nodiscard]] int runInfo(const InfoOptions& options, std::ostream& out, std::ostream& err);

/// Walks every record and verifies every CRC.
[[nodiscard]] int runVerify(const VerifyOptions& options, std::ostream& out, std::ostream& err);

/// Copies a time and frequency range into a new standalone file.
[[nodiscard]] int runExtract(const ExtractOptions& options, std::ostream& out, std::ostream& err);

/// The event stream as JSON Lines, one object per event.
[[nodiscard]] int runEvents(const EventsOptions& options, std::ostream& out, std::ostream& err);

/// The session metadata, as JSON.
[[nodiscard]] int runManifest(const ManifestOptions& options, std::ostream& out, std::ostream& err);

/// The plugin records a session carries.
[[nodiscard]] int runPlugins(const PluginsOptions& options, std::ostream& out, std::ostream& err);

/// Dequantised levels as CSV -- the command that makes a `.sweeps` file useful
/// to somebody with no C++ at all.
[[nodiscard]] int runDump(const DumpOptions& options, std::ostream& out, std::ostream& err);

/// The library and format versions.
[[nodiscard]] int runVersion(std::ostream& out);

/// Usage text for the `sweeps` binary.
void printUsage(std::ostream& out);

} // namespace sweeps::cli
