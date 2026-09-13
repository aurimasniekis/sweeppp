// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/core/Paths.hpp"
#include "sweeppp/core/Version.hpp"
#include "sweeppp/plugin/PluginHost.hpp"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace fs = std::filesystem;

namespace sweeppp {
namespace {

/// What separates entries in $SWEEPPP_PLUGIN_PATH. Colon everywhere but
/// Windows, where a colon is the drive letter's.
#if defined(_WIN32)
constexpr char kPathListSeparator = ';';
#else
constexpr char kPathListSeparator = ':';
#endif

std::vector<fs::path> environmentList(const char* name) {
    std::vector<fs::path> entries;

    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return entries;
    }

    const std::string_view text(value);
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find(kPathListSeparator, start);
        const std::string_view piece = text.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!piece.empty()) {
            entries.emplace_back(piece);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }

    return entries;
}

#if defined(_WIN32)
fs::path environmentPath(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? fs::path(value) : fs::path{};
}
#endif

/// Library prefixes a package might install into, most specific first.
///
/// Each contributes two entries: `<prefix>/sweeppp/plugins`, which is ours and
/// where anything goes, and `<prefix>` itself, which is shared and where only
/// the file prefix identifies a plugin.
std::vector<fs::path> systemLibraryDirs() {
#if defined(_WIN32)
    std::vector<fs::path> dirs;
    if (fs::path programFiles = environmentPath("ProgramFiles"); !programFiles.empty()) {
        dirs.push_back(programFiles / "sweeppp");
    }
    if (fs::path programFiles = environmentPath("ProgramW6432"); !programFiles.empty()) {
        dirs.push_back(programFiles / "sweeppp");
    }
    return dirs;
#elif defined(__APPLE__)
    return {
        "/opt/homebrew/lib",
        "/usr/local/lib",
        "/usr/lib",
    };
#else
    std::vector<fs::path> dirs;

    // The multiarch directories first, because on Debian and its derivatives
    // that is where a packaged library actually lands -- `/usr/lib` alone
    // would miss every plugin installed by a `.deb`. The triplet comes from
    // the build rather than being guessed; an unset one simply contributes
    // nothing.
#if defined(SWEEPPP_LIBRARY_ARCHITECTURE)
    if (const std::string_view triplet{SWEEPPP_LIBRARY_ARCHITECTURE}; !triplet.empty()) {
        dirs.emplace_back(fs::path("/usr/local/lib") / triplet);
        dirs.emplace_back(fs::path("/usr/lib") / triplet);
    }
#endif

    dirs.emplace_back("/usr/local/lib");
    dirs.emplace_back("/usr/lib");
    return dirs;
#endif
}

/// A shared object, by extension rather than by platform. See the note on
/// findPluginBinaries() for why the list is the same everywhere.
bool looksLikeSharedObject(const fs::path& path) {
    const fs::path extension = path.extension();
    return extension == ".so" || extension == ".dylib" || extension == ".dll";
}

} // namespace

bool hasPluginFilePrefix(const fs::path& path) {
    const std::string name = path.filename().string();
    return name.starts_with(SWEEPPP_PLUGIN_FILE_PREFIX) ||
           name.starts_with("lib" SWEEPPP_PLUGIN_FILE_PREFIX);
}

std::vector<PluginSearchEntry> pluginSearchPath() {
    std::vector<PluginSearchEntry> entries;

    const auto add = [&entries](fs::path directory, bool requiresPrefix) {
        // Duplicates are dropped rather than scanned twice: an operator whose
        // $SWEEPPP_PLUGIN_PATH names the config directory should not see every
        // plugin listed as shadowing itself. The first mention wins, which
        // also means a directory named explicitly keeps its "anything goes"
        // reading even if it turns up again as a system one.
        const auto known =
            std::ranges::find_if(entries, [&directory](const PluginSearchEntry& entry) {
                return entry.directory == directory;
            });
        if (known == entries.end()) {
            entries.push_back(PluginSearchEntry{.directory = std::move(directory),
                                                .requiresPrefix = requiresPrefix});
        }
    };

    // Named explicitly, so it is one of ours whatever is in it.
    for (fs::path directory : environmentList("SWEEPPP_PLUGIN_PATH")) {
        add(std::move(directory), false);
    }

    add(Paths::instance().pluginsDir(), false);
    add(Paths::instance().bundledPluginsDir(), false);

    for (const fs::path& prefix : systemLibraryDirs()) {
        add(prefix / appId() / "plugins", false);
        add(prefix, true);
    }

    return entries;
}

std::vector<fs::path> findPluginBinaries(std::span<const PluginSearchEntry> entries) {
    std::vector<fs::path> found;

    // Keyed on the canonical path, so a symlink from the config directory to a
    // bundled plugin loads once rather than twice under two names. Shadowing
    // by *id* is a different question and belongs to the host, which is the
    // only thing that has read the manifests.
    std::set<fs::path> seen;

    for (const PluginSearchEntry& entry : entries) {
        std::error_code ec;
        if (!fs::is_directory(entry.directory, ec)) {
            continue;
        }

        // Sorted within a directory, so the listing an operator sees does not
        // depend on the order the filesystem happens to return entries in.
        std::vector<fs::path> candidates;
        for (const fs::directory_entry& file : fs::directory_iterator(entry.directory, ec)) {
            if (!file.is_regular_file(ec) || !looksLikeSharedObject(file.path())) {
                continue;
            }
            if (entry.requiresPrefix && !hasPluginFilePrefix(file.path())) {
                continue;
            }
            candidates.push_back(file.path());
        }
        std::ranges::sort(candidates);

        for (const fs::path& candidate : candidates) {
            fs::path canonical = fs::weakly_canonical(candidate, ec);
            if (ec) {
                canonical = candidate;
            }
            if (seen.insert(canonical).second) {
                found.push_back(candidate);
            }
        }
    }

    return found;
}

std::vector<fs::path> findPluginBinaries(std::span<const fs::path> directories) {
    std::vector<PluginSearchEntry> entries;
    entries.reserve(directories.size());
    for (const fs::path& directory : directories) {
        entries.push_back(PluginSearchEntry{.directory = directory, .requiresPrefix = false});
    }
    return findPluginBinaries(std::span<const PluginSearchEntry>(entries));
}

} // namespace sweeppp
