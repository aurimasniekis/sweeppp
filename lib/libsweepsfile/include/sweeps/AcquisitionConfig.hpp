// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#pragma once

#include "sweeps/WindowType.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sweeps {

/// Everything that produced a measurement.
///
/// **This is the structure the format's forward compatibility rests on.**
/// Without it a recording is uninterpretable: bins are just numbers unless you
/// know the centre, span, sample rate, FFT size, window and gain that produced
/// them. Every `SegmentOpen` record carries one in full, so tiles written under
/// any past configuration stay interpretable without reference to anything
/// outside the file.
///
/// It is also what makes a session *replayable*: on playback the RBW, FFT size
/// and gain readouts change at the same moments they did live, because each
/// segment says what they were.
struct AcquisitionConfig {
    double centerHz = 0.0;
    double spanHz = 0.0;
    double sampleRate = 0.0;

    std::uint32_t fftSize = 0;
    WindowType window = WindowType::Hann;
    double windowBeta = 8.6;
    /// Equivalent noise bandwidth in bins, carried explicitly so a reader does
    /// not have to know how to regenerate the window to interpret the RBW.
    double windowEnbw = 1.5;
    double overlap = 0.0;

    /// sampleRate * windowEnbw / fftSize.
    double rbwHz = 0.0;

    /// Gain stages, by parameter key. Free-form because it must describe any
    /// device without the format knowing which radios exist.
    std::vector<std::pair<std::string, double>> gains;

    double referenceLevelDbm = 0.0;
    /// Correction from dBFS to dBm for this device and gain setting. Applied at
    /// display time rather than baked into the stored data, so a later
    /// calibration fix can be applied retroactively to old sessions.
    double dbfsToDbmOffset = 0.0;

    std::string deviceId;
    std::string deviceLabel;

    /// True when this configuration's grid differs from another's in a way that
    /// requires a new session segment.
    [[nodiscard]] bool gridDiffers(const AcquisitionConfig& other) const noexcept;
};

} // namespace sweeps
