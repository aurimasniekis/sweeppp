// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <sweeps/SessionWriter.hpp>
#include <utility>
#include <vector>

namespace sweeps::test {

/// The frozen reference session.
///
/// Two things are pinned by this fixture, and they are different claims:
///
///  * That the writer's output has not moved. Every later change to the
///    manifest, the header, tile ordering or quantisation is checked against
///    the committed bytes.
///  * That the *synchronous* writer reproduces, exactly, what the asynchronous
///    recorder that preceded it produced. Sweep++'s own suite runs the same
///    fixture through the recorder and compares against the same file, so the
///    two paths agreeing is a tested property rather than an assumption.
///
/// Determinism is the whole requirement. Nothing here may read a clock, a
/// filesystem or a build version.
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

/// Bins that are a pure function of the frame index.
inline void fillGoldenBins(std::vector<float>& bins, std::uint64_t index, std::uint32_t count) {
    bins.resize(count);
    // A deterministic ramp with a moving narrow peak: it gives every tile a
    // different level range (so originDb varies per tile) and gives max-hold
    // decimation something that must survive to LOD 2.
    for (std::uint32_t bin = 0; bin < count; ++bin) {
        const auto phase = static_cast<float>((bin + index * 7U) % 97U);
        bins[bin] = -100.0F + phase * 0.25F;
    }
    bins[(index * 13U) % count] = -18.5F;
}

inline void fillGoldenConfig(AcquisitionConfig& config, double startHz, double binWidthHz,
                             std::uint32_t bins, std::uint32_t fftSize) {
    config.centerHz = startHz + binWidthHz * static_cast<double>(bins) * 0.5;
    config.spanHz = binWidthHz * static_cast<double>(bins);
    config.sampleRate = 20e6;
    config.fftSize = fftSize;
    config.window = WindowType::Hann;
    config.windowBeta = 8.6;
    config.windowEnbw = 1.5;
    config.rbwHz = 20e6 * 1.5 / static_cast<double>(fftSize);
    config.referenceLevelDbm = -30.0;
    config.dbfsToDbmOffset = -12.5;
    config.deviceId = "golden-0";
    config.deviceLabel = "Golden reference device";
    config.gains.clear();
    config.gains.emplace_back("lna", 24.0);
    config.gains.emplace_back("vga", 16.0);
}

[[nodiscard]] inline WriterConfig goldenWriterConfig(std::uint32_t binsPerLine) {
    WriterConfig config;
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

[[nodiscard]] inline SessionEvent goldenAnnotation() {
    AnnotationData body;
    body.text = "golden";
    body.startHz = 95e6;
    body.stopHz = 96e6;
    return SessionEvent::of(SessionEvent::Kind::Annotation, kFirstMonotonicNs, kCreatedWallNs,
                            std::move(body));
}

[[nodiscard]] inline SessionEvent goldenMarker() {
    MarkerData body;
    body.label = "m1";
    body.frequencyHz = 401e6;
    body.levelDbm = -42.25;
    return SessionEvent::of(SessionEvent::Kind::Marker, kFirstMonotonicNs + 1000,
                            kCreatedWallNs + 1000, std::move(body));
}

/// Writes the reference session. Identical bytes on every run and every host.
[[nodiscard]] inline Status writeGoldenSession(const std::filesystem::path& path) {
    auto writer = SessionWriter::create(path, goldenWriterConfig(kWideBins));
    if (!writer) {
        return unexpected<Error>(writer.error());
    }

    std::uint64_t timeNs = kFirstMonotonicNs;
    std::uint64_t index = 0;

    // A segment-boundary event is queued when the segment opens and written at
    // the head of the *next* frame, after that frame's tiles. That deferral is
    // Sweep++'s recorder behaviour, and reproducing it here is what lets one
    // file pin both the recorder path and this one.
    std::vector<SessionEvent> deferred;

    Status status = ok();

    const auto phase = [&](std::uint32_t lines, double startHz, double binWidthHz,
                           std::uint32_t bins, std::uint32_t fftSize) {
        std::vector<float> values;
        AcquisitionConfig config;
        fillGoldenConfig(config, startHz, binWidthHz, bins, fftSize);

        for (std::uint32_t i = 0; i < lines && status.has_value(); ++i) {
            for (const SessionEvent& event : deferred) {
                if (auto written = (*writer)->recordEvent(event); !written) {
                    status = written;
                    return;
                }
            }
            deferred.clear();

            fillGoldenBins(values, index, bins);

            FrameView frame;
            frame.bins = values.data();
            frame.count = values.size();
            frame.startHz = startHz;
            frame.binWidthHz = binWidthHz;
            frame.monotonicNs = timeNs;
            frame.wallNs = kCreatedWallNs + (timeNs - kFirstMonotonicNs);
            frame.config = &config;

            auto outcome = (*writer)->writeFrame(frame);
            if (!outcome) {
                status = unexpected<Error>(outcome.error());
                return;
            }

            if (outcome->segmentOpened) {
                SegmentBoundaryData body;
                body.reason = outcome->reason;
                deferred.push_back(SessionEvent::of(SessionEvent::Kind::SegmentBoundary,
                                                    frame.monotonicNs, frame.wallNs,
                                                    std::move(body)));
            }

            ++index;
            timeNs += kLineIntervalNs;
        }
    };

    phase(kWideLines, 90e6, 9765.625, kWideBins, 4096);

    // A grid change: new segment, and a `reason` string built from the
    // frequency formatter -- so a change in that formatter moves file bytes.
    phase(kNarrowLines, 400e6, 19531.25, kNarrowBins, 8192);

    if (!status) {
        return status;
    }

    // Whatever is still queued is written at close, ahead of the index.
    deferred.push_back(goldenAnnotation());
    deferred.push_back(goldenMarker());
    for (const SessionEvent& event : deferred) {
        if (auto written = (*writer)->recordEvent(event); !written) {
            return written;
        }
    }

    return (*writer)->close();
}

/// Where the committed reference file lives. Supplied by CMake so the test does
/// not have to guess at the working directory.
[[nodiscard]] inline std::filesystem::path goldenPath() {
    return std::filesystem::path(SWEEPSFILE_TEST_DATA_DIR) / "v1-golden.sweeps";
}

} // namespace sweeps::test
