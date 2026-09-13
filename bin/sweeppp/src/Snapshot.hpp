// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <sweeppp/core/Result.hpp>
#include <vector>

namespace sweeppp::ui {

/// A captured rectangle of the window: RGBA8, top row first.
///
/// Top row first, unlike GL, which reads bottom-up. Every consumer here --
/// PNG, every platform's clipboard -- wants the other order, so the flip
/// happens once at capture rather than three times downstream.
struct Snapshot {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;

    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
};

/// Reads a rectangle out of the frame that has just been rendered.
///
/// `x` and `y` are in framebuffer pixels with the origin at the top left, the
/// way ImGui counts; the flip to GL's convention happens inside. The rectangle
/// is clamped to the framebuffer rather than refused, because a pane can
/// legitimately extend a pixel past it after a resize.
///
/// **Must be called after the frame is drawn and before the buffers are
/// swapped.** After a swap the back buffer's contents are undefined, and there
/// is no other moment at which the finished frame exists to be read.
[[nodiscard]] Snapshot captureFramebuffer(int x, int y, int width, int height);

/// The snapshot as PNG bytes. Empty on failure.
[[nodiscard]] std::vector<std::uint8_t> encodePng(const Snapshot& image);

/// Writes those bytes to `path`.
///
/// Encoding and writing are separate because the two destinations share the
/// encode: the clipboard needs the same bytes, and on Windows a wide path is
/// something std::filesystem handles and stb's fopen wrapper does not.
[[nodiscard]] Status writePng(const std::filesystem::path& path,
                              const std::vector<std::uint8_t>& png);

/// Puts the image on the system clipboard as an image, not as a file path.
///
/// Per platform, because there is no portable clipboard: GLFW's carries text
/// only. Each platform's toolkit is already linked -- AppKit for the file
/// dialog, Win32 for everything, GTK for the same dialog on Linux -- so this
/// adds no dependency.
[[nodiscard]] Status copyToClipboard(const Snapshot& image, const std::vector<std::uint8_t>& png);

} // namespace sweeppp::ui
