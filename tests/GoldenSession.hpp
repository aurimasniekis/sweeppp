// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <sweeppp/fft/Window.hpp>
#include <sweeppp/history/SessionRecorder.hpp>
#include <sweeppp/pipeline/SpectrumFrame.hpp>

namespace sweeppp::golden {

/// Everything about the reference session that must not drift.
///
/// The golden file is the tripwire for the whole extraction: every step that
/// touches code feeding file bytes is checked against it, so a change in
/// manifest emission, tile ordering, header layout or quantisation shows up as
/// a byte diff rather than as a bug discovered months later in a saved
/// recording.
///
/// Determinism is the entire requirement here. Nothing in this fixture may
/// read a clock, a filesystem or the build version.
inline constexpr std::uint64_t kCreatedWallNs = 1'770'000'000'000'000'000ULL;
inline constexpr const char* kApplicationVersion = "0.0.0-golden";
inline constexpr const char* kSessionName = "golden";
inline constexpr const char* kNotes = "frozen reference session";

/// Segment 0: wide enough for two frequency blocks, the second one partial, so
/// the freqBlock split and a short trailing tile are both exercised.
inline constexpr std::uint32_t kWideBins = 1200;
inline constexpr std::uint32_t kWideLines = 40;

/// Segment 1: narrow and long, so timeBlock advances past zero and the pyramid
/// reaches LOD 2.
inline constexpr std::uint32_t kNarrowBins = 128;
inline constexpr std::uint32_t kNarrowLines = 300;

inline constexpr std::uint64_t kFirstMonotonicNs = 1'000'000'000ULL;
inline constexpr std::uint64_t kLineIntervalNs = 20'000'000ULL;

/// A frame whose every field is a pure function of its index.
[[nodiscard]] inline SpectrumFramePtr goldenFrame(std::uint64_t index, std::uint64_t timeNs,
                                                  double startHz, double binWidthHz,
                                                  std::uint32_t bins, std::uint32_t fftSize) {
    auto frame = std::make_shared<SpectrumFrame>();
    frame->sequence = index;
    frame->hostTimeNs = timeNs;
    frame->wallTimeNs = kCreatedWallNs + (timeNs - kFirstMonotonicNs);
    frame->startHz = startHz;
    frame->binWidthHz = binWidthHz;
    frame->binsDbfs.resize(bins);

    // A deterministic ramp with a moving narrow peak: it gives every tile a
    // different level range (so originDb varies per tile) and gives max-hold
    // decimation something that must survive to LOD 2.
    for (std::uint32_t bin = 0; bin < bins; ++bin) {
        const auto phase = static_cast<float>((bin + index * 7U) % 97U);
        frame->binsDbfs[bin] = -100.0F + phase * 0.25F;
    }
    frame->binsDbfs[(index * 13U) % bins] = -18.5F;

    frame->config.centerHz = startHz + binWidthHz * static_cast<double>(bins) * 0.5;
    frame->config.spanHz = binWidthHz * static_cast<double>(bins);
    frame->config.sampleRate = 20e6;
    frame->config.fftSize = fftSize;
    frame->config.window = WindowType::Hann;
    frame->config.windowBeta = 8.6;
    frame->config.windowEnbw = 1.5;
    frame->config.rbwHz = 20e6 * 1.5 / static_cast<double>(fftSize);
    frame->config.referenceLevelDbm = -30.0;
    frame->config.dbfsToDbmOffset = -12.5;
    frame->config.deviceId = "golden-0";
    frame->config.deviceLabel = "Golden reference device";
    frame->config.gains.emplace_back("lna", 24.0);
    frame->config.gains.emplace_back("vga", 16.0);
    return frame;
}

[[nodiscard]] inline session::RecorderConfig goldenWriterConfig(std::uint32_t binsPerLine) {
    session::RecorderConfig config;
    config.binsPerLine = binsPerLine;
    config.sessionName = kSessionName;
    config.notes = kNotes;
    config.applicationVersion = kApplicationVersion;
    config.createdWallNs = kCreatedWallNs;
    // A free-space check that trips would change the file. Disabled rather than
    // trusted to the machine the suite happens to run on.
    config.minFreeBytes = 0;
    return config;
}

/// Writes the reference session. Identical bytes on every run and every host.
[[nodiscard]] inline Status writeGoldenSession(const std::filesystem::path& path) {
    auto writer = session::SessionRecorder::create(path, goldenWriterConfig(kWideBins));
    if (!writer) {
        return std::unexpected(writer.error());
    }

    std::uint64_t timeNs = kFirstMonotonicNs;
    std::uint64_t index = 0;

    const auto phase = [&](std::uint32_t lines, double startHz, double binWidthHz,
                           std::uint32_t bins, std::uint32_t fftSize) {
        for (std::uint32_t i = 0; i < lines; ++i) {
            (*writer)->onFrame(goldenFrame(index, timeNs, startHz, binWidthHz, bins, fftSize));
            // Flushed every line: the writer is asynchronous here, and a
            // deterministic file requires every frame to be processed, in
            // order, with nothing dropped.
            (*writer)->flush();
            ++index;
            timeNs += kLineIntervalNs;
        }
    };

    phase(kWideLines, 90e6, 9765.625, kWideBins, 4096);

    // A grid change: new segment, and a `reason` string built from the
    // frequency formatter -- so a change in that formatter moves file bytes.
    phase(kNarrowLines, 400e6, 19531.25, kNarrowBins, 8192);

    // The same two events the library's own fixture appends, field for field --
    // one file pins both writers, so these must agree exactly.
    (*writer)->recordEvent(session::SessionEvent::of(
        session::SessionEvent::Kind::Annotation, kFirstMonotonicNs, kCreatedWallNs,
        session::AnnotationData{.text = "golden", .startHz = 95e6, .stopHz = 96e6}));
    (*writer)->recordEvent(session::SessionEvent::of(
        session::SessionEvent::Kind::Marker, kFirstMonotonicNs + 1000, kCreatedWallNs + 1000,
        session::MarkerData{.label = "m1", .frequencyHz = 401e6, .levelDbm = -42.25}));

    return (*writer)->close();
}

/// The committed reference file, which lives with the library's conformance
/// suite -- one file, checked from both sides. Supplied by CMake so the test
/// does not have to guess at the working directory.
[[nodiscard]] inline std::filesystem::path goldenPath() {
    return std::filesystem::path(SWEEPSFILE_TEST_DATA_DIR) / "v1-golden.sweeps";
}

} // namespace sweeppp::golden
