// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include "sweeps/MappedFile.hpp"

#include <system_error>
#include <utility>

// The only translation unit in the library that sees <windows.h>. It defines
// `min`, `max`, `ERROR` and several hundred other macros, and keeping it to one
// file is what stops those reaching any header a consumer includes.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fs = std::filesystem;

namespace sweeps {

using detail::fail;

namespace {

constexpr std::intptr_t kNoHandle = -1;

[[nodiscard]] HANDLE toHandle(std::intptr_t value) noexcept {
    return reinterpret_cast<HANDLE>(value);
}

} // namespace

Result<MappedFile> MappedFile::open(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return fail<MappedFile>(ErrorCode::NotFound, "no such file: {}", path.string());
    }

    const auto size = static_cast<std::size_t>(fs::file_size(path, ec));
    if (ec) {
        return fail<MappedFile>(ErrorCode::IoError, "could not size {}", path.string());
    }
    if (size == 0) {
        // CreateFileMapping refuses a zero-length file. Saying so beats
        // reporting a Win32 error code for a file that is simply empty.
        return fail<MappedFile>(ErrorCode::Corrupt, "{} is empty", path.string());
    }

    // FILE_SHARE_WRITE and FILE_SHARE_DELETE are not optional. Without them,
    // opening the session that is *currently recording* fails on Windows and
    // only on Windows -- and that is an advertised operation.
    //
    // path.c_str() with the W entry point, never path.string(): converting to a
    // narrow string goes through the active code page and mangles any path the
    // code page cannot represent.
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return fail<MappedFile>(ErrorCode::IoError, "could not open {}", path.string());
    }

    const HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        ::CloseHandle(file);
        return fail<MappedFile>(ErrorCode::IoError, "could not map {}", path.string());
    }

    void* view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    // The mapping handle is not needed once a view exists; the view keeps the
    // section alive on its own.
    ::CloseHandle(mapping);

    if (view == nullptr) {
        ::CloseHandle(file);
        return fail<MappedFile>(ErrorCode::IoError, "could not map {}", path.string());
    }

    MappedFile mapped;
    mapped.m_data = static_cast<const std::byte*>(view);
    mapped.m_size = size;
    mapped.m_handle = reinterpret_cast<std::intptr_t>(file);
    return Result<MappedFile>(std::move(mapped));
}

void MappedFile::reset() noexcept {
    if (m_data != nullptr) {
        ::UnmapViewOfFile(static_cast<LPCVOID>(m_data));
    }
    if (m_handle != kNoHandle) {
        ::CloseHandle(toHandle(m_handle));
    }
    m_data = nullptr;
    m_size = 0;
    m_handle = kNoHandle;
}

MappedFile::~MappedFile() {
    reset();
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : m_data(other.m_data), m_size(other.m_size), m_handle(other.m_handle) {
    other.m_data = nullptr;
    other.m_size = 0;
    other.m_handle = kNoHandle;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        reset();
        m_data = other.m_data;
        m_size = other.m_size;
        m_handle = other.m_handle;
        other.m_data = nullptr;
        other.m_size = 0;
        other.m_handle = kNoHandle;
    }
    return *this;
}

} // namespace sweeps
