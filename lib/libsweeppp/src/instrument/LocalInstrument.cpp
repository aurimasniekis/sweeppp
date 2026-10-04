// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/instrument/LocalInstrument.hpp"

#include "sweeppp/core/Clock.hpp"
#include "sweeppp/core/Log.hpp"
#include "sweeppp/core/Paths.hpp"
#include "sweeppp/core/Toml.hpp"
#include "sweeppp/rf/RfRouting.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace sweeppp {
namespace {

/// The one parameter key the sweep plan owns as well as the driver.
constexpr std::string_view kSampleRateKey = "sample_rate";

/// How often health, staleness and the like are sampled -- a USB control
/// transfer each, so not per frame.
constexpr std::uint64_t kSlowTickNs = 250'000'000ULL;

} // namespace

ScheduleSummary ScheduleSummary::of(const SweepSchedule& schedule) {
    ScheduleSummary summary;
    summary.stepCount = schedule.steps.size();
    summary.fftSize = schedule.fftSize;
    summary.actualRbwHz = schedule.actualRbwHz;
    summary.gridStartHz = schedule.gridStartHz;
    summary.gridBinWidthHz = schedule.gridBinWidthHz;
    summary.gridBinCount = schedule.gridBinCount;
    summary.estimatedPassSeconds = schedule.estimatedPassSeconds;
    summary.estimatedSweepRateHzPerSec = schedule.estimatedSweepRateHzPerSec;
    summary.retuneOverheadFraction = schedule.retuneOverheadFraction;
    summary.unroutedHz = schedule.unroutedHz;
    summary.portSwitches = schedule.portSwitches;
    return summary;
}

InstrumentPaths InstrumentPaths::fromConfig() {
    const Paths& paths = Paths::instance();
    return InstrumentPaths{.antennaSearchPath = paths.searchPath("antennas"),
                           .antennasDir = paths.antennasDir(),
                           .calibrationDir = paths.calibrationDir()};
}

InstrumentPaths InstrumentPaths::under(const std::filesystem::path& root) {
    return InstrumentPaths{.antennaSearchPath = {root / "antennas"},
                           .antennasDir = root / "antennas",
                           .calibrationDir = root / "calibration"};
}

LocalInstrument::LocalInstrument(FrameBus& output, EventBus& events, Telemetry& telemetry,
                                 InstrumentPaths paths, IFftBackend& backend)
    : m_output(output), m_events(events), m_telemetry(telemetry), m_paths(std::move(paths)),
      m_backend(&backend) {
    m_pipeline = std::make_unique<Pipeline>(m_pipelineBus, m_telemetry, m_events);
    m_sweepEngine = std::make_unique<SweepEngine>(m_output, m_telemetry, m_events);

    // The learners read the sweep through the engine's hook and advance on
    // its passes. Both are wired once, here, and decide per frame whether
    // anything is listening.
    m_sweepEngine->setStepObserver(
        [this](const SpectrumFrame& frame, const SweepStep&) { observeStep(frame); });
    m_passSubscription = m_events.subscribe<SweepPassEvent>(
        [this](const SweepPassEvent&) { m_passesSeen.fetch_add(1, std::memory_order_relaxed); });

    // Fixed-tune defaults.
    //
    // 50% overlap with a Hann window is the textbook pairing: the two halves
    // sum to unity, so no sample is weighted out and a short burst cannot fall
    // between transforms and be missed entirely. Averaging stays at one -- a
    // live display should start responsive.
    m_pipelineConfig = PipelineConfig{.fftSize = 4096,
                                      .window = WindowType::Hann,
                                      .overlap = 0.5,
                                      .averageCount = 1,
                                      .targetFrameRate = 60.0};

    m_sweepPlan.segments = {SweepSegment{.startHz = 88e6, .stopHz = 108e6}};
    m_sweepPlan.sampleRate = 20e6;
    m_sweepPlan.rbwHz = 25e3;
    m_sweepPlan.applyMode(SweepMode::Fast);

    reloadAntennas();
    m_assignments = AntennaAssignments::load(m_paths.antennasDir / "assignments.toml");
    refreshSwitchers();
}

LocalInstrument::~LocalInstrument() {
    stop();
    closeDevice();
    m_sweepEngine->setStepObserver({});
    m_events.unsubscribe(m_passSubscription);
    listenForLearning(false);
}

void LocalInstrument::listenForLearning(bool listen) {
    // Only while a learn runs: the tap takes a lock for every frame, and on
    // the output bus it would be one more consumer for nothing.
    if (listen && m_learnTapSubscription == 0) {
        m_learnTapSubscription = m_output.subscribe(&m_learnTap);
    } else if (!listen && m_learnTapSubscription != 0) {
        m_output.unsubscribe(m_learnTapSubscription);
        m_learnTapSubscription = 0;
    }
}

void LocalInstrument::notify(InstrumentNotice::Kind kind, std::string text) {
    m_notices.push_back(InstrumentNotice{.kind = kind, .text = std::move(text)});
}

std::vector<InstrumentNotice> LocalInstrument::takeNotices() {
    return std::exchange(m_notices, {});
}

// ---- identity ---------------------------------------------------------------

std::string LocalInstrument::profileDriver() const {
    return m_device ? m_device->info().driver : std::string{};
}

std::string LocalInstrument::profileId() const {
    return m_device ? m_device->info().id : std::string{};
}

std::string LocalInstrument::displayLabel() const {
    return m_device ? m_device->info().label : std::string{};
}

bool LocalInstrument::holds(std::string_view driver, std::string_view id) const noexcept {
    return m_device && m_device->info().driver == driver && m_device->info().id == id;
}

// ---- the radio --------------------------------------------------------------

void LocalInstrument::refreshDescriptor() {
    if (!m_device) {
        m_descriptor.reset();
        return;
    }
    DeviceDescriptor descriptor;
    descriptor.info = m_device->info();
    const std::span<const SdrParameter> parameters = m_device->parameters();
    descriptor.parameters.assign(parameters.begin(), parameters.end());
    const std::span<const SdrRxPort> ports = m_device->rxPorts();
    descriptor.rxPorts.assign(ports.begin(), ports.end());
    descriptor.supportedSampleRates = m_device->supportedSampleRates();
    m_descriptor = std::move(descriptor);
}

const DeviceDescriptor* LocalInstrument::device() const noexcept {
    return m_descriptor ? &*m_descriptor : nullptr;
}

std::optional<SdrValue> LocalInstrument::parameter(std::string_view key) const {
    if (!m_device) {
        return std::nullopt;
    }
    auto value = m_device->getParameter(key);
    if (!value) {
        return std::nullopt;
    }
    return std::move(*value);
}

std::string LocalInstrument::selectedRxPort() const {
    return m_device ? std::string(m_device->selectedRxPort()) : std::string{};
}

void LocalInstrument::adoptDevice(std::unique_ptr<ISdrDevice> device) {
    stop();
    closeDevice();
    m_device = std::move(device);
    refreshDescriptor();

    // A freshly opened radio sweeps its whole range by default.
    //
    // The first thing an operator wants from a new device is to see what is
    // out there, and a fixed tune at some arbitrary default centre shows one
    // sample rate's worth of it. Any narrower interest is a zoom away; the
    // reverse -- guessing which 20 MHz the operator meant -- is not.
    const SdrDeviceInfo& info = m_device->info();
    m_sweeping = true;
    m_sweepPlan.segments = {
        SweepSegment{.startHz = info.minFrequencyHz, .stopHz = info.maxFrequencyHz}};

    // The fastest rate the device offers, because sweep rate scales with it:
    // a wider step covers the same span in fewer retunes, and retuning is what
    // a wide sweep spends most of its time doing.
    const std::vector<double> rates = m_device->supportedSampleRates();
    if (const auto fastest = std::ranges::max_element(rates); fastest != rates.end()) {
        m_sweepPlan.sampleRate = *fastest;
    }

    m_events.publish(DeviceOpenedEvent{.monotonicNs = monotonicNs(),
                                       .deviceId = info.id,
                                       .label = info.label,
                                       .serial = info.serial});

    loadCalibration();
    sampleHealth();
    ++m_revision;
}

void LocalInstrument::closeDevice() {
    if (!m_device) {
        return;
    }
    stop();
    m_events.publish(DeviceClosedEvent{.monotonicNs = monotonicNs(),
                                       .deviceId = m_device->info().id,
                                       .reason = "closed by operator"});
    m_device.reset();
    m_descriptor.reset();
    m_health.clear();
    loadCalibration();
    ++m_revision;
}

void LocalInstrument::adoptConfiguration(const SweepPlan& plan, const PipelineConfig& config,
                                         bool sweeping, const CorrectionSettings& corrections) {
    stop();
    m_sweepPlan = plan;
    m_pipelineConfig = config;
    m_sweeping = sweeping;
    setCorrectionSettings(corrections);
}

Status LocalInstrument::setDeviceParameter(const std::string& key, const SdrValue& value) {
    if (!m_device) {
        return fail(ErrorCode::NotFound, "no device is open");
    }

    if (key == kSampleRateKey && m_sweeping) {
        SweepPlan plan = m_sweepPlan;
        plan.sampleRate = asDouble(value);
        if (auto applied = applySweepPlan(plan); !applied) {
            return applied;
        }
    } else if (auto applied = m_device->setParameter(key, value); !applied) {
        return applied;
    }

    // Published here, where the change is made, rather than by whoever asked:
    // a remote panel and a profile being applied are both "whoever asked", and
    // the recorder must hear about each change exactly once.
    const auto declared =
        std::ranges::find(m_descriptor->parameters, std::string_view(key), &SdrParameter::key);
    m_events.publish(ParameterChangedEvent{
        .monotonicNs = monotonicNs(),
        .key = key,
        .value = toString(value),
        .gridAffecting = declared != m_descriptor->parameters.end() && declared->gridAffecting,
        .calibrationAffecting =
            declared != m_descriptor->parameters.end() && declared->calibrationAffecting});
    return ok();
}

bool LocalInstrument::parameterNeedsStop(const SdrParameter& parameter) const noexcept {
    if (!parameter.requiresStop) {
        return false;
    }
    return !(m_sweeping && parameter.key == kSampleRateKey);
}

std::vector<SdrHealthReading> LocalInstrument::health() const {
    return m_health;
}

void LocalInstrument::sampleHealth() {
    // Each of these is a USB control transfer, so it is read on the slow tick
    // rather than per frame -- a hundred round trips a second on the bus
    // carrying the samples would be load for nothing.
    if (m_device) {
        m_health = m_device->healthReadings();
    } else {
        m_health.clear();
    }
}

// ---- running ----------------------------------------------------------------

std::filesystem::path LocalInstrument::calibrationPath() const {
    return m_paths.calibrationFile.empty()
               ? CorrectionSet::pathFor(m_device->info(), m_paths.calibrationDir)
               : m_paths.calibrationFile;
}

Status LocalInstrument::applyPipelineConfig(const PipelineConfig& config) {
    m_pipelineConfig = config;
    if (config.targetFrameRate > 0.0) {
        m_fixedFrameRate = config.targetFrameRate;
    }

    if (!running()) {
        return m_pipeline->configure(*m_backend, config);
    }

    // A sweep is a second thread driving this pipeline, and reconfigure()
    // stops it. The sweep loop reads framesPerBlock() and calls
    // cycleDeviceStream() on the pipeline while stop() is clearing the pool
    // and the device out from under it -- a check-then-use across two threads,
    // which is a null dereference at best. Stop the pair and start it again,
    // which is what re-planning has always done.
    if (m_sweepEngine->running()) {
        return restart();
    }

    return m_pipeline->reconfigure(config);
}

Status LocalInstrument::restart() {
    stop();
    if (auto started = start(); !started) {
        return started;
    }
    adoptEffectivePlan();
    return ok();
}

Status LocalInstrument::setSweeping(bool enabled) {
    if (enabled == m_sweeping) {
        return ok();
    }
    m_sweeping = enabled;

    // Not a display preference. The flag decides what start() builds -- the
    // engine between the two buses, or the display forwarder -- and it also
    // gates the waterfall, which only advances on a completed pass while it is
    // set. Writing it under a running fixed tune therefore froze the waterfall
    // for good: nothing in that mode ever sets passComplete.
    return running() ? restart() : ok();
}

Status LocalInstrument::sweepRange(const SweepPlan& plan) {
    // One restart, not two. Dragging out a range is a single request even
    // though it changes two things, and applying them separately would rebuild
    // acquisition twice -- each rebuild a stream stop and start on the radio.
    m_sweeping = true;
    return applySweepPlan(plan);
}

Status LocalInstrument::applySweepPlan(const SweepPlan& plan) {
    // The plan is kept whatever it says, so the editor always shows what was
    // typed, but only a *valid* one is allowed near the radio.
    //
    // Editing a range passes through states that are briefly nonsense -- a
    // stop below its start, a span of zero -- and every keystroke arrives
    // here. Acting on those would stop a running sweep on the way to a
    // perfectly good range.
    m_sweepPlan = plan;

    if (!m_device) {
        return ok();
    }
    if (auto valid = plan.validate(); !valid) {
        return valid;
    }

    // Applied to a running sweep by cycling acquisition, not by making the
    // operator stop and start. The plan sets the FFT size, the grid and the
    // block size, so the pipeline has to be rebuilt around it; every
    // grid-affecting change opens a new segment in the session file, which is
    // exactly what the format is for.
    if (!running()) {
        if (auto configured = m_sweepEngine->configure(plan, *m_backend, *m_device, m_antennas,
                                                       m_assignments, m_openPaths);
            !configured) {
            return configured;
        }
        adoptEffectivePlan();
        return ok();
    }

    return restart();
}

Status LocalInstrument::startBenchmark(const FftBenchmarkConfig& config) {
    if (m_benchmark.running()) {
        return fail(ErrorCode::Unavailable, "a benchmark is already running");
    }
    m_benchmark.start(config);
    return ok();
}

BenchmarkStatus LocalInstrument::benchmark() const {
    return BenchmarkStatus{.running = m_benchmark.running(),
                           .complete = m_benchmark.complete(),
                           .stepsDone = m_benchmark.stepsDone(),
                           .stepsTotal = m_benchmark.stepsTotal(),
                           .currentStep = m_benchmark.currentStep(),
                           .elapsedSeconds = m_benchmark.elapsedSeconds(),
                           .results = m_benchmark.results()};
}

std::vector<FftBackendInfo> LocalInstrument::fftBackends() const {
    return FftBackendManager::instance().enumerate();
}

std::string LocalInstrument::fftBackendName() const {
    return m_backend != nullptr ? std::string(m_backend->name()) : std::string{};
}

Status LocalInstrument::setFftBackend(std::string_view name) {
    if (m_backend != nullptr && m_backend->name() == name) {
        return ok();
    }

    // Acquired before anything is torn down. A backend that cannot instantiate
    // demotes itself in the registry on first use, and finding that out after
    // stopping acquisition would leave a stopped instrument with the old
    // backend still selected.
    auto acquired = FftBackendManager::instance().acquire(name);
    if (!acquired) {
        return std::unexpected(acquired.error());
    }
    m_backend = *acquired;

    logInfo("fft", "backend is now {}", m_backend->name());

    // The pipeline's plan and the sweep schedule are both built from the
    // backend -- the schedule because `snapSize` decides the FFT size, and so
    // the grid. Both have to be rebuilt, which is what start() does.
    if (running()) {
        return restart();
    }

    if (m_sweeping && m_device) {
        if (auto configured = m_sweepEngine->configure(m_sweepPlan, *m_backend, *m_device,
                                                       m_antennas, m_assignments, m_openPaths);
            !configured) {
            return configured;
        }
        m_pipelineConfig.fftSize = m_sweepEngine->schedule().fftSize;
        adoptEffectivePlan();
    }
    return applyPipelineConfig(m_pipelineConfig);
}

void LocalInstrument::adoptEffectivePlan() {
    // The plan the engine is running, not the one that was asked for.
    //
    // Radios quantise and clamp: a request for 100 MS/s on a device that
    // stops at 20 becomes 20, and the engine plans the grid against what it
    // actually got. Leaving the requested figure in the panel would show a
    // sample rate nothing is using, and the predicted sweep rate derived from
    // it would be wrong by the same factor.
    if (!m_sweepEngine->schedule().steps.empty()) {
        m_sweepPlan = m_sweepEngine->plan();
    }
    m_schedule = ScheduleSummary::of(m_sweepEngine->schedule());

    reportUnroutedRanges();
}

void LocalInstrument::reportUnroutedRanges() {
    const std::vector<std::pair<double, double>>& unrouted = m_sweepEngine->schedule().unroutedHz;

    // Once per change, not once per plan. An operator dragging a range out on
    // the spectrum re-plans on every frame, and a warning per frame would bury
    // the display under the very thing it is trying to say.
    if (unrouted == m_reportedUnroutedHz) {
        return;
    }
    m_reportedUnroutedHz = unrouted;

    if (unrouted.empty()) {
        return;
    }

    std::string ranges;
    for (const auto& [fromHz, toHz] : unrouted) {
        if (!ranges.empty()) {
            ranges += ", ";
        }
        ranges += std::format("{} - {}", toml_util::formatFrequencyShort(fromHz),
                              toml_util::formatFrequencyShort(toHz));
    }

    notify(InstrumentNotice::Kind::Warning,
           std::format("no assigned antenna covers {}; it will be swept through "
                       "whichever connector is selected",
                       ranges));
}

EngineStats LocalInstrument::engineStats() const {
    const SweepEngine::FrameAccounting accounting = m_sweepEngine->frameAccounting();
    return EngineStats{.stitched = accounting.stitched,
                       .unsettled = accounting.unsettled,
                       .unattributed = accounting.unattributed,
                       .tooShort = accounting.tooShort,
                       .lastPassCoverage = m_sweepEngine->lastPassCoverage(),
                       .measuredSweepRateHzPerSec = m_sweepEngine->measuredSweepRateHzPerSec(),
                       .passCount = m_sweepEngine->passCount()};
}

Status LocalInstrument::start() {
    if (!m_device) {
        return fail(ErrorCode::Unavailable, "no device is open");
    }
    if (running()) {
        return ok();
    }

    m_telemetry.reset();

    // Whatever sat between the buses last run is replaced, not stacked --
    // otherwise switching between sweep and fixed tune would leave both
    // attached and publish every frame twice.
    if (m_pipelineSubscription != 0) {
        m_pipelineBus.unsubscribe(m_pipelineSubscription);
        m_pipelineSubscription = 0;
    }

    const auto failed = [this](const Error& error) {
        notify(InstrumentNotice::Kind::Condition, error.describe());
        return Status(std::unexpected(error));
    };

    if (m_sweeping) {
        if (auto configured = m_sweepEngine->configure(m_sweepPlan, *m_backend, *m_device,
                                                       m_antennas, m_assignments, m_openPaths);
            !configured) {
            return failed(configured.error());
        }

        // In sweep mode the engine sits between the two buses.
        m_pipelineConfig.fftSize = m_sweepEngine->schedule().fftSize;

        // Every step's frame must reach the engine. The pipeline's frame-rate
        // cap is a *display* concern, and applying it here would silently drop
        // most steps' measurements before they could be stitched -- the engine
        // throttles its own stitched output instead.
        m_pipelineConfig.targetFrameRate = 0.0;

        m_pipelineSubscription = m_pipelineBus.subscribe(m_sweepEngine.get());
    } else {
        // Fixed tune: forwarded straight through, so the display cap applies
        // to the pipeline directly.
        m_pipelineConfig.targetFrameRate = m_fixedFrameRate;
        m_pipelineSubscription = m_pipelineBus.subscribe(&m_displayForwarder);
    }

    if (auto configured = m_pipeline->configure(*m_backend, m_pipelineConfig); !configured) {
        // Through stop(), so the subscription made above does not outlive the
        // failed start. Stopping a pipeline that never started is a no-op.
        stop();
        return failed(configured.error());
    }
    pushCorrectionSettings();

    const double sampleRate =
        asDouble(m_device->getParameter("sample_rate").value_or(SdrValue{20e6}));
    const double centerHz = asDouble(m_device->getParameter("center_hz").value_or(SdrValue{100e6}));
    m_pipeline->setTuning(centerHz, sampleRate, sampleRate);

    // Block size is a sweep-rate decision, not a constant.
    //
    // A block is stamped with one centre frequency, so it must not span a
    // retune. At 20 MS/s a 262144-frame block covers 13 ms, while a sweep step
    // may dwell for well under a millisecond -- one block would then straddle
    // a dozen retunes. Sizing it to the step's own collection window keeps one
    // block inside one step. Fixed tune has no such constraint and prefers
    // large blocks, which cost fewer wakeups per sample.
    std::size_t framesPerBlock = 262'144;
    if (m_sweeping) {
        const std::uint32_t fftSize = m_sweepEngine->schedule().fftSize;
        const std::uint32_t averages = std::max(m_pipelineConfig.averageCount, 1U);
        framesPerBlock =
            std::clamp<std::size_t>(static_cast<std::size_t>(fftSize) * averages, 2048, 262'144);
    }

    const StreamConfig streamConfig{
        .framesPerBlock = framesPerBlock, .blockCount = 64, .format = m_device->nativeFormat()};

    if (auto started = m_pipeline->start(*m_device, streamConfig); !started) {
        stop();
        return failed(started.error());
    }

    if (m_sweeping) {
        if (auto started = m_sweepEngine->start(*m_device, *m_pipeline); !started) {
            stop();
            return failed(started.error());
        }
    }

    // Here rather than only in applySweepPlan, so pressing Start also settles
    // the panel onto what the radio agreed to.
    adoptEffectivePlan();
    beginAutoSpurs();

    ++m_startGeneration;
    notify(InstrumentNotice::Kind::ClearCondition, {});
    return ok();
}

void LocalInstrument::stop() {
    // A learn measures a running radio; without one there is nothing to
    // finish, and the settings it borrowed go back.
    abortLearning();
    endAutoSpurs();

    m_sweepEngine->stop();
    m_pipeline->stop();

    // Detached here rather than only on the way back in. A subscriber
    // outliving what it points at -- the sweep engine is torn down and rebuilt
    // around a device that may be closed in between -- is the failure the
    // bus's own unsubscribe ordering exists to prevent.
    if (m_pipelineSubscription != 0) {
        m_pipelineBus.unsubscribe(m_pipelineSubscription);
        m_pipelineSubscription = 0;
    }
}

bool LocalInstrument::running() const noexcept {
    return m_pipeline->running();
}

void LocalInstrument::tick(std::uint64_t nowNs) {
    advanceLearning();
    updateAutoSpurs();

    if (nowNs - m_lastSlowTickNs < kSlowTickNs) {
        return;
    }
    m_lastSlowTickNs = nowNs;

    // Whether the learned floor still applies: a gain change reaches the
    // device through several paths, and this is the one place that sees the
    // result of all of them.
    refreshFloorStaleness();
    sampleHealth();
}

// ---- antennas ---------------------------------------------------------------

void LocalInstrument::reloadAntennas() {
    std::vector<std::string> problems;
    m_antennas = AntennaLibrary::discover(m_paths.antennaSearchPath, &problems);

    // One malformed row is worth saying so about and nothing more: the
    // operator hand-edits this file, and losing the other nine antennas
    // because the tenth has its stop below its start would be the worse
    // failure by far.
    for (const std::string& problem : problems) {
        logWarn("antennas", "{}", problem);
    }
    ++m_revision;
}

Status LocalInstrument::setUserAntennas(std::vector<Antenna> entries) {
    AntennaLibrary edited;
    for (Antenna& antenna : entries) {
        antenna.builtin = false;
        edited.add(std::move(antenna));
    }

    std::error_code ec;
    std::filesystem::create_directories(m_paths.antennasDir, ec);
    if (auto saved = edited.saveUserFile(m_paths.antennasDir / "custom.toml"); !saved) {
        return saved;
    }

    // Read back, so what the editor lists is what the file holds -- including
    // which entries the shipped set still owns after a copy over one was
    // deleted.
    reloadAntennas();
    applyAntennaChange();
    return ok();
}

Status LocalInstrument::setAntennaAssignments(AntennaAssignments assignments) {
    m_assignments = std::move(assignments);
    ++m_revision;

    std::error_code ec;
    std::filesystem::create_directories(m_paths.antennasDir, ec);
    const Status saved = m_assignments.save(m_paths.antennasDir / "assignments.toml");
    applyAntennaChange();
    return saved;
}

void LocalInstrument::applyAntennaChange() {
    // A box that has just been named has to be opened before anything can be
    // routed through it, and one that has just been detached has to be let go.
    refreshSwitchers();

    // Only a routed sweep is planned against the antennas. With routing off
    // they change the readouts and nothing else, and cycling acquisition to
    // apply a change the sweep does not read would be an unexplained gap in
    // the waterfall.
    if (!m_sweepPlan.antennaRouting || !m_device) {
        return;
    }

    // The same path a range edit takes: an antenna moving is a plan change,
    // because the plan is what routes through it.
    if (auto applied = applySweepPlan(m_sweepPlan); !applied) {
        notify(InstrumentNotice::Kind::Condition, applied.error().describe());
    }
}

std::string LocalInstrument::deviceAntennaKey() const {
    return m_device ? AntennaAssignments::deviceKey(m_device->info()) : std::string{};
}

const Antenna* LocalInstrument::antennaOnPort(std::string_view portId) const {
    if (!m_device) {
        return nullptr;
    }
    const std::string_view id =
        m_assignments.antennaFor(AntennaAssignments::deviceKey(m_device->info()), portId);
    return id.empty() ? nullptr : m_antennas.find(id);
}

std::vector<RfLegView> LocalInstrument::rfPath() const {
    if (!m_device) {
        return {};
    }

    const std::span<const SdrRxPort> ports = m_device->rxPorts();
    const std::string_view selectedId = m_device->selectedRxPort();
    const auto selected = std::ranges::find_if(
        ports, [selectedId](const SdrRxPort& port) { return port.id == selectedId; });
    const std::size_t selectedIndex =
        selected != ports.end() ? static_cast<std::size_t>(selected - ports.begin()) : kNoPort;

    std::vector<RfLegView> views;
    for (const RfLeg& leg :
         resolveRfPath(m_device->info(), ports, m_antennas, m_assignments, m_openPaths)) {
        RfLegView view;
        view.route = leg.route;
        view.antenna = *leg.antenna;
        view.portLabel = leg.portLabel;
        if (leg.switcher != nullptr) {
            const auto open = std::ranges::find(m_openPaths, leg.switcher, &OpenRfPath::path);
            view.switcherKey = open != m_openPaths.end() ? open->key : std::string{};
        }
        view.live = leg.route.portIndex == selectedIndex &&
                    (leg.route.inputIndex == kNoInput || leg.switcher == nullptr ||
                     leg.route.inputIndex == leg.switcher->selectedInput());
        views.push_back(std::move(view));
    }
    return views;
}

std::vector<std::pair<double, double>> LocalInstrument::antennaCoverage() const {
    if (!m_device) {
        return {};
    }
    const std::vector<RfLeg> legs = resolveRfPath(m_device->info(), m_device->rxPorts(), m_antennas,
                                                  m_assignments, m_openPaths);
    return coveredRanges(legs);
}

const SwitcherView* LocalInstrument::switcher(std::string_view key) const {
    const auto match = std::ranges::find(m_switcherViews, key, &SwitcherView::key);
    return match != m_switcherViews.end() ? &*match : nullptr;
}

void LocalInstrument::rescanSwitchers() {
    m_availableSwitchers = RfPathManager::instance().enumerateAll();
    refreshSwitcherViews();
}

void LocalInstrument::refreshSwitcherViews() {
    std::vector<SwitcherView> views;
    views.reserve(m_openPaths.size());
    for (const OpenRfPath& open : m_openPaths) {
        SwitcherView view;
        view.key = open.key;
        view.info = open.path->info();
        const std::span<const RfPathInput> inputs = open.path->inputs();
        view.inputs.assign(inputs.begin(), inputs.end());
        view.selectedInput = open.path->selectedInput();
        views.push_back(std::move(view));
    }
    m_switcherViews = std::move(views);
}

void LocalInstrument::refreshSwitchers() {
    // Every box an assignment names, whichever radio it was filed against: a
    // switcher stays on the bench when the receiver is unplugged, and the
    // Antennas rows have to keep describing the chain either way.
    std::vector<std::string> wanted;
    for (const AntennaAssignment& entry : m_assignments.entries()) {
        if (entry.switcher.empty() || entry.device.empty()) {
            continue;
        }
        if (std::ranges::find(wanted, entry.switcher) == wanted.end()) {
            wanted.push_back(entry.switcher);
        }
    }

    // Kept rather than reopened. Reopening on every assignment edit would
    // click every relay in the box each time the operator changed a combo, and
    // on hardware that counts its own cycles that is wear for nothing.
    std::vector<std::unique_ptr<IRfPath>> keep;
    std::vector<OpenRfPath> open;

    for (const std::string& key : wanted) {
        const auto existing =
            std::ranges::find_if(m_switchers, [&key](const std::unique_ptr<IRfPath>& path) {
                return path && rfPathKey(path->info()) == key;
            });
        if (existing != m_switchers.end()) {
            open.push_back(OpenRfPath{.key = key, .path = existing->get()});
            keep.push_back(std::move(*existing));
            continue;
        }

        auto opened = RfPathManager::instance().openSpecifier(key);
        if (!opened) {
            // Listed as missing rather than dropped: the assignment survives so
            // reconnecting the box restores the whole chain, and the sweep
            // refuses to route through what it cannot drive.
            logWarn("antennas", "switcher '{}' is assigned but not connected: {}", key,
                    opened.error().describe());
            continue;
        }
        open.push_back(OpenRfPath{.key = key, .path = opened->get()});
        keep.push_back(std::move(*opened));
    }

    m_switchers = std::move(keep);
    m_openPaths = std::move(open);
    refreshSwitcherViews();
    ++m_revision;
}

// ---- receiver corrections ---------------------------------------------------

CalibrationContext LocalInstrument::currentContext() const {
    if (!m_device) {
        return {};
    }
    return calibrationContextFor(*m_device, m_correctionSettings.dcRemoval);
}

CorrectionSettings LocalInstrument::effectiveCorrectionSettings() const noexcept {
    CorrectionSettings settings = m_correctionSettings;
    if (m_learn) {
        // A learn measures the receiver as it is, so the corrections it is
        // about to replace stay out of the data. The scan for fixed spurs
        // runs through the LO-offset set it just produced, because what it
        // looks for is what that set leaves behind.
        const bool throughLoSet = m_learn->phase == LearnRun::Phase::Absolute;
        settings.flatten = throughLoSet;
        settings.spurMask = throughLoSet;
    }
    return settings;
}

void LocalInstrument::pushCorrectionSettings() {
    m_pipeline->setCorrectionSettings(effectiveCorrectionSettings());
}

void LocalInstrument::setCorrectionSettings(const CorrectionSettings& settings) {
    const bool dcRemovalWas = m_correctionSettings.dcRemoval;
    const bool autoSpursWere = m_correctionSettings.autoSpurs;
    m_correctionSettings = settings;
    pushCorrectionSettings();

    // DC removal is part of the context a floor was learned in.
    if (dcRemovalWas != settings.dcRemoval) {
        installCorrections();
    }
    if (autoSpursWere != settings.autoSpurs) {
        if (settings.autoSpurs) {
            beginAutoSpurs();
        } else {
            endAutoSpurs();
        }
    }
}

CorrectionSummary LocalInstrument::correctionSummary() const {
    CorrectionSummary summary;
    summary.floorStaleReason = m_floorStaleReason;
    if (!m_corrections) {
        return summary;
    }
    summary.present = true;
    summary.floorPoints = m_corrections->floor.levelDb.size();
    summary.spurs = m_corrections->spurs.size();
    summary.automaticSpurs = static_cast<std::size_t>(std::ranges::count_if(
        m_corrections->spurs, [](const SpurEntry& entry) { return entry.automatic; }));
    summary.learnedAt = m_corrections->learnedAt;
    return summary;
}

void LocalInstrument::loadCalibration() {
    m_corrections.reset();
    m_floorStaleReason.clear();

    if (m_device) {
        const std::filesystem::path path = calibrationPath();
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            if (auto loaded = CorrectionSet::load(path)) {
                m_corrections = std::move(*loaded);
                logInfo("calibration", "{}: floor over {} points, {} spurs, learned {}",
                        path.filename().string(), m_corrections->floor.levelDb.size(),
                        m_corrections->spurs.size(), m_corrections->learnedAt);
            } else {
                logWarn("calibration", "{}: {}", path.string(), loaded.error().describe());
            }
        }
    }

    installCorrections();
}

void LocalInstrument::installCorrections() {
    if (!m_corrections || !m_device) {
        m_floorStaleReason.clear();
        m_pipeline->setCorrections(nullptr);
        return;
    }

    // A copy, so the set the pipeline reads never changes under it and the
    // stale floor can be left out of what it sees while staying on record.
    auto installed = std::make_shared<CorrectionSet>(*m_corrections);
    m_floorStaleReason.clear();
    if (!installed->floor.empty()) {
        m_floorStaleReason = installed->context.firstDifference(currentContext());
        if (!m_floorStaleReason.empty()) {
            installed->floor = {};
        }
    }
    m_pipeline->setCorrections(std::move(installed));
}

void LocalInstrument::refreshFloorStaleness() {
    if (!m_corrections || m_corrections->floor.empty() || !m_device) {
        return;
    }
    if (m_corrections->context.firstDifference(currentContext()) != m_floorStaleReason) {
        installCorrections();
    }
}

std::size_t LocalInstrument::learnPasses() noexcept {
    return LearnParameters{}.absolutePasses + 2;
}

Status LocalInstrument::startLearning() {
    if (!running() || !m_device) {
        return fail(ErrorCode::Unavailable, "start acquisition before learning");
    }
    if (m_learn) {
        return fail(ErrorCode::AlreadyExists, "a learn is already running");
    }

    const bool sweeping = m_sweeping && m_sweepEngine->running();
    const std::size_t fftSize =
        sweeping ? m_sweepEngine->schedule().fftSize : m_pipeline->config().fftSize;
    const double sampleRate =
        sweeping ? m_sweepEngine->plan().sampleRate
                 : asDouble(m_device->getParameter(kSampleRateKey).value_or(SdrValue{0.0}));
    if (fftSize == 0 || sampleRate <= 0.0) {
        return fail(ErrorCode::Unavailable, "no grid to learn against");
    }

    {
        const std::lock_guard lock(m_learnMutex);
        LearnRun& run = m_learn.emplace();
        run.phase = sweeping ? LearnRun::Phase::LoOffsets : LearnRun::Phase::Fixed;
        run.savedSettings = m_correctionSettings;
        run.context = currentContext();
        run.passesHandled = m_passesSeen.load(std::memory_order_relaxed);
        run.learner.begin(fftSize, sampleRate, sweeping);
    }
    listenForLearning(true);
    pushCorrectionSettings();

    notify(InstrumentNotice::Kind::Info,
           sweeping ? std::format("Learning receiver corrections over the next {} "
                                  "passes. Keep the antenna disconnected.",
                                  learnPasses())
                    : std::format("Learning receiver corrections from the next {} "
                                  "frames. Keep the antenna disconnected.",
                                  kLearnFrames));
    return ok();
}

bool LocalInstrument::learning() const {
    const std::lock_guard lock(m_learnMutex);
    return m_learn.has_value();
}

std::string LocalInstrument::learningLabel() const {
    const std::lock_guard lock(m_learnMutex);
    if (!m_learn) {
        return {};
    }
    switch (m_learn->phase) {
    case LearnRun::Phase::LoOffsets:
        return "learning the floor and LO-offset spurs";
    case LearnRun::Phase::Absolute:
        return std::format("scanning for fixed spurs, pass {} of {}",
                           m_learn->gridLearner.frameCount() + 1, LearnParameters{}.absolutePasses);
    case LearnRun::Phase::Fixed:
        return std::format("learning, {} of {} frames", m_learn->learner.frameCount(),
                           kLearnFrames);
    }
    return {};
}

void LocalInstrument::cancelLearning() {
    if (m_learn) {
        abortLearning();
        notify(InstrumentNotice::Kind::Info, "Learning cancelled");
    }
}

void LocalInstrument::LearnTap::onFrame(const SpectrumFramePtr& frame) noexcept {
    // On a publishing thread. One pass over the bins, or a pointer copy --
    // nothing heavier.
    const std::lock_guard lock(m_owner.m_learnMutex);
    if (!m_owner.m_learn) {
        return;
    }
    LearnRun& run = *m_owner.m_learn;
    if (run.phase == LearnRun::Phase::Fixed) {
        run.learner.addFrame(frame->binsDbfs, frame->config.centerHz);
    } else if (run.phase == LearnRun::Phase::Absolute && frame->passComplete &&
               frame->sweepPass > run.installedAtPass) {
        // The grid as one wide frame: its span for the sample rate and its
        // middle for the centre, so the offsets come back as frequencies.
        CorrectionLearner& grid = run.gridLearner;
        if (grid.binCount() != frame->binCount()) {
            grid.begin(frame->binCount(),
                       frame->binWidthHz * static_cast<double>(frame->binCount()), false);
        }
        grid.addFrame(frame->binsDbfs, frame->centerHz());
    }
}

void LocalInstrument::observeStep(const SpectrumFrame& frame) noexcept {
    const std::lock_guard lock(m_learnMutex);
    if (m_learn) {
        if (m_learn->phase == LearnRun::Phase::LoOffsets) {
            m_learn->learner.addFrame(frame.binsDbfs, frame.config.centerHz);
        }
    } else if (m_autoLearner) {
        m_autoLearner->addFrame(frame.binsDbfs, frame.config.centerHz);
    }
}

void LocalInstrument::advanceLearning() {
    if (!m_learn) {
        return;
    }
    LearnRun& run = *m_learn;

    switch (run.phase) {
    case LearnRun::Phase::LoOffsets: {
        const std::uint64_t passes = m_passesSeen.load(std::memory_order_relaxed);
        if (passes == run.passesHandled) {
            return;
        }
        run.passesHandled = passes;

        // The pass that was under way when the learn began only contributed
        // its remainder. Half the steps is enough for the per-bin statistics
        // -- every step measures every local bin -- and less than that waits
        // for the next pass.
        CorrectionLearner learner;
        {
            const std::lock_guard lock(m_learnMutex);
            learner = run.learner;
        }
        const std::size_t steps = m_sweepEngine->schedule().steps.size();
        if (learner.frameCount() < std::max<std::size_t>(1, steps / 2)) {
            return;
        }

        run.floor = learner.floorShape();
        run.loSpurs = learner.loSpurs();

        // Installed now, so the next pass is measured through them and what
        // it still shows is at a fixed frequency.
        CorrectionSet interim;
        interim.context = run.context;
        interim.floor = run.floor;
        interim.spurs = run.loSpurs;
        m_corrections = std::move(interim);
        installCorrections();

        if (!m_sweepEngine->plan().continuous) {
            notify(InstrumentNotice::Kind::Warning,
                   "One-shot plan: fixed-frequency spurs were not scanned. Learn on a "
                   "continuous sweep to find them.");
            finishLearning({});
            return;
        }

        {
            const std::lock_guard lock(m_learnMutex);
            run.phase = LearnRun::Phase::Absolute;
            run.installedAtPass = m_sweepEngine->passCount();
            run.gridLearner = {};
        }
        pushCorrectionSettings();
        notify(InstrumentNotice::Kind::Info,
               std::format("Floor and {} LO-offset spur(s) learned; scanning the next "
                           "{} passes for fixed spurs",
                           run.loSpurs.size(), LearnParameters{}.absolutePasses));
        return;
    }

    case LearnRun::Phase::Absolute: {
        CorrectionLearner grid;
        {
            const std::lock_guard lock(m_learnMutex);
            if (run.gridLearner.frameCount() < LearnParameters{}.absolutePasses) {
                return;
            }
            grid = run.gridLearner;
        }
        finishLearning(grid.loSpurs());
        return;
    }

    case LearnRun::Phase::Fixed: {
        CorrectionLearner learner;
        {
            const std::lock_guard lock(m_learnMutex);
            if (run.learner.frameCount() < kLearnFrames) {
                return;
            }
            learner = run.learner;
        }
        run.floor = learner.floorShape();
        run.loSpurs = learner.loSpurs();
        finishLearning({});
        return;
    }
    }
}

void LocalInstrument::finishLearning(const std::vector<SpurEntry>& absoluteSpurs) {
    LearnRun run;
    {
        const std::lock_guard lock(m_learnMutex);
        run = std::move(*m_learn);
        m_learn.reset();
    }
    listenForLearning(false);

    CorrectionSet set;
    set.context = run.context;
    set.floor = std::move(run.floor);
    set.spurs = std::move(run.loSpurs);
    const std::size_t loCount = set.spurs.size();
    set.spurs.insert(set.spurs.end(), absoluteSpurs.begin(), absoluteSpurs.end());
    set.learnedAt = formatWallClockIso8601(wallClockNs());

    std::string savedAs;
    if (m_device) {
        const std::filesystem::path path = calibrationPath();
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (auto saved = set.save(path); !saved) {
            notify(InstrumentNotice::Kind::Error,
                   std::format("could not save the calibration: {}", saved.error().describe()));
        } else {
            savedAs = path.filename().string();
        }
    }

    m_corrections = std::move(set);

    // Learning is a request to use the result: the switches come back as
    // they were, with these two on.
    m_correctionSettings = run.savedSettings;
    m_correctionSettings.flatten = true;
    m_correctionSettings.spurMask = true;
    pushCorrectionSettings();
    installCorrections();
    beginAutoSpurs();

    notify(InstrumentNotice::Kind::Success,
           std::format("Corrections learned: floor over {} points, {} LO-offset and "
                       "{} fixed spur(s){}",
                       m_corrections->floor.levelDb.size(), loCount, absoluteSpurs.size(),
                       savedAs.empty() ? std::string{} : std::format(", saved as {}", savedAs)));
}

void LocalInstrument::abortLearning() {
    if (!m_learn) {
        return;
    }
    CorrectionSettings saved;
    {
        const std::lock_guard lock(m_learnMutex);
        saved = m_learn->savedSettings;
        m_learn.reset();
    }
    listenForLearning(false);
    m_correctionSettings = saved;
    pushCorrectionSettings();

    // An interim set may have gone in after the first phase; what is on disk
    // is what stands.
    loadCalibration();
}

void LocalInstrument::beginAutoSpurs() {
    if (!m_correctionSettings.autoSpurs || !m_sweeping || !running() || !m_sweepEngine->running()) {
        return;
    }
    const std::lock_guard lock(m_learnMutex);
    m_autoLearner.emplace();
    m_autoLearner->begin(m_sweepEngine->schedule().fftSize, m_sweepEngine->plan().sampleRate, true);
    m_autoPassesHandled = m_passesSeen.load(std::memory_order_relaxed);
}

void LocalInstrument::endAutoSpurs() {
    const std::lock_guard lock(m_learnMutex);
    m_autoLearner.reset();
}

void LocalInstrument::updateAutoSpurs() {
    if (!m_autoLearner || m_learn) {
        return;
    }
    const std::uint64_t passes = m_passesSeen.load(std::memory_order_relaxed);
    if (passes == m_autoPassesHandled) {
        return;
    }
    m_autoPassesHandled = passes;

    // One pass at a time: the minimum over a pass is what tells a spur from
    // a signal, and the next pass starts from nothing.
    CorrectionLearner learner;
    {
        const std::lock_guard lock(m_learnMutex);
        learner = *m_autoLearner;
        m_autoLearner->begin(learner.binCount(), learner.sampleRate(), true);
    }
    if (learner.frameCount() == 0 || learner.binCount() == 0) {
        return;
    }

    const std::vector<SpurEntry> found = learner.loSpurs();
    if (found.empty()) {
        return;
    }

    if (!m_corrections) {
        m_corrections.emplace();
        m_corrections->context = currentContext();
    }

    // Additive: the data this saw was already masked, so anything it found
    // is something the mask does not yet cover.
    const double binWidthHz = learner.sampleRate() / static_cast<double>(learner.binCount());
    std::size_t added = 0;
    for (SpurEntry spur : found) {
        const bool covered =
            std::ranges::any_of(m_corrections->spurs, [&](const SpurEntry& existing) {
                return existing.kind == SpurKind::LoOffset &&
                       std::abs(existing.hz - spur.hz) <=
                           ((existing.widthHz + spur.widthHz) * 0.5) + binWidthHz;
            });
        if (covered) {
            continue;
        }
        spur.automatic = true;
        m_corrections->spurs.push_back(spur);
        ++added;
    }

    if (added > 0) {
        installCorrections();
        logInfo("calibration", "{} LO-offset spur(s) added automatically", added);
    }
}

void LocalInstrument::clearAutoSpurs() {
    if (!m_corrections) {
        return;
    }
    std::erase_if(m_corrections->spurs, [](const SpurEntry& e) { return e.automatic; });
    installCorrections();
}

void LocalInstrument::clearCorrections() {
    abortLearning();
    if (m_device) {
        const std::filesystem::path path = calibrationPath();
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (ec) {
            notify(InstrumentNotice::Kind::Error,
                   std::format("could not remove {}: {}", path.string(), ec.message()));
        }
    }
    m_corrections.reset();
    m_floorStaleReason.clear();
    installCorrections();
    beginAutoSpurs();
}

} // namespace sweeppp
