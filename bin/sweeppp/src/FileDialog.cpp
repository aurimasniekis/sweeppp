// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "FileDialog.hpp"

#include <array>
#include <nfd.h>
#include <string>
#include <sweeppp/core/Log.hpp>
#include <vector>

#if defined(_WIN32)
// windows.h first, and not merely by convention: shellapi.h uses EXTERN_C,
// DECLSPEC_IMPORT and HDROP without including anything that defines them, so
// in alphabetical order it does not compile.
//
// Pinned rather than described, because describing it did not hold: a
// formatting pass sorted the two into alphabetical order and the Windows build
// failed on sixty-one errors inside shellapi.h, none of which name this file.
// clang-format off
#include <windows.h>

#include <shellapi.h>
// clang-format on
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace sweeppp::ui {
namespace {

#if !defined(_WIN32)
/// Runs `argv` detached, without a shell.
///
/// posix_spawn rather than system(): a path is operator data and can contain
/// spaces, quotes and semicolons, and handing it to a shell would make
/// "reveal this folder" a command-injection surface for the sake of one less
/// function. The argument vector never goes through a parser at all.
///
/// The child is reaped so a session that opens a dozen folders does not
/// accumulate zombies; the *file manager* it launches is the desktop's to own,
/// and outlives this wait because these launchers all return immediately.
bool spawnDetached(const std::vector<std::string>& argv) {
    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (const std::string& argument : argv) {
        raw.push_back(const_cast<char*>(argument.c_str()));
    }
    raw.push_back(nullptr);

    pid_t child = 0;
    if (posix_spawnp(&child, raw[0], nullptr, nullptr, raw.data(), environ) != 0) {
        return false;
    }

    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

} // namespace

std::optional<std::filesystem::path> saveFileDialog(const std::filesystem::path& defaultDirectory,
                                                    const std::string& defaultName,
                                                    const std::string& filterLabel,
                                                    const std::string& filterExtension) {
    // Initialised per call rather than once at start-up.
    //
    // NFD's own guidance, and it matters on Linux where the portal backend
    // holds a D-Bus connection: keeping one open for the life of the
    // application would leave a connection idle for hours to serve a dialog
    // opened twice. Init/Quit around the call is cheap.
    if (NFD_Init() != NFD_OKAY) {
        logWarn("ui", "could not open the file dialog: {}", NFD_GetError());
        return std::nullopt;
    }

    // The directory has to exist or the dialog silently ignores it and opens
    // somewhere unrelated, which reads as the default being wrong.
    std::error_code ec;
    std::filesystem::create_directories(defaultDirectory, ec);

    const std::string directory = defaultDirectory.string();
    const nfdu8filteritem_t filters[1] = {{filterLabel.c_str(), filterExtension.c_str()}};

    nfdu8char_t* chosen = nullptr;
    const nfdresult_t result =
        NFD_SaveDialogU8(&chosen, filters, 1, directory.c_str(), defaultName.c_str());

    std::optional<std::filesystem::path> path;
    if (result == NFD_OKAY) {
        path = std::filesystem::path(chosen);
        NFD_FreePathU8(chosen);
    } else if (result == NFD_ERROR) {
        logWarn("ui", "file dialog failed: {}", NFD_GetError());
    }

    NFD_Quit();
    return path;
}

std::optional<std::filesystem::path> openFileDialog(const std::filesystem::path& defaultDirectory,
                                                    const std::string& filterLabel,
                                                    const std::string& filterExtension) {
    if (NFD_Init() != NFD_OKAY) {
        logWarn("ui", "could not open the file dialog: {}", NFD_GetError());
        return std::nullopt;
    }

    // Not created here, unlike the save dialog: opening somewhere that does not
    // exist is a reason to fall back to the platform default, not to make the
    // directory as a side effect of browsing.
    const std::string directory = defaultDirectory.string();
    const nfdu8filteritem_t filters[1] = {{filterLabel.c_str(), filterExtension.c_str()}};

    nfdu8char_t* chosen = nullptr;
    const nfdresult_t result = NFD_OpenDialogU8(&chosen, filters, 1, directory.c_str());

    std::optional<std::filesystem::path> path;
    if (result == NFD_OKAY) {
        path = std::filesystem::path(chosen);
        NFD_FreePathU8(chosen);
    } else if (result == NFD_ERROR) {
        logWarn("ui", "file dialog failed: {}", NFD_GetError());
    }

    NFD_Quit();
    return path;
}

bool revealInFileManager(const std::filesystem::path& path) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec)) {
        return false;
    }

    const bool isDirectory = std::filesystem::is_directory(path, ec);
    const std::filesystem::path directory = isDirectory ? path : path.parent_path();

#if defined(_WIN32)
    // Explorer takes the selection as part of one argument, and ShellExecuteW
    // does not go through a shell, so the path needs no quoting of its own.
    const std::wstring arguments = isDirectory ? path.wstring() : (L"/select," + path.wstring());
    const HINSTANCE result =
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
    // ShellExecute returns a value above 32 on success. A genuinely awful API,
    // and the check is the documented one.
    return reinterpret_cast<INT_PTR>(result) > 32;
#elif defined(__APPLE__)
    // -R reveals the file with its folder open around it, which is what an
    // operator asking "where is this" wants; a directory is opened instead,
    // because revealing one shows its parent.
    if (isDirectory) {
        return spawnDetached({"open", path.string()});
    }
    return spawnDetached({"open", "-R", path.string()});
#else
    // No portable way to select a file: the freedesktop file-manager
    // interface is D-Bus and not universally present, so the folder is opened
    // and the operator finds the file in it. Better than nothing, and better
    // than a dependency on a service that may not be running.
    //
    // xdg-open first because it is what respects the session's chosen file
    // manager; the rest are the desktops that ship their own and may not have
    // xdg-utils installed.
    for (const char* launcher : {"xdg-open", "gio", "nautilus", "dolphin", "thunar"}) {
        const std::string name(launcher);
        const bool launched = name == "gio" ? spawnDetached({name, "open", directory.string()})
                                            : spawnDetached({name, directory.string()});
        if (launched) {
            return true;
        }
    }
    return false;
#endif
}

bool openInBrowser(const std::string& url) {
    // Checked here rather than trusted to the caller: the address comes from a
    // server's JSON, and a "file://" or "javascript:" one handed to the
    // platform opener is that server choosing what this machine runs.
    if (!url.starts_with("https://") && !url.starts_with("http://")) {
        logWarn("ui", "refusing to open {}: only http and https", url);
        return false;
    }

#if defined(_WIN32)
    const std::wstring wide(url.begin(), url.end());
    const HINSTANCE result =
        ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
#elif defined(__APPLE__)
    return spawnDetached({"open", url});
#else
    for (const char* launcher : {"xdg-open", "gio", "firefox"}) {
        const std::string name(launcher);
        const bool launched =
            name == "gio" ? spawnDetached({name, "open", url}) : spawnDetached({name, url});
        if (launched) {
            return true;
        }
    }
    return false;
#endif
}

} // namespace sweeppp::ui
