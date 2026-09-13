// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <string>

struct GLFWwindow;

namespace sweeppp::ui {

/// Owns the GLFW window, the GL 4.1 core context, and the ImGui/ImPlot
/// contexts. Everything above this is renderer-agnostic UI code.
class AppWindow {
public:
    struct Options {
        std::string title = "Sweep++";
        int width = 1600;
        int height = 950;
        bool vsync = true;

        /// Interface text size in points, from the operator's preferences.
        /// The atlas is built at this size; changing it later is a style
        /// value rather than a reload, so this is only the starting point.
        float fontSize = 15.0F;

        /// Glyph coverage boost, against what the chosen face asks for.
        /// Unlike the size, this is baked into the glyphs, so changing it
        /// later does mean a reload -- see `setFontWeight`.
        float fontWeight = 1.0F;
    };

    AppWindow() = default;
    ~AppWindow();

    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    /// Creates the window and both contexts, or explains why it could not.
    [[nodiscard]] std::expected<void, std::string> create(const Options& options);

    /// True until the user closes the window.
    [[nodiscard]] bool running() const noexcept;

    /// Pumps events and opens a new ImGui frame.
    void beginFrame();

    /// Renders the ImGui draw data and swaps buffers.
    void endFrame();

    /// Backend identification strings, for the About panel and bug reports.
    [[nodiscard]] const std::string& rendererDescription() const noexcept {
        return m_rendererDescription;
    }

    /// Which font was loaded, or why the built-in one is in use.
    [[nodiscard]] const std::string& fontDescription() const noexcept { return m_fontDescription; }

    /// Ratio of framebuffer pixels to logical points. 2 on a Retina display.
    [[nodiscard]] float dpiScale() const noexcept { return m_dpiScale; }

    /// What the interface should be multiplied by on this display, before the
    /// operator overrides it.
    ///
    /// The content scale *divided by* the framebuffer scale, and the division
    /// is the whole point. On macOS both are 2, because the window is measured
    /// in points and the framebuffer holds twice as many pixels -- ImGui
    /// already rasterises against that, so the answer is 1 and scaling again
    /// would draw everything twice the size. On Windows and X11 at 150% the
    /// content scale is 1.5 and the framebuffer scale is 1, because there a
    /// window coordinate *is* a pixel: nothing scales the interface, which is
    /// why the application rendered as though the display were at 100%.
    [[nodiscard]] float displayUiScale() const noexcept { return m_displayUiScale; }

    [[nodiscard]] GLFWwindow* handle() const noexcept { return m_window; }

    /// Called with each file dropped onto the window.
    ///
    /// Dropping is how a file gets into a running window, and on macOS it is
    /// also how one gets in at all: Finder opens documents by Apple Event
    /// rather than by argument, and only for an application bundle, which this
    /// is not. Delivered from the event pump, so the handler runs inside
    /// `beginFrame()` and may touch anything the frame may.
    void setFileDropHandler(std::function<void(const std::filesystem::path&)> handler);

    /// Runs `capture` once, after the next frame has been rendered and before
    /// its buffers are swapped.
    ///
    /// That instant is the only one at which a finished frame exists to be
    /// read back: before the render there is nothing in the buffer, and after
    /// the swap its contents are undefined. Reading during the frame would
    /// also capture a half-drawn window, which is the same problem a screen
    /// recorder has and solves the same way.
    ///
    /// One-shot, and it replaces any request not yet served -- a snapshot is
    /// something an operator asked for now, not a queue.
    void requestFrameCapture(std::function<void()> capture);

    /// Sets how heavily glyph coverage is boosted, against the chosen face's
    /// own figure, and rebuilds the atlas if that is a change.
    ///
    /// A reload, where the text size is not: the boost multiplies the coverage
    /// of a glyph as it is rasterised, so it is baked into every glyph already
    /// in the atlas and cannot be applied to them afterwards. The rebuild is
    /// deferred to the top of the next frame, which is the only point at which
    /// no draw list refers to the atlas.
    ///
    /// Safe to call with the value already in force: it then does nothing.
    void setFontWeight(float weight);

private:
    void destroy() noexcept;
    void loadFonts(float baseSize);

    /// Merges the icon font into the text font, when one was vendored.
    ///
    /// Failure is not fatal and not reported: without it every icon renders as
    /// a missing glyph, so the UI checks `iconsAvailable()` and falls back to
    /// text labels rather than showing a row of empty boxes.
    void loadIconFont(float baseSize);

    GLFWwindow* m_window = nullptr;
    bool m_imguiInitialised = false;
    bool m_implotInitialised = false;
    bool m_glfwInitialised = false;
    std::string m_rendererDescription;
    std::string m_fontDescription;

    /// Whether icon glyphs can actually be drawn. False falls the UI back to
    /// text labels -- a row of empty boxes is worse than words.
    bool m_iconsAvailable = false;
    float m_dpiScale = 1.0F;
    float m_displayUiScale = 1.0F;

    /// What the atlas was built from, kept so a weight change can rebuild it.
    float m_fontBaseSize = 15.0F;
    float m_fontWeight = 1.0F;
    bool m_fontsStale = false;

    std::function<void(const std::filesystem::path&)> m_fileDropHandler;
    std::function<void()> m_frameCapture;
};

} // namespace sweeppp::ui
