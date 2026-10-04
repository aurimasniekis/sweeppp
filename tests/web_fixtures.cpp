// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// Records a real stream off a loopback server, and what the C++ decoder makes
// of it, for the browser's protocol port to be checked against:
//
//   sweeppp-web-fixtures <directory>
//
// writes stream.bin (the stream as the server sent it, header included) and
// expected.json (every frame's levels, every state's sections re-encoded).

#include "ReferenceFft.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <print>
#include <sweeppp/backends/sdr/SyntheticDevice.hpp>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/crypto/Sha256.hpp>
#include <sweeppp/fft/FftBackendManager.hpp>
#include <sweeppp/instrument/LocalInstrument.hpp>
#include <sweeppp/net/Socket.hpp>
#include <sweeppp/remote/FrameCodec.hpp>
#include <sweeppp/remote/Handshake.hpp>
#include <sweeppp/remote/Messages.hpp>
#include <sweeppp/remote/RemoteServer.hpp>
#include <sweeppp/remote/WireCodec.hpp>
#include <sweeppp/sdr/ISdrDevice.hpp>
#include <sweeps/FileFormat.hpp>
#include <sweeps/Stream.hpp>
#include <thread>
#include <vector>

using namespace sweeppp;
using namespace std::chrono_literals;
using json = nlohmann::json;

namespace {

using Clock = std::chrono::steady_clock;

SweepPlan planOver(double startHz, double stopHz) {
    SweepPlan plan;
    plan.segments = {SweepSegment{.startHz = startHz, .stopHz = stopHz}};
    plan.sampleRate = 8e6;
    plan.rbwHz = 20e3;
    plan.applyMode(SweepMode::Fast);
    return plan;
}

std::vector<std::byte> commandBytes(std::uint64_t seq, std::string_view name,
                                    sweeps::Metadata args = {}) {
    std::vector<std::byte> out;
    remote::appendMessage(
        out, remote::msg::kCommand,
        remote::Command{.seq = seq, .op = std::string(name), .args = std::move(args)}.toMetadata(),
        monotonicNs());
    return out;
}

std::string hexOf(const std::vector<std::byte>& bytes) {
    return crypto::toHex({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

int fail(std::string_view why) {
    std::println(stderr, "sweeppp-web-fixtures: {}", why);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return fail("usage: sweeppp-web-fixtures <directory>");
    }
    const std::filesystem::path directory = argv[1];
    std::filesystem::create_directories(directory);
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / std::format("sweeppp-fixtures-{}", monotonicNs());

    registerReferenceFftBackend();
    registerBuiltinSdrDevices();
    auto backend = FftBackendManager::instance().acquire("reference");
    if (!backend) {
        return fail(backend.error().describe());
    }
    FrameBus output;
    EventBus events;
    Telemetry telemetry;
    LocalInstrument instrument(output, events, telemetry, InstrumentPaths::under(root), **backend);
    auto device = SdrDeviceManager::instance().open("synthetic", "");
    if (!device) {
        return fail(device.error().describe());
    }
    instrument.adoptDevice(std::move(*device));
    (void)instrument.applySweepPlan(planOver(100e6, 140e6));

    remote::RemoteServer server(instrument, output, events, telemetry,
                                remote::ServerConfig{.port = 0, .serverName = "fixtures"});
    if (auto started = server.start(); !started) {
        return fail(started.error().describe());
    }

    auto socket = net::TcpSocket::connect("127.0.0.1", server.port(), 2000ms);
    if (!socket) {
        return fail(socket.error().describe());
    }
    auto channel =
        remote::openClientChannel(std::move(*socket), {},
                                  remote::Hello{.protocolVersion = remote::kProtocolVersion,
                                                .software = "fixtures",
                                                .kind = std::string(remote::client::kWeb),
                                                .name = "fixtures"},
                                  Clock::now() + 3s);
    if (!channel) {
        return fail(channel.error().describe());
    }

    // What the page will be fed: the server's stream header, then every byte
    // the channel carried.
    std::vector<std::byte> stream;
    sweeps::encodeStreamHeader(stream, sweeps::StreamHeader{});
    const std::size_t headerBytes = stream.size();

    const auto sendCommand = [&](const std::vector<std::byte>& bytes) {
        (void)channel->sendAll({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    };
    sendCommand(commandBytes(1, remote::op::kStart));

    // Two plans, so the stream closes a segment and opens another.
    std::size_t frames = 0;
    bool replanned = false;
    std::vector<std::byte> chunk(65536);
    sweeps::RecordFramer counter(remote::kMaxServerRecordBytes);
    const auto deadline = Clock::now() + 20s;
    while (frames < 80 && Clock::now() < deadline) {
        auto readable = channel->waitReadable(50ms);
        if (!readable || !*readable) {
            continue;
        }
        auto got = channel->receive({reinterpret_cast<std::uint8_t*>(chunk.data()), chunk.size()});
        if (!got || *got == 0) {
            return fail("the server closed the stream");
        }
        stream.insert(stream.end(), chunk.begin(),
                      chunk.begin() + static_cast<std::ptrdiff_t>(*got));
        counter.feed(chunk.data(), *got);
        sweeps::StreamRecord record;
        while (counter.next(record).value_or(false)) {
            if (record.header.type == static_cast<std::uint16_t>(sweeps::RecordType::PluginData)) {
                if (auto message = remote::decodeMessage(record);
                    message && message->name == remote::msg::kFrame) {
                    ++frames;
                }
            }
        }
        if (frames >= 40 && !replanned) {
            sweeps::Metadata args;
            args.setHash("plan", remote::encodePlan(planOver(400e6, 430e6)));
            sendCommand(commandBytes(2, remote::op::kApplySweepPlan, std::move(args)));
            replanned = true;
        }
    }
    channel->close();
    server.stop();
    if (frames < 80) {
        return fail(std::format("only {} frames arrived", frames));
    }

    // What the C++ decoder makes of the same bytes.
    json expected{{"frames", json::array()}, {"states", json::array()}};
    sweeps::RecordFramer framer(remote::kMaxServerRecordBytes);
    framer.feed(stream.data() + headerBytes, stream.size() - headerBytes);
    remote::FrameMirror mirror;
    std::size_t telemetryRecords = 0;
    std::size_t eventRecords = 0;
    sweeps::StreamRecord record;
    while (framer.next(record).value_or(false)) {
        using sweeps::RecordType;
        const auto type = static_cast<RecordType>(record.header.type);
        if (type == RecordType::SegmentOpen || type == RecordType::Tile ||
            type == RecordType::SegmentClose) {
            if (auto applied = mirror.apply(record); !applied) {
                return fail(applied.error().describe());
            }
            continue;
        }
        if (type == RecordType::Telemetry) {
            ++telemetryRecords;
            continue;
        }
        if (type == RecordType::Event) {
            ++eventRecords;
            continue;
        }
        if (type != RecordType::PluginData) {
            continue;
        }
        auto message = remote::decodeMessage(record);
        if (!message) {
            return fail(message.error().describe());
        }
        if (message->name == remote::msg::kWelcome) {
            expected["serverName"] = remote::Welcome::from(message->body).serverName;
        } else if (message->name == remote::msg::kFrame) {
            const remote::FrameCommit commit = remote::FrameCommit::from(message->body);
            auto frame = mirror.commit(commit);
            if (!frame) {
                return fail(frame.error().describe());
            }
            json levels = json::array();
            for (const float level : (*frame)->binsDbfs) {
                levels.push_back(std::stod(std::format("{:.9g}", level)));
            }
            expected["frames"].push_back(json{{"segmentId", commit.segmentId},
                                              {"line", commit.line},
                                              {"sequence", commit.sequence},
                                              {"passComplete", commit.passComplete},
                                              {"startHz", (*frame)->startHz},
                                              {"binWidthHz", (*frame)->binWidthHz},
                                              {"levels", std::move(levels)}});
        } else if (message->name == remote::msg::kState) {
            const remote::State state = remote::State::from(message->body);
            std::vector<std::byte> sections;
            state.sections.encode(sections);
            json names = json::array();
            for (const auto& [name, value] : state.sections) {
                names.push_back(name);
            }
            expected["states"].push_back(json{{"ackSeq", state.ackSeq},
                                              {"sections", std::move(names)},
                                              {"encoded", hexOf(sections)}});
        }
    }
    expected["telemetryRecords"] = telemetryRecords;
    expected["eventRecords"] = eventRecords;
    expected["finalPlanLowestHz"] = 400e6;

    std::ofstream(directory / "stream.bin", std::ios::binary)
        .write(reinterpret_cast<const char*>(stream.data()),
               static_cast<std::streamsize>(stream.size()));
    std::ofstream(directory / "expected.json") << expected.dump();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::println("wrote {} bytes, {} frames, {} states", stream.size(), expected["frames"].size(),
                 expected["states"].size());
    return 0;
}
