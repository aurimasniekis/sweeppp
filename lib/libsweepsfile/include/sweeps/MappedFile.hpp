// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#pragma once

#include "sweeps/Result.hpp"

#include <cstddef>
#include <filesystem>

namespace sweeps {

/// A read-only memory mapping of a whole file.
///
/// Mapping rather than reading is what makes scrollback into a multi-hour
/// session cheap: a tile query touches only the pages it needs, and the
/// operating system evicts the rest. Reading the file would mean loading
/// gigabytes to draw one screenful.
///
/// The mapping deliberately permits other processes to keep writing to and even
/// delete the file, because "open the session that is recording right now" is a
/// supported operation. The cost is stated in the specification's security
/// section: the size captured at open is the only bound, and every read is
/// checked against it.
class MappedFile {
public:
    [[nodiscard]] static Result<MappedFile> open(const std::filesystem::path& path);

    MappedFile() = default;
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    [[nodiscard]] const std::byte* data() const noexcept { return m_data; }
    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] bool valid() const noexcept { return m_data != nullptr; }

private:
    void reset() noexcept;

    const std::byte* m_data = nullptr;
    std::size_t m_size = 0;

    /// The platform handle the mapping was made from: a file descriptor on
    /// POSIX, a HANDLE on Windows. Stored as an intptr so this header stays
    /// free of <windows.h>, which defines macros that break anything unlucky
    /// enough to be included after it.
    std::intptr_t m_handle = -1;
};

} // namespace sweeps
