// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "remote/StreamIo.hpp"

#include <array>

namespace sweeppp::remote::io {

Result<sweeps::StreamRecord> readRecord(net::SecureChannel& channel, sweeps::RecordFramer& framer,
                                        Clock::time_point deadline,
                                        const std::atomic<bool>& stopping) {
    std::vector<std::byte> buffer(kReceiveChunk);
    sweeps::StreamRecord record;
    while (true) {
        auto next = adopt(framer.next(record));
        if (!next) {
            return std::unexpected(std::move(next).error());
        }
        if (*next) {
            return record;
        }
        if (stopping.load()) {
            return fail<sweeps::StreamRecord>(ErrorCode::Cancelled, "cancelled");
        }
        const auto now = Clock::now();
        if (now >= deadline) {
            return fail<sweeps::StreamRecord>(ErrorCode::TimedOut, "nothing arrived in time");
        }
        const auto wait = std::min<std::chrono::milliseconds>(
            kReadSlice, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        auto readable = channel.waitReadable(wait);
        if (!readable) {
            return std::unexpected(std::move(readable).error());
        }
        if (!*readable) {
            continue;
        }
        auto got = channel.receive({reinterpret_cast<std::uint8_t*>(buffer.data()), buffer.size()});
        if (!got) {
            return std::unexpected(std::move(got).error());
        }
        if (*got == 0) {
            return fail<sweeps::StreamRecord>(ErrorCode::IoError, "the peer closed the connection");
        }
        framer.feed(buffer.data(), *got);
    }
}

Result<sweeps::StreamHeader> readStreamHeader(net::SecureChannel& channel,
                                              Clock::time_point deadline,
                                              const std::atomic<bool>& stopping) {
    std::array<std::uint8_t, sweeps::StreamHeader::kBytes> header{};
    if (auto read = readExactly(channel, header, deadline, stopping); !read) {
        return std::unexpected(std::move(read).error());
    }
    return adopt(sweeps::decodeStreamHeader(reinterpret_cast<const std::byte*>(header.data()),
                                            header.size()));
}

Status sendStreamHeader(net::SecureChannel& channel) {
    std::vector<std::byte> header;
    sweeps::encodeStreamHeader(header, sweeps::StreamHeader{});
    return channel.sendAll(asBytes(header));
}

} // namespace sweeppp::remote::io
