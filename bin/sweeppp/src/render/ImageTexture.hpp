// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace sweeppp::ui {

/// A PNG on disk as a texture ImGui can draw. Needs the GL context current.
class ImageTexture {
public:
    /// Nothing when the file is missing or is not a PNG this can read.
    [[nodiscard]] static std::unique_ptr<ImageTexture> load(const std::filesystem::path& path);

    ~ImageTexture();
    ImageTexture(const ImageTexture&) = delete;
    ImageTexture& operator=(const ImageTexture&) = delete;

    [[nodiscard]] std::uint32_t id() const noexcept { return m_id; }
    [[nodiscard]] int width() const noexcept { return m_width; }
    [[nodiscard]] int height() const noexcept { return m_height; }

private:
    ImageTexture(std::uint32_t id, int width, int height) noexcept
        : m_id(id), m_width(width), m_height(height) {}

    std::uint32_t m_id = 0;
    int m_width = 0;
    int m_height = 0;
};

} // namespace sweeppp::ui
