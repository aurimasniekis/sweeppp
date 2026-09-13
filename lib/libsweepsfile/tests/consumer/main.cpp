// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

/// Reads a session with nothing but the installed package.
///
/// Deliberately sets no C++ standard of its own: the exported target must carry
/// its own requirement, or a project that never thought about it fails to
/// compile these headers.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sweeps/Clock.hpp>
#include <sweeps/FileFormat.hpp>
#include <sweeps/SessionReader.hpp>
#include <sweeps/SessionWriter.hpp>
#include <sweeps/Text.hpp>

int main(int argc, char** argv) {
    // Round-trip through the writer, so both halves of the interface are
    // exercised without needing a file to be shipped alongside.
    const std::string path = argc > 1 ? argv[1] : "sweeps-consumer.sweeps";

    sweeps::WriterConfig config;
    config.binsPerLine = 256;
    config.sessionName = "consumer";
    config.createdWallNs = 1'700'000'000'000'000'000ULL;
    config.minFreeBytes = 0;

    auto writer = sweeps::SessionWriter::create(path, config);
    if (!writer) {
        std::fprintf(stderr, "write failed: %s\n", writer.error().describe().c_str());
        return 1;
    }

    sweeps::AcquisitionConfig acquisition;
    acquisition.centerHz = 100e6;
    acquisition.spanHz = 2.5e6;
    acquisition.sampleRate = 2.5e6;
    acquisition.fftSize = 1024;
    acquisition.window = sweeps::WindowType::Hann;
    acquisition.deviceId = "consumer-0";

    std::vector<float> bins(256, -95.0F);
    for (int line = 0; line < 300; ++line) {
        bins[static_cast<std::size_t>(line) % bins.size()] = -20.0F;

        sweeps::FrameView frame;
        frame.bins = bins.data();
        frame.count = bins.size();
        frame.startHz = 98.75e6;
        frame.binWidthHz = 2.5e6 / 256.0;
        frame.monotonicNs = 1'000'000'000ULL + static_cast<std::uint64_t>(line) * 20'000'000ULL;
        frame.wallNs = frame.monotonicNs;
        frame.config = &acquisition;

        if (auto written = (*writer)->writeFrame(frame); !written) {
            std::fprintf(stderr, "frame failed: %s\n", written.error().describe().c_str());
            return 1;
        }
        bins[static_cast<std::size_t>(line) % bins.size()] = -95.0F;
    }

    if (auto closed = (*writer)->close(); !closed) {
        std::fprintf(stderr, "close failed: %s\n", closed.error().describe().c_str());
        return 1;
    }

    auto reader = sweeps::SessionReader::open(path);
    if (!reader) {
        std::fprintf(stderr, "open failed: %s\n", reader.error().describe().c_str());
        return 1;
    }

    auto records = (*reader)->verify();
    if (!records) {
        std::fprintf(stderr, "verify failed: %s\n", records.error().describe().c_str());
        return 1;
    }

    const sweeps::SessionSummary& summary = (*reader)->summary();
    std::printf("sweeps %s | %s | %llu records, %llu tiles, %s .. %s | created %s\n",
                summary.versionString().c_str(), sweeps::formatBytes(summary.fileBytes).c_str(),
                static_cast<unsigned long long>(*records),
                static_cast<unsigned long long>(summary.totalTiles),
                sweeps::formatFrequencyShort(summary.lowestHz).c_str(),
                sweeps::formatFrequencyShort(summary.highestHz).c_str(),
                sweeps::formatWallClockIso8601(summary.createdWallNs).c_str());

    if (summary.totalTiles == 0 || (*reader)->segments().size() != 1) {
        std::fprintf(stderr, "the round-tripped session is not what was written\n");
        return 1;
    }

    std::remove(path.c_str());
    return 0;
}
