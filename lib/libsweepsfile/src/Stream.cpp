// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include "sweeps/Stream.hpp"

#include "sweeps/Records.hpp"

#include <cstdio>
#include <cstring>

namespace sweeps {

using detail::fail;

void encodeStreamHeader(std::vector<std::byte>& out, const StreamHeader& header) {
    writeBytes(out, header.magic.data(), header.magic.size());
    writeU32(out, header.majorVersion);
    writeU32(out, header.minorVersion);
    writeU32(out, header.incompatibleFeatures);
}

Result<StreamHeader> decodeStreamHeader(const std::byte* data, std::size_t bytes) {
    ByteReader in(data, bytes);

    StreamHeader header;
    if (auto read = in.readBytes(header.magic.data(), header.magic.size()); !read) {
        return fail<StreamHeader>(ErrorCode::ProtocolError, "truncated stream header");
    }
    if (header.magic != kMagic) {
        return fail<StreamHeader>(ErrorCode::ProtocolError, "not a .sweeps stream (bad magic)");
    }

    auto major = in.readU32();
    auto minor = in.readU32();
    auto features = in.readU32();
    if (!major || !minor || !features) {
        return fail<StreamHeader>(ErrorCode::ProtocolError, "truncated stream header");
    }
    header.majorVersion = *major;
    header.minorVersion = *minor;
    header.incompatibleFeatures = *features;

    if (header.majorVersion > kMajorVersion) {
        return fail<StreamHeader>(ErrorCode::Unsupported,
                                  "the stream is .sweeps major version {}, newer than this "
                                  "build's {}",
                                  header.majorVersion, kMajorVersion);
    }
    if (const std::uint32_t unknown = header.incompatibleFeatures & ~kKnownFeatures; unknown != 0) {
        std::array<char, 16> bits{};
        std::snprintf(bits.data(), bits.size(), "0x%08X", static_cast<unsigned>(unknown));
        return fail<StreamHeader>(ErrorCode::Unsupported,
                                  "the stream needs .sweeps features this build does not "
                                  "implement (unknown bits {})",
                                  bits.data());
    }
    return header;
}

void RecordFramer::feed(const std::byte* data, std::size_t bytes) {
    if (m_broken || bytes == 0) {
        return;
    }
    // Consumed bytes are dropped before more are appended, so the buffer holds
    // at most one partial record plus whatever arrived with it.
    if (m_read > 0) {
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(m_read));
        m_read = 0;
    }
    m_buffer.insert(m_buffer.end(), data, data + bytes);
}

Result<bool> RecordFramer::next(StreamRecord& out) {
    if (m_broken) {
        return unexpected<Error>(m_error);
    }

    const std::size_t available = m_buffer.size() - m_read;
    if (available < RecordHeader::kBytes) {
        return false;
    }

    ByteReader in(m_buffer.data() + m_read, available);
    auto header = decodeRecordHeader(in);
    if (!header) {
        return false;
    }

    if (header->payloadBytes > m_maxPayloadBytes) {
        m_broken = true;
        m_error = Error(ErrorCode::ProtocolError,
                        detail::format("a record of {} bytes exceeds the stream's limit of {}",
                                       header->payloadBytes, m_maxPayloadBytes));
        return unexpected<Error>(m_error);
    }

    const std::size_t total = RecordHeader::kBytes + header->payloadBytes;
    if (available < total) {
        return false;
    }

    const std::byte* payload = m_buffer.data() + m_read + RecordHeader::kBytes;
    if (crc32(payload, header->payloadBytes) != header->checksum) {
        m_broken = true;
        m_error = Error(ErrorCode::Corrupt,
                        detail::format("checksum mismatch in a {} record",
                                       toString(static_cast<RecordType>(header->type))));
        return unexpected<Error>(m_error);
    }

    out.header = *header;
    out.payload.assign(payload, payload + header->payloadBytes);
    m_read += total;
    if (m_read == m_buffer.size()) {
        m_buffer.clear();
        m_read = 0;
    }
    return true;
}

} // namespace sweeps
