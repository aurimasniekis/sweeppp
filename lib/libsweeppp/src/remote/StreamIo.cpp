// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "remote/StreamIo.hpp"

#include <algorithm>
#include <array>

namespace sweeppp::remote::io {

Result<sweeps::StreamRecord> readRecord(net::TcpSocket& socket, sweeps::RecordFramer& framer,
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
        auto readable = socket.waitReadable(wait);
        if (!readable) {
            return std::unexpected(std::move(readable).error());
        }
        if (!*readable) {
            continue;
        }
        auto got = socket.receive({reinterpret_cast<std::uint8_t*>(buffer.data()), buffer.size()});
        if (!got) {
            return std::unexpected(std::move(got).error());
        }
        if (*got == 0) {
            return fail<sweeps::StreamRecord>(ErrorCode::IoError, "the peer closed the connection");
        }
        framer.feed(buffer.data(), *got);
    }
}

Result<sweeps::StreamHeader> readStreamHeader(net::TcpSocket& socket, Clock::time_point deadline,
                                              const std::atomic<bool>& stopping) {
    std::array<std::byte, sweeps::StreamHeader::kBytes> header{};
    std::size_t have = 0;
    while (have < header.size()) {
        if (stopping.load()) {
            return fail<sweeps::StreamHeader>(ErrorCode::Cancelled, "cancelled");
        }
        const auto now = Clock::now();
        if (now >= deadline) {
            return fail<sweeps::StreamHeader>(ErrorCode::TimedOut, "no stream header in time");
        }
        const auto wait = std::min<std::chrono::milliseconds>(
            kReadSlice, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        auto readable = socket.waitReadable(wait);
        if (!readable) {
            return std::unexpected(std::move(readable).error());
        }
        if (!*readable) {
            continue;
        }
        auto got = socket.receive(
            {reinterpret_cast<std::uint8_t*>(header.data()) + have, header.size() - have});
        if (!got) {
            return std::unexpected(std::move(got).error());
        }
        if (*got == 0) {
            return fail<sweeps::StreamHeader>(ErrorCode::IoError, "the peer closed the connection");
        }
        have += *got;
    }
    return adopt(sweeps::decodeStreamHeader(header.data(), header.size()));
}

Status sendStreamHeader(net::TcpSocket& socket) {
    std::vector<std::byte> header;
    sweeps::encodeStreamHeader(header, sweeps::StreamHeader{});
    return socket.sendAll(asBytes(header));
}

} // namespace sweeppp::remote::io
