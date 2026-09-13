// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/core/Toml.hpp"
#include "sweeppp/fft/Window.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace sweeppp {

/// One contiguous frequency range to cover.
struct SweepSegment {
    double startHz = 0.0;
    double stopHz = 0.0;

    /// Dwell for this range; zero inherits the plan's.
    ///
    /// The only per-segment override. RBW and sample rate belong to the plan,
    /// because every step stitches into a single grid of one bin width.
    double dwellSeconds = 0.0;

    [[nodiscard]] double spanHz() const noexcept { return stopHz - startHz; }
    [[nodiscard]] bool valid() const noexcept { return stopHz > startHz; }
};

/// How to trade sweep speed against resolution.
enum class SweepMode : std::uint8_t {
    /// Throughput: minimal dwell, no averaging, wider steps. Finds a signal.
    Fast,
    /// Resolution: more averaging and more overlap. Measures it.
    Detail,
};

[[nodiscard]] std::string_view toString(SweepMode mode) noexcept;
[[nodiscard]] Result<SweepMode> sweepModeFromString(std::string_view name);

/// A sweep job.
///
/// Segments may be discontinuous -- "1-2 GHz and 5-6 GHz" is one plan, not
/// two. That matters because a single plan produces one coherent frequency
/// grid and one sweep pass, so the display and the session file treat it as a
/// unit rather than as two interleaved sweeps.
struct SweepPlan {
    std::string name;
    std::vector<SweepSegment> segments;

    SweepMode mode = SweepMode::Fast;

    /// Target resolution bandwidth. FFT size is derived from it and the sample
    /// rate, then snapped to what the backend accepts.
    double rbwHz = 100e3;

    /// Sample rate for each tune step; also the raw step width before cropping.
    double sampleRate = 20e6;

    /// Fraction of the sample rate that is actually usable.
    ///
    /// A tuner's output is not flat across its full bandwidth: there is a DC
    /// spike at the centre and filter roll-off at the edges. Using the full
    /// span would stitch the roll-off of one step against the roll-off of the
    /// next and put a periodic scallop across the whole sweep -- a classic
    /// artefact that looks like real structure.
    double usableBandwidthFraction = 0.75;

    /// Extra overlap between adjacent steps, as a fraction of the usable
    /// width. Guards against a small tuning error leaving a gap.
    double stepOverlap = 0.05;

    /// Bandwidth discarded around each step's own centre, as a fraction of the
    /// sample rate.
    ///
    /// A direct-conversion tuner leaks its local oscillator into its own
    /// output, so every step carries a spike at exactly the frequency it is
    /// tuned to. Cropping the band edges does not touch it -- it sits dead
    /// centre -- and stitching the steps together therefore prints a row of
    /// identical peaks spaced one step apart, which reads as real signal.
    ///
    /// Discarding the guard costs sweep rate, and not a little: the hole it
    /// leaves is in the middle of the step, where no amount of edge overlap
    /// reaches, so the only way to cover it is for a neighbouring step to
    /// measure that frequency well away from *its* centre. That caps the step
    /// advance at one sub-band, roughly halving the sweep rate. Set to zero to
    /// take the speed back and live with the spikes.
    double dcGuardFraction = 0.05;

    /// Time spent collecting at each step, beyond the retune settle.
    double dwellSeconds = 0.0;

    /// FFTs averaged per step. Detail mode raises it.
    std::uint32_t averageCount = 1;

    WindowType window = WindowType::Hann;
    double windowBeta = 8.6;

    /// FFT overlap within a step's samples.
    double fftOverlap = 0.0;

    /// How to combine bins where two steps overlap.
    enum class OverlapResolution : std::uint8_t {
        /// Keep whichever step measured the frequency best -- furthest from
        /// its own tuning and from its band edge.
        ///
        /// The right default, and not the obvious one. Overlap exists because
        /// a frequency near one step's edge is near the *middle* of its
        /// neighbour's, so one of the two measurements is always better. The
        /// receiver's own artefacts -- the noise hump around its tuning, the
        /// filter roll-off at the band edge -- live at fixed offsets from the
        /// LO, so choosing by position rejects them outright, while any rule
        /// that chooses by level cannot tell them from signal.
        Best,
        /// Take the larger value.
        ///
        /// Never hides a signal one step saw and the other missed, which reads
        /// as the safe choice for a detector -- but at a step boundary it
        /// systematically prefers the *contaminated* sample, because the
        /// receiver's own artefacts are louder than the spectrum underneath
        /// them. It prints a peak at every step centre.
        Max,
        /// Average them. Smoother noise floor, but dilutes a signal seen by
        /// only one step, and still admits half of any artefact.
        Mean,
    };
    OverlapResolution overlapResolution = OverlapResolution::Best;

    /// Repeat forever, or stop after one pass.
    bool continuous = true;

    /// Send each part of the sweep through the connector whose antenna covers
    /// it, instead of measuring the lot through whatever is selected.
    ///
    /// Off by default, and a schedule planned with it off is byte-identical to
    /// one planned before the feature existed -- including the predicted pass
    /// time, which must not grow by a switch cost nothing is paying.
    bool antennaRouting = false;

    /// Which port wins a frequency more than one antenna covers.
    enum class PortStrategy : std::uint8_t {
        /// The narrowest coverage containing the band. A horn dedicated to
        /// 5-6 GHz beats a discone claiming 25 MHz - 6 GHz, which is what an
        /// operator means by "the right antenna for that".
        TightestFit,
        /// The first port that covers it. For a bench where RX1 is simply the
        /// one to prefer.
        PortOrder,
        /// The most gain, whatever it is pointed at.
        HighestGain,
        /// Hold the connector for as long as it covers what is asked of it.
        ///
        /// Fewest port changes rather than the best antenna, which on a radio
        /// that rebuilds its stream to switch is the difference between a pass
        /// that measures and one that spends itself switching. The cost is
        /// that a dedicated antenna nested inside a wideband one is never
        /// reached: whichever got there first keeps the band.
        FewestSwitches,
    };
    PortStrategy portStrategy = PortStrategy::TightestFit;

    /// Adds a range, merging it into any it overlaps or touches.
    ///
    /// Merging rather than appending because overlapping segments are not a
    /// plan the stitcher can resolve -- two steps would write the same grid
    /// bins from different settings -- and validate() rejects them outright.
    /// An operator dragging out a band that happens to abut one already in the
    /// plan means "also look here", not "give me an error".
    ///
    /// A merged range keeps the dwell of the lower of the two; picking the
    /// lower is arbitrary but stable.
    void addSegment(SweepSegment segment);

    [[nodiscard]] Status validate() const;

    /// Total frequency covered, ignoring overlap.
    [[nodiscard]] double totalSpanHz() const noexcept;

    /// Lowest start and highest stop across all segments.
    [[nodiscard]] double lowestHz() const noexcept;
    [[nodiscard]] double highestHz() const noexcept;

    /// Applies the mode's characteristic settings. Called by the planner, and
    /// exposed so the UI can show what switching mode would do before doing it.
    void applyMode(SweepMode requested);

    [[nodiscard]] static Result<SweepPlan> load(const std::filesystem::path& path);
    [[nodiscard]] Status save(const std::filesystem::path& path) const;

    /// Conversion to and from a TOML table, without touching a file.
    ///
    /// Exposed so a profile can embed a plan using this exact schema rather
    /// than a second copy of it. Two definitions of what a plan looks like on
    /// disk would drift the first time a field was added, and the failure
    /// would be a silently-ignored setting rather than an error.
    [[nodiscard]] static Result<SweepPlan> fromTable(const ::toml::table& table);
    void writeInto(::toml::table& root) const;

    /// For the CLI: "2.4G-2.5G,5.1G-5.9G".
    [[nodiscard]] static Result<SweepPlan> parseSegmentList(std::string_view text);
};

[[nodiscard]] std::string_view toString(SweepPlan::PortStrategy strategy) noexcept;
[[nodiscard]] Result<SweepPlan::PortStrategy> portStrategyFromString(std::string_view name);

/// A step this pass leaves on whatever port is already selected.
///
/// Not an error: a range no assigned antenna covers is still swept, because an
/// instrument that silently dropped spectrum the operator asked for would be
/// worse than one that measures it through the wrong antenna and says so.
inline constexpr std::size_t kNoPort = static_cast<std::size_t>(-1);

/// A leg with no switcher in it: the antenna is on the connector directly.
inline constexpr std::size_t kNoInput = static_cast<std::size_t>(-1);

/// One path from the tuner to an antenna, resolved to the band it can hear.
///
/// The planner's whole view of the RF path in front of the tuner: an antenna's
/// coverage intersected with everything between it and the tuner, and what it
/// costs to get there. The planner never learns what an antenna or a switcher
/// *is*, which is what keeps it a pure function of its arguments and testable
/// with no hardware and no library on disk.
///
/// One entry per reachable antenna, so a four-way switcher behind RX2
/// contributes four of these, all naming the same `portIndex`.
struct RoutePort {
    std::size_t portIndex = 0; ///< Into the device's own `rxPorts()`
    std::string id;            ///< `SdrRxPort::id`, what the engine selects by

    /// Which input of the switcher behind that port, or `kNoInput` when the
    /// antenna is on the connector itself.
    std::size_t inputIndex = kNoInput;

    double startHz = 0.0; ///< Antenna coverage, clipped to the port and the input
    double stopHz = 0.0;
    double gainDbi = 0.0;

    /// What it costs to select this port, and this input, separately.
    ///
    /// Separately because moving between two antennas on the same switcher
    /// pays only the second: the connector never changes, and charging a
    /// stream cycle for a relay click would make the predicted pass time wrong
    /// in the direction that matters.
    double switchSeconds = 0.0;
    double inputSwitchSeconds = 0.0;

    [[nodiscard]] bool covers(double fromHz, double toHz) const noexcept {
        return stopHz > startHz && fromHz >= startHz && toHz <= stopHz;
    }
};

/// One tuning position within a pass.
struct SweepStep {
    std::uint32_t index = 0;
    double centerHz = 0.0;

    /// The portion of this step's spectrum that is kept, in absolute Hz.
    /// Narrower than the sample rate by usableBandwidthFraction.
    double usableStartHz = 0.0;
    double usableStopHz = 0.0;

    /// One contiguous run of bins copied from a step's FFT into the grid.
    struct BinRange {
        /// Where to read from, within this step's own FFT output.
        std::size_t firstLocalBin = 0;
        /// Where to write to, in the global grid.
        std::size_t firstGlobalBin = 0;
        std::size_t binCount = 0;
    };

    /// Bins kept from this step: up to two runs, one either side of the
    /// discarded guard around the step's own centre frequency. Two rather
    /// than one because what is thrown away is in the middle, not at an edge
    /// -- see SweepPlan::dcGuardFraction. With no guard there is a single run
    /// spanning the whole usable band.
    std::array<BinRange, 2> ranges{};
    std::size_t rangeCount = 0;

    /// Total bins this step contributes, across all of its runs.
    [[nodiscard]] std::size_t totalBinCount() const noexcept {
        std::size_t total = 0;
        for (std::size_t i = 0; i < rangeCount; ++i) {
            total += ranges[i].binCount;
        }
        return total;
    }

    /// One past the highest local bin this step reads, so a caller can check a
    /// frame is long enough before copying from it.
    [[nodiscard]] std::size_t localBinLimit() const noexcept {
        std::size_t limit = 0;
        for (std::size_t i = 0; i < rangeCount; ++i) {
            limit = std::max(limit, ranges[i].firstLocalBin + ranges[i].binCount);
        }
        return limit;
    }

    double dwellSeconds = 0.0;
    double settleSeconds = 0.0;

    /// Which plan segment produced this step.
    std::size_t segmentIndex = 0;

    /// Which receive port measures this step, as an index into the `RoutePort`
    /// span the schedule was planned against. `kNoPort` when routing is off or
    /// no assigned antenna covers this step -- the engine then leaves the port
    /// alone rather than choosing one.
    std::size_t portIndex = kNoPort;
};

/// The steps of one pass plus the global grid they stitch into.
struct SweepSchedule {
    std::vector<SweepStep> steps;

    /// The absolute-frequency grid every step maps into. Contiguous across the
    /// whole plan, including the gaps between discontinuous segments -- gaps
    /// are represented as bins nothing writes to, which keeps a single linear
    /// index valid for the entire plan.
    double gridStartHz = 0.0;
    double gridBinWidthHz = 0.0;
    std::size_t gridBinCount = 0;

    std::uint32_t fftSize = 0;
    double actualRbwHz = 0.0;

    /// Half-widths of the band each step keeps, and of the guard discarded
    /// around its own tuning. Carried on the schedule because the stitcher
    /// needs them to judge how good a contribution is: a bin taken from the
    /// middle of a step's usable band is worth more than the same bin taken
    /// from just outside another step's guard.
    double usableHalfWidthHz = 0.0;
    double dcGuardHalfWidthHz = 0.0;

    /// Bins the plan does not cover, which the display draws as gaps rather
    /// than as a noise floor that was never measured.
    std::vector<std::pair<std::size_t, std::size_t>> coveredRanges;

    /// Frequency ranges, in Hz, that no assigned antenna covers.
    ///
    /// Named apart from `coveredRanges` deliberately: that one is in *bin*
    /// space and means something else entirely. These are swept anyway, on
    /// whatever port happens to be selected, and listed here so the panel can
    /// say which bands are being measured through an unknown antenna.
    std::vector<std::pair<double, double>> unroutedHz;

    /// Port changes this pass performs, counted between consecutive steps.
    ///
    /// The number that says whether routing is working: the ordinary
    /// two-antenna case is one, and a library whose ranges interleave without
    /// hysteresis would be one per step. A continuous plan whose first and
    /// last steps differ also pays for the wrap back to the start, and that
    /// one is counted here too -- it happens on every pass but the first.
    std::uint32_t portSwitches = 0;

    /// Predicted performance, so the operator sees the trade-off *before*
    /// starting rather than discovering it from a slow waterfall.
    double estimatedPassSeconds = 0.0;
    double estimatedSweepRateHzPerSec = 0.0;
    /// Fraction of pass time spent retuning rather than collecting. At high
    /// sample rates this dominates, and it is the number that explains why a
    /// faster radio did not make the sweep faster.
    double retuneOverheadFraction = 0.0;

    [[nodiscard]] double gridStopHz() const noexcept {
        return gridStartHz + gridBinWidthHz * static_cast<double>(gridBinCount);
    }

    [[nodiscard]] std::size_t binForFrequency(double hz) const noexcept;
    [[nodiscard]] double frequencyForBin(std::size_t bin) const noexcept;
};

class IFftBackend;

/// Turns a plan into a schedule.
class SweepPlanner {
public:
    /// `retuneSettleSeconds` comes from the device -- a HackRF and a BladeRF
    /// differ by an order of magnitude, and guessing would either waste time
    /// or keep untrustworthy samples.
    ///
    /// `deliveryGranularitySeconds` likewise: it is how much signal arrives at
    /// once, and therefore how finely a retune can be resolved at all. It sets
    /// the floor on the step time, so the predicted sweep rate is one the
    /// device can actually produce rather than one that would leave most steps
    /// unmeasured. See ISdrDevice::deliveryGranularitySeconds.
    ///
    /// `tuneMinHz`/`tuneMaxHz` bound where the radio can be tuned; zero means
    /// unbounded. A step's centre sits half a usable band outside the range it
    /// measures, so at the edges of a device's coverage that centre may be
    /// unreachable -- and a refused retune leaves that band silently
    /// unmeasured. Steps are placed at the nearest frequency the device can
    /// reach instead, which still covers the edge because the step's bandwidth
    /// extends past its centre.
    ///
    /// `routePorts` is the RF path in front of the tuner, already resolved by
    /// the caller. Ignored unless `plan.antennaRouting` is set; empty means
    /// there is nothing to route between, which is every single-connector
    /// radio and every bench where nothing has been assigned.
    ///
    /// `fallbackPort` indexes `routePorts` and is where a step no antenna
    /// covers is measured. `kNoPort` leaves such a step on whatever the
    /// previous one selected -- reproducible, but arbitrary, which is why the
    /// operator gets to name one.
    [[nodiscard]] static Result<SweepSchedule>
    plan(const SweepPlan& plan, IFftBackend& backend, double retuneSettleSeconds,
         double deliveryGranularitySeconds = 0.0, double tuneMinHz = 0.0, double tuneMaxHz = 0.0,
         std::span<const RoutePort> routePorts = {}, std::size_t fallbackPort = kNoPort);
};

} // namespace sweeppp
