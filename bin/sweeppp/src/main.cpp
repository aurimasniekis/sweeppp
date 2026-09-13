// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AppSettings.hpp"
#include "AppState.hpp"
#include "AppWindow.hpp"
#include "MainWindow.hpp"

#include <GLFW/glfw3.h>
#include <filesystem>
#include <functional>
#include <imgui.h>
#include <print>
#include <string>
#include <string_view>
#include <sweeppp/core/CrashHandler.hpp>
#include <sweeppp/core/Log.hpp>
#include <sweeppp/core/Paths.hpp>
#include <sweeppp/core/Version.hpp>

#if defined(_WIN32)
// Apart from the block above, and not merely by convention: <windows.h>
// defines macros that break anything unlucky enough to use those names, so a
// pass that sorts includes must not be able to move it in among the headers
// that do.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdio>
#include <windows.h>
#endif

#if defined(__APPLE__)
namespace {

/// Where an opened document is delivered. A file-scope pointer because the
/// delegate method that receives it is C and cannot carry a capture; see
/// MacOpenDocument.m.
sweeppp::ui::MainWindow* g_openDocumentTarget = nullptr;

void openDocumentFromFinder(const char* path) {
    if (g_openDocumentTarget != nullptr && path != nullptr) {
        g_openDocumentTarget->openSessionFile(std::filesystem::path(path));
    }
}

} // namespace

extern "C" bool sweepppInstallOpenDocumentHandler(void (*handler)(const char*));
#endif

int main(int argc, char** argv) {
    using namespace sweeppp;

#if defined(_WIN32)
    // Linked for the GUI subsystem, so this process has no console of its own
    // -- which is the point: opening the application must not put a black
    // window behind it. Started from a terminal there is one to borrow, and
    // everything written below is worth reading there, so the standard streams
    // are pointed at it when it exists and left alone when it does not.
    if (AttachConsole(ATTACH_PARENT_PROCESS) != 0) {
        FILE* attached = nullptr;
        freopen_s(&attached, "CONOUT$", "w", stdout);
        freopen_s(&attached, "CONOUT$", "w", stderr);
    }
#endif

    Log::setLevel(LogLevel::Info);

    // `--config-dir <dir>` swaps the whole config root, which Paths reads once
    // on first use -- so it is parsed before anything below touches Paths.
    //
    // `--history [file]` runs this binary as a session viewer: no radio, no
    // sweep, just the History window filling an ordinary application window.
    //
    // A separate process rather than a second window inside the main one. The
    // only way ImGui can place a window on the desktop is the multi-viewport
    // flag, which is context-global: it makes screen coordinates
    // desktop-absolute, breaking anything that maps them to GL, and turns
    // popups and modals into platform windows of their own. A viewer that is
    // its own process gets a real window for free and changes nothing about
    // the instrument.
    //
    // A bare path means the same thing without the flag -- `sweeppp
    // capture.sweeps` opens the viewer on it. That is the form every desktop
    // hands an application when a file is opened with it, so the association
    // needs nothing beyond this loop.
    bool viewerMode = false;
    std::filesystem::path viewerPath;
    std::filesystem::path configDir;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        const bool wantsNextAsPath =
            argument == "--history" && i + 1 < argc && argv[i + 1][0] != '-';

        if (argument == "--config-dir" || argument.starts_with("--config-dir=")) {
            // Consumed here, value and all, so the bare-path rule below never
            // mistakes the directory for a session to view.
            std::string_view value;
            if (argument == "--config-dir") {
                value = i + 1 < argc ? std::string_view(argv[++i]) : std::string_view{};
            } else {
                value = argument.substr(std::string_view("--config-dir=").size());
            }
            if (value.empty()) {
                std::println(stderr, "sweeppp: --config-dir needs a directory");
                return 2;
            }
            configDir = value;
        } else if (argument == "--history") {
            viewerMode = true;
            if (wantsNextAsPath) {
                viewerPath = argv[++i];
            }
        } else if (!argument.empty() && argument.front() != '-') {
            viewerMode = true;
            viewerPath = argument;
        }
    }

    if (!configDir.empty()) {
        // Absolute, so the log path and the directory handed on to a history
        // viewer do not depend on where this process was started from. Without
        // a trailing separator too: quoted for cmd.exe, `"C:\alt\"` escapes its
        // own closing quote.
        std::error_code ec;
        std::filesystem::path absolute =
            std::filesystem::absolute(configDir, ec).lexically_normal();
        if (!absolute.has_filename() && absolute.has_relative_path()) {
            absolute = absolute.parent_path();
        }
        Paths::setConfigDirOverride(ec ? configDir : absolute);
    }

    // Diagnostics before anything that can fail, and before the window: the
    // failures worth having a file for are the ones during start-up, and a GUI
    // process has no terminal to have printed them to anyway.
    //
    // The config tree is created here rather than left to AppState, which also
    // ensures it: this needs it a few hundred lines earlier. Its own failure is
    // reported there, with the diagnosis; a second copy of that here would say
    // the same thing twice on the one run where it matters.
    const Paths& paths = Paths::instance();
    if (paths.ensureConfigTree()) {
        Log::setLogFile(paths.logFile().string());
        CrashHandler::install(paths.configDir(), "sweeppp");
    }

    // Read before the window, because the font atlas is built at this size.
    // Everything else in here is applied through the style, live.
    const ui::AppSettings appSettings = ui::AppSettings::load(paths.configDir() / "app.toml");

#if defined(__APPLE__)
    // Before the window, and that ordering is the whole of it. A document
    // double-clicked while the application is not running is what starts it,
    // and macOS delivers that document during GLFW's own initialisation --
    // measured at 0.22 s, against anything of ours that waits for a window at
    // 0.24 s. This puts the handling in place first; there is nowhere to open
    // a document yet, so one arriving now is held until there is.
    if (!sweepppInstallOpenDocumentHandler(nullptr)) {
        logWarn("ui", "no application delegate to open documents through; opening a session "
                      "from the Finder will not work");
    }
#endif

    ui::AppWindow window;
    if (auto created = window.create({.title = viewerMode ? std::format("{} History", productName())
                                                          : std::string(productName()),
                                      .fontSize = appSettings.fontSize,
                                      .fontWeight = appSettings.fontWeight});
        !created) {
        std::println(stderr, "sweeppp: {}", created.error());
        return 1;
    }

    std::println("sweeppp {} | {}", buildString(), window.rendererDescription());
    // The UI scale as well as the DPI: the two differ on Windows and X11, and
    // when the interface comes out the wrong size that difference is the first
    // thing worth knowing.
    std::println("  font {} | dpi scale {:.1f}x | ui scale {:.2f}x", window.fontDescription(),
                 static_cast<double>(window.dpiScale()),
                 static_cast<double>(window.displayUiScale()));

    ui::AppState state;
    state.setViewerMode(viewerMode);
    // Both before initialise(), which applies the theme -- and the theme is
    // where the scale is applied.
    state.setDisplayUiScale(window.displayUiScale());
    state.setBaseFontSize(appSettings.fontSize);
    state.adoptAppSettings(appSettings);
    if (auto initialised = state.initialise(); !initialised) {
        std::println(stderr, "sweeppp: {}", initialised.error().describe());
        return 1;
    }

    ui::MainWindow mainWindow(state);
    if (viewerMode) {
        mainWindow.enterHistoryViewer(viewerPath);
    }

    // Dropping a session on the window is the same request as naming one on
    // the command line. Anything that is not a session is left to the reader
    // to reject, which it does with a message naming the file.
    window.setFileDropHandler([&mainWindow](const std::filesystem::path& dropped) {
        mainWindow.openSessionFile(dropped);
    });

#if defined(__APPLE__)
    // And the third way in, which only macOS has: the Finder hands a document
    // to the application delegate rather than passing a path, so the
    // association declared in Info.plist needs something listening for it.
    // This is also where a document that arrived during start-up is handed
    // over.
    g_openDocumentTarget = &mainWindow;
    (void)sweepppInstallOpenDocumentHandler(&openDocumentFromFinder);
#endif

    // The other direction: a snapshot is a read-back of the finished frame,
    // which only exists inside the window's own render.
    mainWindow.setFrameCaptureRequest([&window](std::function<void()> capture) {
        window.requestFrameCapture(std::move(capture));
    });

    // Text weight is baked into the glyphs, so changing it rebuilds the atlas
    // rather than setting a style value, and only the window can do that.
    mainWindow.setFontWeightRequest([&window](float weight) { window.setFontWeight(weight); });

    while (window.running() && !mainWindow.wantsQuit()) {
        window.beginFrame();
        mainWindow.draw();
        window.endFrame();

        if (const std::string title = mainWindow.pendingWindowTitle(); !title.empty()) {
            glfwSetWindowTitle(window.handle(), title.c_str());
        }

        // The window manager's close button asks the UI first, so a session
        // with unsaved data gets the Save/Discard prompt rather than
        // disappearing.
        if (glfwWindowShouldClose(window.handle()) != 0) {
            glfwSetWindowShouldClose(window.handle(), GLFW_FALSE);
            mainWindow.requestQuit();
        }
    }

    state.stop();

    // Written after the loop rather than during it, so the file records how
    // the instrument was left rather than being rewritten sixty times a
    // second, and so a device that was closed on the way out is not saved as
    // still open.
    state.saveSettings();
    return 0;
}
