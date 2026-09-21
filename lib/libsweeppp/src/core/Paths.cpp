// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/core/Paths.hpp"

#include "sweeppp/core/Version.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace sweeppp {
namespace {

fs::path g_configOverride;

fs::path environmentPath(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? fs::path(value) : fs::path{};
}

fs::path homeDir() {
#if defined(_WIN32)
    if (fs::path profile = environmentPath("USERPROFILE"); !profile.empty()) {
        return profile;
    }
#endif
    if (fs::path home = environmentPath("HOME"); !home.empty()) {
        return home;
    }
    return fs::current_path();
}

/// The platform's directory for per-user configuration, under which every
/// channel of the application has a directory of its own.
fs::path configRoot() {
#if defined(__APPLE__)
    return homeDir() / "Library" / "Application Support";
#elif defined(_WIN32)
    if (fs::path appData = environmentPath("APPDATA"); !appData.empty()) {
        return appData;
    }
    return homeDir() / "AppData" / "Roaming";
#else
    if (fs::path xdg = environmentPath("XDG_CONFIG_HOME"); !xdg.empty()) {
        return xdg;
    }
    return homeDir() / ".config";
#endif
}

/// The release channel's appId(), spelled here because a nightly has to find
/// the release's directory without being one.
constexpr std::string_view kReleaseAppId = "sweeppp";

/// This channel's own directory, whether or not it is the one in use.
fs::path channelConfigDir() {
    return configRoot() / appId();
}

/// Present when a nightly is to use the release's directory. An empty file
/// rather than a setting in a settings file, because the settings file lives
/// in whichever directory this decides.
fs::path releaseConfigMarker() {
    return channelConfigDir() / "use-release-config";
}

fs::path defaultConfigDir() {
    return Paths::usesReleaseConfig() ? configRoot() / kReleaseAppId : channelConfigDir();
}

/// Walks up from the executable looking for a resources/ directory, so the
/// binary works both from a build tree (build/dev/dist/sweeppp) and from an
/// installed prefix (prefix/bin/sweeppp) without a compiled-in path.
fs::path locateResourcesDir(const fs::path& exeDir) {
    static constexpr int kMaxLevels = 5;
    fs::path candidate = exeDir;
    std::error_code ec;

    for (int level = 0; level < kMaxLevels; ++level) {
        if (fs::is_directory(candidate / "resources", ec)) {
            return candidate / "resources";
        }
        if (fs::is_directory(candidate / "share" / appId() / "resources", ec)) {
            return candidate / "share" / appId() / "resources";
        }
        if (!candidate.has_parent_path() || candidate.parent_path() == candidate) {
            break;
        }
        candidate = candidate.parent_path();
    }

    return exeDir / "resources";
}

#if defined(__APPLE__)
/// The directory an .app was launched from, or empty when this is not one.
///
/// Inside a bundle the executable sits in Sweep++.app/Contents/MacOS, two
/// levels deeper than a plain binary, which is enough to put the resources and
/// the plugins out of reach of the walk above.
fs::path enclosingBundleDir(const fs::path& exeDir) {
    if (exeDir.filename() != "MacOS" || exeDir.parent_path().filename() != "Contents") {
        return {};
    }
    return exeDir.parent_path().parent_path().parent_path();
}
#endif

std::string withTomlSuffix(std::string_view name) {
    std::string result(name);
    if (!result.ends_with(".toml")) {
        result += ".toml";
    }
    return result;
}

} // namespace

fs::path executablePath() {
    std::error_code ec;

#if defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
        buffer.resize(std::char_traits<char>::length(buffer.c_str()));
        if (fs::path resolved = fs::weakly_canonical(fs::path(buffer), ec); !resolved.empty()) {
            return resolved;
        }
    }
#elif defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0) {
        buffer.resize(length);
        return fs::path(buffer);
    }
#else
    if (fs::path resolved = fs::read_symlink("/proc/self/exe", ec); !ec && !resolved.empty()) {
        return resolved;
    }
#endif

    return {};
}

fs::path executableDir() {
    if (const fs::path binary = executablePath(); !binary.empty()) {
        return binary.parent_path();
    }
    return fs::current_path();
}

std::string slugify(std::string_view name) {
    std::string slug;
    slug.reserve(name.size());
    bool pendingDash = false;

    for (const char c : name) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) != 0) {
            if (pendingDash && !slug.empty()) {
                slug.push_back('-');
            }
            pendingDash = false;
            slug.push_back(static_cast<char>(std::tolower(uc)));
        } else {
            pendingDash = true;
        }
    }

    return slug;
}

void Paths::setConfigDirOverride(fs::path dir) {
    g_configOverride = std::move(dir);
}

bool Paths::usesReleaseConfig() {
    if (channel() == "release") {
        return false;
    }
    std::error_code ec;
    return fs::exists(releaseConfigMarker(), ec);
}

Status Paths::setUsesReleaseConfig(bool enabled) {
    const fs::path marker = releaseConfigMarker();
    std::error_code ec;

    if (!enabled) {
        fs::remove(marker, ec);
        if (ec) {
            return fail(ErrorCode::IoError, "could not remove {}: {}", marker.string(),
                        ec.message());
        }
        return ok();
    }

    fs::create_directories(marker.parent_path(), ec);
    if (ec && !fs::is_directory(marker.parent_path())) {
        return fail(ErrorCode::IoError, "could not create {}: {}", marker.parent_path().string(),
                    ec.message());
    }
    if (std::ofstream out(marker); !out) {
        return fail(ErrorCode::IoError, "could not write {}", marker.string());
    }
    return ok();
}

Paths::Paths() {
    m_configDir = g_configOverride.empty() ? defaultConfigDir() : g_configOverride;

    const fs::path exeDir = executableDir();
    m_resourcesDir = locateResourcesDir(exeDir);
    m_bundledPluginsDir = exeDir / "plugins";

#if defined(__APPLE__)
    // A bundle carries its own Contents/Resources and Contents/PlugIns when it
    // is one thing on its own. In a package it sits beside the CLI tools and
    // shares the resources and plugins they already need, rather than shipping
    // a second copy inside itself -- so both layouts are tried, bundle first.
    if (const fs::path bundleDir = enclosingBundleDir(exeDir); !bundleDir.empty()) {
        const fs::path contents = exeDir.parent_path();
        std::error_code ec;

        m_resourcesDir = fs::is_directory(contents / "Resources" / "themes", ec)
                             ? contents / "Resources"
                             : locateResourcesDir(bundleDir);
        m_bundledPluginsDir = fs::is_directory(contents / "PlugIns", ec) ? contents / "PlugIns"
                                                                         : bundleDir / "plugins";
    }
#endif
}

const Paths& Paths::instance() {
    static const Paths paths;
    return paths;
}

Status Paths::ensureConfigTree() const {
    const fs::path directories[] = {
        m_configDir,     themesDir(),  colormapsDir(), bandPlansDir(), profilesDir(),
        sweepPlansDir(), pluginsDir(), sessionsDir(),  antennasDir(),  calibrationDir(),
    };

    for (const fs::path& dir : directories) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec && !fs::is_directory(dir)) {
            return fail(ErrorCode::IoError, "could not create {}: {}", dir.string(), ec.message());
        }
    }
    return ok();
}

std::vector<fs::path> Paths::searchPath(std::string_view kind) const {
    // User directory first: that is what makes an override an override.
    return {m_configDir / kind, m_resourcesDir / kind};
}

Result<fs::path> Paths::findResource(std::string_view kind, std::string_view name) const {
    const std::string filename = withTomlSuffix(name);

    for (const fs::path& dir : searchPath(kind)) {
        const fs::path candidate = dir / filename;
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }

    return fail<fs::path>(ErrorCode::NotFound, "no {} named '{}' in {} or {}", kind, name,
                          (m_configDir / kind).string(), (m_resourcesDir / kind).string());
}

std::vector<std::string> Paths::listResources(std::string_view kind) const {
    std::set<std::string> names;

    for (const fs::path& dir : searchPath(kind)) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            continue;
        }
        for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".toml") {
                names.insert(entry.path().stem().string());
            }
        }
    }

    return {names.begin(), names.end()};
}

} // namespace sweeppp
