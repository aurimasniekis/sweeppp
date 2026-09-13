// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include "sweeps/MappedFile.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace fs = std::filesystem;

namespace sweeps {

using detail::fail;

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
        // mmap of a zero-length file fails with EINVAL, which would be reported
        // as an obscure I/O error rather than the truth.
        return fail<MappedFile>(ErrorCode::Corrupt, "{} is empty", path.string());
    }

    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return fail<MappedFile>(ErrorCode::IoError, "could not open {}", path.string());
    }

    void* mapped = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        ::close(fd);
        return fail<MappedFile>(ErrorCode::IoError, "could not map {}", path.string());
    }

    MappedFile file;
    file.m_data = static_cast<const std::byte*>(mapped);
    file.m_size = size;
    file.m_handle = static_cast<std::intptr_t>(fd);
    return Result<MappedFile>(std::move(file));
}

void MappedFile::reset() noexcept {
    if (m_data != nullptr) {
        ::munmap(const_cast<void*>(static_cast<const void*>(m_data)), m_size);
    }
    if (m_handle >= 0) {
        ::close(static_cast<int>(m_handle));
    }
    m_data = nullptr;
    m_size = 0;
    m_handle = -1;
}

MappedFile::~MappedFile() {
    reset();
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : m_data(other.m_data), m_size(other.m_size), m_handle(other.m_handle) {
    other.m_data = nullptr;
    other.m_size = 0;
    other.m_handle = -1;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        reset();
        m_data = other.m_data;
        m_size = other.m_size;
        m_handle = other.m_handle;
        other.m_data = nullptr;
        other.m_size = 0;
        other.m_handle = -1;
    }
    return *this;
}

} // namespace sweeps
