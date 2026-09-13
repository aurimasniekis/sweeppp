// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/sweep/SweepPlan.hpp"

#include "sweeppp/core/Clock.hpp"
#include "sweeppp/core/Log.hpp"
#include "sweeppp/core/Toml.hpp"
#include "sweeppp/fft/IFftBackend.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace sweeppp {
namespace {

/// Which port measures a band, given what covered the previous one.
///
/// Hysteresis first, and it is what makes the feature usable rather than a
/// curiosity: without it a library whose ranges interleave has the planner
/// alternating ports step by step, and each alternation costs a stream cycle.
/// With it, the ordinary two-antenna case is exactly one switch per pass.
/// Whether `a` is a strictly better answer than `b` under `strategy`.
///
/// Strict, so an equal candidate never displaces the one already chosen. That
/// is what makes the current port win ties without a separate rule, and what
/// keeps the schedule reproducible: two connectors carrying identical antennas
/// must not have the winner depend on how the vector was built.
bool betterPort(const RoutePort& a, const RoutePort& b, SweepPlan::PortStrategy strategy) {
    switch (strategy) {
    case SweepPlan::PortStrategy::TightestFit:
        return a.stopHz - a.startHz < b.stopHz - b.startHz;
    case SweepPlan::PortStrategy::PortOrder:
        return a.portIndex < b.portIndex;
    case SweepPlan::PortStrategy::HighestGain:
        return a.gainDbi > b.gainDbi;
    case SweepPlan::PortStrategy::FewestSwitches:
        // Whichever reaches furthest up the band. Only consulted once the
        // current port has stopped covering, and then this is the classic
        // greedy for covering a line with the fewest intervals -- which is
        // what makes the name a claim rather than a hope.
        return a.stopHz > b.stopHz;
    }
    return false;
}

/// Which port measures a band.
///
/// The strategy decides, and the port already selected only wins ties. It is
/// tempting to keep the current port whenever it still covers the band -- that
/// is one switch per pass in the ordinary two-antenna case and it never
/// thrashes -- but it also means a dedicated antenna nested inside a wideband
/// one is never selected at all. An ADS-B stick inside a discone's range would
/// sit unused while the discone measured 1090 MHz, which is precisely the
/// arrangement the operator bought the stick to avoid, and "tightest fit"
/// would be a setting that did not fit tightest.
///
/// `FewestSwitches` is that behaviour, kept and named: it holds the current
/// port for as long as it covers anything asked of it. Worth having, because
/// on a radio that rebuilds its stream to change connector each switch is a
/// fifth of a second of dead time, and a library whose ranges nest deeply can
/// spend more of the pass switching than measuring.
std::size_t choosePort(std::span<const RoutePort> ports, std::size_t currentPort, double fromHz,
                       double toHz, SweepPlan::PortStrategy strategy) {
    const bool currentCovers = currentPort != kNoPort && currentPort < ports.size() &&
                               ports[currentPort].covers(fromHz, toHz);

    if (strategy == SweepPlan::PortStrategy::FewestSwitches && currentCovers) {
        return currentPort;
    }

    // Seeded with the port already selected, so it survives everything that is
    // merely as good.
    std::size_t best = currentCovers ? currentPort : kNoPort;

    for (std::size_t i = 0; i < ports.size(); ++i) {
        if (!ports[i].covers(fromHz, toHz)) {
            continue;
        }
        if (best == kNoPort || betterPort(ports[i], ports[best], strategy)) {
            best = i;
        }
    }
    return best;
}

/// What moving between two legs costs.
///
/// Only what actually changes: two antennas on the same switcher pay the
/// relay's settle and nothing else, because the connector never moves. A leg
/// on a different port pays the connector *and* its switcher, since the box
/// behind the new port is wherever it was left.
double transitionSeconds(const RoutePort& from, const RoutePort& to) {
    if (from.portIndex != to.portIndex) {
        return to.switchSeconds + (to.inputIndex != kNoInput ? to.inputSwitchSeconds : 0.0);
    }
    return from.inputIndex != to.inputIndex ? to.inputSwitchSeconds : 0.0;
}

/// Appends a range to `ranges`, merging it into the last when they touch.
///
/// The steps arrive in frequency order and overlap by design, so an uncovered
/// band spanning forty steps has to come out as one range an operator can
/// read rather than forty adjacent ones.
void appendRange(std::vector<std::pair<double, double>>& ranges, double fromHz, double toHz) {
    if (toHz <= fromHz) {
        return;
    }
    if (!ranges.empty() && fromHz <= ranges.back().second) {
        ranges.back().second = std::max(ranges.back().second, toHz);
        return;
    }
    ranges.emplace_back(fromHz, toHz);
}

} // namespace

std::string_view toString(SweepMode mode) noexcept {
    switch (mode) {
    case SweepMode::Fast:
        return "fast";
    case SweepMode::Detail:
        return "detail";
    }
    return "fast";
}

Result<SweepMode> sweepModeFromString(std::string_view name) {
    if (name == "fast") {
        return SweepMode::Fast;
    }
    if (name == "detail") {
        return SweepMode::Detail;
    }
    return fail<SweepMode>(ErrorCode::InvalidArgument, "unknown sweep mode '{}'", name);
}

std::string_view toString(SweepPlan::PortStrategy strategy) noexcept {
    switch (strategy) {
    case SweepPlan::PortStrategy::TightestFit:
        return "tightest_fit";
    case SweepPlan::PortStrategy::PortOrder:
        return "port_order";
    case SweepPlan::PortStrategy::HighestGain:
        return "highest_gain";
    case SweepPlan::PortStrategy::FewestSwitches:
        return "fewest_switches";
    }
    return "tightest_fit";
}

Result<SweepPlan::PortStrategy> portStrategyFromString(std::string_view name) {
    if (name == "tightest_fit") {
        return SweepPlan::PortStrategy::TightestFit;
    }
    if (name == "port_order") {
        return SweepPlan::PortStrategy::PortOrder;
    }
    if (name == "highest_gain") {
        return SweepPlan::PortStrategy::HighestGain;
    }
    if (name == "fewest_switches") {
        return SweepPlan::PortStrategy::FewestSwitches;
    }
    return fail<SweepPlan::PortStrategy>(ErrorCode::InvalidArgument, "unknown port strategy '{}'",
                                         name);
}

void SweepPlan::addSegment(SweepSegment segment) {
    if (segment.stopHz <= segment.startHz) {
        return;
    }

    segments.push_back(segment);
    std::ranges::sort(segments, [](const SweepSegment& a, const SweepSegment& b) {
        return a.startHz < b.startHz;
    });

    std::vector<SweepSegment> merged;
    for (const SweepSegment& candidate : segments) {
        // `<=` rather than `<`: two ranges that merely touch are one range,
        // and leaving a zero-width seam between them would be a gap the sweep
        // has to pay a retune for and the display has to draw as unmeasured.
        if (!merged.empty() && candidate.startHz <= merged.back().stopHz) {
            merged.back().stopHz = std::max(merged.back().stopHz, candidate.stopHz);
        } else {
            merged.push_back(candidate);
        }
    }
    segments = std::move(merged);
}

Status SweepPlan::validate() const {
    if (segments.empty()) {
        return fail(ErrorCode::InvalidArgument, "a sweep plan needs at least one segment");
    }
    if (sampleRate <= 0.0) {
        return fail(ErrorCode::InvalidArgument, "sample rate must be positive");
    }
    if (rbwHz <= 0.0) {
        return fail(ErrorCode::InvalidArgument, "RBW must be positive");
    }
    if (usableBandwidthFraction <= 0.0 || usableBandwidthFraction > 1.0) {
        return fail(ErrorCode::InvalidArgument,
                    "usable bandwidth fraction must be in (0, 1], got {}", usableBandwidthFraction);
    }
    if (stepOverlap < 0.0 || stepOverlap >= 1.0) {
        return fail(ErrorCode::InvalidArgument, "step overlap must be in [0, 1), got {}",
                    stepOverlap);
    }

    for (std::size_t i = 0; i < segments.size(); ++i) {
        const SweepSegment& segment = segments[i];
        if (!segment.valid()) {
            return fail(ErrorCode::InvalidArgument, "segment {} is empty or inverted ({} to {})", i,
                        toml_util::formatFrequencyShort(segment.startHz),
                        toml_util::formatFrequencyShort(segment.stopHz));
        }
    }

    // Overlapping segments would produce two steps writing the same global
    // bins from different plan settings, which the stitcher cannot resolve
    // meaningfully. Caught here rather than producing a confusing display.
    std::vector<const SweepSegment*> sorted;
    sorted.reserve(segments.size());
    for (const SweepSegment& segment : segments) {
        sorted.push_back(&segment);
    }
    std::ranges::sort(sorted, [](const SweepSegment* a, const SweepSegment* b) {
        return a->startHz < b->startHz;
    });
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i]->startHz < sorted[i - 1]->stopHz) {
            return fail(ErrorCode::InvalidArgument, "segments {} and {} overlap",
                        toml_util::formatFrequencyShort(sorted[i - 1]->startHz),
                        toml_util::formatFrequencyShort(sorted[i]->startHz));
        }
    }

    return ok();
}

double SweepPlan::totalSpanHz() const noexcept {
    double total = 0.0;
    for (const SweepSegment& segment : segments) {
        total += segment.spanHz();
    }
    return total;
}

double SweepPlan::lowestHz() const noexcept {
    double lowest = std::numeric_limits<double>::max();
    for (const SweepSegment& segment : segments) {
        lowest = std::min(lowest, segment.startHz);
    }
    return segments.empty() ? 0.0 : lowest;
}

double SweepPlan::highestHz() const noexcept {
    double highest = std::numeric_limits<double>::lowest();
    for (const SweepSegment& segment : segments) {
        highest = std::max(highest, segment.stopHz);
    }
    return segments.empty() ? 0.0 : highest;
}

void SweepPlan::applyMode(SweepMode requested) {
    mode = requested;

    switch (requested) {
    case SweepMode::Fast:
        // Cover ground. One FFT per step, no dwell beyond the settle, minimal
        // overlap -- the sweep rate is what matters.
        averageCount = 1;
        fftOverlap = 0.0;
        dwellSeconds = 0.0;
        stepOverlap = 0.05;
        usableBandwidthFraction = 0.8;
        break;

    case SweepMode::Detail:
        // Measure properly. Averaging pulls the noise floor down by ~10*log10(N)
        // and makes a weak signal readable; the extra overlap removes the
        // scallop at step boundaries.
        averageCount = 8;
        fftOverlap = 0.5;
        dwellSeconds = 0.002;
        stepOverlap = 0.15;
        usableBandwidthFraction = 0.7;
        break;
    }
}

Result<SweepPlan> SweepPlan::parseSegmentList(std::string_view text) {
    SweepPlan plan;
    plan.name = "cli";

    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string_view piece = text.substr(
            start, comma == std::string_view::npos ? text.size() - start : comma - start);
        if (!piece.empty()) {
            const std::size_t dash = piece.find('-', piece.front() == '-' ? 1 : 0);
            if (dash == std::string_view::npos) {
                return fail<SweepPlan>(ErrorCode::ParseError,
                                       "'{}' is not a range; expected start-stop", piece);
            }

            auto from = toml_util::parseFrequency(piece.substr(0, dash));
            auto to = toml_util::parseFrequency(piece.substr(dash + 1));
            if (!from) {
                return std::unexpected(from.error());
            }
            if (!to) {
                return std::unexpected(to.error());
            }

            plan.segments.push_back(SweepSegment{.startHz = *from, .stopHz = *to});
        }

        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }

    if (auto valid = plan.validate(); !valid) {
        return std::unexpected(valid.error());
    }
    return plan;
}

Result<SweepPlan> SweepPlan::load(const std::filesystem::path& path) {
    auto table = toml_util::load(path);
    if (!table) {
        return std::unexpected(table.error());
    }

    auto plan = fromTable(*table);
    if (plan && plan->name.empty()) {
        plan->name = path.stem().string();
    }
    return plan;
}

Result<SweepPlan> SweepPlan::fromTable(const ::toml::table& source) {
    const ::toml::table* table = &source;

    SweepPlan plan;
    plan.name = toml_util::getString(*table, "sweep.name", "");

    if (const auto modeName = toml_util::getString(*table, "sweep.mode", "fast");
        !modeName.empty()) {
        auto mode = sweepModeFromString(modeName);
        if (!mode) {
            return std::unexpected(mode.error());
        }
        plan.applyMode(*mode);
    }

    // Applied after the mode so an explicit value in the file always wins over
    // the mode's preset -- the file is the operator's intent.
    plan.rbwHz = toml_util::getDouble(*table, "sweep.rbw", plan.rbwHz);
    plan.sampleRate = toml_util::getDouble(*table, "sweep.sample_rate", plan.sampleRate);
    plan.usableBandwidthFraction =
        toml_util::getDouble(*table, "sweep.usable_bandwidth", plan.usableBandwidthFraction);
    plan.stepOverlap = toml_util::getDouble(*table, "sweep.step_overlap", plan.stepOverlap);
    plan.dcGuardFraction = toml_util::getDouble(*table, "sweep.dc_guard", plan.dcGuardFraction);
    plan.dwellSeconds = toml_util::getDouble(*table, "sweep.dwell", plan.dwellSeconds);
    plan.averageCount =
        static_cast<std::uint32_t>(toml_util::getInt(*table, "sweep.average", plan.averageCount));
    plan.fftOverlap = toml_util::getDouble(*table, "sweep.fft_overlap", plan.fftOverlap);
    plan.continuous = toml_util::getBool(*table, "sweep.continuous", plan.continuous);
    plan.antennaRouting = toml_util::getBool(*table, "sweep.antenna_routing", plan.antennaRouting);

    if (const auto strategyName = toml_util::getString(*table, "sweep.port_strategy", "");
        !strategyName.empty()) {
        auto strategy = portStrategyFromString(strategyName);
        if (!strategy) {
            return std::unexpected(strategy.error());
        }
        plan.portStrategy = *strategy;
    }

    if (const auto windowName = toml_util::getString(*table, "sweep.window", "");
        !windowName.empty()) {
        auto window = windowTypeFromString(windowName);
        if (!window) {
            return std::unexpected(window.error());
        }
        plan.window = *window;
    }

    if (const auto resolution = toml_util::getString(*table, "sweep.overlap_resolution", "best");
        !resolution.empty()) {
        plan.overlapResolution = resolution == "mean"  ? OverlapResolution::Mean
                                 : resolution == "max" ? OverlapResolution::Max
                                                       : OverlapResolution::Best;
    }

    // Frequencies accept either a number or a suffixed string, so both
    // start = 2.4e9 and start = "2.4 GHz" work.
    if (const ::toml::array* segments = toml_util::at(*table, "sweep.segments").as_array()) {
        for (const ::toml::node& node : *segments) {
            const ::toml::table* entry = node.as_table();
            if (entry == nullptr) {
                continue;
            }

            auto from = toml_util::frequencyFrom((*entry)["start"], "sweep.segments[].start");
            auto to = toml_util::frequencyFrom((*entry)["stop"], "sweep.segments[].stop");
            if (!from) {
                return std::unexpected(from.error());
            }
            if (!to) {
                return std::unexpected(to.error());
            }

            plan.segments.push_back(SweepSegment{
                .startHz = *from, .stopHz = *to, .dwellSeconds = (*entry)["dwell"].value_or(0.0)});
        }
    }

    if (auto valid = plan.validate(); !valid) {
        return std::unexpected(valid.error());
    }
    return plan;
}

Status SweepPlan::save(const std::filesystem::path& path) const {
    ::toml::table root;
    writeInto(root);
    return toml_util::save(path, root, "Sweep++ sweep plan");
}

void SweepPlan::writeInto(::toml::table& root) const {
    ::toml::table& sweep = toml_util::ensureTable(root, "sweep");

    sweep.insert_or_assign("name", name);
    sweep.insert_or_assign("mode", std::string(toString(mode)));
    sweep.insert_or_assign("rbw", rbwHz);
    sweep.insert_or_assign("sample_rate", sampleRate);
    sweep.insert_or_assign("usable_bandwidth", usableBandwidthFraction);
    sweep.insert_or_assign("step_overlap", stepOverlap);
    sweep.insert_or_assign("dc_guard", dcGuardFraction);
    sweep.insert_or_assign("dwell", dwellSeconds);
    sweep.insert_or_assign("average", static_cast<std::int64_t>(averageCount));
    sweep.insert_or_assign("window", std::string(toString(window)));
    sweep.insert_or_assign("fft_overlap", fftOverlap);
    sweep.insert_or_assign("continuous", continuous);
    sweep.insert_or_assign("antenna_routing", antennaRouting);
    sweep.insert_or_assign("port_strategy", std::string(toString(portStrategy)));
    sweep.insert_or_assign("overlap_resolution",
                           std::string(overlapResolution == OverlapResolution::Best  ? "best"
                                       : overlapResolution == OverlapResolution::Max ? "max"
                                                                                     : "mean"));

    ::toml::array segmentArray;
    for (const SweepSegment& segment : segments) {
        ::toml::table entry;
        entry.insert_or_assign("start", segment.startHz);
        entry.insert_or_assign("stop", segment.stopHz);
        if (segment.dwellSeconds > 0.0) {
            entry.insert_or_assign("dwell", segment.dwellSeconds);
        }
        segmentArray.push_back(std::move(entry));
    }
    sweep.insert_or_assign("segments", std::move(segmentArray));
}

std::size_t SweepSchedule::binForFrequency(double hz) const noexcept {
    if (gridBinWidthHz <= 0.0) {
        return 0;
    }
    const double index = (hz - gridStartHz) / gridBinWidthHz;
    if (index <= 0.0) {
        return 0;
    }
    if (index >= static_cast<double>(gridBinCount)) {
        return gridBinCount > 0 ? gridBinCount - 1 : 0;
    }
    return static_cast<std::size_t>(index);
}

double SweepSchedule::frequencyForBin(std::size_t bin) const noexcept {
    // Bin centre, not edge: a peak reported at a bin's left edge would be
    // consistently low by half an RBW.
    return gridStartHz + gridBinWidthHz * (static_cast<double>(bin) + 0.5);
}

Result<SweepSchedule> SweepPlanner::plan(const SweepPlan& plan, IFftBackend& backend,
                                         double retuneSettleSeconds,
                                         double deliveryGranularitySeconds, double tuneMinHz,
                                         double tuneMaxHz, std::span<const RoutePort> routePorts,
                                         std::size_t fallbackPort) {
    if (auto valid = plan.validate(); !valid) {
        return std::unexpected(valid.error());
    }

    SweepSchedule schedule;

    // FFT size from the requested RBW. RBW = sampleRate * ENBW / N, so
    // N = sampleRate * ENBW / RBW -- the ENBW factor is what makes the
    // resulting RBW match what was asked for rather than being 1.5x too wide.
    auto window = Window::create(plan.window, 1024, plan.windowBeta);
    if (!window) {
        return std::unexpected(window.error());
    }
    const double enbw = window->properties().enbw;

    const auto desiredSize =
        static_cast<std::size_t>(std::ceil(plan.sampleRate * enbw / plan.rbwHz));
    const std::size_t fftSize = backend.snapSize(desiredSize);
    if (!backend.supportsSize(fftSize)) {
        return fail<SweepSchedule>(ErrorCode::InvalidArgument,
                                   "no FFT size near {} is supported by {}", desiredSize,
                                   backend.name());
    }

    schedule.fftSize = static_cast<std::uint32_t>(fftSize);
    schedule.actualRbwHz = plan.sampleRate * enbw / static_cast<double>(fftSize);

    const double binWidth = plan.sampleRate / static_cast<double>(fftSize);
    schedule.gridBinWidthHz = binWidth;

    // One grid spanning the whole plan, including the gaps between
    // discontinuous segments. Gaps stay as bins nothing writes to, which keeps
    // a single linear index valid across the entire plan -- far simpler than
    // per-segment grids that the display would have to reconcile.
    schedule.gridStartHz = plan.lowestHz();
    const double gridSpan = plan.highestHz() - plan.lowestHz();
    schedule.gridBinCount = static_cast<std::size_t>(std::ceil(gridSpan / binWidth));

    if (schedule.gridBinCount == 0) {
        return fail<SweepSchedule>(ErrorCode::InvalidArgument, "the plan covers no bandwidth");
    }

    // Crop the DC spike and the filter roll-off. Stitching one step's roll-off
    // against the next would put a periodic scallop across the whole sweep --
    // an artefact that looks convincingly like real structure.
    const double usableWidth = plan.sampleRate * plan.usableBandwidthFraction;

    // The guard is centred on the step's own tuning, so it splits the usable
    // band in two rather than trimming it.
    const double dcGuardWidth =
        std::clamp(plan.sampleRate * plan.dcGuardFraction, 0.0, usableWidth * 0.5);
    const double subBandWidth = (usableWidth - dcGuardWidth) * 0.5;

    // With a guard, the advance is capped at one sub-band.
    //
    // What the guard discards is in the middle of the step, and overlap
    // between neighbours only ever extends their edges -- so the only way that
    // frequency gets measured is if the next step reaches it with a sub-band,
    // which requires advancing by no more than one. That is the whole cost of
    // removing the spikes, and it is a factor of two.
    schedule.usableHalfWidthHz = usableWidth * 0.5;
    schedule.dcGuardHalfWidthHz = dcGuardWidth * 0.5;

    const double reach = dcGuardWidth > 0.0 ? subBandWidth : usableWidth;
    const double advance = reach * (1.0 - plan.stepOverlap);
    if (advance <= 0.0) {
        return fail<SweepSchedule>(ErrorCode::InvalidArgument,
                                   "usable bandwidth and overlap leave no forward progress");
    }

    std::uint32_t stepIndex = 0;
    double totalDwell = 0.0;
    double totalSettle = 0.0;
    double totalSwitch = 0.0;

    // Routing is off unless it was both asked for and possible. Both halves
    // matter: with routing off the schedule below must come out identical to
    // one planned before the feature existed, down to the predicted pass time,
    // and an empty port list is the state of every single-connector radio.
    const bool routing = plan.antennaRouting && !routePorts.empty();
    std::size_t currentPort = kNoPort;
    std::size_t firstPort = kNoPort;

    // Segments are visited in frequency order, whatever order they were
    // written in.
    //
    // The schedule must come out sorted by centre frequency: the engine finds
    // the step a frame belongs to by binary search over it, and a binary search
    // on an unsorted range does not fail, it silently returns the wrong step.
    // An operator listing 2.4 GHz before 420 MHz would find the 420 MHz band
    // simply never appeared, with nothing anywhere saying why.
    //
    // The original index is carried through so per-segment settings still
    // resolve against the plan the operator wrote.
    std::vector<std::size_t> order(plan.segments.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::ranges::sort(order, [&plan](std::size_t a, std::size_t b) {
        return plan.segments[a].startHz < plan.segments[b].startHz;
    });

    for (const std::size_t segmentIndex : order) {
        const SweepSegment& segment = plan.segments[segmentIndex];
        const double dwell = segment.dwellSeconds > 0.0 ? segment.dwellSeconds : plan.dwellSeconds;

        // The first step is placed so its usable band starts at the segment
        // start, rather than centring on it -- otherwise the lower half of the
        // first step falls outside the requested range and is wasted.
        double centerHz = segment.startHz + usableWidth * 0.5;

        double lastTunedHz = std::numeric_limits<double>::quiet_NaN();

        while (centerHz - usableWidth * 0.5 < segment.stopHz) {
            // Placed where the radio can actually go.
            //
            // The first and last steps of a segment need centres half a usable
            // band outside it, and at the edge of a device's range that centre
            // does not exist -- a BladeRF asked to tune to 6.022 GHz refuses,
            // the retune fails, and the top of the sweep is silently never
            // measured. Tuning to the nearest reachable frequency still covers
            // that band, because the step's own bandwidth extends past its
            // centre; it is the only way the ends of a device's range get
            // measured at all.
            const double tunedHz = std::clamp(centerHz, tuneMinHz > 0.0 ? tuneMinHz : centerHz,
                                              tuneMaxHz > 0.0 ? tuneMaxHz : centerHz);

            // Past the edge every further step would tune to the same place and
            // re-measure the same band. Advance until the loop ends instead.
            if (tunedHz == lastTunedHz) {
                centerHz += advance;
                continue;
            }
            lastTunedHz = tunedHz;

            SweepStep step;
            step.index = stepIndex++;
            step.centerHz = tunedHz;
            step.segmentIndex = segmentIndex;
            step.dwellSeconds = dwell;
            step.settleSeconds = retuneSettleSeconds;

            // Clip the usable band to the segment so a step never contributes
            // bins outside what was asked for.
            step.usableStartHz = std::max(tunedHz - usableWidth * 0.5, segment.startHz);
            step.usableStopHz = std::min(tunedHz + usableWidth * 0.5, segment.stopHz);

            const double stepStartHz = tunedHz - plan.sampleRate * 0.5;

            // The kept band as up to two runs: below the guard and above it.
            // With no guard the two collapse into one spanning the lot.
            const std::array<std::pair<double, double>, 2> subBands{
                std::pair{step.usableStartHz,
                          dcGuardWidth > 0.0 ? tunedHz - dcGuardWidth * 0.5 : step.usableStopHz},
                std::pair{tunedHz + dcGuardWidth * 0.5, step.usableStopHz}};
            const std::size_t subBandCount = dcGuardWidth > 0.0 ? 2 : 1;

            for (std::size_t band = 0; band < subBandCount; ++band) {
                const double fromHz = std::max(subBands[band].first, step.usableStartHz);
                const double toHz = std::min(subBands[band].second, step.usableStopHz);
                if (toHz <= fromHz) {
                    continue;
                }

                SweepStep::BinRange range;
                range.firstLocalBin =
                    static_cast<std::size_t>(std::round((fromHz - stepStartHz) / binWidth));
                range.firstGlobalBin = static_cast<std::size_t>(
                    std::round((fromHz - schedule.gridStartHz) / binWidth));
                range.binCount = static_cast<std::size_t>(std::round((toHz - fromHz) / binWidth));

                // Clamp so a rounding error at the last step cannot write past
                // the end of either buffer.
                if (range.firstLocalBin >= fftSize ||
                    range.firstGlobalBin >= schedule.gridBinCount) {
                    continue;
                }
                range.binCount = std::min(range.binCount, fftSize - range.firstLocalBin);
                range.binCount =
                    std::min(range.binCount, schedule.gridBinCount - range.firstGlobalBin);

                if (range.binCount > 0) {
                    step.ranges[step.rangeCount++] = range;
                }
            }

            if (step.rangeCount > 0) {
                // Routed only once the step is known to survive. A step whose
                // bin ranges clamp to nothing is never executed, so charging a
                // switch for it would put a cost in the prediction that the
                // pass does not pay -- and worse, would let it move the
                // hysteresis state the next real step reads.
                if (routing) {
                    // Chosen from the band this step actually keeps, not from
                    // where it is tuned: the tuning is a means, and at a
                    // segment edge it sits outside the measured band entirely.
                    step.portIndex = choosePort(routePorts, currentPort, step.usableStartHz,
                                                step.usableStopHz, plan.portStrategy);

                    if (step.portIndex == kNoPort) {
                        // Swept anyway, and recorded so the panel can say so.
                        // An instrument that silently dropped spectrum the
                        // operator asked for would be the worse failure by
                        // far.
                        appendRange(schedule.unroutedHz, step.usableStartHz, step.usableStopHz);

                        // On the connector the operator nominated for exactly
                        // this, if they nominated one. Still recorded above:
                        // the range is being measured through an antenna that
                        // does not claim to hear it, and that stays worth
                        // saying however deliberately the port was chosen.
                        step.portIndex = fallbackPort < routePorts.size() ? fallbackPort : kNoPort;
                    }

                    if (step.portIndex != kNoPort) {
                        if (currentPort != kNoPort && step.portIndex != currentPort) {
                            ++schedule.portSwitches;
                            totalSwitch += transitionSeconds(routePorts[currentPort],
                                                             routePorts[step.portIndex]);
                        }
                        if (firstPort == kNoPort) {
                            firstPort = step.portIndex;
                        }
                        currentPort = step.portIndex;
                    }
                }

                for (std::size_t i = 0; i < step.rangeCount; ++i) {
                    schedule.coveredRanges.emplace_back(step.ranges[i].firstGlobalBin,
                                                        step.ranges[i].binCount);
                }
                schedule.steps.push_back(step);

                // One collection window, not two.
                //
                // The engine holds a step for *up to* two -- neither
                // acquisition blocks nor USB transfers are aligned to retunes,
                // so two is what it takes to guarantee one lands wholly inside
                // the step. But that is a deadline, not a dwell: the step ends
                // as soon as it has been measured, which on a device
                // delivering steadily is after about one. Predicting the
                // worst case told the operator the sweep was half the speed it
                // runs at, which is its own kind of wrong.
                const double collectSeconds = static_cast<double>(plan.averageCount) *
                                              static_cast<double>(fftSize) / plan.sampleRate;
                totalDwell += dwell + std::max(collectSeconds, deliveryGranularitySeconds);
                totalSettle += retuneSettleSeconds;
            }

            centerHz += advance;
        }
    }

    if (schedule.steps.empty()) {
        return fail<SweepSchedule>(ErrorCode::InvalidArgument,
                                   "the plan produced no usable tune steps");
    }

    // The wrap back to the start of the next pass. Real, and charged: a
    // continuous two-antenna sweep switches to the high port on the way up and
    // back to the low one at the top of every pass but the first, and a
    // predicted rate that ignored the second one would overstate a bladeRF's
    // by a fifth of a second per pass.
    if (routing && plan.continuous && firstPort != kNoPort && currentPort != kNoPort &&
        firstPort != currentPort) {
        ++schedule.portSwitches;
        totalSwitch += transitionSeconds(routePorts[currentPort], routePorts[firstPort]);
    }

    schedule.estimatedPassSeconds = totalDwell + totalSettle + totalSwitch;
    schedule.estimatedSweepRateHzPerSec = schedule.estimatedPassSeconds > 0.0
                                              ? plan.totalSpanHz() / schedule.estimatedPassSeconds
                                              : 0.0;
    // At high sample rates the limiter is retune latency, not FFT throughput.
    // Reporting the split is what lets an operator see that a faster radio
    // would not make this sweep faster. Port switches are counted in it for
    // the same reason: on a radio that has to rebuild its stream, a plan whose
    // antennas interleave spends most of the pass not measuring anything, and
    // that is precisely a cost a faster radio would not remove.
    schedule.retuneOverheadFraction =
        schedule.estimatedPassSeconds > 0.0
            ? (totalSettle + totalSwitch) / schedule.estimatedPassSeconds
            : 0.0;

    logInfo("sweep",
            "{} steps, {} point FFT, RBW {}, grid {} bins, ~{} per pass ({} MHz/s, {:.0f}% retune)",
            schedule.steps.size(), schedule.fftSize,
            toml_util::formatFrequencyShort(schedule.actualRbwHz), schedule.gridBinCount,
            formatDuration(schedule.estimatedPassSeconds),
            schedule.estimatedSweepRateHzPerSec / 1e6, schedule.retuneOverheadFraction * 100.0);

    if (routing) {
        logInfo("sweep", "routed across {} port{}, {} switch{} per pass{}", routePorts.size(),
                routePorts.size() == 1 ? "" : "s", schedule.portSwitches,
                schedule.portSwitches == 1 ? "" : "es",
                schedule.unroutedHz.empty() ? "" : ", some of it through no assigned antenna");

        for (const auto& [fromHz, toHz] : schedule.unroutedHz) {
            logWarn("sweep", "{} - {} is covered by no assigned antenna; it will be swept on {}",
                    toml_util::formatFrequencyShort(fromHz), toml_util::formatFrequencyShort(toHz),
                    fallbackPort < routePorts.size() ? routePorts[fallbackPort].id
                                                     : std::string("whichever port is selected"));
        }
    }

    return schedule;
}

} // namespace sweeppp
