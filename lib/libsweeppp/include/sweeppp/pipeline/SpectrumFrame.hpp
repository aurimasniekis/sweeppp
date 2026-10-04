// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <sweeps/AcquisitionConfig.hpp>
#include <sweeps/FileFormat.hpp>
#include <vector>

namespace sweeppp {

/// Snapshot of everything that produced a frame.
///
/// **This is the single most important structure in the project's forward
/// compatibility.** Without it, a recording is uninterpretable: bins are just
/// numbers unless you know the centre, span, sample rate, FFT size, window and
/// gain that produced them, and an alert cannot reference an exact moment
/// unless the moment carries its own configuration.
///
/// It is defined by libsweepsfile rather than here, because it is written into
/// every `SegmentOpen` record -- it is part of the file format, and a session
/// reader with no pipeline attached still needs it.
using AcquisitionConfig = sweeps::AcquisitionConfig;

/// The level a bin nothing has measured carries.
///
/// Not zero and not the noise floor: a bin that was never looked at must stay
/// distinguishable from one measured as quiet, or every stage downstream --
/// the trace, the waterfall, the recorder, a plugin's detector -- invents a
/// noise floor for frequencies the radio never visited. A discontinuous sweep
/// plan produces these by the thousand, in the bins between its spans.
///
/// Taken from the file format so the float and the stored byte cannot drift
/// apart: this is exactly what byte 0 of a tile dequantises to.
inline constexpr float kUnmeasuredDbfs = static_cast<float>(sweeps::kUnmeasuredDb);

/// Whether a level is a reading at all.
///
/// A floor rather than an equality, because smoothing, averaging or a resample
/// that touches one unmeasured bin lands near the sentinel rather than on it.
/// Shared because every reader of a level has to make the same distinction: a
/// trace stops drawing there, and a readout that printed "-200.0 dBFS" would
/// be reporting a measurement nobody took.
[[nodiscard]] inline bool measured(float levelDb) noexcept {
    return sweeps::isMeasuredDb(static_cast<double>(levelDb));
}

/// One spectrum, ready to display, store or transmit.
///
/// Bins are dBFS in ascending frequency order (already fft-shifted). Frames
/// are immutable once published and shared by `shared_ptr` across consumers,
/// so a slow consumer holding one costs memory but never blocks the producer.
struct SpectrumFrame {
    /// Monotonic per stream. A gap tells a consumer it dropped frames, which
    /// is how each consumer keeps its own honest drop count.
    std::uint64_t sequence = 0;

    std::uint64_t hostTimeNs = 0;   ///< Monotonic, for intervals.
    std::uint64_t wallTimeNs = 0;   ///< Unix epoch, for display and filenames.
    std::uint64_t deviceTimeNs = 0; ///< 0 when the device provides none.

    /// Sweep pass this frame belongs to, and the step within it. A full-span
    /// sweep emits many partial frames per pass -- the display updates as each
    /// step lands rather than waiting for the pass to finish.
    std::uint64_t sweepPass = 0;
    std::uint32_t sweepStep = 0;
    bool passComplete = false;

    /// Absolute frequency of the first bin, and the spacing between bins. A
    /// partial sweep frame covers only its step's slice, so these are not
    /// derivable from the config alone.
    double startHz = 0.0;
    double binWidthHz = 0.0;

    std::vector<float> binsDbfs;

    AcquisitionConfig config;

    /// How many FFTs were averaged into this frame.
    std::uint32_t averageCount = 1;

    /// Fraction of input samples that were clipping. Non-zero means every
    /// level here is suspect, and the UI says so rather than showing a
    /// confident wrong number.
    float clippedFraction = 0.0F;

    /// The bins that differ from the frame published before this one, as
    /// [dirtyFirstBin, dirtyEndBin), when the publisher knows. A sweep's
    /// partial frame changes one step's worth of a wide grid, and a consumer
    /// passing frames on can leave the rest alone.
    bool dirtyKnown = false;
    std::size_t dirtyFirstBin = 0;
    std::size_t dirtyEndBin = 0;

    [[nodiscard]] std::size_t binCount() const noexcept { return binsDbfs.size(); }
    [[nodiscard]] double stopHz() const noexcept {
        return startHz + binWidthHz * static_cast<double>(binsDbfs.size());
    }
    [[nodiscard]] double centerHz() const noexcept { return (startHz + stopHz()) * 0.5; }
};

/// Frames are shared, never copied, once published.
using SpectrumFramePtr = std::shared_ptr<const SpectrumFrame>;

} // namespace sweeppp
