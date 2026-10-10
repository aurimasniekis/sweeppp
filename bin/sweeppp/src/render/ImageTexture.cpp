// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ImageTexture.hpp"

#include <sweeppp_gl.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <fstream>
#include <iterator>
#include <stb_image.h>
#include <vector>

namespace sweeppp::ui {

std::unique_ptr<ImageTexture> ImageTexture::load(const std::filesystem::path& path) {
    // Read here rather than by stb, so a path with non-ASCII characters opens
    // on Windows as it does everywhere else.
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return nullptr;
    }
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                           std::istreambuf_iterator<char>());

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                  &width, &height, &channels, 4);
    if (pixels == nullptr) {
        return nullptr;
    }

    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);

    return std::unique_ptr<ImageTexture>(new ImageTexture(id, width, height));
}

ImageTexture::~ImageTexture() {
    if (m_id != 0) {
        const GLuint id = m_id;
        glDeleteTextures(1, &id);
    }
}

} // namespace sweeppp::ui
