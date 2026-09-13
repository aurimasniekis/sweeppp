// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Snapshot.hpp"

#include <algorithm>
#include <fstream>
#include <sweeppp_gl.h>

// No stdio: the file is written through std::filesystem below, which is what
// makes a path with characters outside the active codepage work on Windows.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

#if defined(__APPLE__)
extern "C" bool sweepppCopyPngToPasteboard(const unsigned char* png, unsigned long bytes);
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <gtk/gtk.h>
#endif

namespace sweeppp::ui {
namespace {

constexpr GLenum kViewport = 0x0BA2; // GL_VIEWPORT, absent from the loader's list.

void appendEncoded(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(context);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

} // namespace

Snapshot captureFramebuffer(int x, int y, int width, int height) {
    Snapshot image;
    if (width <= 0 || height <= 0) {
        return image;
    }

    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(kViewport, viewport);
    const int framebufferWidth = viewport[2];
    const int framebufferHeight = viewport[3];
    if (framebufferWidth <= 0 || framebufferHeight <= 0) {
        return image;
    }

    // Clamped rather than refused. A pane's rectangle is computed from ImGui's
    // layout in points and converted, so a rounding step can put its right or
    // bottom edge one pixel outside the framebuffer -- which is not a reason
    // to refuse the operator a snapshot.
    const int left = std::clamp(x, 0, framebufferWidth);
    const int top = std::clamp(y, 0, framebufferHeight);
    const int right = std::clamp(x + width, left, framebufferWidth);
    const int bottom = std::clamp(y + height, top, framebufferHeight);
    if (right <= left || bottom <= top) {
        return image;
    }

    image.width = static_cast<std::uint32_t>(right - left);
    image.height = static_cast<std::uint32_t>(bottom - top);

    const std::size_t rowBytes = static_cast<std::size_t>(image.width) * 4;
    image.pixels.resize(rowBytes * image.height);

    // GL counts rows from the bottom, so the requested top edge becomes the
    // distance from the bottom of the *far* edge.
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(left, framebufferHeight - bottom, static_cast<GLsizei>(image.width),
                 static_cast<GLsizei>(image.height), GL_RGBA, GL_UNSIGNED_BYTE,
                 image.pixels.data());

    // Flipped in place, and the alpha forced opaque: the window is rendered
    // without blending against a desktop behind it, but the buffer's alpha
    // channel is whatever the last draw left there, and a PNG that carries it
    // shows as transparent holes wherever it happened to be low.
    std::vector<std::uint8_t> scratch(rowBytes);
    for (std::uint32_t row = 0; row < image.height / 2; ++row) {
        std::uint8_t* upper = image.pixels.data() + static_cast<std::size_t>(row) * rowBytes;
        std::uint8_t* lower =
            image.pixels.data() + static_cast<std::size_t>(image.height - 1 - row) * rowBytes;
        std::copy_n(upper, rowBytes, scratch.data());
        std::copy_n(lower, rowBytes, upper);
        std::copy_n(scratch.data(), rowBytes, lower);
    }
    for (std::size_t i = 3; i < image.pixels.size(); i += 4) {
        image.pixels[i] = 0xFF;
    }

    return image;
}

std::vector<std::uint8_t> encodePng(const Snapshot& image) {
    std::vector<std::uint8_t> png;
    if (image.empty()) {
        return png;
    }

    png.reserve(image.pixels.size() / 4);
    const int written = stbi_write_png_to_func(
        appendEncoded, &png, static_cast<int>(image.width), static_cast<int>(image.height), 4,
        image.pixels.data(), static_cast<int>(image.width) * 4);
    if (written == 0) {
        png.clear();
    }
    return png;
}

Status writePng(const std::filesystem::path& path, const std::vector<std::uint8_t>& png) {
    if (png.empty()) {
        return fail(ErrorCode::InvalidArgument, "there is nothing to write");
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return fail(ErrorCode::IoError, "could not open {}", path.string());
    }
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!out) {
        return fail(ErrorCode::IoError, "could not write {}", path.string());
    }
    return ok();
}

#if defined(__APPLE__)

Status copyToClipboard(const Snapshot& image, const std::vector<std::uint8_t>& png) {
    (void)image;
    if (png.empty()) {
        return fail(ErrorCode::InvalidArgument, "there is nothing to copy");
    }
    if (!sweepppCopyPngToPasteboard(png.data(), png.size())) {
        return fail(ErrorCode::Unavailable, "the pasteboard refused the image");
    }
    return ok();
}

#elif defined(_WIN32)

Status copyToClipboard(const Snapshot& image, const std::vector<std::uint8_t>& png) {
    (void)png;
    if (image.empty()) {
        return fail(ErrorCode::InvalidArgument, "there is nothing to copy");
    }

    // CF_DIBV5 rather than CF_DIB: V5 is the only DIB header that describes
    // its channel masks, so an alpha channel survives into applications that
    // read it. The payload is the header followed by the pixels, with no file
    // header -- that is what makes a DIB a DIB rather than a .bmp.
    const std::size_t pixelBytes = static_cast<std::size_t>(image.width) * image.height * 4;
    const HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + pixelBytes);
    if (handle == nullptr) {
        return fail(ErrorCode::Unavailable, "could not allocate the clipboard image");
    }

    auto* header = static_cast<BITMAPV5HEADER*>(GlobalLock(handle));
    if (header == nullptr) {
        GlobalFree(handle);
        return fail(ErrorCode::Unavailable, "could not lock the clipboard image");
    }

    *header = BITMAPV5HEADER{};
    header->bV5Size = sizeof(BITMAPV5HEADER);
    header->bV5Width = static_cast<LONG>(image.width);
    // Negative: a DIB is bottom-up unless its height says otherwise, and the
    // capture is already the right way up.
    header->bV5Height = -static_cast<LONG>(image.height);
    header->bV5Planes = 1;
    header->bV5BitCount = 32;
    header->bV5Compression = BI_BITFIELDS;
    header->bV5SizeImage = static_cast<DWORD>(pixelBytes);
    header->bV5RedMask = 0x00FF0000;
    header->bV5GreenMask = 0x0000FF00;
    header->bV5BlueMask = 0x000000FF;
    header->bV5AlphaMask = 0xFF000000;
    header->bV5CSType = LCS_WINDOWS_COLOR_SPACE;
    header->bV5Intent = LCS_GM_IMAGES;

    // RGBA to the BGRA order the masks above declare.
    auto* out = reinterpret_cast<std::uint8_t*>(header + 1);
    for (std::size_t i = 0; i < pixelBytes; i += 4) {
        out[i + 0] = image.pixels[i + 2];
        out[i + 1] = image.pixels[i + 1];
        out[i + 2] = image.pixels[i + 0];
        out[i + 3] = image.pixels[i + 3];
    }
    GlobalUnlock(handle);

    if (OpenClipboard(nullptr) == 0) {
        GlobalFree(handle);
        return fail(ErrorCode::Unavailable, "another application owns the clipboard");
    }
    EmptyClipboard();
    const bool placed = SetClipboardData(CF_DIBV5, handle) != nullptr;
    CloseClipboard();

    if (!placed) {
        // Ownership only transfers on success; on failure it is still ours.
        GlobalFree(handle);
        return fail(ErrorCode::Unavailable, "the clipboard refused the image");
    }
    return ok();
}

#else

Status copyToClipboard(const Snapshot& image, const std::vector<std::uint8_t>& png) {
    (void)png;
    if (image.empty()) {
        return fail(ErrorCode::InvalidArgument, "there is nothing to copy");
    }

    // GTK may not have been initialised yet: the file dialog does it when it
    // opens one, and an operator can reach this button first. gtk_init_check
    // is idempotent and returns false rather than aborting where there is no
    // display, which is the headless case this must not kill the process for.
    if (gtk_init_check(nullptr, nullptr) == FALSE) {
        return fail(ErrorCode::Unavailable, "no display to copy through");
    }

    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_data(
        image.pixels.data(), GDK_COLORSPACE_RGB, TRUE, 8, static_cast<int>(image.width),
        static_cast<int>(image.height), static_cast<int>(image.width) * 4, nullptr, nullptr);
    if (pixbuf == nullptr) {
        return fail(ErrorCode::Unavailable, "could not wrap the image for the clipboard");
    }

    GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_image(clipboard, pixbuf);

    // Hands the data to the clipboard manager, if one is running, so it
    // outlives the process. Without a manager the image stays available only
    // while Sweep++ is running, which is X11's own rule and not something a
    // client can change.
    gtk_clipboard_store(clipboard);

    // The pixbuf borrows our buffer, so it must not outlive this call; the
    // clipboard has taken its own reference to the data it needs.
    g_object_unref(pixbuf);
    return ok();
}

#endif

} // namespace sweeppp::ui
