// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#pragma once

#include "sweeps/FileFormat.hpp"
#include "sweeps/Result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

/// The same records as a file, carried over a byte stream (Appendix C).
///
/// A stream has no index and cannot be seeked, so it opens with a short header
/// of its own instead of the file's, and its records are read as they arrive
/// rather than found by scanning.
namespace sweeps {

/// The first bytes each side of a stream sends: the file header without the
/// fields that only mean something on disk.
struct StreamHeader {
    std::array<char, 4> magic = kMagic;
    std::uint32_t majorVersion = kMajorVersion;
    std::uint32_t minorVersion = kMinorVersion;
    std::uint32_t incompatibleFeatures = 0;

    static constexpr std::size_t kBytes = 16;
};

void encodeStreamHeader(std::vector<std::byte>& out, const StreamHeader& header);

/// Decodes and checks a header by the file's own rules (§11.2): the magic, a
/// major version no newer than this one, and no feature bits this build does
/// not know. A newer minor version is accepted.
[[nodiscard]] Result<StreamHeader> decodeStreamHeader(const std::byte* data, std::size_t bytes);

/// One record taken off a stream.
struct StreamRecord {
    RecordHeader header;
    std::vector<std::byte> payload;
};

/// Splits a byte stream into records, however the bytes happen to arrive.
///
/// A file can recover from a bad record by scanning on; a stream cannot, since
/// nothing after a corrupt length can be trusted to start a record. A checksum
/// mismatch or an oversized record is therefore fatal, and every later call
/// returns the same error.
class RecordFramer {
public:
    /// What a stream allows by default. A full-width line of a megabin grid
    /// is sent as one tile per 1024 bins, so no legitimate record comes close.
    static constexpr std::uint32_t kDefaultMaxPayloadBytes = 16U * 1024U * 1024U;

    explicit RecordFramer(std::uint32_t maxPayloadBytes = kDefaultMaxPayloadBytes) noexcept
        : m_maxPayloadBytes(maxPayloadBytes) {}

    /// Checked against each record's declared length before its payload is
    /// buffered, so a hostile length cannot become an allocation.
    void setMaxPayloadBytes(std::uint32_t bytes) noexcept { m_maxPayloadBytes = bytes; }
    [[nodiscard]] std::uint32_t maxPayloadBytes() const noexcept { return m_maxPayloadBytes; }

    void feed(const std::byte* data, std::size_t bytes);

    /// True with `out` filled when a record is complete, false when more
    /// bytes are needed, an error when the stream is broken.
    [[nodiscard]] Result<bool> next(StreamRecord& out);

    /// Bytes received and not yet returned as a record.
    [[nodiscard]] std::size_t buffered() const noexcept { return m_buffer.size() - m_read; }

private:
    std::vector<std::byte> m_buffer;
    std::size_t m_read = 0;
    std::uint32_t m_maxPayloadBytes;
    bool m_broken = false;
    Error m_error;
};

} // namespace sweeps
