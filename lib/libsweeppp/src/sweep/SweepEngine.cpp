// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/sweep/SweepEngine.hpp"

#include "sweeppp/core/Clock.hpp"
#include "sweeppp/core/Log.hpp"
#include "sweeppp/core/Toml.hpp"
#include "sweeppp/rf/RfRouting.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace sweeppp {
namespace {

/// What moving between two legs of the RF path costs, in seconds.
///
/// Only what actually changes: two antennas on one switcher pay the relay's
/// settle and nothing else, because the connector never moves. Kept in step
/// with the planner's own figure, which is what makes the predicted pass time
/// and the settle windows describe the same sweep.
double transitionSeconds(const RoutePort& from, const RoutePort& to) {
    if (from.portIndex != to.portIndex) {
        return to.switchSeconds + (to.inputIndex != kNoInput ? to.inputSwitchSeconds : 0.0);
    }
    return from.inputIndex != to.inputIndex ? to.inputSwitchSeconds : 0.0;
}

/// "rx2" or "rx2/in3" -- the whole chain, for the session's event stream.
std::string describeLeg(const RoutePort& leg, const IRfPath* switcher) {
    if (switcher == nullptr || leg.inputIndex == kNoInput) {
        return leg.id;
    }
    const std::span<const RfPathInput> inputs = switcher->inputs();
    if (leg.inputIndex >= inputs.size()) {
        return leg.id;
    }
    return std::format("{}/{}", leg.id, inputs[leg.inputIndex].id);
}

} // namespace

SweepEngine::SweepEngine(FrameBus& output, Telemetry& telemetry, EventBus& events)
    : m_output(output), m_telemetry(telemetry), m_events(events) {
}

SweepEngine::~SweepEngine() {
    SweepEngine::stop();
}

Status SweepEngine::configure(const SweepPlan& plan, IFftBackend& backend, ISdrDevice& device,
                              const AntennaLibrary& antennas, const AntennaAssignments& assignments,
                              std::span<const OpenRfPath> switchers) {
    if (running()) {
        return fail(ErrorCode::Unavailable, "cannot reconfigure a running sweep");
    }

    // The sample rate has to reach the radio before the grid is built from it.
    //
    // Every mapping the planner produces is derived from this one number: bin
    // width, the usable width of a step, how far each step advances, and each
    // step's first local and global bin. If the device is left running at some
    // other rate, each step's FFT covers a different amount of spectrum than
    // the grid reserves for it, and its bins are written at the wrong
    // frequencies -- stretched by exactly the ratio between the two rates.
    //
    // Reading the rate back matters as much as setting it. Radios quantise,
    // and a plan written for one machine may ask for more than this one can
    // deliver; planning against the accepted value keeps the grid honest
    // instead of describing a sweep that is not happening.
    SweepPlan effective = plan;
    if (auto applied = device.setParameter("sample_rate", SdrValue{plan.sampleRate}); !applied) {
        return std::unexpected(
            applied.error().withContext("applying the sweep plan's sample rate"));
    }
    if (const auto actual = device.getParameter("sample_rate")) {
        effective.sampleRate = asDouble(*actual);
    }

    if (std::abs(effective.sampleRate - plan.sampleRate) > 1.0) {
        logInfo("sweep", "{} accepted {} S/s for a requested {} S/s; planning against the former",
                device.info().label, toml_util::formatFrequencyShort(effective.sampleRate),
                toml_util::formatFrequencyShort(plan.sampleRate));
    }

    m_deliveryGranularitySeconds = device.deliveryGranularitySeconds(effective.sampleRate);

    // The plan may ask for a range the radio cannot reach. Refusing with the
    // device's actual limits is more useful than sweeping a truncated range
    // and leaving the operator to notice the missing spectrum.
    //
    // Checked before planning, not after: a range wholly outside the device
    // produces no usable steps, and the planner's "no usable tune steps" says
    // nothing about why.
    const SdrDeviceInfo& info = device.info();
    if (plan.lowestHz() < info.minFrequencyHz || plan.highestHz() > info.maxFrequencyHz) {
        return fail(ErrorCode::InvalidArgument, "the plan covers {} to {}, but {} tunes {} to {}",
                    toml_util::formatFrequencyShort(plan.lowestHz()),
                    toml_util::formatFrequencyShort(plan.highestHz()), info.label,
                    toml_util::formatFrequencyShort(info.minFrequencyHz),
                    toml_util::formatFrequencyShort(info.maxFrequencyHz));
    }

    // The RF path in front of the tuner, resolved through the one function
    // that knows what a chain is, so the planner never learns what an antenna
    // is and the panel's coverage readout cannot disagree with the sweep's.
    m_routePorts.clear();
    m_legSwitchers.clear();
    m_fallbackLeg = kNoPort;

    if (effective.antennaRouting) {
        const std::span<const SdrRxPort> ports = device.rxPorts();
        const std::vector<RfLeg> legs =
            resolveRfPath(info, ports, antennas, assignments, switchers);

        m_routePorts.reserve(legs.size() + 1);
        m_legSwitchers.reserve(legs.size() + 1);
        for (const RfLeg& leg : legs) {
            m_routePorts.push_back(leg.route);
            m_legSwitchers.push_back(leg.switcher);

            if (leg.antenna->needsBiasT && leg.route.portIndex < ports.size() &&
                !ports[leg.route.portIndex].biasTee) {
                // Said out loud and nothing more. Bias tee is the operator's
                // switch: an unpowered LNA measures its own noise floor, and
                // the sweep quietly turning the supply on would be a sweep
                // changing the instrument's calibration behind their back.
                logWarn("sweep",
                        "{} needs bias tee and {} cannot supply it; its band will be "
                        "measured through an unpowered antenna",
                        leg.antenna->name, leg.portLabel);
            }
        }

        // Where the frequencies nothing covers get measured.
        //
        // Appended as a leg of its own when the nominated port carries no
        // antenna, which is the ordinary case -- a wideband whip left on RX1
        // for exactly this is not something the operator has to describe
        // before it is usable. Its band is empty, so coverage never chooses
        // it; only the fallback rule does.
        if (const std::string_view fallbackId =
                assignments.fallbackPort(AntennaAssignments::deviceKey(info));
            !fallbackId.empty()) {
            const auto port = std::ranges::find_if(ports, [fallbackId](const SdrRxPort& candidate) {
                return candidate.id == fallbackId;
            });
            if (port != ports.end()) {
                const auto portIndex = static_cast<std::size_t>(port - ports.begin());
                const auto existing =
                    std::ranges::find_if(m_routePorts, [portIndex](const RoutePort& leg) {
                        return leg.portIndex == portIndex && leg.inputIndex == kNoInput;
                    });

                if (existing != m_routePorts.end()) {
                    m_fallbackLeg = static_cast<std::size_t>(existing - m_routePorts.begin());
                } else if (!m_routePorts.empty()) {
                    m_fallbackLeg = m_routePorts.size();
                    m_routePorts.push_back(RoutePort{.portIndex = portIndex,
                                                     .id = port->id,
                                                     .inputIndex = kNoInput,
                                                     .switchSeconds = port->switchSeconds});
                    m_legSwitchers.push_back(nullptr);
                }
            }
        }
    }

    auto schedule = SweepPlanner::plan(effective, backend, device.retuneSettleSeconds(),
                                       m_deliveryGranularitySeconds, info.minFrequencyHz,
                                       info.maxFrequencyHz, m_routePorts, m_fallbackLeg);
    if (!schedule) {
        return std::unexpected(schedule.error());
    }

    // The schedule and the grid it describes are swapped together, under the
    // lock. onFrame indexes m_stepValidFromNs by a step found in
    // m_schedule.steps, so a reader that saw the new steps and the old vector
    // would index out of bounds. Refusing while running is what makes that
    // reasoning hold for the lock-free reads in onFrame as well.
    const std::lock_guard lock(m_gridMutex);

    m_plan = effective;
    m_schedule = std::move(*schedule);

    m_grid.assign(m_schedule.gridBinCount, kUnmeasuredDbfs);
    m_gridWritten.assign(m_schedule.gridBinCount, 0);
    m_gridQuality.assign(m_schedule.gridBinCount, 0.0F);
    // Zero means "this step has not been tuned to yet", so nothing is
    // stitched for it until a retune records a time.
    m_stepValidFromNs.assign(m_schedule.steps.size(), 0);

    m_gridConfig = AcquisitionConfig{};
    m_gridConfig.sampleRate = m_plan.sampleRate;
    m_gridConfig.fftSize = m_schedule.fftSize;
    m_gridConfig.window = m_plan.window;
    m_gridConfig.windowBeta = m_plan.windowBeta;
    m_gridConfig.overlap = m_plan.fftOverlap;
    m_gridConfig.rbwHz = m_schedule.actualRbwHz;
    m_gridConfig.centerHz = (m_schedule.gridStartHz + m_schedule.gridStopHz()) * 0.5;
    m_gridConfig.spanHz = m_schedule.gridStopHz() - m_schedule.gridStartHz;
    m_gridConfig.deviceId = info.id;
    m_gridConfig.deviceLabel = info.label;

    if (auto window = Window::create(m_plan.window, m_schedule.fftSize, m_plan.windowBeta);
        window) {
        m_gridConfig.windowEnbw = window->properties().enbw;
    }

    return ok();
}

Status SweepEngine::start(ISdrDevice& device, Pipeline& pipeline) {
    if (running()) {
        return fail(ErrorCode::AlreadyExists, "sweep is already running");
    }
    if (m_schedule.steps.empty()) {
        return fail(ErrorCode::Unavailable, "configure() must succeed before starting a sweep");
    }

    m_device = &device;
    m_pipeline = &pipeline;
    m_passId.store(1, std::memory_order_relaxed);
    m_frameSequence.store(0, std::memory_order_relaxed);

    // Everything a previous run left behind, cleared before the new one can
    // see it. The settle deadlines are the one that bites: they are absolute
    // instants, so a deadline from a run that ended ten seconds ago is already
    // in the past and the first frames of this run -- taken while the
    // synthesiser is still moving -- sail through the discard that exists to
    // catch exactly them.
    m_currentStep.store(0, std::memory_order_relaxed);
    m_stepSatisfied.store(false, std::memory_order_relaxed);
    m_stitchedFrames.store(0, std::memory_order_relaxed);
    m_unsettledFrames.store(0, std::memory_order_relaxed);
    m_unattributedFrames.store(0, std::memory_order_relaxed);
    m_shortFrames.store(0, std::memory_order_relaxed);
    m_lastPassCoverage.store(0.0, std::memory_order_relaxed);
    m_measuredRate.store(0.0, std::memory_order_relaxed);
    m_stepsThisPass = 0;

    {
        const std::lock_guard lock(m_gridMutex);
        std::ranges::fill(m_stepValidFromNs, 0);
        std::ranges::fill(m_grid, kUnmeasuredDbfs);
        std::ranges::fill(m_gridWritten, std::uint8_t{0});
        std::ranges::fill(m_gridQuality, 0.0F);
    }

    m_running.store(true, std::memory_order_release);

    m_thread = std::jthread([this](std::stop_token stop) { sweepLoop(stop); });
    return ok();
}

void SweepEngine::stop() {
    // Not gated on m_running, which is why this is not an exchange.
    //
    // sweepLoop clears that flag itself when a plan that is not continuous
    // finishes its pass, so an early return here would leave a one-shot sweep
    // that ran to completion with its thread never joined and m_device still
    // pointing at a radio the caller is on the point of destroying.
    m_running.store(false, std::memory_order_release);
    m_thread.request_stop();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_device = nullptr;
    m_pipeline = nullptr;
}

bool SweepEngine::applyPort(const SweepStep& step) {
    const RoutePort& port = m_routePorts[step.portIndex];
    const std::span<const SdrRxPort> ports = m_device->rxPorts();
    IRfPath* switcher = m_legSwitchers[step.portIndex];

    const bool portChanged =
        m_currentPort == kNoPort || m_routePorts[m_currentPort].portIndex != port.portIndex;

    // Only what actually moves. Two antennas on one switcher share a
    // connector, and cycling the receiver's stream to click a relay would cost
    // a fifth of a second per switch for nothing.
    const bool requiresStop =
        (portChanged && port.portIndex < ports.size() && ports[port.portIndex].requiresStop) ||
        (switcher != nullptr && switcher->info().requiresStop);

    // Everything the switch has to do, in the order it has to do it, so the
    // stopped-stream case and the live one are one piece of code rather than
    // two that could drift.
    //
    // Bias tee is deliberately not among them. It is the operator's switch,
    // and a sweep that drove it would be a sweep changing the instrument's
    // calibration behind their back -- and would fight the panel's checkbox
    // several times a second. Where an antenna wants power the port is not
    // giving it, the row says so and the operator decides.
    const auto select = [this, &port, switcher, portChanged]() -> Status {
        if (portChanged) {
            if (auto selected = m_device->selectRxPort(port.id); !selected) {
                return selected;
            }
        }

        // The connector first, then the box behind it. The other order would
        // move a switcher that is still feeding the port being left, which on
        // a shared box is somebody else's measurement.
        if (switcher != nullptr && port.inputIndex != kNoInput) {
            if (auto selected = switcher->selectInput(static_cast<std::uint32_t>(port.inputIndex));
                !selected) {
                return selected;
            }
        }

        return ok();
    };

    Status switched = requiresStop && m_pipeline != nullptr && m_pipeline->running()
                          ? m_pipeline->cycleDeviceStream(select)
                          : select();

    if (!switched) {
        logWarn("sweep", "could not select {}: {}", describeLeg(port, switcher),
                switched.error().describe());
        return false;
    }

    // Calibration-affecting, genuinely: a different antenna is a different
    // response. This one line is what lets a recorded session say which
    // antenna measured which part of the sweep, months later, to somebody
    // reading the file with no idea what was on the bench.
    m_events.publish(ParameterChangedEvent{.monotonicNs = monotonicNs(),
                                           .key = "rx_port",
                                           .value = describeLeg(port, switcher),
                                           .gridAffecting = false,
                                           .calibrationAffecting = true});
    return true;
}

void SweepEngine::sweepLoop(std::stop_token stop) {
    m_passStartNs = monotonicNs();
    m_lastEmitNs = m_passStartNs;
    m_tunedHz = std::numeric_limits<double>::quiet_NaN();
    m_currentPort = kNoPort;

    while (!stop.stop_requested()) {
        for (std::size_t i = 0; i < m_schedule.steps.size() && !stop.stop_requested(); ++i) {
            const SweepStep& step = m_schedule.steps[i];

            m_stepSatisfied.store(false, std::memory_order_relaxed);
            m_currentStep.store(step.index, std::memory_order_release);

            // The port before the tuning, and this order is load-bearing.
            //
            // `retunedAtNs` below is stamped *after* this, so the step's
            // settle window opens after the switch and every frame still in
            // flight from the old port falls before it and is discarded as
            // unsettled. Those frames carry centre frequencies that belong to
            // the new port's steps, so stitching them would print a quiet
            // antenna's noise floor as spectrum -- silently, and identically
            // on every pass, which is exactly what reads as real structure.
            //
            // The other half of that guarantee is that each frequency belongs
            // to one port: `addSegment` merges overlapping and touching
            // segments and `validate()` rejects overlapping ones outright, so
            // two steps cannot share a centre and the centre alone stays a
            // usable key. Relax either and the failure is invisible in the
            // data -- see the test that asserts both.
            double switchSeconds = 0.0;
            if (step.portIndex != kNoPort && step.portIndex != m_currentPort) {
                if (!applyPort(step)) {
                    // Skipped rather than measured through whatever happens to
                    // be selected. The grid shows the gap, which is honest;
                    // the alternative prints one antenna's response as
                    // another's.
                    m_currentPort = kNoPort;
                    continue;
                }
                switchSeconds = m_currentPort == kNoPort
                                    ? m_routePorts[step.portIndex].switchSeconds +
                                          m_routePorts[step.portIndex].inputSwitchSeconds
                                    : transitionSeconds(m_routePorts[m_currentPort],
                                                        m_routePorts[step.portIndex]);
                m_currentPort = step.portIndex;

                // Gain, bandwidth and frequency are per-channel on hardware
                // that has several, and a driver that re-read them across the
                // switch has just replaced the cache. Forgetting where the
                // radio was tuned forces the retune below, which is what stops
                // this step measuring the band the *other* port was left on.
                m_tunedHz = std::numeric_limits<double>::quiet_NaN();
            }

            // A schedule of one step tunes once and stays there.
            //
            // Retuning to the frequency the radio is already on costs a command
            // and then a settle window of samples thrown away -- every pass,
            // for nothing. A span narrower than one step's usable band is a
            // fixed tune that happens to be driven by the sweep loop, and it
            // should cost what a fixed tune costs.
            const bool moved = !(std::abs(step.centerHz - m_tunedHz) < 1.0);
            if (moved) {
                if (auto retuned = m_device->retune(step.centerHz); !retuned) {
                    logWarn("sweep", "retune to {} failed: {}",
                            toml_util::formatFrequencyShort(step.centerHz),
                            retuned.error().describe());
                    m_tunedHz = std::numeric_limits<double>::quiet_NaN();
                    continue;
                }
                m_tunedHz = step.centerHz;
                m_telemetry.process().retunes.fetch_add(1, std::memory_order_relaxed);
            }

            m_pipeline->setSweepPosition(m_passId.load(std::memory_order_relaxed), step.index,
                                         false);

            const std::uint64_t retunedAtNs = monotonicNs();

            // Samples captured during the settle window are not trustworthy:
            // the synthesiser is still moving. Frames stamped before this
            // instant are discarded in onFrame() rather than stitched. Left
            // alone when nothing moved -- the window it opened has long since
            // passed, and reopening it would discard good frames for a
            // synthesiser that never left.
            if (moved) {
                {
                    // The switch settle rides on top of the retune's: a port
                    // change may have rebuilt the whole stream, and the first
                    // buffers out of a rebuilt one are no more trustworthy
                    // than the first samples after a retune.
                    const std::lock_guard lock(m_gridMutex);
                    m_stepValidFromNs[i] =
                        retunedAtNs + secondsToNs(step.settleSeconds + switchSeconds);
                }

                m_events.publish(RetuneEvent{.monotonicNs = retunedAtNs,
                                             .centerHz = step.centerHz,
                                             .stepIndex = step.index});
            }

            // Dwell = settle + collection, where the collection window must be
            // long enough for a whole acquisition block to land inside it.
            //
            // Blocks are the unit the radio actually delivers, and they arrive
            // on their own boundaries -- nothing aligns them to a retune. A
            // block is usable only if it both starts after the settle window
            // and finishes before the next retune, so in the worst phase
            // alignment the window has to be two block times wide for one to
            // fall wholly inside it.
            //
            // Sizing this to a single block instead (the FFT's own collection
            // time) is what produced an evenly spaced comb of unmeasured bands:
            // only steps whose window happened to open exactly on a block
            // boundary were ever measured, and since both periods are fixed,
            // the ones that missed did so at a regular frequency interval.
            //
            // This is a deadline, not a fixed dwell. The loop below leaves as
            // soon as a step is satisfied, so a device that delivers promptly
            // pays none of this.
            const double blockSeconds =
                m_pipeline->framesPerBlock() > 0
                    ? static_cast<double>(m_pipeline->framesPerBlock()) / m_plan.sampleRate
                    : static_cast<double>(m_plan.averageCount) *
                          static_cast<double>(m_schedule.fftSize) / m_plan.sampleRate;

            // ...and never shorter than two of whatever the device hands over
            // at a time. A radio that delivers a USB transfer at a time cannot
            // resolve a retune any finer than that transfer, however small the
            // blocks are cut afterwards: everything in it arrives with one
            // tuning attached. Retuning faster than the radio delivers is what
            // makes a linear sweep look like it is hopping at random -- the
            // few steps that coincide with a delivery collect everything and
            // the rest are never measured at all.
            const double collectSeconds =
                step.dwellSeconds + 2.0 * std::max(blockSeconds, m_deliveryGranularitySeconds);
            const double settleSeconds = moved ? step.settleSeconds + switchSeconds : 0.0;
            const std::uint64_t stepEndNs =
                retunedAtNs + secondsToNs(settleSeconds + collectSeconds);

            // Leave the step as soon as it has produced data, rather than
            // sitting out the full budget. On a fast device this is what turns
            // the planner's conservative estimate into a real sweep rate.
            while (!stop.stop_requested() && monotonicNs() < stepEndNs) {
                if (m_stepSatisfied.load(std::memory_order_acquire) &&
                    monotonicNs() > retunedAtNs + secondsToNs(settleSeconds)) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }

            emitPartial();

            // The gap between the planner's predicted step time and the real
            // one is the single most useful number when a sweep is slower than
            // it should be, and it is invisible from the outside: the pass
            // rate alone cannot say whether the cost is retuning, waiting for
            // samples, or stitching.
            if (Log::enabled(LogLevel::Debug) && (i % 128) == 0) {
                logDebug("sweep", "step {}/{} at {}: {:.1f} us actual vs {:.1f} us planned", i,
                         m_schedule.steps.size(), toml_util::formatFrequencyShort(step.centerHz),
                         nsToSeconds(monotonicNs() - retunedAtNs) * 1e6,
                         (step.settleSeconds + collectSeconds) * 1e6);
            }
        }

        if (stop.stop_requested()) {
            break;
        }

        completePass();

        if (!m_plan.continuous) {
            break;
        }
    }

    m_running.store(false, std::memory_order_release);
}

void SweepEngine::onFrame(const SpectrumFramePtr& frame) noexcept {
    if (!frame || !m_running.load(std::memory_order_acquire)) {
        return;
    }

    // Which step this frame *came from*, found by its own centre frequency --
    // not by whichever step the sweep loop has reached by now.
    //
    // The pipeline has real latency: a block is queued, a worker converts and
    // transforms it, and only then is a frame published. At ~1300 steps a
    // second the radio has retuned several times in that window, so trusting
    // the current step index writes one band's measurements into another
    // band's bins. The block carries the centre it was captured at precisely
    // so this does not have to be guessed.
    const std::optional<std::size_t> stepIndex = findStep(frame->config.centerHz);
    if (!stepIndex) {
        m_unattributedFrames.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const SweepStep& step = m_schedule.steps[*stepIndex];

    // Discard the settle window. Keeping these would smear the previous step's
    // signal across this step's band -- a convincing-looking artefact.
    //
    // Compared per step rather than against a single global instant, because
    // frames can arrive out of order with respect to retunes.
    {
        const std::lock_guard lock(m_gridMutex);
        const std::uint64_t validFrom = m_stepValidFromNs[*stepIndex];
        if (validFrom == 0 || frame->hostTimeNs < validFrom) {
            m_unsettledFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    if (frame->binCount() < step.localBinLimit()) {
        m_shortFrames.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    m_stitchedFrames.fetch_add(1, std::memory_order_relaxed);
    stitch(*frame, step);

    // Only a frame from the step being collected right now can end its dwell.
    // Frames for earlier steps are still arriving -- the pipeline runs several
    // steps behind at speed -- and letting one of those satisfy the current
    // step would cut its collection short before the radio had delivered
    // anything at this frequency.
    if (*stepIndex == m_currentStep.load(std::memory_order_acquire)) {
        m_stepSatisfied.store(true, std::memory_order_release);
    }
}

std::optional<std::size_t> SweepEngine::findStep(double centerHz) const {
    if (m_schedule.steps.empty() || centerHz <= 0.0) {
        return std::nullopt;
    }

    // Steps ascend in frequency within each segment, so the nearest centre is
    // one binary search away. A linear scan would also work at these sizes,
    // but this runs per frame at the full step rate.
    const auto it = std::ranges::lower_bound(m_schedule.steps, centerHz, {},
                                             [](const SweepStep& step) { return step.centerHz; });

    std::size_t best = it == m_schedule.steps.end()
                           ? m_schedule.steps.size() - 1
                           : static_cast<std::size_t>(it - m_schedule.steps.begin());

    if (best > 0 && std::abs(m_schedule.steps[best - 1].centerHz - centerHz) <
                        std::abs(m_schedule.steps[best].centerHz - centerHz)) {
        --best;
    }

    // Reject anything not plausibly from a planned step. A frame from before
    // the sweep started, or from a discontinuous plan's gap, must not be
    // stitched into the nearest band just because it is nearest.
    const double tolerance = std::max(m_plan.sampleRate * 0.25, 1.0);
    if (std::abs(m_schedule.steps[best].centerHz - centerHz) > tolerance) {
        return std::nullopt;
    }

    return best;
}

void SweepEngine::stitch(const SpectrumFrame& frame, const SweepStep& step) {
    const std::lock_guard lock(m_gridMutex);

    // Up to two runs per step, either side of the discarded guard around the
    // step's own centre frequency.
    for (std::size_t r = 0; r < step.rangeCount; ++r) {
        const SweepStep::BinRange& range = step.ranges[r];

        for (std::size_t i = 0; i < range.binCount; ++i) {
            const std::size_t globalBin = range.firstGlobalBin + i;
            if (globalBin >= m_grid.size()) {
                break;
            }

            const float value = frame.binsDbfs[range.firstLocalBin + i];

            // How good this particular measurement of this particular
            // frequency is: how far inside the step's kept band it sits.
            //
            // The receiver's own artefacts live at fixed offsets from the LO
            // -- a noise hump around the tuning, roll-off at the band edge --
            // so distance from both is a direct measure of how much of what
            // was measured is actually the spectrum. Overlap exists precisely
            // because a frequency near one step's edge is near the middle of
            // its neighbour's, so there is nearly always a better copy.
            const double frequency =
                m_schedule.gridStartHz +
                m_schedule.gridBinWidthHz * (static_cast<double>(globalBin) + 0.5);
            const double offset = std::abs(frequency - step.centerHz);
            const auto depth = static_cast<float>(std::min(offset - m_schedule.dcGuardHalfWidthHz,
                                                           m_schedule.usableHalfWidthHz - offset));

            if (m_gridWritten[globalBin] == 0) {
                m_grid[globalBin] = value;
                m_gridQuality[globalBin] = depth;
                m_gridWritten[globalBin] = 1;
                continue;
            }

            switch (m_plan.overlapResolution) {
            case SweepPlan::OverlapResolution::Best:
                if (depth > m_gridQuality[globalBin]) {
                    m_grid[globalBin] = value;
                    m_gridQuality[globalBin] = depth;
                }
                break;
            case SweepPlan::OverlapResolution::Max:
                m_grid[globalBin] = std::max(m_grid[globalBin], value);
                break;
            case SweepPlan::OverlapResolution::Mean:
                m_grid[globalBin] = 0.5F * (m_grid[globalBin] + value);
                break;
            }
        }
    }
}

void SweepEngine::emitPartial() {
    const std::uint64_t now = monotonicNs();

    // Rate-limited here, at the *output*, rather than by the pipeline on the
    // input side.
    //
    // This is the correct place for it, and putting it upstream was a real
    // bug: at ~1300 steps per second against a 60 Hz publish limit, the
    // pipeline dropped ~96% of per-step frames before the engine ever saw
    // them, and because the two rates are fixed the surviving frames landed on
    // a regular stride -- leaving an evenly spaced comb of never-measured
    // bands across the sweep.
    //
    // The display gains nothing from more than a few updates a second, and
    // each one copies the entire stitched grid, which can be megabytes on a
    // wide plan. Throttling the copy is free; throttling the measurement is
    // data loss.
    constexpr double kPartialEmitsPerSecond = 30.0;
    const auto minimumInterval = static_cast<std::uint64_t>(1e9 / kPartialEmitsPerSecond);
    if (m_lastEmitNs != 0 && now - m_lastEmitNs < minimumInterval) {
        return;
    }

    auto frame = std::make_shared<SpectrumFrame>();

    {
        const std::lock_guard lock(m_gridMutex);
        frame->binsDbfs = m_grid;
        frame->config = m_gridConfig;
    }

    frame->sequence = m_frameSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    frame->hostTimeNs = now;
    frame->wallTimeNs = wallClockNs();
    frame->sweepPass = m_passId.load(std::memory_order_relaxed);
    frame->sweepStep = m_currentStep.load(std::memory_order_relaxed);
    frame->passComplete = false;
    frame->startHz = m_schedule.gridStartHz;
    frame->binWidthHz = m_schedule.gridBinWidthHz;
    frame->averageCount = m_plan.averageCount;

    ++m_stepsThisPass;
    m_lastEmitNs = now;

    // Published on every step, not once per pass. This is what keeps the
    // waterfall alive across a multi-GHz span instead of freezing between
    // passes.
    m_output.publish(frame);
}

void SweepEngine::completePass() {
    const std::uint64_t now = monotonicNs();
    const double passSeconds = nsToSeconds(now - m_passStartNs);
    const std::uint64_t passId = m_passId.load(std::memory_order_relaxed);

    if (passSeconds > 0.0) {
        const double rate = m_plan.totalSpanHz() / passSeconds;
        m_measuredRate.store(rate, std::memory_order_relaxed);
        m_telemetry.render().sweepSpeedHzPerSec.store(rate, std::memory_order_relaxed);
    }

    m_events.publish(SweepPassEvent{.monotonicNs = now,
                                    .passId = passId,
                                    .startHz = m_schedule.gridStartHz,
                                    .stopHz = m_schedule.gridStopHz(),
                                    .durationSeconds = passSeconds});

    // The completed pass is published with passComplete set, so a consumer
    // that only wants whole passes (an export, a plugin measuring a full span)
    // can wait for it while the display keeps taking the partials.
    auto frame = std::make_shared<SpectrumFrame>();
    {
        const std::lock_guard lock(m_gridMutex);
        frame->binsDbfs = m_grid;
        frame->config = m_gridConfig;

        // How much of the span this pass actually replaced, taken before the
        // flags are cleared. Because values persist, this is the only place
        // the difference between "measured" and "still showing what it
        // measured a while ago" is visible.
        const auto written = static_cast<double>(
            std::ranges::count_if(m_gridWritten, [](std::uint8_t v) { return v != 0; }));
        m_lastPassCoverage.store(
            m_gridWritten.empty() ? 0.0 : written / static_cast<double>(m_gridWritten.size()),
            std::memory_order_relaxed);

        // Carrying the previous pass forward would make a signal that has
        // stopped transmitting appear to persist indefinitely. Resetting the
        // written flags -- while leaving the values -- means the display keeps
        // showing the last measurement until it is genuinely replaced, but the
        // overlap resolution starts fresh.
        std::ranges::fill(m_gridWritten, 0);
    }

    frame->sequence = m_frameSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    frame->hostTimeNs = now;
    frame->wallTimeNs = wallClockNs();
    frame->sweepPass = passId;
    frame->sweepStep = static_cast<std::uint32_t>(m_schedule.steps.size());
    frame->passComplete = true;
    frame->startHz = m_schedule.gridStartHz;
    frame->binWidthHz = m_schedule.gridBinWidthHz;
    frame->averageCount = m_plan.averageCount;

    m_output.publish(frame);

    m_telemetry.process().sweepPassesCompleted.fetch_add(1, std::memory_order_relaxed);
    if (m_pipeline != nullptr) {
        m_pipeline->setSweepPosition(passId, 0, true);
    }

    m_passId.fetch_add(1, std::memory_order_relaxed);
    m_passStartNs = now;
    m_stepsThisPass = 0;
}

} // namespace sweeppp
