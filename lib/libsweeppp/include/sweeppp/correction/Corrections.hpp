// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/sdr/SdrDeviceInfo.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace sweeppp {

class ISdrDevice;

/// Which receiver corrections the pipeline applies to every frame.
///
/// Switches only. What they apply -- the learned floor and the spur list --
/// is a `CorrectionSet`, kept per radio in its own file; these travel in a
/// profile because they are a choice about the job, not a fact about the
/// bench.
struct CorrectionSettings {
    /// Subtracts each block's mean I/Q before the transform, removing the
    /// spike a direct-conversion tuner leaks at its own LO. A carrier sitting
    /// exactly on the LO goes with it.
    bool dcRemoval = true;
    /// Subtracts the learned floor shape, flattening the hump the front end
    /// puts around its LO. Needs a calibration whose context still matches.
    bool flatten = true;
    /// Replaces the bins under each learned spur with a line between their
    /// neighbours.
    bool spurMask = true;
    /// While sweeping, keeps looking for LO-offset spurs that the mask does
    /// not yet cover and adds them for this session.
    bool autoSpurs = false;

    /// The four switches as one byte, so the pipeline can read them with a
    /// single atomic load on its hot path.
    [[nodiscard]] std::uint8_t toBits() const noexcept;
    [[nodiscard]] static CorrectionSettings fromBits(std::uint8_t bits) noexcept;
};

/// Where a spur sits.
enum class SpurKind : std::uint8_t {
    /// At a fixed offset from the LO: it moves with the tuning, so it is the
    /// same local bin at every sweep step.
    LoOffset,
    /// At a fixed frequency: a reference-clock harmonic, say. It lands in a
    /// different local bin at every step.
    Absolute,
};

[[nodiscard]] std::string_view toString(SpurKind kind) noexcept;

struct SpurEntry {
    SpurKind kind = SpurKind::LoOffset;
    /// Offset from the LO, or the absolute frequency, by `kind`.
    double hz = 0.0;
    double widthHz = 0.0;
    /// Found while sweeping rather than learned. Never saved.
    bool automatic = false;
};

/// The receiver's own floor, in dB against LO offset.
///
/// Sampled uniformly over [-fs/2, +fs/2) at the sample rate it was learned
/// at, independent of any FFT size: it is resampled onto whatever bin width
/// the transform has when it is applied.
struct FloorShape {
    double sampleRate = 0.0;
    std::vector<float> levelDb;

    [[nodiscard]] bool empty() const noexcept { return levelDb.empty(); }

    /// The level the shape is flattened *to*: a correction is a difference
    /// from this, so the overall level of a spectrum does not move.
    [[nodiscard]] float medianDb() const;
};

/// What a floor shape is only valid for.
///
/// Gain, filter bandwidth, sample rate -- anything that moves the floor or
/// redefines the grid -- is captured with the shape, so a shape learned at
/// one gain is never quietly subtracted at another. The centre frequency is
/// deliberately absent: the shape is keyed to LO offset precisely so it holds
/// across tuning.
struct CalibrationContext {
    std::string driver;
    std::string deviceId;
    bool dcRemoval = true;
    /// Every `calibrationAffecting` or `gridAffecting` parameter except
    /// `center_hz`, as `toString(SdrValue)` renders it, sorted by key.
    std::vector<std::pair<std::string, std::string>> parameters;

    /// The first key on which the two differ, or empty when they agree. A
    /// key present on one side only counts as differing.
    [[nodiscard]] std::string firstDifference(const CalibrationContext& other) const;
};

/// The context a calibration taken on `device`, as it is set right now, is
/// bound to. The one definition of which parameters count, for both the
/// learn that records it and the check that compares it.
[[nodiscard]] CalibrationContext calibrationContextFor(const ISdrDevice& device, bool dcRemoval);

/// Everything learned about one radio.
struct CorrectionSet {
    CalibrationContext context;
    FloorShape floor;
    std::vector<SpurEntry> spurs;
    /// ISO 8601 wall-clock time of the learn that produced this.
    std::string learnedAt;

    [[nodiscard]] bool empty() const noexcept { return floor.empty() && spurs.empty(); }

    [[nodiscard]] static Result<CorrectionSet> load(const std::filesystem::path& path);

    /// Automatic spurs are left out: they were found against whatever was in
    /// the air at the time, and a saved list must hold only what was learned
    /// with the antenna off.
    [[nodiscard]] Status save(const std::filesystem::path& path) const;

    /// `Paths::calibrationDir() / <device key>.toml`, one file per radio.
    [[nodiscard]] static std::filesystem::path pathFor(const SdrDeviceInfo& info);
};

/// Per-grid work `applyCorrections` keeps between frames, so a frame costs one
/// pass over its bins and no allocation once the grid has been seen.
///
/// Keyed on the bin count and sample rate: an FFT-size change is a different
/// grid and rebuilds everything. Whoever owns it must also reset it when the
/// set it was built from is replaced.
struct CorrectionScratch {
    std::size_t binCount = 0;
    double sampleRate = 0.0;
    /// The floor correction per bin, already relative to the shape's median.
    std::vector<float> floorDelta;
    /// Bins under an LO-offset spur, which are the same at every centre, and
    /// whether there are any.
    std::vector<std::uint8_t> loMask;
    bool loMasked = false;
    /// Working mask for the frame in hand: `loMask` plus the absolute spurs
    /// that fall inside it.
    std::vector<std::uint8_t> mask;

    void reset() noexcept {
        binCount = 0;
        sampleRate = 0.0;
    }
};

/// Applies the floor and the spur mask to one frame's bins, in place.
///
/// `bins` are dBFS in ascending frequency, `centerHz` and `sampleRate` the
/// tuning they were taken at. The floor is subtracted first, then each masked
/// run is replaced by a straight line in dB between the nearest unmasked
/// measured bin on either side; a run with one side missing takes the other
/// side's level, and one with neither is left alone. Unmeasured bins are never
/// read as neighbours and never written.
///
/// `settings.dcRemoval` and `autoSpurs` are not this function's business:
/// the first acts before the transform and the second decides what goes into
/// `set`.
///
/// Allocates once per grid, on the first frame that shows it; after that a
/// frame is one pass over its bins and nothing else.
void applyCorrections(std::span<float> bins, double centerHz, double sampleRate,
                      const CorrectionSet& set, CorrectionSettings settings,
                      CorrectionScratch& scratch);

} // namespace sweeppp
