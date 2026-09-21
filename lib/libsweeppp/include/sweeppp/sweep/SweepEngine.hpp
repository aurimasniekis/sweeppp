// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/EventBus.hpp"
#include "sweeppp/core/Telemetry.hpp"
#include "sweeppp/pipeline/FrameBus.hpp"
#include "sweeppp/pipeline/Pipeline.hpp"
#include "sweeppp/rf/Antenna.hpp"
#include "sweeppp/rf/AntennaAssignments.hpp"
#include "sweeppp/rf/IRfPath.hpp"
#include "sweeppp/sweep/SweepPlan.hpp"

#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace sweeppp {

/// Drives a device through a schedule and stitches the results.
///
/// Sits between the pipeline and the frame bus: it consumes the per-step
/// frames the pipeline produces, maps their bins into the global grid, and
/// republishes stitched frames.
///
/// **Partial emission is the defining behaviour.** The display updates as each
/// step lands, not only when a pass completes. On a 5 GHz span at a few
/// hundred milliseconds per pass, waiting for completion would leave the
/// waterfall frozen for most of the time; real sweepers show the sweep
/// happening. It also means a signal is visible the moment its step is
/// measured rather than up to a full pass later.
class SweepEngine final : public IFrameConsumer {
public:
    SweepEngine(FrameBus& output, Telemetry& telemetry, EventBus& events);
    ~SweepEngine() override;

    SweepEngine(const SweepEngine&) = delete;
    SweepEngine& operator=(const SweepEngine&) = delete;

    /// Plans the sweep and prepares the stitching grid. Does not start it.
    ///
    /// Applies the plan's sample rate to `device` and plans against whatever
    /// the radio actually accepted, so the grid can never describe a rate the
    /// device is not running at. `plan()` returns the adjusted plan.
    ///
    /// `antennas` and `assignments` are only read when `plan.antennaRouting`
    /// is set, and only to resolve each of the device's ports into the band it
    /// can hear. Both default to empty, which is what the tests and the
    /// headless tools want -- and what a bench with nothing assigned yields
    /// anyway.
    /// `switchers` are the antenna switchers the application has open, so a
    /// chain of port -> switcher -> input -> antenna can be resolved and
    /// driven. Empty is the ordinary bench.
    [[nodiscard]] Status configure(const SweepPlan& plan, IFftBackend& backend, ISdrDevice& device,
                                   const AntennaLibrary& antennas = {},
                                   const AntennaAssignments& assignments = {},
                                   std::span<const OpenRfPath> switchers = {});

    /// The RF path this sweep was planned against, one entry per port that has
    /// an antenna assigned. Empty unless routing is on.
    [[nodiscard]] std::span<const RoutePort> routePorts() const noexcept { return m_routePorts; }

    /// Begins sweeping. `pipeline` must already be configured and started
    /// against `device`; the engine only drives tuning and stitching.
    [[nodiscard]] Status start(ISdrDevice& device, Pipeline& pipeline);
    void stop();
    [[nodiscard]] bool running() const noexcept {
        return m_running.load(std::memory_order_acquire);
    }

    /// Receives per-step frames from the pipeline's bus.
    void onFrame(const SpectrumFramePtr& frame) noexcept override;
    [[nodiscard]] std::string_view consumerName() const noexcept override { return "sweep-engine"; }

    /// Sees every frame the engine accepts for stitching, with the step it
    /// was attributed to -- exactly the frames `frameAccounting().stitched`
    /// counts, and none of the ones it rejected.
    ///
    /// For the correction learner, which wants each step's raw measurement
    /// rather than the stitched grid. Runs on the bus thread, under the same
    /// contract as `onFrame`: it must not block. Empty removes it.
    using StepObserver = std::function<void(const SpectrumFrame&, const SweepStep&)>;
    void setStepObserver(StepObserver observer);

    [[nodiscard]] const SweepSchedule& schedule() const noexcept { return m_schedule; }
    [[nodiscard]] const SweepPlan& plan() const noexcept { return m_plan; }

    /// Passes completed since start.
    [[nodiscard]] std::uint64_t passCount() const noexcept {
        return m_passId.load(std::memory_order_relaxed);
    }

    /// What became of the per-step frames the pipeline produced.
    ///
    /// A sweep that looks sparse is almost always discarding measurements
    /// rather than failing to take them, and these say which reason: samples
    /// taken while the synthesiser was still moving, or frames whose centre
    /// matches no planned step. Without the split, a partly-filled display
    /// gives no clue whether the settle window is too long, the steps too
    /// short, or the tuning simply wrong.
    struct FrameAccounting {
        std::uint64_t stitched = 0;
        std::uint64_t unsettled = 0;
        std::uint64_t unattributed = 0;
        std::uint64_t tooShort = 0;
    };

    [[nodiscard]] FrameAccounting frameAccounting() const noexcept {
        return {m_stitchedFrames.load(std::memory_order_relaxed),
                m_unsettledFrames.load(std::memory_order_relaxed),
                m_unattributedFrames.load(std::memory_order_relaxed),
                m_shortFrames.load(std::memory_order_relaxed)};
    }

    /// Fraction of the grid the most recent pass actually measured, in [0, 1].
    ///
    /// Distinct from what the display shows: bins keep their last measurement
    /// until something replaces it, so a grid can look complete while each
    /// pass is only filling a fraction of it and the rest is stale. This is
    /// the number that says whether the sweep is keeping up.
    [[nodiscard]] double lastPassCoverage() const noexcept {
        return m_lastPassCoverage.load(std::memory_order_relaxed);
    }

    /// Measured sweep rate, which may differ from the planner's estimate once
    /// real retune latency is known.
    [[nodiscard]] double measuredSweepRateHzPerSec() const noexcept {
        return m_measuredRate.load(std::memory_order_relaxed);
    }

private:
    void sweepLoop(std::stop_token stop);
    void stitch(const SpectrumFrame& frame, const SweepStep& step);

    /// Selects the port a step needs, cycling the device stream first when the
    /// port demands it. False when the switch failed, in which case the step
    /// is skipped rather than measured through the wrong antenna.
    [[nodiscard]] bool applyPort(const SweepStep& step);

    /// The step a frame came from, by its captured centre frequency.
    /// Empty when it does not plausibly belong to any planned step.
    [[nodiscard]] std::optional<std::size_t> findStep(double centerHz) const;
    void emitPartial();
    void completePass();

    FrameBus& m_output;
    Telemetry& m_telemetry;
    EventBus& m_events;

    SweepPlan m_plan;
    SweepSchedule m_schedule;

    ISdrDevice* m_device = nullptr;
    Pipeline* m_pipeline = nullptr;

    /// Cached from the device at configure time; see
    /// ISdrDevice::deliveryGranularitySeconds.
    double m_deliveryGranularitySeconds = 0.0;

    std::atomic<bool> m_running{false};
    std::jthread m_thread;

    /// Index of the step currently being collected. Read by the frame
    /// callback, written by the sweep thread.
    std::atomic<std::uint32_t> m_currentStep{0};
    /// Per step, the instant after which its samples are trustworthy.
    ///
    /// Per step rather than one global value: frames can arrive out of order
    /// with respect to retunes, so a single "valid from now" would discard
    /// perfectly good data from an earlier step and admit unsettled data from
    /// a later one. Guarded by m_gridMutex.
    std::vector<std::uint64_t> m_stepValidFromNs;
    std::atomic<std::uint64_t> m_passId{1};
    std::atomic<double> m_measuredRate{0.0};
    std::atomic<std::uint64_t> m_frameSequence{0};
    std::atomic<bool> m_stepSatisfied{false};

    std::atomic<double> m_lastPassCoverage{0.0};
    std::atomic<std::uint64_t> m_stitchedFrames{0};
    std::atomic<std::uint64_t> m_unsettledFrames{0};
    std::atomic<std::uint64_t> m_unattributedFrames{0};
    std::atomic<std::uint64_t> m_shortFrames{0};

    /// Guarded by m_gridMutex, and copied out before it is called so the
    /// caller may replace it from inside its own callback.
    StepObserver m_stepObserver;

    /// The stitched grid, and which bins have been written this pass.
    mutable std::mutex m_gridMutex;
    std::vector<float> m_grid;
    std::vector<std::uint8_t> m_gridWritten;

    /// How good the measurement currently held in each bin is -- how deep
    /// inside its step's usable band it was taken. Compared against, never
    /// displayed.
    std::vector<float> m_gridQuality;
    AcquisitionConfig m_gridConfig;
    std::uint64_t m_passStartNs = 0;
    std::uint64_t m_lastEmitNs = 0;
    std::uint32_t m_stepsThisPass = 0;

    /// Where the radio is tuned, so a step that asks for the frequency it is
    /// already on costs nothing. NaN until the loop has tuned it, and again
    /// after a retune that failed.
    double m_tunedHz = std::numeric_limits<double>::quiet_NaN();

    /// The RF path each port sees, resolved at configure time. Empty unless
    /// routing is on; indexed by `SweepStep::portIndex`.
    std::vector<RoutePort> m_routePorts;

    /// Where a frequency no antenna covers is measured, as an index into
    /// `m_routePorts`, or `kNoPort` to leave the port alone.
    std::size_t m_fallbackLeg = kNoPort;

    /// The switcher each leg goes through, or null. Parallel for the same
    /// reason, and resolved once so the sweep thread never looks a box up by
    /// name.
    std::vector<IRfPath*> m_legSwitchers;

    /// Which port the radio is on, following the same idempotence rule as
    /// `m_tunedHz`: `kNoPort` until the loop has selected one, and again after
    /// a selection that failed, so the next step re-applies rather than
    /// assuming.
    std::size_t m_currentPort = kNoPort;
};

} // namespace sweeppp
