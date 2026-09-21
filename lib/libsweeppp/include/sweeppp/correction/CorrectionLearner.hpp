// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/correction/Corrections.hpp"
#include "sweeppp/pipeline/SpectrumFrame.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace sweeppp {

/// What counts as a spur, and how the floor under it is estimated.
struct LearnParameters {
    /// The least a bin must stand above the running median of its neighbours.
    float spurThresholdDb = 6.0F;
    /// The threshold also scales with the scatter still left in the mean:
    /// this many robust standard deviations. A noise floor's per-bin mean
    /// over M frames scatters by about 5.6/sqrt(M) dB, so one pass of a
    /// stitched grid needs a tall spur and eight passes a modest one, and no
    /// number of bins makes noise pass.
    float sigmaMultiplier = 6.0F;
    /// Half the window the running median is taken over, in bins.
    std::size_t medianHalfWindow = 32;
    /// Widest run of raised bins still taken for a spur; anything broader is
    /// a hump and belongs to the floor.
    std::size_t maxSpurBins = 5;
    /// Stitched passes averaged before fixed-frequency spurs are read off a
    /// sweep. Each grid bin is measured once per pass, so this is what the
    /// scatter above is divided by.
    std::size_t absolutePasses = 8;
};

/// Accumulates per-bin statistics across frames and turns them into a floor
/// shape and a spur list.
///
/// Per local bin it keeps the running mean of the level in dB. Smoothed, that
/// is the floor; against its own smoothing it finds spurs. The mean in dB is
/// the statistic because it concentrates -- by 1/sqrt(frames) -- while
/// staying nearly indifferent to a signal that was in a few steps of a sweep:
/// thirty dB in one step of fifty moves it by half a dB. The minimum across
/// frames, the obvious alternative, does not concentrate at all: the minimum
/// of a chi-square floor scatters as much as one frame, so a twentieth of the
/// bins clear any fixed test and read as spurs.
///
/// One float and one count per bin, nothing per frame. Fed from the sweep
/// engine's step observer while sweeping, from the pipeline bus at a fixed
/// tune, and with completed passes for the stitched grid. `addFrame` runs on
/// the publishing thread and does one pass over the bins; everything else is
/// for the caller's own thread once the frames are in.
class CorrectionLearner {
public:
    /// Starts over for a grid. `sweeping` decides what `loSpurs` returns:
    /// offsets from the LO across a sweep, absolute frequencies otherwise --
    /// a fixed tune, or a stitched grid begun with the grid's own span as
    /// `sampleRate`.
    void begin(std::size_t binCount, double sampleRate, bool sweeping);

    /// Folds one frame in. A frame of the wrong size is ignored; unmeasured
    /// bins are skipped.
    void addFrame(std::span<const float> bins, double centerHz) noexcept;

    [[nodiscard]] std::size_t frameCount() const noexcept { return m_frames; }
    [[nodiscard]] std::size_t binCount() const noexcept { return m_mean.size(); }
    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] bool sweeping() const noexcept { return m_sweeping; }

    /// The running median of the per-bin mean, resampled to `points` samples
    /// over [-fs/2, +fs/2). Empty until a frame has been added.
    [[nodiscard]] FloorShape floorShape(std::size_t points = 2048,
                                        const LearnParameters& params = {}) const;

    /// Narrow runs standing above the floor in the per-bin mean. LO-offset
    /// entries when sweeping; absolute ones, placed against the last centre
    /// seen, when not.
    [[nodiscard]] std::vector<SpurEntry> loSpurs(const LearnParameters& params = {}) const;

private:
    std::vector<float> m_mean;
    std::vector<std::uint32_t> m_count;
    std::size_t m_frames = 0;
    double m_sampleRate = 0.0;
    double m_centerHz = 0.0;
    bool m_sweeping = true;
};

} // namespace sweeppp
