// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <sweeps/Clock.hpp>
#include <sweeps/SessionReader.hpp>
#include <sweeps/SessionWriter.hpp>
#include <system_error>
#include <vector>

namespace sweeps::test {

class ScopedTempDir {
public:
    ScopedTempDir()
        : m_path(std::filesystem::temp_directory_path() /
                 ("sweepsfile-" + std::to_string(monotonicNs()))) {
        std::filesystem::create_directories(m_path);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    [[nodiscard]] std::filesystem::path file(const std::string& name) const {
        return m_path / name;
    }

private:
    std::filesystem::path m_path;
};

/// Owns the bins a FrameView points at.
///
/// The writer's input is a view over somebody else's buffer, so a test that
/// built one from a temporary would be handing it a dangling pointer.
struct TestFrame {
    std::vector<float> bins;
    AcquisitionConfig config;
    double startHz = 0.0;
    double binWidthHz = 0.0;
    std::uint64_t monotonicNs = 0;
    std::uint64_t wallNs = 0;

    [[nodiscard]] FrameView view() const {
        FrameView frame;
        frame.bins = bins.data();
        frame.count = bins.size();
        frame.startHz = startHz;
        frame.binWidthHz = binWidthHz;
        frame.monotonicNs = monotonicNs;
        frame.wallNs = wallNs;
        frame.config = &config;
        return frame;
    }
};

/// A frame with a controllable noise floor and an optional burst, so a test can
/// assert that a known transient survives.
[[nodiscard]] inline TestFrame makeFrame(std::uint64_t timeNs, double startHz, double binWidthHz,
                                         std::size_t bins, float floorDb,
                                         std::size_t burstBin = static_cast<std::size_t>(-1),
                                         float burstDb = 0.0F, std::uint32_t fftSize = 4096,
                                         double sampleRate = 20e6) {
    TestFrame frame;
    frame.bins.assign(bins, floorDb);
    if (burstBin < bins) {
        frame.bins[burstBin] = burstDb;
    }
    frame.startHz = startHz;
    frame.binWidthHz = binWidthHz;
    frame.monotonicNs = timeNs;
    frame.wallNs = timeNs;

    frame.config.centerHz = startHz + binWidthHz * static_cast<double>(bins) * 0.5;
    frame.config.spanHz = binWidthHz * static_cast<double>(bins);
    frame.config.sampleRate = sampleRate;
    frame.config.fftSize = fftSize;
    frame.config.window = WindowType::Hann;
    frame.config.windowEnbw = 1.5;
    frame.config.rbwHz = sampleRate * 1.5 / static_cast<double>(fftSize);
    frame.config.deviceId = "test-0";
    frame.config.deviceLabel = "Test device";
    return frame;
}

struct WrittenSession {
    std::uint64_t firstNs = 0;
    std::uint64_t lastNs = 0;
    std::uint64_t burstNs = 0;
    std::size_t burstBin = 0;
    std::uint32_t lineCount = 0;
};

/// Writes a session, returning the timestamps used, so tests can query by time.
///
/// A plain loop, because the writer is synchronous: no pacing and no drop
/// accounting, so the fixture is deterministic rather than merely reliable.
[[nodiscard]] inline WrittenSession writeSession(const std::filesystem::path& path,
                                                 std::uint32_t lines, bool injectBurst,
                                                 WriterConfig config = {}) {
    config.binsPerLine = 2048;

    WrittenSession written;
    written.lineCount = lines;
    written.burstBin = 900;

    auto writer = SessionWriter::create(path, config);
    if (!writer) {
        return written;
    }

    constexpr std::uint64_t kLineIntervalNs = 20'000'000; // 50 lines/s
    const std::uint64_t start = 1'000'000'000;

    // A single-line burst placed deliberately mid-session: it is the transient
    // that max-hold decimation must preserve at every LOD level, and that mean
    // decimation would erase.
    const std::uint32_t burstLine = lines / 2;

    for (std::uint32_t i = 0; i < lines; ++i) {
        const std::uint64_t timeNs = start + i * kLineIntervalNs;
        const bool burst = injectBurst && i == burstLine;
        if (burst) {
            written.burstNs = timeNs;
        }

        const TestFrame frame =
            makeFrame(timeNs, 90e6, 9765.625, 2048, -90.0F,
                      burst ? written.burstBin : static_cast<std::size_t>(-1), -20.0F);
        (void)(*writer)->writeFrame(frame.view());

        if (i == 0) {
            written.firstNs = timeNs;
        }
        written.lastNs = timeNs;
    }

    (void)(*writer)->close();
    return written;
}

} // namespace sweeps::test
