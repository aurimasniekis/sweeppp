// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MainWindow.hpp"

#include "BarChrome.hpp"
#include "ContributionOverlay.hpp"
#include "FileDialog.hpp"
#include "Icons.hpp"
#include "Snapshot.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <span>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/core/Log.hpp>
#include <sweeppp/core/Paths.hpp>
#include <sweeppp/core/Toml.hpp>
#include <sweeppp/core/Version.hpp>
#include <sweeppp/plugin/PluginHost.hpp>

namespace sweeppp::ui {
namespace {

// The bar's measurements and furniture come from BarChrome.hpp, so the session
// viewer -- which lives in another translation unit -- is built to the same
// ones. They were file-local here, which is exactly how the two windows ended
// up sharing no dimension at all.
using bar::framePadding;
using bar::splitterThickness;
using bar::statusBarHeight;
using bar::statusChip;
using bar::statusFramePadding;
using bar::toolbarHeight;

/// How tall a popover hanging off the toolbar may grow.
///
/// Measured from the viewport rather than fixed, because the analysis panel in
/// its advanced form is longer than a laptop screen: a constant that fits one
/// display runs off the bottom of another, and a popup taller than the window
/// cannot be scrolled back to.
float popoverMaxHeight() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    return std::max(viewport->WorkSize.y - toolbarHeight() - bar::scaled(32.0F),
                    bar::scaled(320.0F));
}

/// Hangs the next popover off the item just submitted.
///
/// Position, so a panel appears where the thing that opened it is rather than
/// wherever the pointer happened to be. Width as a narrow band rather than
/// "whatever the content asks for": these panels share one two-column layout,
/// and sizing each to its own longest label made them read as unrelated windows
/// that happen to hang off the same bar. The floor is what does that work -- a
/// panel narrower than the band still opens at the band's width -- and the
/// ceiling only stops a long device description from stretching one of them
/// across the spectrum.
///
/// The band is overridable for the one panel that is not a column of settings
/// rows: a table of measurements has a width its columns actually need, and
/// squeezing it into the band truncates every one of them to "779.4 M".
///
/// The band is in points and scaled here, like every other measurement in the
/// chrome: text that has grown by half inside a panel whose width has not is a
/// panel of wrapped labels and a scrollbar.
void anchorPopoverUnderItem(float minWidth = 360.0F, float maxWidth = 440.0F) {
    ImGui::SetNextWindowPos(
        ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + bar::scaled(4.0F)));
    ImGui::SetNextWindowSizeConstraints(ImVec2(bar::scaled(minWidth), 0.0F),
                                        ImVec2(bar::scaled(maxWidth), popoverMaxHeight()));
}

ImU32 packed(const Color& color) {
    return color.packed();
}

/// Where a session is offered to be saved by default.
///
/// A timestamped name in the sessions directory: unique without asking, and
/// sorted chronologically by the only thing that distinguishes one sweep from
/// another after the fact.
std::filesystem::path defaultSessionPath() {
    return Paths::instance().sessionsDir() /
           std::format("session-{}.sweeps", formatWallClockCompact(wallClockNs()));
}

ImVec4 toImVec4(const Color& color) {
    return {color.r, color.g, color.b, color.a};
}

/// How one severity of message is drawn.
///
/// Colours come from the theme's own badge set rather than from constants
/// here, so a toast agrees with the drop indicator and the PAUSED badge about
/// what "wrong" looks like. The chrome field is `danger`, not `error`.
struct ToastLook {
    const Color* accent;
    const char* glyph;
    const char* fallback;
};

ToastLook toastLook(ToastSeverity severity, const ChromeTheme& chrome) {
    switch (severity) {
    case ToastSeverity::Success:
        return {.accent = &chrome.ok, .glyph = icon::kSuccess, .fallback = "[ok]"};
    case ToastSeverity::Warning:
        return {.accent = &chrome.warning, .glyph = icon::kWarning, .fallback = "[!]"};
    case ToastSeverity::Error:
        return {.accent = &chrome.danger, .glyph = icon::kError, .fallback = "[x]"};
    case ToastSeverity::Info:
        break;
    }
    return {.accent = &chrome.accent, .glyph = icon::kInfo, .fallback = "[i]"};
}

/// A card's opacity: solid until its last moments, then out.
///
/// Purely a function of the clock, which is what makes it free here -- the
/// application renders continuously rather than waiting on events, so an
/// animation needs nothing to ask for the frames it plays over. Warnings and
/// errors have no deadline and so never fade.
float toastAlpha(const Toast& toast, std::uint64_t nowNs) {
    constexpr std::uint64_t kFadeNs = 400'000'000;

    if (toast.expiresNs == 0 || toast.expiresNs > nowNs + kFadeNs) {
        return 1.0F;
    }
    if (toast.expiresNs <= nowNs) {
        return 0.0F;
    }
    return static_cast<float>(toast.expiresNs - nowNs) / static_cast<float>(kFadeNs);
}

} // namespace

MainWindow::MainWindow(AppState& state) : m_state(state) {
    m_editingColorMap = builtinColorMap("spectral");
}

void MainWindow::requestQuit() {
    // The session this quit is already waiting on. The progress card takes the
    // input, but the window manager's close button reaches past it, and
    // leaving before the file is in place is the one thing it must not do.
    if (m_state.sessionSaveRunning()) {
        m_quitWhenSessionSaved = true;
        return;
    }

    // Never discard silently: an operator who has been sweeping for an hour
    // must be asked before that data goes away.
    //
    // The condition is just "is there unsaved data". It used to also require
    // that nothing was being recorded, from when recording meant "already
    // being written somewhere the operator chose". Retention now runs for as
    // long as acquisition does, so that clause was true exactly when the
    // prompt was needed, and quitting skipped it every time.
    if (m_state.hasUnsavedSession()) {
        m_closeRequested = true;
        return;
    }
    m_quitConfirmed = true;
}

void MainWindow::pollUpdateCheck() {
    if (!UpdateCheck::supported() || !m_state.appSettings().checkForUpdates) {
        return;
    }

    // Started from the first frame rather than from initialise(): start-up is
    // already the slowest part of the application, and this is the least
    // urgent thing it does.
    m_updateCheck.start();

    const std::optional<UpdateCheck::Release> release = m_updateCheck.poll();
    if (!release) {
        return;
    }

    m_updateUrl = release->url;
    m_state.toasts().post(ToastSeverity::Info,
                          std::format("Sweep++ {} is available. You are running {}.",
                                      release->version, buildString()),
                          {ToastAction{.id = "update.show", .label = "Show"},
                           ToastAction{.id = "update.disable", .label = "Stop checking"}},
                          monotonicNs());
}

void MainWindow::handleToastAction(const std::string& id) {
    if (id == "update.show") {
        if (!openInBrowser(m_updateUrl)) {
            // The address rather than a failure: a machine with no browser is
            // still one somebody can read a URL off.
            toast(ToastSeverity::Info, m_updateUrl);
        }
        return;
    }

    if (id == "update.disable") {
        m_state.appSettings().checkForUpdates = false;
        if (auto saved = m_state.saveAppSettings(); !saved) {
            toast(ToastSeverity::Error, saved.error().describe());
            return;
        }
        toast(ToastSeverity::Info, "Update checks are off. The Application menu turns them back "
                                   "on.");
        return;
    }

    logWarn("ui", "unknown toast action {}", id);
}

void MainWindow::setFrameCaptureRequest(std::function<void(std::function<void()>)> request) {
    m_frameCaptureRequest = std::move(request);
}

void MainWindow::setFontWeightRequest(std::function<void(float)> request) {
    m_fontWeightRequest = std::move(request);
}

void MainWindow::takeSnapshot(bool toFile) {
    if (!m_frameCaptureRequest) {
        toast(ToastSeverity::Error, "this window cannot take snapshots");
        return;
    }
    if (m_snapshotRect.z <= 1.0F || m_snapshotRect.w <= 1.0F) {
        toast(ToastSeverity::Error, "there is nothing on screen to snapshot yet");
        return;
    }

    std::filesystem::path path;
    if (toFile) {
        // Before the capture is requested, not inside it: this blocks until
        // the operator answers, and doing that between the render and the swap
        // would hold a window that cannot repaint. Asking first also means the
        // frame that gets captured is the one drawn after the dialog closed,
        // with nothing of the dialog left over it.
        const auto chosen =
            saveFileDialog(Paths::instance().sessionsDir(),
                           std::format("sweeppp-{}.png", formatWallClockCompact(wallClockNs())),
                           "PNG image", "png");
        if (!chosen) {
            return;
        }
        path = *chosen;
    }

    // Points to framebuffer pixels: on a Retina display the two differ by 2x,
    // and reading the rectangle in points would capture the top-left quarter.
    // The rectangle is already relative to the main viewport, which is the
    // framebuffer being read.
    const ImVec2 scale = ImGui::GetMainViewport()->FramebufferScale;
    const int x = static_cast<int>(m_snapshotRect.x * scale.x);
    const int y = static_cast<int>(m_snapshotRect.y * scale.y);
    const int width = static_cast<int>(m_snapshotRect.z * scale.x);
    const int height = static_cast<int>(m_snapshotRect.w * scale.y);

    m_frameCaptureRequest([this, x, y, width, height, path] {
        const Snapshot image = captureFramebuffer(x, y, width, height);
        const std::vector<std::uint8_t> png = encodePng(image);
        if (png.empty()) {
            toast(ToastSeverity::Error, "the snapshot could not be encoded");
            return;
        }

        if (path.empty()) {
            if (auto copied = copyToClipboard(image, png); !copied) {
                toast(ToastSeverity::Error, copied.error().describe());
                return;
            }
            toast(ToastSeverity::Success,
                  std::format("{}x{} snapshot copied", image.width, image.height));
            return;
        }

        if (auto written = writePng(path, png); !written) {
            toast(ToastSeverity::Error, written.error().describe());
            return;
        }
        toast(ToastSeverity::Success, std::format("snapshot saved to {}", path.string()));
    });
}

void MainWindow::launchHistoryViewer(const std::filesystem::path& path) {
    // This very binary, asked for its own path, rather than a directory and a
    // guessed name. Inside the macOS bundle the GUI is called "Sweep++" and
    // nothing named "sweeppp" is there at all, so the guess found no viewer to
    // start and the History button reported one missing.
    const std::filesystem::path binary = executablePath();

    std::error_code ec;
    if (binary.empty() || !std::filesystem::exists(binary, ec)) {
        toast(ToastSeverity::Error,
              std::format("could not find {} to open the history viewer", binary.string()));
        return;
    }

    // A file asked for by name wins; otherwise the running session is handed
    // over when there is one, so the viewer opens on what is being swept now
    // rather than on an empty chooser.
    const std::filesystem::path session = !path.empty()           ? path
                                          : m_state.sessionOpen() ? m_state.sessionPath()
                                                                  : std::filesystem::path{};

    // The same config root, always: the viewer reads its settings and finds the
    // live session there, and this window may have been started on another.
    const std::string configDir = Paths::instance().configDir().string();

    // std::system() hands this to the platform's shell, and the two spell
    // "run it detached and do not wait" differently: cmd.exe has `start`,
    // where the empty first argument is the window title it would otherwise
    // take the quoted path for. Redirecting is only worth it on the POSIX
    // side, where the child would otherwise inherit this terminal.
#if defined(_WIN32)
    const std::string command =
        session.empty() ? std::format("start \"\" \"{}\" --config-dir \"{}\" --history",
                                      binary.string(), configDir)
                        : std::format("start \"\" \"{}\" --config-dir \"{}\" --history \"{}\"",
                                      binary.string(), configDir, session.string());
#else
    const std::string command =
        session.empty()
            ? std::format("\"{}\" --config-dir \"{}\" --history >/dev/null 2>&1 &", binary.string(),
                          configDir)
            : std::format("\"{}\" --config-dir \"{}\" --history \"{}\" >/dev/null 2>&1 &",
                          binary.string(), configDir, session.string());
#endif

    if (std::system(command.c_str()) != 0) {
        toast(ToastSeverity::Error, "could not start the history viewer");
    }
}

void MainWindow::enterHistoryViewer(const std::filesystem::path& path) {
    m_historyViewerMode = true;
    m_showHistory = true;

    if (!path.empty()) {
        beginHistoryOpen(path);
    }
}

void MainWindow::openSessionFile(const std::filesystem::path& path) {
    if (path.empty()) {
        return;
    }

    if (m_historyViewerMode) {
        beginHistoryOpen(path);
        return;
    }

    launchHistoryViewer(path);
}

std::string MainWindow::pendingWindowTitle() {
    if (!m_windowTitleDirty) {
        return {};
    }
    m_windowTitleDirty = false;
    return m_windowTitle;
}

void MainWindow::setWindowTitle(std::string title) {
    if (title == m_windowTitle) {
        return;
    }
    m_windowTitle = std::move(title);
    m_windowTitleDirty = true;
}

void MainWindow::updateInstrumentTitle() {
    std::string title(productName());

    const DeviceDescriptor* device = m_state.device();
    if (device != nullptr) {
        title += std::format(" ({})", m_state.instrument().displayLabel());
    }

    // The plan, not the view, and the same reading the range button on the bar
    // gives: zooming does not change what the instrument is doing. Several
    // ranges collapse to their outer bounds, which is the only pair of numbers
    // that covers all of them and all a title bar has room for.
    const SweepPlan& plan = m_state.sweepPlan();
    if (m_state.sweeping() && !plan.segments.empty()) {
        title += std::format(" ({} - {})", toml_util::formatFrequencyShort(plan.lowestHz()),
                             toml_util::formatFrequencyShort(plan.highestHz()));
    } else if (device != nullptr) {
        if (auto centre = m_state.instrument().parameter("center_hz")) {
            title += std::format(" (fixed {})", toml_util::formatFrequencyShort(asDouble(*centre)));
        }
    }

    setWindowTitle(std::move(title));
}

void MainWindow::draw() {
    // First of all, and outside every window: a style rebuilt from inside one
    // is half undone by the pops on the way back out.
    m_state.applyPendingStyle();

    if (m_historyViewerMode) {
        refreshMarkers();
        beginContributionFrame(m_contributions);
        drawHistoryWindow();
        return;
    }

    m_state.pumpFrames();

    // Once a frame, for every panel: each pushes these unless it is paused.
    m_frameLines = m_state.takePendingWaterfallLines();
    m_reducedLines.clear();
    m_state.telemetry().render().waterfallLines.fetch_add(m_frameLines.size(),
                                                          std::memory_order_relaxed);

    // The radio the saved profile named is opened on a worker thread; this is
    // where it lands, between frames, on the thread that owns the engine.
    m_state.pollDeviceStartup();

    // A session being closed lands here too. The quit that asked for it waits
    // until it has, so the process cannot exit with the file half moved.
    m_state.pollEndSession();
    if (m_quitWhenSessionSaved && !m_state.sessionSaveRunning()) {
        m_quitWhenSessionSaved = false;
        if (m_state.sessionSaveError().empty()) {
            m_quitConfirmed = true;
        } else {
            // A save that failed keeps the window up. The message naming the
            // volume that was full is worth nothing after the process has gone,
            // and the working file is still on disk to try again from.
            toast(ToastSeverity::Error, m_state.sessionSaveError());
        }
    }

    updateInstrumentTitle();

    // The views follow the plan before anything is drawn from them, and the
    // markers are measured once for every panel that will show them.
    followPlan();
    syncPanels();
    refreshMarkers();
    beginContributionFrame(m_contributions);

    // A radio arriving answers the question the chooser was asking. The latch
    // is only cleared on the transition, so "Change device" -- which puts the
    // panel in the same mode deliberately, with a device already open -- is
    // left alone.
    const bool deviceOpen = m_state.device() != nullptr;
    if (deviceOpen && !m_deviceWasOpen) {
        m_deviceChooserMode = false;
    }
    m_deviceWasOpen = deviceOpen;

    // Backspace deletes the selected marker, and falls through to stepping back
    // through the ranges visited when there is none; shift+backspace is forward
    // through the ranges either way.
    //
    // One key for both because both are "undo what I just did to the view", and
    // which one is meant is never ambiguous: a marker is only selected because
    // the operator put it there or clicked it.
    //
    // Guarded on WantTextInput, not on focus: backspace inside the range
    // fields or a preset name has to keep deleting characters. Without the
    // guard, editing a frequency would fling the radio somewhere it had been
    // ten minutes ago.
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        const bool forward = ImGui::GetIO().KeyShift;
        MarkerSet& markers = m_state.markers();

        if (!forward && markers.active() != nullptr) {
            markers.remove(markers.activeId);
        } else if (auto moved = forward ? m_state.goForward() : m_state.goBack(); !moved) {
            toast(ToastSeverity::Error, moved.error().describe());
        }
    }

    // B and C: the allocations and the named channels, on and off. With shift,
    // the panel behind the bar button that does the same thing.
    //
    // Two readings of the same axis, and which one is wanted changes while
    // watching rather than while configuring -- so they are a keystroke each
    // and not a trip through the menu. Shift lands on the same letter because
    // the pair is the same subject: the key says "these, off", and the key
    // with shift says "these -- which ones, exactly?".
    //
    // The panel is opened by whichever plugin declared it draws that type's
    // button, not by name: the flags above are keyed on the contribution type
    // so a channel list the host has never heard of is covered, and a key that
    // named a plugin would undo that in one line.
    //
    // Guarded on WantTextInput like the history keys above, or naming a preset
    // "Beacons" would flash the plot twice.
    if (!ImGui::GetIO().WantTextInput && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeySuper) {
        const bool shift = ImGui::GetIO().KeyShift;

        if (ImGui::IsKeyPressed(ImGuiKey_B, false)) {
            if (shift) {
                PluginManager::instance().openToolbarPanel(SWEEPPP_CONTRIBUTION_BAND);
            } else {
                m_state.view().showBandContributions = !m_state.view().showBandContributions;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            if (shift) {
                PluginManager::instance().openToolbarPanel(SWEEPPP_CONTRIBUTION_CHANNEL);
            } else {
                m_state.view().showChannelContributions = !m_state.view().showChannelContributions;
            }
        }
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    constexpr ImGuiWindowFlags kRootFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::Begin("##root", nullptr, kRootFlags);
    ImGui::PopStyleVar(3);

    // Held while the bus is being probed and the saved radio opened. Not for
    // appearance: every control here either claims hardware or configures the
    // radio that is on its way, and a second open running against the first is
    // exactly the collision the worker thread would otherwise introduce.
    ImGui::BeginDisabled(m_state.deviceStartupRunning());
    drawToolbar();
    ImGui::EndDisabled();
    drawControlBanner();

    // Stacked children have ItemSpacing.y inserted between them, so the body
    // must give that back as well as the status bar's own height. Without it
    // the status bar is pushed past the bottom of the window and its buttons
    // are clipped.
    const float bodyHeight =
        ImGui::GetContentRegionAvail().y - statusBarHeight() - ImGui::GetStyle().ItemSpacing.y;

    // The plots own the entire body. Settings live behind the launchers in the
    // top bar, so nothing permanently occupies width that the spectrum could
    // be using -- on a 6 GHz sweep every pixel is another few MHz resolved.
    //
    // The container takes the plot background so the whole middle of the
    // window is one unbroken surface: the axis gutters, the readout row and
    // the gap between the panes all sit on the same black as the plots
    // themselves, rather than framing them as separate panels.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, toImVec4(m_state.theme().spectrum().background));
    ImGui::BeginChild("##plots", ImVec2(0, bodyHeight), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        // Where a snapshot is cut from: every attached panel, headers
        // included -- the centre, span and RBW are what make a picture of a
        // trace mean anything a week later. Relative to the main viewport,
        // whose framebuffer is what gets read, since with viewports on screen
        // coordinates are the desktop's.
        const ImVec2 snapshotTopLeft = ImGui::GetCursorScreenPos();
        const ImVec2 snapshotSize = ImGui::GetContentRegionAvail();
        const ImVec2 mainOrigin = ImGui::GetMainViewport()->Pos;

        drawPanels();

        m_snapshotRect = ImVec4(snapshotTopLeft.x - mainOrigin.x, snapshotTopLeft.y - mainOrigin.y,
                                snapshotSize.x, snapshotSize.y);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    drawStatusBar();
    drawStartupCard();
    drawSessionSaveCard();

    ImGui::End();

    // Torn-off panels are windows of their own, so they are begun here, after
    // the root has ended, and before the markers are published below so a
    // gesture in one of them is published this frame too.
    drawFloatingPanels();

    if (m_closePanelId != 0) {
        closePanel(std::exchange(m_closePanelId, 0));
    }

    // After the panes, because every gesture that moves a marker has run by
    // now, and before the plugin windows below, so one opened on a marker sees
    // this frame's position rather than the previous frame's.
    publishMarkerChanges();

    // The "…" list, if the cursor is on one. Out here because it is a window
    // and the flags that open it are drawn inside a plot, where a window
    // cannot be begun -- and because it has to be able to take the cursor and
    // be scrolled, which a tooltip drawn in place could not.
    drawContributionOverflow(m_contributions);

    // Outside the root window, like the panels below it: a toast is a window
    // of its own, floating over whatever the frame drew.
    // Before the cards are drawn, so an answer that arrived during this frame
    // is on screen in this one.
    pollUpdateCheck();
    drawToasts();

    if (m_showPerformance) {
        drawPerformancePanel();
    }
    if (m_showFftBenchmark) {
        drawFftBenchmarkWindow();
    }
    if (m_showGradientEditor) {
        drawGradientEditor();
    }
    if (m_showServerEditor) {
        drawServerEditor();
    }
    if (m_showAntennaEditor) {
        drawAntennaEditor();
    }
    if (m_closeRequested) {
        drawClosePrompt();
    }
    if (m_showLearnPrompt) {
        drawLearnPrompt();
    }
    if (m_showClearCorrectionsPrompt) {
        drawClearCorrectionsPrompt();
    }

    // A plugin's own floating window, beside the application's own and outside
    // every popup and child: the argument for the spot existing is that these
    // four calls are where a window belongs.
    PluginManager::instance().drawSpot(SWEEPPP_UI_SPOT_WINDOW);
}

void MainWindow::publishMarkerChanges() {
    const MarkerSet& markers = m_state.markers();

    for (const Marker& marker : markers.items) {
        const auto published = m_publishedMarkers.find(marker.id);
        if (published != m_publishedMarkers.end() && published->second == marker.frequencyHz) {
            continue;
        }
        m_publishedMarkers[marker.id] = marker.frequencyHz;

        m_state.events().publish(MarkerEvent{
            .monotonicNs = monotonicNs(),
            .label = std::format("Marker {}", marker.id),
            .frequencyHz = marker.frequencyHz,
            .levelDbm = static_cast<double>(marker.levelDb),
        });
    }

    // A marker being deleted is not something to announce a frequency for, so
    // its entry is simply dropped rather than published as anything.
    //
    // Not merely housekeeping: the numbering comes back down with the set, so
    // M1 placed after the list was cleared is a different marker with the same
    // name. Keeping the old entry would compare the new one against where the
    // old one sat and, if they happened to match, publish nothing at all.
    std::erase_if(m_publishedMarkers, [&markers](const auto& entry) {
        return std::ranges::none_of(
            markers.items, [&entry](const Marker& marker) { return marker.id == entry.first; });
    });
}

void MainWindow::drawToolbar() {

    const ChromeTheme& chrome = m_state.theme().chrome();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, toImVec4(chrome.headerBackground));
    ImGui::BeginChild("##toolbar", ImVec2(0, toolbarHeight()), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    // Taller controls for the whole bar, in one place.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, framePadding());
    ImGui::SetCursorPos(ImVec2(bar::scaled(8.0F), bar::padding()));

    drawPanelLaunchers();

    // The range button: what is being swept, and the way to change it.
    //
    // Given the most prominent spot on the bar because it is the one setting
    // an operator touches constantly, and because a sweeping analyser has no
    // more important state than the two frequencies bounding it. It shows the
    // *plan*, not the view -- zooming is the wheel and the buttons to its
    // right, and conflating the two is what made the old pair of Start/Stop
    // fields ambiguous.
    {
        const SweepPlan& plan = m_state.sweepPlan();
        const std::string caption =
            m_state.sweeping()
                ? (plan.segments.size() > 1
                       ? std::format("{} ranges   {} - {}##rangebutton", plan.segments.size(),
                                     toml_util::formatFrequencyShort(plan.lowestHz()),
                                     toml_util::formatFrequencyShort(plan.highestHz()))
                       : std::format("Start {}   Stop {}##rangebutton",
                                     toml_util::formatFrequencyShort(plan.lowestHz()),
                                     toml_util::formatFrequencyShort(plan.highestHz())))
                : std::string("Fixed tune##rangebutton");

        // Centred on the window, not merely placed after whatever came
        // before. The range is the single most important piece of state on
        // screen, and a control that drifts sideways as the device name or the
        // launcher labels change width is one the eye has to hunt for.
        // Sized from a template, not from the caption.
        //
        // "Start 2.4 GHz  Stop 2.5 GHz" and "Start 2.403 GHz  Stop 2.443 GHz"
        // are different widths, and a centred button sized to its own text
        // slides sideways every time a digit appears. Reserving the wider case
        // keeps it nailed to the middle of the bar.
        const float templateWidth =
            ImGui::CalcTextSize("Start 8888.888 MHz   Stop 8888.888 MHz").x +
            ImGui::GetStyle().FramePadding.x * 2.0F;
        const float buttonWidth = std::max(ImGui::CalcTextSize(caption.c_str(), nullptr, true).x +
                                               ImGui::GetStyle().FramePadding.x * 2.0F,
                                           templateWidth);
        const float centred = (ImGui::GetWindowWidth() - buttonWidth) * 0.5F;

        // Never behind the launchers: on a narrow window the centre would put
        // it underneath them, and SameLine ignores a position already passed.
        ImGui::SameLine(std::max(centred, ImGui::GetCursorPosX() + 16.0F));

        if (ImGui::Button(caption.c_str(), ImVec2(buttonWidth, 0))) {
            ImGui::OpenPopup("##rangepopup");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Click to edit the swept range.\n\n"
                              "Drag: pan\n"
                              "Shift+drag: zoom\n"
                              "Shift+Ctrl+drag: sweep that band\n"
                              "Shift+Ctrl+Alt+drag: add it to the plan\n"
                              "Backspace / Shift+Backspace: previous / next range");
        }

        ImGui::SetNextWindowPos(
            ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + bar::scaled(4.0F)));
        ImGui::SetNextWindowSizeConstraints(ImVec2(bar::scaled(480.0F), 0.0F),
                                            ImVec2(bar::scaled(680.0F), popoverMaxHeight()));
        const bar::PanelMetrics metrics;
        if (ImGui::BeginPopup("##rangepopup")) {
            drawRangeSection();
            ImGui::EndPopup();
        }
    }

    ImGui::SameLine(0.0F, 16.0F);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("|");
    ImGui::SameLine(0.0F, 16.0F);

    // The zoom buttons act on the focused panel; the wheel and the drags act
    // on whichever panel they happen in.
    PanelView& focused = m_state.view().layout.focused();
    if (ImGui::Button(icon::glyphOr(icon::kResetZoom, "Reset zoom").append("##resetzoom").c_str(),
                      ImVec2(0, 0))) {
        focused.viewStartHz = 0.0;
        focused.viewStopHz = 0.0;
    }

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Fit the whole span in the focused panel");
    }

    ImGui::SameLine();
    if (ImGui::Button(icon::glyphOr(icon::kZoomOut, "-").append("##zoomout").c_str(),
                      ImVec2(0, 0))) {
        const FrequencySpan range = panelRange(focused);
        const FrequencySpan wider = zoomAbout(range, range.centre(), 1.5);
        setPanelRange(focused, wider.startHz, wider.stopHz);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Zoom out");
    }
    ImGui::SameLine();
    if (ImGui::Button(icon::glyphOr(icon::kZoomIn, "+").append("##zoomin").c_str(), ImVec2(0, 0))) {
        const FrequencySpan range = panelRange(focused);
        const FrequencySpan narrower = zoomAbout(range, range.centre(), 0.5);
        setPanelRange(focused, narrower.startHz, narrower.stopHz);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Zoom in");
    }

    ImGui::SameLine();
    if (ImGui::Button(icon::glyphOr(icon::kChart, "Chart").append("##chartbtn").c_str(),
                      ImVec2(0, 0))) {
        ImGui::OpenPopup("##chartsettings");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Spectrum display: points, traces, fill and levels");
    }
    // After the tooltip, never before it: BeginTooltip is a Begin, and it would
    // consume the position and constraints meant for the popup.
    anchorPopoverUnderItem();
    drawChartSettingsPopup();

    ImGui::SameLine();
    if (ImGui::Button(icon::glyphOr(icon::kWaterfall, "Waterfall").append("##waterfallbtn").c_str(),
                      ImVec2(0, 0))) {
        ImGui::OpenPopup("##waterfallsettings");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Waterfall: pause, history and gradient");
    }
    anchorPopoverUnderItem();
    drawWaterfallSettingsPopup();

    ImGui::SameLine();
    if (ImGui::Button(icon::glyphOr(icon::kLayoutGrid, "Panels").append("##panelsbtn").c_str(),
                      ImVec2(0, 0))) {
        ImGui::OpenPopup("##panelssettings");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Panels: layout, mirror or spans, and the overview strip");
    }
    anchorPopoverUnderItem();
    drawPanelsPopup();

    // Start / stop, right-aligned and alone.
    //
    // The one control whose position must never move: it is reached for
    // without looking, and anything sharing its end of the bar would shift it
    // as that thing's text changed width. The health badge that used to sit
    // beside it now lives at the far end of the status bar, with the rest of
    // the at-a-glance state.
    {
        const float startButtonWidth = bar::scaled(92.0F);

        ImGui::SameLine(std::max(ImGui::GetWindowWidth() - startButtonWidth - bar::rightMargin(),
                                 ImGui::GetCursorPosX() + ImGui::GetStyle().ItemSpacing.x));

        const bool running = m_state.running();
        ImGui::BeginDisabled(!m_state.instrument().canControl());
        ImGui::PushStyleColor(ImGuiCol_Button, toImVec4(running ? chrome.stop : chrome.start));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              toImVec4((running ? chrome.stop : chrome.start).withAlpha(0.85F)));
        if (ImGui::Button(running ? "Stop" : "Start", ImVec2(startButtonWidth, 0))) {
            if (running) {
                m_state.stop();
            } else if (auto started = m_state.start(); !started) {
                toast(ToastSeverity::Error, started.error().describe());
            } else {
                // Taken back on success, otherwise the reason an earlier
                // attempt failed sits in the corner contradicting a radio that
                // is plainly running. Only the latched condition needs this --
                // an ordinary message expires on its own.
                m_state.clearError();
            }
        }
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();
    }

    ImGui::PopStyleVar(); // framePadding
    ImGui::EndChild();
}

void MainWindow::drawPaneSplitter(float budget, float& fraction) {
    ImGui::InvisibleButton("##panesplit", ImVec2(-1.0F, splitterThickness()));

    if (ImGui::IsItemActive() && budget > 0.0F) {
        // Dragging down grows the spectrum and shrinks the waterfall. Applied
        // as a delta rather than from the cursor's absolute position, so
        // grabbing the divider anywhere along its height moves it by how far
        // the cursor travels instead of snapping it under the pointer.
        fraction = std::clamp(fraction - ImGui::GetIO().MouseDelta.y / budget, 0.05F, 0.95F);
    }
    if (ImGui::IsItemActive() || ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }

    // A grip only while it is being pointed at. The two panes are meant to
    // read as one surface, so a permanent rule across the middle would be
    // exactly the seam this layout is trying not to have.
    if (ImGui::IsItemActive() || ImGui::IsItemHovered()) {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float y = (min.y + max.y) * 0.5F;
        const ChromeTheme& chrome = m_state.theme().chrome();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(min.x, y), ImVec2(max.x, y),
            packed(ImGui::IsItemActive() ? chrome.accent : chrome.border), 2.0F);
    }
}

float MainWindow::drawPanelLauncher(const char* label, const char* id, const char* tooltip,
                                    void (MainWindow::*body)(), float minWidth, float maxWidth) {
    const float startX = ImGui::GetCursorPosX();

    if (ImGui::Button(label)) {
        ImGui::OpenPopup(id);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }

    anchorPopoverUnderItem(minWidth, maxWidth);

    // Back to panel metrics: the bar's frame padding is still pushed here, and
    // it is sized for buttons aimed at, not for rows read. Scoped tightly so
    // the SameLine below still spaces the launchers by the bar's own metrics.
    {
        const bar::PanelMetrics metrics;
        if (ImGui::BeginPopup(id)) {
            (this->*body)();
            ImGui::EndPopup();
        }
    }

    const float width = ImGui::GetCursorPosX() - startX;
    ImGui::SameLine();
    return width;
}

void MainWindow::drawPanelLaunchers() {
    // The device launcher carries the open radio's name, so the bar always
    // says what is connected without a panel being open.
    const DeviceDescriptor* device = m_state.device();
    const std::string deviceLabel = std::format(
        "{} {}##devicepanel", icon::glyphOr(icon::kDevice, ""),
        device != nullptr ? m_state.instrument().displayLabel() : std::string("Select device"));

    drawPanelLauncher(deviceLabel.c_str(), "##devicepopup",
                      "Device settings, and the list of detected radios",
                      &MainWindow::drawSourceSection);

    // Nothing is connected and nothing has been offered yet: open the panel
    // that lists the radios, once.
    //
    // Not while one is still being opened. Asked then, the answer is always
    // "nothing is connected" -- the radio is seconds away -- and the operator
    // gets a chooser they did not ask for over a window that is about to have
    // a device.
    if (!m_devicePromptShown && !m_state.deviceStartupRunning()) {
        m_devicePromptShown = true;
        if (device == nullptr) {
            m_deviceChooserMode = true;
            ImGui::OpenPopup("##devicepopup");
        }
    }

    drawPanelLauncher(icon::glyphOr(icon::kAnalysis, "Analysis").append("##analysis").c_str(),
                      "##analysispopup", "Resolution, window and throughput -- simple or advanced",
                      &MainWindow::drawAnalysisSection);

    // Everything an operator sets up once and returns to rarely, behind a
    // single menu.
    //
    // Display, theme, profiles and plugins were four launchers competing for
    // bar width with the controls actually used while watching a sweep. They
    // are all "configure the application" rather than "operate the
    // instrument", which is the distinction the bar should be making.
    drawPanelLauncher(icon::glyphOr(icon::kMenu, "Menu").append("##menu").c_str(), "##menupopup",
                      "Display, theme, profiles, plugins and application settings",
                      &MainWindow::drawMenuSection);

    // Then what annotates the plot: the markers, and after them whatever a
    // plugin puts on the spectrum. The device and analysis launchers ahead of
    // the menu configure the instrument; everything from here is about what is
    // drawn over the measurement, and grouping them is what the divider after
    // this run of buttons already implies.
    //
    // Wider than the settings panels: this one is a table of measurements, and
    // a frequency truncated to "779.4 M" is not a reading.
    drawPanelLauncher(icon::glyphOr(icon::kMarker, "Markers").append("##markers").c_str(),
                      "##markerspopup", "Markers and what is under them",
                      &MainWindow::drawMarkersSection, 470.0F, 560.0F);

    // Plugin launchers last, so a plugin can never push the buttons an
    // operator reaches for constantly off to a position that moves as plugins
    // are enabled.
    PluginManager::instance().drawSpot(SWEEPPP_UI_SPOT_TOOLBAR);
}

void MainWindow::drawMenuSection() {
    // Collapsing headers rather than tabs: these are read top to bottom when
    // setting the application up, and more than one is often open at once.
    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawDisplaySection();
    }
    if (ImGui::CollapsingHeader("Theme")) {
        drawThemeSection();
    }

    // Beside Theme, because it answers the same kind of question -- how the
    // application looks rather than what it measures -- and separate from it
    // because a theme travels in a profile and these do not.
    if (ImGui::CollapsingHeader("Appearance")) {
        drawAppearanceSection();
    }
    if (ImGui::CollapsingHeader("Profiles")) {
        drawProfilesSection();
    }

    // Above the data contributors and below the profiles: an antenna is part
    // of the instrument rather than of what is drawn over the measurement, and
    // the list is maintained about as often as a profile is saved.
    if (ImGui::CollapsingHeader("Antennas")) {
        drawAntennasSection();
    }

    // Who answers "what is at this frequency", and in what order. Above the
    // per-plugin sections because it is one question asked across all of them
    // -- and the order it settles is what the marker's title and every
    // contribution drawn on the plots read from.
    if (ImGui::CollapsingHeader("Data contributors")) {
        drawContributorsSection();
    }

    // A plugin's own settings get a section beside Display and Theme, because
    // that is what they are. Putting them inside the row that enables the
    // plugin would bury "which band plan" two levels below "show the grid" for
    // no reason an operator would recognise -- the Plugins section below is
    // about managing plugins, not about configuring them.
    for (const auto& [id, name] : PluginManager::instance().settingsSections()) {
        ImGui::PushID(id.c_str());
        if (ImGui::CollapsingHeader(name.c_str())) {
            ImGui::Indent(6.0F);
            if (!PluginManager::instance().drawSettings(id)) {
                ImGui::TextDisabled("No settings.");
            }
            ImGui::Unindent(6.0F);
        }
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Plugins")) {
        drawPluginsSection();
    }
    if (ImGui::CollapsingHeader("Application")) {
        drawGeneralSettingsBody();
    }
}

void MainWindow::drawRxPortChip(const ChromeTheme& chrome) {
    const DeviceDescriptor* device = m_state.device();
    if (device == nullptr) {
        return;
    }

    const std::span<const SdrRxPort> ports = device->rxPorts;
    if (ports.empty()) {
        // One implicit connector. There is nothing to report that the device
        // name does not already say, and a chip reading "RX1" on a radio with
        // one connector is a control-shaped thing with no state behind it.
        return;
    }

    const std::string selectedId = m_state.instrument().selectedRxPort();
    const auto port = std::ranges::find_if(
        ports, [selectedId](const SdrRxPort& candidate) { return candidate.id == selectedId; });
    if (port == ports.end()) {
        return;
    }

    // Which leg of the RF path is live, which for a switcher means the input
    // the box is on rather than the one assigned first.
    const std::vector<RfLegView> legs = m_state.instrument().rfPath();
    const auto leg = std::ranges::find_if(legs, &RfLegView::live);

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("On");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();

    const std::string label =
        leg != legs.end() ? std::format("{} · {}", leg->portLabel, leg->antenna.name) : port->label;

    // Dim when nothing is assigned: the port is a fact, but "what is on it" is
    // the part worth reading, and a bright chip naming only a connector claims
    // more than it knows.
    ImGui::PushStyleColor(ImGuiCol_Text,
                          toImVec4(leg != legs.end() ? chrome.text : chrome.textDim));
    ImGui::TextUnformatted(label.c_str());
    ImGui::PopStyleColor();

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0F);
        ImGui::TextUnformatted(
            std::format("{}{}", port->label,
                        port->connector.empty() ? "" : std::format(" -- {}", port->connector))
                .c_str());
        if (leg != legs.end()) {
            ImGui::TextUnformatted(std::format("{} · {} · {:+.1f} dBi", leg->antenna.name,
                                               leg->antenna.describeRange(), leg->antenna.gainDbi)
                                       .c_str());
        } else {
            ImGui::TextUnformatted("Nothing assigned to this connector.");
        }
        ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(chrome.textDim));
        ImGui::TextUnformatted(m_state.sweepPlan().antennaRouting ? "Antenna routing on"
                                                                  : "Antenna routing off");
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    ImGui::SameLine(0.0F, 24.0F);
}

void MainWindow::drawControlBanner() {
    const remote::RemoteInstrument* remote = m_state.remoteInstrument();
    if (remote == nullptr || !remote->linkUp() || remote->canControl()) {
        return;
    }
    const ChromeTheme& chrome = m_state.theme().chrome();
    const remote::ControlState& control = remote->control();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, toImVec4(chrome.warning.withAlpha(0.18F)));
    ImGui::BeginChild("##controlbanner",
                      ImVec2(0, ImGui::GetFrameHeight() + (bar::padding() * 2.0F)),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    ImGui::SetCursorPos(ImVec2(bar::scaled(10.0F), bar::padding()));

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(control.held ? std::format("Watching — {} ({}) has control",
                                                      control.controller, control.controllerKind)
                                              .c_str()
                                        : "Watching — nobody has control");
    ImGui::SameLine(0.0F, 16.0F);
    if (ImGui::Button("Take control")) {
        if (auto taken = m_state.instrument().takeControl(); !taken) {
            toast(ToastSeverity::Error, taken.error().describe());
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Change the radio from here. %s goes on watching.",
                          control.held ? control.controller.c_str() : "Whoever has it");
    }
    ImGui::EndChild();
}

void MainWindow::drawLinkChip(const ChromeTheme& chrome) {
    const remote::RemoteInstrument* remote = m_state.remoteInstrument();
    if (remote == nullptr) {
        return;
    }
    const LinkStats link = remote->link();

    // A pass merged into the next means the link, not the radio, is what is
    // setting the waterfall's pace. Said for a few seconds after it happens
    // rather than only in the instant it does.
    const std::uint64_t now = monotonicNs();
    if (link.passesCoalesced != m_linkMergedSeen) {
        m_linkMergedSeen = link.passesCoalesced;
        m_linkMergedAtNs = now;
    }
    const bool behind = m_linkMergedAtNs != 0 && now - m_linkMergedAtNs < 3'000'000'000ULL;

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Via");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(behind ? chrome.warning : chrome.text));
    ImGui::TextUnformatted(std::format("{}{} · {:.0f} ms · {:.1f} MB/s", remote->serverName(),
                                       remote->canControl() ? "" : " · watching", link.roundTripMs,
                                       link.bytesPerSec / 1e6)
                               .c_str());
    ImGui::PopStyleColor();

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(
            std::format("{} ({})", remote->serverName(), remote->endpoint().address()).c_str());
        ImGui::TextUnformatted(std::format("Round trip {:.1f} ms", link.roundTripMs).c_str());
        ImGui::TextUnformatted(std::format("Receiving {:.2f} MB/s, {:.1f} MB in all",
                                           link.bytesPerSec / 1e6,
                                           static_cast<double>(link.bytesReceived) / 1e6)
                                   .c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(behind ? chrome.warning : chrome.textDim));
        ImGui::TextUnformatted(
            std::format("{} passes and {} partial updates merged for a slow link",
                        link.passesCoalesced, link.partialsCoalesced)
                .c_str());
        if (behind) {
            ImGui::TextUnformatted("A lower Network resolution, in Analysis, keeps up.");
        }
        ImGui::PopStyleColor();
        ImGui::EndTooltip();
    }

    ImGui::SameLine(0.0F, 24.0F);
}

void MainWindow::drawStatusBar() {
    const ChromeTheme& chrome = m_state.theme().chrome();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, toImVec4(chrome.headerBackground));
    ImGui::BeginChild("##statusbar", ImVec2(0, statusBarHeight()), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, statusFramePadding());
    ImGui::SetCursorPos(ImVec2(bar::scaled(10.0F), bar::padding()));

    // What is at the marker, ranked. Host-owned, because several contributors
    // answer the same frequency and the operator decides which of them titles
    // the chip -- a plugin drawing its own could only ever speak for itself.
    {
        // The selected marker when there is one, the middle of the focused
        // panel when there is not, so the chip always describes the same
        // frequency the plot the operator is working in does.
        const Marker* selected = m_state.markers().active();
        const double queryHz = selected != nullptr && selected->visible
                                   ? selected->frequencyHz
                                   : panelRange(m_state.view().layout.focused()).centre();

        drawContributionChip(queryHz, chrome);
        ImGui::SameLine(0.0F, 24.0F);
    }

    drawRxPortChip(chrome);
    drawLinkChip(chrome);

    // A plugin's own chip, for something the ranked answer above has no
    // vocabulary for.
    if (PluginManager::instance().hasUiSpot(SWEEPPP_UI_SPOT_STATUS_CHIP)) {
        PluginManager::instance().drawSpot(SWEEPPP_UI_SPOT_STATUS_CHIP);
        ImGui::SameLine(0.0F, 24.0F);
    }

    // Retention indicator, and a way to keep what has been captured so far
    // without waiting for the close prompt.
    //
    // Not a start/stop control: retention runs for as long as acquisition
    // does. Recording is a decision about *keeping* what was retained, which
    // is why this saves rather than starts.
    const bool retaining = m_state.sessionOpen();
    ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(retaining ? chrome.record : chrome.textDim));
    ImGui::BeginDisabled(!retaining || m_state.sessionLines() == 0);
    if (ImGui::Button(icon::glyphOr(icon::kRecord, retaining ? "[R] Save now" : "[R] Not recording")
                          .append("##saveses")
                          .c_str())) {
        // Straight to the system dialog. An intermediate modal asking for a
        // path only to hand it to a second dialog is a step with no content.
        if (const auto chosen = saveFileDialog(Paths::instance().sessionsDir(),
                                               defaultSessionPath().filename().string(),
                                               "Sweep session", "sweeps")) {
            m_state.endSession(*chosen);
            if (auto resumed = m_state.beginSession(); !resumed) {
                toast(ToastSeverity::Error, resumed.error().describe());
            } else {
                toast(ToastSeverity::Success, std::format("session saved to {}", chosen->string()));
            }
        }
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (retaining) {
            ImGui::SetTooltip("Save the session to a file (%llu lines)",
                              static_cast<unsigned long long>(m_state.sessionLines()));
        } else {
            ImGui::SetTooltip("Nothing recorded yet");
        }
    }

    ImGui::SameLine();
    if (ImGui::Button(icon::glyphOr(icon::kSnapshot, "[C] Snapshot").append("##snap").c_str())) {
        // Clipboard by default: the common case is pasting a picture of what
        // is on screen into a message about it, and a file for that is an
        // extra two steps and something to delete afterwards.
        takeSnapshot(ImGui::GetIO().KeyShift);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Copy the plots as a PNG\nShift-click to save to a file");
    }

    ImGui::SameLine();
    if (ImGui::Button(icon::glyphOr(icon::kHistory, "History").append("##histbtn").c_str())) {
        launchHistoryViewer();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Session history");
    }

    ImGui::SameLine();
    if (ImGui::Button(
            icon::glyphOr(icon::kPerformance, "Performance").append("##perfbtn").c_str())) {
        m_showPerformance = !m_showPerformance;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Performance: input rate, drops, throttle and latency");
    }

    // A plugin's own button, after the host's own and before the numbers.
    //
    // The metrics group below places itself absolutely, so a trailing
    // `SameLine` from a plugin cannot push it sideways -- which is what makes
    // this spot safe to append to a row whose right-hand end is reserved.
    if (PluginManager::instance().hasUiSpot(SWEEPPP_UI_SPOT_STATUS_ACTION)) {
        ImGui::SameLine();
        PluginManager::instance().drawSpot(SWEEPPP_UI_SPOT_STATUS_ACTION);
    }

    // Telemetry, right-aligned and placed before the message so it keeps the
    // same position whatever the message says. A readout that slid left
    // whenever a long error appeared would be exactly the wrong behaviour for
    // the numbers that say whether the instrument is telling the truth.
    const TelemetrySnapshot& badgeStats = m_state.stats();

    // Each number is its own field, in its own reserved box.
    //
    // Putting them in one string was not enough: the group stopped moving as a
    // whole, but a digit appearing in the sweep rate still pushed the frame
    // rate along behind it. They update independently and at different rates,
    // so they have to be laid out independently -- otherwise reading any one
    // of them means tracking a number that keeps sliding.
    //
    // Widths come from the widest each field can get, so only the digits
    // inside a box ever move.
    struct Metric {
        std::string text;
        const char* widest;
        const Color* dot;
    };

    const Color badgeColor = badgeStats.stream.dropFraction > 0.01  ? chrome.danger
                             : badgeStats.stream.dropFraction > 0.0 ? chrome.warning
                                                                    : chrome.ok;

    const std::array<Metric, 5> metrics{{
        {std::format("CPU {:.0f}%", badgeStats.render.cpuPercent), "CPU 888%", &badgeColor},
        {std::format("{:.0f}% shown", badgeStats.process.processedFraction * 100.0), "888% shown",
         nullptr},
        {std::format("{:.1f} MHz/s", badgeStats.process.sweepSpeedHzPerSec / 1e6), "88888.8 MHz/s",
         nullptr},
        {std::format("{:.0f} lines/s", badgeStats.render.waterfallLinesPerSec), "8888 lines/s",
         nullptr},
        {std::format("{:.0f} FPS", badgeStats.render.fps), "888 FPS", nullptr},
    }};

    float groupWidth = 0.0F;
    for (const Metric& metric : metrics) {
        groupWidth += ImGui::CalcTextSize(metric.widest).x +
                      (metric.dot != nullptr ? bar::dotColumn() : 0.0F);
    }
    groupWidth += bar::separatorGap() * static_cast<float>(metrics.size() - 1);

    const float groupX = ImGui::GetWindowWidth() - groupWidth - bar::rightMargin();

    float x = groupX;
    for (std::size_t i = 0; i < metrics.size(); ++i) {
        const Metric& metric = metrics[i];
        const float boxWidth = ImGui::CalcTextSize(metric.widest).x +
                               (metric.dot != nullptr ? bar::dotColumn() : 0.0F);

        ImGui::SameLine(std::max(x, ImGui::GetCursorPosX() + 8.0F));

        if (metric.dot != nullptr) {
            // The health dot turns amber then red as drops appear -- the
            // operator should never have to open a panel to learn the
            // instrument is lying.
            statusChip(metric.text.c_str(), *metric.dot,
                       "Green: no drops\n"
                       "Amber: some samples dropped\n"
                       "Red: over 1% dropped");
        } else {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(metric.text.c_str());
        }

        x += boxWidth;

        if (i + 1 < metrics.size()) {
            ImGui::SameLine(x + bar::separatorGap() * 0.5F - ImGui::CalcTextSize("|").x * 0.5F);
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(chrome.separator));
            ImGui::TextUnformatted("|");
            ImGui::PopStyleColor();
            x += bar::separatorGap();
        }
    }

    ImGui::PopStyleVar(); // statusFramePadding
    ImGui::EndChild();
}

void MainWindow::drawToasts() {
    ToastCenter& toasts = m_state.toasts();

    // One clock reading for the whole stack: expiry and the fade have to agree
    // about what time it is, or a card can fade out and then be drawn solid
    // for one more frame.
    const std::uint64_t now = monotonicNs();
    toasts.update(now, m_toastHovered);
    m_toastHovered = false;

    const std::span<const Toast> cards = toasts.visible();
    if (cards.empty()) {
        return;
    }

    const ChromeTheme& chrome = m_state.theme().chrome();

    // Fixed width, wrapped text, height from the content.
    //
    // `io.IniFilename` is null, so nothing here has remembered geometry to
    // come back to -- and a stack whose cards each sized themselves to their
    // own sentence would change width every time one expired, moving the rest
    // while they are being read.
    constexpr float kWidth = 320.0F;
    constexpr float kSpacing = 6.0F;
    constexpr float kStripeWidth = 3.0F;
    constexpr float kRounding = 6.0F;

    // Below the top bar, measured from it rather than from a constant, for the
    // reason every other bar dimension is.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float right = viewport->WorkPos.x + viewport->WorkSize.x - bar::rightMargin();
    float y = viewport->WorkPos.y + toolbarHeight() + 4.0F;

    // Not NoInputs: the whole card is the dismiss target, so it has to be
    // hit-tested.
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;

    ToastId clicked = 0;
    std::string action;

    for (const Toast& toast : cards) {
        const ToastLook look = toastLook(toast.severity, chrome);
        const float alpha = toastAlpha(toast, now);
        const std::string id = std::format("##toast{}", toast.id);

        ImGui::SetNextWindowPos(ImVec2(right, y), ImGuiCond_Always, ImVec2(1.0F, 0.0F));

        // Zero height is ImGui's "fit this axis to the content".
        ImGui::SetNextWindowSize(ImVec2(kWidth, 0.0F));
        ImGui::SetNextWindowBgAlpha(alpha * 0.95F);

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, kRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0F, 9.0F));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, toImVec4(chrome.panelBackground));
        ImGui::PushStyleColor(ImGuiCol_Border, toImVec4(chrome.border));

        if (ImGui::Begin(id.c_str(), nullptr, kFlags)) {
            ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(*look.accent));
            ImGui::TextUnformatted(icon::glyphOr(look.glyph, look.fallback).c_str());
            ImGui::PopStyleColor();

            // Wrapped from where the icon left off, so the second line hangs
            // under the first rather than under the glyph. These are sentences
            // from a driver or a filesystem, not labels, and several of them
            // carry a path.
            ImGui::SameLine(0.0F, 8.0F);
            ImGui::PushTextWrapPos(0.0F);
            ImGui::TextUnformatted(toast.text.c_str());
            ImGui::PopTextWrapPos();

            // Repeats are a count on the one card. Without it, a failure
            // raised from inside a drag would arrive once per frame.
            if (toast.count > 1) {
                ImGui::SameLine(0.0F, 8.0F);
                ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(chrome.textDim));
                ImGui::TextUnformatted(std::format("x{}", toast.count).c_str());
                ImGui::PopStyleColor();
            }

            // The severity stripe down the left edge. Drawn rather than
            // styled, because a window border takes one colour on all four
            // sides. Its alpha is applied by hand -- the style-wide Alpha does
            // not reach draw-list commands.
            const ImVec2 origin = ImGui::GetWindowPos();
            ImGui::GetWindowDrawList()->AddRectFilled(
                origin, ImVec2(origin.x + kStripeWidth, origin.y + ImGui::GetWindowHeight()),
                packed(look.accent->withAlpha(alpha)), kRounding, ImDrawFlags_RoundCornersLeft);

            // Buttons under the text, not beside it: the text is wrapped to
            // a fixed width and a row that shared it would push sentences
            // into three lines to make room for two words.
            bool overButton = false;
            if (!toast.actions.empty()) {
                ImGui::Spacing();
                for (const ToastAction& offered : toast.actions) {
                    if (ImGui::SmallButton(offered.label.c_str())) {
                        action = offered.id;
                        clicked = toast.id;
                    }
                    overButton = overButton || ImGui::IsItemHovered();
                    ImGui::SameLine(0.0F, 6.0F);
                }
                ImGui::NewLine();
            }

            if (ImGui::IsWindowHovered()) {
                m_toastHovered = true;
                // A click anywhere on a card dismisses it, so a click on a
                // button would dismiss the card and take its action. The
                // button has already been handled above.
                if (!overButton && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clicked = toast.id;
                }
            }
        }

        y += ImGui::GetWindowHeight() + kSpacing;
        ImGui::End();

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(4);
    }

    // Only once there is a stack to clear. One card is dismissed by clicking
    // it, and a button under a single toast is more furniture than help.
    if (cards.size() >= 2) {
        ImGui::SetNextWindowPos(ImVec2(right, y), ImGuiCond_Always, ImVec2(1.0F, 0.0F));
        ImGui::SetNextWindowBgAlpha(0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));

        if (ImGui::Begin("##toastdismissall", nullptr,
                         kFlags | ImGuiWindowFlags_AlwaysAutoResize)) {
            if (ImGui::SmallButton("Dismiss all")) {
                // Everything, including what a click has just marked: the
                // stack is gone either way, and `clicked` is a no-op below
                // once its card no longer exists.
                toasts.dismissAll();
            }
            if (ImGui::IsWindowHovered()) {
                m_toastHovered = true;
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    // After the loop, never inside it: dismissing rewrites the list `cards`
    // is a view of, and so does anything an action does.
    toasts.dismiss(clicked);
    if (!action.empty()) {
        handleToastAction(action);
    }
}

void MainWindow::toast(ToastSeverity severity, std::string text) {
    m_state.toasts().post(severity, std::move(text), monotonicNs());
}

void MainWindow::drawClosePrompt() {
    ImGui::OpenPopup("Save session?");

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));

    if (ImGui::BeginPopupModal("Save session?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("This session has unsaved data.\n"
                           "Save it as a .sweeps file to reopen and replay it later.");
        ImGui::Separator();

        ImGui::TextDisabled("%llu lines captured",
                            static_cast<unsigned long long>(m_state.sessionLines()));

        ImGui::Separator();

        // Cancelling the system dialog returns here rather than quitting: the
        // operator backed out of choosing a file, not out of saving.
        // Started, not waited for. An hour of sweeping takes long enough to
        // close that doing it on this thread is a frozen window, and the
        // progress card holds the frame until the file is where it was asked
        // to go.
        if (ImGui::Button("Save...", ImVec2(120, 0))) {
            if (const auto chosen = saveFileDialog(Paths::instance().sessionsDir(),
                                                   defaultSessionPath().filename().string(),
                                                   "Sweep session", "sweeps")) {
                m_state.beginEndSession(*chosen);
                m_state.markSessionHandled();
                m_quitWhenSessionSaved = true;
                m_closeRequested = false;
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(120, 0))) {
            // Explicit, and only ever from here: this is the one path that
            // deletes measurements.
            m_state.beginEndSession(std::nullopt);
            m_state.markSessionHandled();
            m_quitWhenSessionSaved = true;
            m_closeRequested = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            m_closeRequested = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

} // namespace sweeppp::ui
