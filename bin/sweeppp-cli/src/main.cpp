// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Options.hpp"

#include <Commands.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <print>
#include <sweeppp/backends/sdr/SyntheticDevice.hpp>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/core/CrashHandler.hpp>
#include <sweeppp/core/EventBus.hpp>
#include <sweeppp/core/Log.hpp>
#include <sweeppp/core/Paths.hpp>
#include <sweeppp/core/Telemetry.hpp>
#include <sweeppp/core/Toml.hpp>
#include <sweeppp/core/Version.hpp>
#include <sweeppp/correction/CorrectionLearner.hpp>
#include <sweeppp/correction/Corrections.hpp>
#include <sweeppp/crypto/Sha256.hpp>
#include <sweeppp/fft/FftBackendManager.hpp>
#include <sweeppp/history/IFrameSource.hpp>
#include <sweeppp/history/SessionReader.hpp>
#include <sweeppp/history/SessionRecorder.hpp>
#include <sweeppp/history/SweepsLog.hpp>
#include <sweeppp/instrument/LocalInstrument.hpp>
#include <sweeppp/net/Socket.hpp>
#include <sweeppp/pipeline/AsyncFrameConsumer.hpp>
#include <sweeppp/pipeline/FrameBus.hpp>
#include <sweeppp/pipeline/Pipeline.hpp>
#include <sweeppp/plugin/PluginHost.hpp>
#include <sweeppp/remote/RemoteServer.hpp>
#include <sweeppp/rf/Antenna.hpp>
#include <sweeppp/rf/AntennaAssignments.hpp>
#include <sweeppp/rf/IRfPath.hpp>
#include <sweeppp/sdr/ISdrDevice.hpp>
#include <sweeppp/sweep/SweepEngine.hpp>
#include <thread>

namespace {

using namespace sweeppp;
using namespace sweeppp::cli;

std::atomic<bool> g_interrupted{false};

void handleInterrupt(int) {
    g_interrupted.store(true);
}

/// Both registrations, in the order that makes a name clash a refusal rather
/// than a silent replacement: a plugin claiming a driver name a built-in
/// already holds is listed with that as its reason.
///
/// The driver names a built-in protects are only `synthetic` and `iqfile`.
/// Every radio and every FFT backend arrives through a plugin, so this
/// ordering is also what makes `--device hackrf` and `--fft-backend fftw`
/// resolvable at all.
void registerBuiltinsAndPlugins() {
    registerBuiltinSdrDevices();
    PluginManager::instance().discover();
}

/// The log file and the crash handler, for a command that is about to run.
///
/// Both want the config directory to exist, and both are pointless without it,
/// so a tree that cannot be created is reported and the command carries on
/// with stderr alone -- refusing to sweep because the log could not be opened
/// would be the wrong trade.
void installDiagnostics() {
    const Paths& paths = Paths::instance();
    if (auto ready = paths.ensureConfigTree(); !ready) {
        std::println(stderr, "sweeppp-cli: {}", ready.error().describe());
        return;
    }
    Log::setLogFile(paths.logFile().string());
    CrashHandler::install(paths.configDir(), "sweeppp-cli");
}

/// Attaches the plugin host to a command's buses, and detaches before they go.
///
/// Both buses here are locals and the plugin host is a process-wide singleton,
/// so this is not tidiness: a frame processor still subscribed when the bus is
/// destroyed is a dangling pointer waiting for the next shutdown, and every
/// error path in these commands is an early return.
class PluginScope {
public:
    PluginScope(FrameBus& frames, EventBus& events) {
        PluginManager::instance().setFrameBus(&frames);
        PluginManager::instance().attachEvents(events);
    }

    ~PluginScope() { PluginManager::instance().shutdown(); }

    PluginScope(const PluginScope&) = delete;
    PluginScope& operator=(const PluginScope&) = delete;
};

/// Writes each published frame as a CSV row.
///
/// Attaches to the FrameBus like any other consumer, which is the point: the
/// CLI's output path is not a special case, it is the same mechanism the
/// recorder and the remote server use.
class CsvWriter final : public IFrameConsumer {
public:
    CsvWriter(std::ostream& out, bool quiet, bool completePassesOnly = false)
        : m_out(out), m_quiet(quiet), m_completePassesOnly(completePassesOnly) {}

    void onFrame(const SpectrumFramePtr& frame) noexcept override {
        // A sweep publishes a partial grid every time a step lands, tens of
        // times a second, and each one is the entire span -- hundreds of
        // thousands of bins, mostly unchanged from the row before. Writing
        // them all is not just wasteful output: formatting happens on the
        // publishing thread, so it stalls the sweep itself. One row per
        // completed pass is both what a CSV consumer wants and what keeps the
        // writer from becoming the thing that limits the sweep rate.
        if (m_completePassesOnly && !frame->passComplete) {
            return;
        }

        try {
            if (!m_wroteHeader) {
                writeHeader(*frame);
                m_wroteHeader = true;
            }

            m_out << frame->sequence << ',' << frame->hostTimeNs << ',' << frame->sweepPass << ','
                  << frame->sweepStep << ',' << frame->startHz << ',' << frame->binWidthHz << ','
                  << frame->binCount();

            for (const float bin : frame->binsDbfs) {
                m_out << ',' << bin;
            }
            m_out << '\n';

            ++m_rows;
            if (!m_quiet && (m_rows % 100) == 0) {
                std::print(stderr, "\r{} frames", m_rows);
            }
        } catch (...) {
            // Never let an output failure stop acquisition; count and move on.
            ++m_failures;
        }
    }

    [[nodiscard]] std::string_view consumerName() const noexcept override { return "csv-writer"; }

    [[nodiscard]] std::uint64_t rows() const noexcept { return m_rows; }
    [[nodiscard]] std::uint64_t failures() const noexcept { return m_failures; }

private:
    void writeHeader(const SpectrumFrame& frame) {
        // A self-describing header, for the same reason frames are
        // self-describing: a CSV with bare numbers is uninterpretable a week
        // later.
        m_out << "# sweeppp " << versionString() << '\n'
              << "# device: " << frame.config.deviceLabel << '\n'
              << "# center_hz: " << frame.config.centerHz << '\n'
              << "# sample_rate: " << frame.config.sampleRate << '\n'
              << "# fft_size: " << frame.config.fftSize << '\n'
              << "# window: " << toString(frame.config.window) << '\n'
              << "# rbw_hz: " << frame.config.rbwHz << '\n'
              << "# enbw_bins: " << frame.config.windowEnbw << '\n'
              << "sequence,host_time_ns,sweep_pass,sweep_step,start_hz,bin_width_hz,bin_count,"
                 "bins_dbfs...\n";
    }

    std::ostream& m_out;
    bool m_quiet;
    bool m_completePassesOnly = false;
    bool m_wroteHeader = false;
    std::uint64_t m_rows = 0;
    std::uint64_t m_failures = 0;
};

/// Deliberately slow consumer used by the forward-compatibility proof (§3.7,
/// verification item 8): it must drop its own frames while the CSV writer
/// stays at full rate and acquisition is untouched.
///
/// Built on AsyncFrameConsumer rather than by sleeping inside onFrame, because
/// sleeping inline would prove the wrong thing. A consumer that blocks the
/// publishing worker is not a slow consumer, it is a broken one -- it would
/// back up the block queue and turn into dropped samples, which is precisely
/// the coupling the design forbids. A realistic slow recorder buffers a little
/// and drops when it cannot keep up, and that is what this models.
class SlowConsumer final : public AsyncFrameConsumer {
public:
    explicit SlowConsumer(std::chrono::milliseconds delay)
        : AsyncFrameConsumer("slow-test", 8), m_delay(delay) {
        startWorker();
    }

    ~SlowConsumer() override { shutdown(); }

protected:
    void processFrame(const SpectrumFramePtr&) override { std::this_thread::sleep_for(m_delay); }

private:
    std::chrono::milliseconds m_delay;
};

/// Tracks how much of the swept span a stitched frame actually measured.
///
/// The number that matters for a sweep, and the one a bin count cannot give
/// you: a grid with holes has the same total as a solid one covering less
/// ground, so runs of unmeasured bins are counted separately. Evenly spaced
/// runs mean steps are being missed on a fixed stride rather than at random.
class CoverageProbe final : public IFrameConsumer {
public:
    void onFrame(const SpectrumFramePtr& frame) noexcept override {
        if (!frame || frame->binCount() == 0) {
            return;
        }

        std::size_t measuredBins = 0;
        std::size_t gapRuns = 0;
        bool inGap = false;
        for (const float value : frame->binsDbfs) {
            // The engine writes an unmistakably impossible level into bins no
            // step has reached, precisely so "never looked" stays
            // distinguishable from "looked, and it was quiet".
            const bool seen = measured(value);
            measuredBins += seen ? 1U : 0U;
            if (!seen && !inGap) {
                ++gapRuns;
            }
            inGap = !seen;
        }

        const std::lock_guard lock(m_mutex);
        m_bins = frame->binCount();
        m_measured = measuredBins;
        m_gapRuns = gapRuns;
        ++m_frames;
    }

    [[nodiscard]] std::string_view consumerName() const noexcept override { return "coverage"; }

    struct Snapshot {
        std::size_t bins = 0;
        std::size_t measured = 0;
        std::size_t gapRuns = 0;
        std::uint64_t frames = 0;
    };

    [[nodiscard]] Snapshot snapshot() const {
        const std::lock_guard lock(m_mutex);
        return {m_bins, m_measured, m_gapRuns, m_frames};
    }

private:
    mutable std::mutex m_mutex;
    std::size_t m_bins = 0;
    std::size_t m_measured = 0;
    std::size_t m_gapRuns = 0;
    std::uint64_t m_frames = 0;
};

/// The named radio's RF inputs, and what the antenna store says is on each.
void printRxPorts(const std::string& specifier) {
    auto device = SdrDeviceManager::instance().openSpecifier(specifier);
    if (!device) {
        std::println("\nRF inputs\n  ({})", device.error().describe());
        return;
    }

    const std::span<const SdrRxPort> ports = (*device)->rxPorts();
    std::println("\nRF inputs");
    if (ports.empty()) {
        std::println("  (one, unnamed -- this radio has a single connector)");
    }

    std::vector<std::string> problems;
    const AntennaLibrary antennas =
        AntennaLibrary::discover(Paths::instance().searchPath("antennas"), &problems);
    const AntennaAssignments assignments =
        AntennaAssignments::load(Paths::instance().antennasDir() / "assignments.toml");
    const std::string key = AntennaAssignments::deviceKey((*device)->info());

    const std::string_view selected = (*device)->selectedRxPort();
    for (const SdrRxPort& port : ports) {
        std::println("  [{}] {:<6} {:<14} {}{}", port.id == selected ? 'x' : ' ', port.id,
                     port.label, port.connector, port.biasTee ? " | bias-T available" : "");

        const std::string_view antennaId = assignments.antennaFor(key, port.id);
        const Antenna* antenna = antennaId.empty() ? nullptr : antennas.find(antennaId);
        if (antenna != nullptr) {
            std::println("      {} -- {}, {:+.1f} dBi{}", antenna->name, antenna->describeRange(),
                         antenna->gainDbi,
                         antenna->needsBiasT ? (port.biasTee ? " | needs bias-T"
                                                             : " | NEEDS BIAS-T, THIS PORT "
                                                               "CANNOT SUPPLY IT")
                                             : "");
        } else if (!antennaId.empty()) {
            std::println("      antenna '{}' is missing from the library", antennaId);
        } else {
            std::println("      nothing assigned");
        }
    }
}

int runInfo(const Options& options) {
    registerBuiltinsAndPlugins();

    std::println("sweeppp-cli {}\n", buildString());

    std::println("FFT backends");
    for (const FftBackendInfo& backend : FftBackendManager::instance().enumerate()) {
        if (backend.available) {
            std::println("  [x] {:<22} {}", backend.name, backend.displayName);
            std::println("      {} | sizes {}..{} | batch {} | thread-safe execute {}",
                         toString(backend.capabilities.type), backend.capabilities.minSize,
                         backend.capabilities.maxSize,
                         backend.capabilities.supportsBatch ? "yes" : "no",
                         backend.capabilities.threadSafeExecute ? "yes" : "no");
        } else {
            // Unavailable backends are listed *with the reason*. A backend
            // that silently vanishes teaches the operator nothing.
            std::println("  [ ] {:<22} {} -- {}", backend.name, backend.displayName,
                         backend.unavailableReason);
        }
    }

    std::println("\nSDR devices");
    const std::vector<SdrDeviceInfo> devices = SdrDeviceManager::instance().enumerateAll();
    if (devices.empty()) {
        std::println("  (none detected)");
    }
    for (const SdrDeviceInfo& device : devices) {
        std::println("  {}:{}  {}", device.driver, device.id, device.label);
        std::println("      {} .. {} | {} .. {} S/s{}",
                     toml_util::formatFrequencyShort(device.minFrequencyHz),
                     toml_util::formatFrequencyShort(device.maxFrequencyHz),
                     toml_util::formatFrequencyShort(device.minSampleRate),
                     toml_util::formatFrequencyShort(device.maxSampleRate),
                     device.linkDescription.empty() ? std::string{}
                                                    : std::format(" | {}", device.linkDescription));
    }

    // The RF path in front of the tuner, but only for a radio the operator
    // named. A headless node is exactly where "which antenna is on RX2" is
    // hardest to check by looking, and it needs an open handle -- so opening
    // one nobody asked about would claim hardware to answer a question that
    // was never posed.
    if (options.deviceGiven) {
        printRxPorts(options.device);
    }

    std::println("\nDrivers: {}", [] {
        std::string joined;
        for (const std::string& driver : SdrDeviceManager::instance().drivers()) {
            joined += (joined.empty() ? "" : ", ") + driver;
        }
        return joined;
    }());

    // Plugins, in the same available/unavailable-with-reason shape as the FFT
    // backends above. This is also what makes discovery testable without the
    // GUI: the loader has no ImGui in it, so `info` exercises the whole path.
    std::println("\nPlugins");
    const std::vector<PluginInfo> plugins = PluginManager::instance().enumerate();
    if (plugins.empty()) {
        std::println("  (none found)");
    }
    for (const PluginInfo& plugin : plugins) {
        if (!plugin.loaded) {
            std::println("  [!] {:<28} {} -- {}", plugin.path.filename().string(),
                         plugin.id.empty() ? std::string("<unreadable>") : plugin.id,
                         plugin.failureReason);
            continue;
        }

        std::println("  [{}] {:<28} {} {}", plugin.active ? 'x' : ' ', plugin.id, plugin.name,
                     displayVersion(plugin.version));
        if (!plugin.failureReason.empty()) {
            std::println("      {}", plugin.failureReason);
        }
        if (!plugin.description.empty()) {
            std::println("      {}", plugin.description);
        }
        for (const PluginFacetInfo& facet : plugin.facets) {
            std::println("      {} {:<20} {}{}", facet.active ? '+' : '-', toString(facet.kind),
                         facet.id,
                         facet.failureReason.empty() ? std::string{}
                                                     : std::format(" -- {}", facet.failureReason));
        }
        std::println("      {}", plugin.path.string());
    }

    if (const std::vector<std::string> datasets = PluginManager::instance().providedDatasets();
        !datasets.empty()) {
        std::println("\nProvided datasets");
        for (const std::string& dataset : datasets) {
            std::println("  {}", dataset);
        }
    }

    std::println("\nPlugin search path");
    for (const PluginSearchEntry& entry : pluginSearchPath()) {
        std::error_code ec;
        const bool present = std::filesystem::is_directory(entry.directory, ec);
        // Saying which directories are prefix-only is the difference between
        // "my plugin is not being found" and "my plugin is not called
        // sweeppp-plugin-*".
        std::println("  {}{}{}", entry.directory.string(),
                     entry.requiresPrefix ? "  (" SWEEPPP_PLUGIN_FILE_PREFIX "* only)" : "",
                     present ? "" : "  (missing)");
    }

    return 0;
}

/// `live` is the last snapshot taken while streaming -- rates, throttle state
/// and worker utilisation are only meaningful there. `final` is taken after the
/// pipeline has stopped and drained, which is the only point at which the
/// sample accounting can balance.
void printStatsSummary(const TelemetrySnapshot& live, const TelemetrySnapshot& settled,
                       const FrameBus& bus, double wallSeconds) {
    const TelemetrySnapshot& snapshot = live;

    // Averages over the whole run, not the live EWMA. The EWMA is right for a
    // moving readout but wrong for a summary: it decays toward zero the moment
    // the stream stops, so a run that sustained its rate perfectly would still
    // report a rate error.
    const double averageSps =
        wallSeconds > 0.0 ? static_cast<double>(settled.stream.samplesDelivered) / wallSeconds
                          : 0.0;

    std::println("\n--- stream ---");
    std::println("  configured            {}",
                 toml_util::formatFrequencyShort(snapshot.stream.configuredSps));
    std::println("  measured (mean)       {}", toml_util::formatFrequencyShort(averageSps));
    std::println("  input rate            {}",
                 toml_util::formatByteRate(snapshot.stream.bytesPerSecIn));
    if (snapshot.stream.linkCapacityBytesPerSec > 0.0) {
        std::println("  link utilisation      {:.1f}% of {}",
                     snapshot.stream.linkUtilisation * 100.0,
                     toml_util::formatByteRate(snapshot.stream.linkCapacityBytesPerSec));
    }
    std::println("  samples delivered     {}", snapshot.stream.samplesDelivered);
    std::println("  samples dropped       {} ({:.3f}%)",
                 snapshot.stream.samplesDropped + snapshot.stream.samplesLostAtSource,
                 snapshot.stream.dropFraction * 100.0);
    std::println("    discarded           {}", snapshot.stream.samplesDropped);
    std::println("    never taken         {}", snapshot.stream.samplesLostAtSource);
    std::println("  device overruns       {}", snapshot.stream.deviceOverruns);
    std::println("  ring-full events      {}", snapshot.stream.ringFullEvents);
    std::println("  pool-exhausted events {}", snapshot.stream.poolExhaustedEvents);
    std::println("  sequence gaps         {}", snapshot.stream.sequenceGaps);

    std::println("\n--- processing ---");
    std::println("  FFTs computed         {} ({:.0f}/s)", snapshot.process.fftsComputed,
                 snapshot.process.fftsPerSec);
    std::println("  FFTs skipped          {}", snapshot.process.fftsSkipped);
    std::println("  samples processed     {:.2f}% of delivered",
                 snapshot.process.processedFraction * 100.0);
    std::println("  FFT latency           p50 {:.1f} us | p99 {:.1f} us | max {:.1f} us",
                 snapshot.process.fftLatencyP50Us, snapshot.process.fftLatencyP99Us,
                 snapshot.process.fftLatencyMaxUs);
    std::println("  workers               {} ({:.0f}% busy)", snapshot.process.workerCount,
                 snapshot.process.workerUtilisation * 100.0);
    std::println("  throttle              {}", toString(snapshot.process.throttleReason));
    std::println("  frames published      {} ({:.1f}/s)", bus.framesPublished(),
                 snapshot.process.framesPerSec);

    std::println("\n--- consumers ---");
    for (const ConsumerSnapshot& consumer : bus.consumerStats()) {
        std::println("  {:<14} delivered {:<8} dropped {:<8} ({:.2f}%)  max callback {} us",
                     consumer.name, consumer.framesDelivered, consumer.framesDropped,
                     consumer.dropFraction * 100.0, consumer.maxCallbackUs);
    }

    std::println("\n--- process ---");
    std::println("  wall time             {}", formatDuration(wallSeconds));
    std::println("  cpu                   {:.0f}% of one core", snapshot.render.cpuPercent);

    // The acceptance criterion for the high-throughput requirement: everything
    // the device produced is either processed or accounted as dropped, with no
    // third category. A mismatch here means a leak in the accounting, which
    // would make every other number untrustworthy.
    // From the settled snapshot: taken after the pipeline drained, so nothing
    // is still in flight and the tally can actually balance.
    std::println("\n--- accounting ---");
    std::println("  delivered             {}", settled.stream.samplesDelivered);
    std::println("  processed             {}", settled.process.samplesProcessedTotal);
    std::println("  dropped               {}", settled.stream.samplesDropped);
    // Excluded from the identity below on purpose: these never reached us, so
    // they were never delivered and cannot appear on both sides.
    std::println("  never taken           {}", settled.stream.samplesLostAtSource);

    const std::uint64_t accounted =
        settled.process.samplesProcessedTotal + settled.stream.samplesDropped;
    const std::int64_t discrepancy = static_cast<std::int64_t>(accounted) -
                                     static_cast<std::int64_t>(settled.stream.samplesDelivered);
    std::println("  processed + dropped   {} (delivered {:+})", accounted, discrepancy);
    std::println("  reconciles            {}", discrepancy == 0 ? "yes" : "NO -- accounting leak");

    const double rateError =
        snapshot.stream.configuredSps > 0.0
            ? std::abs(averageSps - snapshot.stream.configuredSps) / snapshot.stream.configuredSps
            : 0.0;
    std::println("  rate error            {:.2f}%", rateError * 100.0);
}

/// Applies one `--param`, typed by the parameter's own declaration. Applied
/// through the generic parameter model, so the CLI needs no per-device
/// knowledge -- exactly the property that lets the UI generate its panel from
/// parameters() alone. False after printing why.
bool applyDeviceParameter(ISdrDevice& device, const std::string& key, const std::string& text) {
    const auto parameters = device.parameters();
    const auto match =
        std::ranges::find_if(parameters, [&key](const SdrParameter& p) { return p.key == key; });
    if (match == parameters.end()) {
        std::println(stderr, "sweeppp-cli: device has no parameter '{}'", key);
        return false;
    }
    auto value = parseSdrValue(text, match->type);
    if (!value) {
        std::println(stderr, "sweeppp-cli: {}", value.error().describe());
        return false;
    }
    if (auto applied = device.setParameter(key, *value); !applied) {
        std::println(stderr, "sweeppp-cli: {}", applied.error().describe());
        return false;
    }
    return true;
}

/// Opens the radio the options name, selects its port if one was asked for,
/// and applies every `--param`. Null after printing why.
std::unique_ptr<ISdrDevice> openNamedDevice(const Options& options) {
    auto device = SdrDeviceManager::instance().openSpecifier(options.device);
    if (!device) {
        std::println(stderr, "sweeppp-cli: {}", device.error().describe());
        return nullptr;
    }

    // The port before anything else: gain and bandwidth are per-connector on
    // hardware that has several, so a parameter applied first would be applied
    // to the one being left.
    if (!options.rxPort.empty()) {
        if (auto selected = (*device)->selectRxPort(options.rxPort); !selected) {
            std::println(stderr, "sweeppp-cli: {}", selected.error().describe());
            return nullptr;
        }
    }

    for (const auto& [key, value] : options.deviceParameters) {
        if (!applyDeviceParameter(**device, key, value)) {
            return nullptr;
        }
    }
    return std::move(*device);
}

/// `openNamedDevice`, tuned to the options' sample rate and centre first.
///
/// Shared by `sweep` and `calibrate`: a calibration is only applied while the
/// radio is set the way it was learned, so the two must set it the same way.
/// The `--param`s go last, so one naming `sample_rate` still wins.
std::unique_ptr<ISdrDevice> openConfiguredDevice(const Options& options) {
    Options tuned = options;
    tuned.deviceParameters.clear();
    std::unique_ptr<ISdrDevice> device = openNamedDevice(tuned);
    if (!device) {
        return nullptr;
    }
    if (!applyDeviceParameter(*device, "sample_rate", std::format("{}", options.sampleRate)) ||
        !applyDeviceParameter(*device, "center_hz", std::format("{}", options.centerHz))) {
        return nullptr;
    }
    for (const auto& [key, value] : options.deviceParameters) {
        if (!applyDeviceParameter(*device, key, value)) {
            return nullptr;
        }
    }
    return device;
}

/// The radio the options name, adopted by an instrument with the analysis and
/// corrections they ask for, set to sweep or to stay tuned. Null after printing
/// why.
///
/// The same orchestration the desktop and the server run, so `sweep` and
/// `calibrate` measure what they would.
std::unique_ptr<LocalInstrument> openInstrument(const Options& options, FrameBus& output,
                                                EventBus& events, Telemetry& telemetry,
                                                IFftBackend& backend, WindowType window,
                                                bool sweeping, bool forCalibration) {
    std::unique_ptr<ISdrDevice> device = openConfiguredDevice(options);
    if (!device) {
        return nullptr;
    }

    InstrumentPaths paths = InstrumentPaths::fromConfig();
    paths.calibrationFile = options.calibrationPath;
    auto instrument =
        std::make_unique<LocalInstrument>(output, events, telemetry, std::move(paths), backend);
    instrument->adoptDevice(std::move(device));

    std::uint32_t fftSize = options.fftSize;
    if (!sweeping && options.rbwHz > 0.0) {
        // FFT size from the requested RBW, snapped to what the backend
        // accepts. The planner does the same thing; doing it here keeps
        // --rbw meaningful for a fixed-tune run too.
        const auto desired = static_cast<std::size_t>(options.sampleRate / options.rbwHz);
        fftSize = static_cast<std::uint32_t>(backend.snapSize(desired));
    }

    const PipelineConfig pipelineConfig{
        .fftSize = fftSize,
        .window = window,
        .windowBeta = 8.6,
        .overlap = options.overlap,
        .workerCount = options.workerCount,
        .throttleMode = options.throttle == "every-nth"     ? ThrottleMode::EveryNth
                        : options.throttle == "all-samples" ? ThrottleMode::AllSamples
                                                            : ThrottleMode::Auto,
        .everyNth = options.everyNth,
        .averageCount = options.averageCount,
        .planQuality = FftPlanQuality::Balanced,
        // Headless: no display to keep up with, so a fixed tune publishes
        // only what the output can absorb. A sweep publishes every step to
        // the engine whatever this says, and the engine caps its own output.
        .targetFrameRate = 30.0,
        .dbfsToDbmOffset = 0.0,
    };
    if (auto applied = instrument->applyPipelineConfig(pipelineConfig); !applied) {
        std::println(stderr, "sweeppp-cli: {}", applied.error().describe());
        return nullptr;
    }

    // A calibration is measured as the receiver is: nothing but DC removal,
    // which is part of the context the result is bound to.
    instrument->setCorrectionSettings(
        CorrectionSettings{.dcRemoval = options.dcRemoval,
                           .flatten = !forCalibration && options.flatten,
                           .spurMask = !forCalibration && options.spurMask,
                           .autoSpurs = false});

    if (auto set = instrument->setSweeping(sweeping); !set) {
        std::println(stderr, "sweeppp-cli: {}", set.error().describe());
        return nullptr;
    }
    if (sweeping) {
        SweepPlan plan;
        plan.name = forCalibration ? "calibrate" : "cli";
        plan.segments = {SweepSegment{.startHz = options.startHz, .stopHz = options.stopHz}};
        plan.applyMode(SweepMode::Fast);
        plan.sampleRate = options.sampleRate;
        plan.window = window;
        plan.windowBeta = 8.6;
        if (options.rbwHz > 0.0) {
            plan.rbwHz = options.rbwHz;
        }
        if (options.averageCount > 1) {
            plan.averageCount = options.averageCount;
        }
        // Routing is off unless asked for, and reads the same library and
        // assignments the desktop wrote: "which antenna is on RX2" must not
        // be a different answer depending on which binary is asking.
        plan.antennaRouting = !forCalibration && options.antennaRouting;
        if (auto applied = instrument->applySweepPlan(plan); !applied) {
            std::println(stderr, "sweeppp-cli: {}", applied.error().describe());
            return nullptr;
        }
    }
    return instrument;
}

/// Prints what the instrument raised and keeps the session alive: learning
/// advances in `tick()`, as it does under the desktop's frame loop.
void service(LocalInstrument& instrument, bool quiet) {
    instrument.tick(monotonicNs());
    for (const InstrumentNotice& notice : instrument.takeNotices()) {
        const bool trouble = notice.kind == InstrumentNotice::Kind::Error ||
                             notice.kind == InstrumentNotice::Kind::Warning ||
                             notice.kind == InstrumentNotice::Kind::Condition;
        if (trouble || (!quiet && notice.kind == InstrumentNotice::Kind::Info)) {
            if (!notice.text.empty()) {
                std::println(stderr, "sweeppp-cli: {}", notice.text);
            }
        }
    }
}

/// Sleeps in short steps until `done` or the operator interrupts.
[[nodiscard]] bool waitUntil(const std::function<bool()>& done) {
    while (!g_interrupted.load()) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

/// The calibration file a run reads or writes: the one named, or the radio's.
std::filesystem::path calibrationPathFor(const Options& options, const SdrDeviceInfo& info) {
    return options.calibrationPath.empty() ? CorrectionSet::pathFor(info)
                                           : std::filesystem::path(options.calibrationPath);
}

int runSweep(const Options& options) {
    registerBuiltinsAndPlugins();

    Log::setLevel(options.verbose ? LogLevel::Debug
                  : options.quiet ? LogLevel::Warn
                                  : LogLevel::Info);

    // Buses and the plugin scope before the radio, because destruction runs in
    // reverse: a device a plugin driver handed out carries a vtable in that
    // plugin's image, so the scope withdrawing it must outlive the device.
    // Plugins see the same frames and the same events the GUI does.
    FrameBus frameBus;
    Telemetry telemetry;
    EventBus eventBus;
    installLoggingSubscribers(eventBus);
    const PluginScope pluginScope{frameBus, eventBus};

    auto backend = FftBackendManager::instance().acquireOrDefault(options.fftBackend);
    if (!backend) {
        std::println(stderr, "sweeppp-cli: {}", backend.error().describe());
        return 1;
    }
    auto window = windowTypeFromString(options.window);
    if (!window) {
        std::println(stderr, "sweeppp-cli: {}", window.error().describe());
        return 1;
    }

    // --start with --stop means sweep; anything else is a fixed tune.
    const bool sweeping = options.startHz > 0.0 && options.stopHz > options.startHz;

    std::unique_ptr<LocalInstrument> instrument =
        openInstrument(options, frameBus, eventBus, telemetry, **backend, *window, sweeping, false);
    if (!instrument) {
        return 1;
    }

    if (options.flatten || options.spurMask) {
        const CorrectionSummary learned = instrument->correctionSummary();
        if (!learned.present) {
            std::println(stderr, "sweeppp-cli: NotFound: no such file: {}",
                         calibrationPathFor(options, instrument->device()->info).string());
            return 1;
        }
        // The floor is only what it was measured at; a differing setting
        // leaves it out and says so once. The spurs stay: a reference
        // harmonic does not move with the gain.
        if (options.flatten && !learned.floorStaleReason.empty()) {
            std::println(stderr,
                         "sweeppp-cli: the learned floor is not applied: '{}' differs from "
                         "when it was learned",
                         learned.floorStaleReason);
        }
    }

    std::ofstream fileStream;
    std::ostream* out = &std::cout;
    if (!options.outputPath.empty() && options.outputPath != "-") {
        fileStream.open(options.outputPath, std::ios::trunc);
        if (!fileStream) {
            std::println(stderr, "sweeppp-cli: could not open {}", options.outputPath);
            return 1;
        }
        out = &fileStream;
    }

    CsvWriter csv(*out, options.quiet || options.outputPath == "-", sweeping);
    const FrameBus::SubscriptionId csvId = frameBus.subscribe(&csv);

    CoverageProbe coverage;
    if (sweeping) {
        frameBus.subscribe(&coverage);
    }

    // The forward-compatibility proof: a deliberately slow second consumer.
    // It must fall behind while the CSV writer keeps up and acquisition is
    // untouched. Only attached when --stats asked for the full picture, and
    // slower than the frame interval on purpose, so it is guaranteed to fall
    // behind and the isolation is demonstrated rather than merely asserted.
    std::unique_ptr<SlowConsumer> slow;
    if (options.stats) {
        slow = std::make_unique<SlowConsumer>(std::chrono::milliseconds(100));
        frameBus.subscribe(slow.get());
    }

    std::signal(SIGINT, handleInterrupt);

    const std::uint64_t startedNs = monotonicNs();
    if (auto started = instrument->start(); !started) {
        std::println(stderr, "sweeppp-cli: {}", started.error().describe());
        return 1;
    }
    (void)instrument->takeNotices();

    if (sweeping && !options.quiet) {
        const ScheduleSummary& schedule = instrument->schedule();
        std::println(
            stderr,
            "sweeping {} to {} in {} steps of {} S/s, {} point {} window "
            "({} RBW), ~{} per pass ({} MHz/s, {:.0f}% retune), for {}",
            toml_util::formatFrequencyShort(options.startHz),
            toml_util::formatFrequencyShort(options.stopHz), schedule.stepCount,
            toml_util::formatFrequencyShort(instrument->sweepPlan().sampleRate), schedule.fftSize,
            toString(*window), toml_util::formatFrequencyShort(schedule.actualRbwHz),
            formatDuration(schedule.estimatedPassSeconds),
            schedule.estimatedSweepRateHzPerSec / 1e6, schedule.retuneOverheadFraction * 100.0,
            formatDuration(options.durationSeconds));
    } else if (!options.quiet) {
        const std::uint32_t fftSize = instrument->pipelineConfig().fftSize;
        std::println(stderr, "sweeping {} at {} S/s, {} point {} window ({} RBW) for {}",
                     instrument->device()->info.label,
                     toml_util::formatFrequencyShort(options.sampleRate), fftSize,
                     toString(*window),
                     toml_util::formatFrequencyShort(options.sampleRate * 1.5 /
                                                     static_cast<double>(fftSize)),
                     formatDuration(options.durationSeconds));
    }

    // The snapshot taken inside the loop is the one that describes the run.
    // Sampling again here would divide by a near-zero interval and report
    // every rate as zero, and sampling after stop() would report a stopped
    // throttle state for a run that was fully busy a moment earlier.
    TelemetrySnapshot live;
    while (!g_interrupted.load() &&
           nsToSeconds(monotonicNs() - startedNs) < options.durationSeconds) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        live = telemetry.sample();
        service(*instrument, options.quiet);
    }

    const double wallSeconds = nsToSeconds(monotonicNs() - startedNs);
    instrument->stop();

    // Only now can the accounting balance: every block has been either
    // processed or counted as dropped by stop().
    const TelemetrySnapshot settled = telemetry.sample();

    frameBus.unsubscribe(csvId);
    out->flush();

    if (!options.quiet) {
        std::println(stderr, "\n{} frames written", csv.rows());
    }

    if (sweeping && !options.quiet) {
        const EngineStats engine = instrument->engineStats();
        const CoverageProbe::Snapshot covered = coverage.snapshot();
        const double percent = covered.bins > 0 ? 100.0 * static_cast<double>(covered.measured) /
                                                      static_cast<double>(covered.bins)
                                                : 0.0;
        std::println(stderr, "\n--- sweep coverage ---");
        std::println(stderr, "  passes                {}", engine.passCount - 1);
        std::println(stderr, "  measured sweep rate   {:.1f} MHz/s",
                     engine.measuredSweepRateHzPerSec / 1e6);
        std::println(stderr, "  grid bins             {}", covered.bins);
        std::println(stderr, "  measured              {} ({:.1f}%)", covered.measured, percent);
        std::println(stderr, "  unmeasured runs       {}", covered.gapRuns);
        std::println(stderr, "  last pass measured    {:.1f}% of the span",
                     engine.lastPassCoverage * 100.0);

        const std::uint64_t total =
            engine.stitched + engine.unsettled + engine.unattributed + engine.tooShort;
        const auto share = [total](std::uint64_t part) {
            return total > 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(total) : 0.0;
        };
        std::println(stderr, "  step frames stitched  {} ({:.1f}%)", engine.stitched,
                     share(engine.stitched));
        std::println(stderr, "    discarded unsettled {} ({:.1f}%)", engine.unsettled,
                     share(engine.unsettled));
        std::println(stderr, "    unattributable      {} ({:.1f}%)", engine.unattributed,
                     share(engine.unattributed));
        std::println(stderr, "    too few bins        {} ({:.1f}%)", engine.tooShort,
                     share(engine.tooShort));
    }

    if (options.stats) {
        printStatsSummary(live, settled, frameBus, wallSeconds);
        if (slow) {
            std::println("\n--- isolation proof (§3.7) ---");
            std::println("  a consumer 100 ms/frame slower than the stream:");
            std::println("    processed           {}", slow->processedFrames());
            std::println("    dropped (its own)   {}", slow->droppedFrames());
            std::println("    CSV writer rows     {}", csv.rows());
            std::println("    samples dropped     {}", settled.stream.samplesDropped);
            std::println("  -> the slow consumer degrades alone; acquisition and the fast "
                         "consumer are unaffected.");
        }
    }

    return csv.failures() == 0 ? 0 : 1;
}

/// The learn the GUI's button runs, headless: the same instrument, its learn
/// advanced by the same calls, and the result written where `sweep --flatten`
/// reads it.
int runCalibrate(const Options& options) {
    registerBuiltinsAndPlugins();

    Log::setLevel(options.verbose ? LogLevel::Debug
                  : options.quiet ? LogLevel::Warn
                                  : LogLevel::Info);

    FrameBus frameBus;
    Telemetry telemetry;
    EventBus eventBus;
    installLoggingSubscribers(eventBus);
    const PluginScope pluginScope{frameBus, eventBus};

    auto backend = FftBackendManager::instance().acquireOrDefault(options.fftBackend);
    if (!backend) {
        std::println(stderr, "sweeppp-cli: {}", backend.error().describe());
        return 1;
    }
    auto window = windowTypeFromString(options.window);
    if (!window) {
        std::println(stderr, "sweeppp-cli: {}", window.error().describe());
        return 1;
    }

    const bool sweeping = options.startHz > 0.0 && options.stopHz > options.startHz;
    std::unique_ptr<LocalInstrument> instrument =
        openInstrument(options, frameBus, eventBus, telemetry, **backend, *window, sweeping, true);
    if (!instrument) {
        return 1;
    }

    std::signal(SIGINT, handleInterrupt);
    if (auto started = instrument->start(); !started) {
        std::println(stderr, "sweeppp-cli: {}", started.error().describe());
        return 1;
    }
    (void)instrument->takeNotices();

    // A sweep's first pass starts wherever the engine came up; waiting for it
    // to complete makes the learn's first pass a whole one.
    if (sweeping && !waitUntil([&] {
            service(*instrument, true);
            return instrument->engineStats().passCount >= 2;
        })) {
        instrument->stop();
        std::println(stderr, "sweeppp-cli: interrupted");
        return 1;
    }

    if (auto learning = instrument->startLearning(); !learning) {
        instrument->stop();
        std::println(stderr, "sweeppp-cli: {}", learning.error().describe());
        return 1;
    }
    (void)instrument->takeNotices();

    if (!options.quiet) {
        if (sweeping) {
            std::println(stderr,
                         "calibrating {} to {} in {} steps: pass 1 of {}, floor and "
                         "LO-offset spurs",
                         toml_util::formatFrequencyShort(options.startHz),
                         toml_util::formatFrequencyShort(options.stopHz),
                         instrument->schedule().stepCount, LearnParameters{}.absolutePasses + 1);
        } else {
            std::println(stderr, "calibrating {} at {}: {} frames",
                         toml_util::formatFrequencyShort(options.centerHz),
                         toml_util::formatFrequencyShort(options.sampleRate),
                         LocalInstrument::kLearnFrames);
        }
    }

    // The floor and LO-offset spurs land when the learn moves on to the
    // fixed spurs, which is when the label changes.
    std::string label = instrument->learningLabel();
    std::string failure;
    const bool finished = waitUntil([&] {
        instrument->tick(monotonicNs());
        for (const InstrumentNotice& notice : instrument->takeNotices()) {
            if (notice.kind == InstrumentNotice::Kind::Error) {
                failure = notice.text;
            }
        }
        if (!instrument->learning()) {
            return true;
        }
        if (const std::string now = instrument->learningLabel(); now != label) {
            label = now;
            if (sweeping && !options.quiet) {
                const CorrectionSummary interim = instrument->correctionSummary();
                std::println(stderr,
                             "  floor over {} points, {} LO-offset spur(s); {} more passes "
                             "for fixed spurs",
                             interim.floorPoints, interim.spurs, LearnParameters{}.absolutePasses);
            }
        }
        return false;
    });
    if (!finished) {
        instrument->cancelLearning();
        instrument->stop();
        std::println(stderr, "sweeppp-cli: interrupted");
        return 1;
    }
    instrument->stop();
    if (!failure.empty()) {
        std::println(stderr, "sweeppp-cli: {}", failure);
        return 1;
    }

    const std::filesystem::path path = calibrationPathFor(options, instrument->device()->info);
    auto set = CorrectionSet::load(path);
    if (!set) {
        std::println(stderr, "sweeppp-cli: {}", set.error().describe());
        return 1;
    }
    const auto loCount = static_cast<std::size_t>(std::ranges::count_if(
        set->spurs, [](const SpurEntry& spur) { return spur.kind == SpurKind::LoOffset; }));

    std::println("calibration written to {}", path.string());
    std::println("  floor              {} points over {} S/s", set->floor.levelDb.size(),
                 toml_util::formatFrequencyShort(set->floor.sampleRate));
    std::println("  LO-offset spurs    {}", loCount);
    std::println("  fixed spurs        {}", set->spurs.size() - loCount);
    for (const SpurEntry& spur : set->spurs) {
        std::println("    {:<10} {:>16} width {}", toString(spur.kind),
                     spur.kind == SpurKind::LoOffset ? std::format("{:+.3f} MHz", spur.hz / 1e6)
                                                     : toml_util::formatFrequencyShort(spur.hz),
                     toml_util::formatFrequencyShort(spur.widthHz));
    }
    std::println("  context            {} parameter(s), DC removal {}",
                 set->context.parameters.size(), set->context.dcRemoval ? "on" : "off");
    return 0;
}

/// `info <file>` for a session.
///
/// Option translation and nothing else. The output text lives in
/// libsweepsfile's CLI core, so `sweeppp-cli info` and `sweeps info` cannot
/// drift apart -- there is one implementation, not two that agree today.
int runSessionInfo(const Options& options) {
    sweeps::cli::InfoOptions info;
    info.path = options.inputPath;
    return sweeps::cli::runInfo(info, std::cout, std::cerr);
}

int runRecord(const Options& options) {
    registerBuiltinsAndPlugins();

    if (options.outputPath.empty()) {
        std::println(stderr, "sweeppp-cli: record needs -o <file.sweeps>");
        return 1;
    }

    // Buses and the plugin scope before the radio, because destruction runs in
    // reverse: a device a plugin driver handed out carries a vtable in that
    // plugin's image, so the scope withdrawing it must outlive the device.
    // Plugins see the same frames and the same events the GUI does. Without
    // this a frame-processor facet would be listed as having no bus to attach
    // to, which is true of `info` and would be a lie here.
    FrameBus frameBus;
    Telemetry telemetry;
    EventBus eventBus;
    installLoggingSubscribers(eventBus);
    const PluginScope pluginScope{frameBus, eventBus};

    auto backend = FftBackendManager::instance().acquireOrDefault(options.fftBackend);
    if (!backend) {
        std::println(stderr, "sweeppp-cli: {}", backend.error().describe());
        return 1;
    }

    auto device = SdrDeviceManager::instance().openSpecifier(options.device);
    if (!device) {
        std::println(stderr, "sweeppp-cli: {}", device.error().describe());
        return 1;
    }

    if (!options.rxPort.empty()) {
        if (auto selected = (*device)->selectRxPort(options.rxPort); !selected) {
            std::println(stderr, "sweeppp-cli: {}", selected.error().describe());
            return 1;
        }
    }

    (void)(*device)->setParameter("sample_rate", SdrValue{options.sampleRate});
    (void)(*device)->setParameter("center_hz", SdrValue{options.centerHz});
    for (const auto& [key, value] : options.deviceParameters) {
        const auto parameters = (*device)->parameters();
        const auto match = std::ranges::find_if(
            parameters, [&key](const SdrParameter& p) { return p.key == key; });
        if (match != parameters.end()) {
            if (auto parsed = parseSdrValue(value, match->type)) {
                (void)(*device)->setParameter(key, *parsed);
            }
        }
    }

    auto window = windowTypeFromString(options.window);
    if (!window) {
        std::println(stderr, "sweeppp-cli: {}", window.error().describe());
        return 1;
    }

    auto writer = session::SessionRecorder::create(
        options.outputPath, session::RecorderConfig{{.sessionName = "cli-record"}});
    if (!writer) {
        std::println(stderr, "sweeppp-cli: {}", writer.error().describe());
        return 1;
    }
    (*writer)->attachEvents(eventBus);
    frameBus.subscribe(writer->get());

    Pipeline pipeline(frameBus, telemetry, eventBus);
    if (auto configured =
            pipeline.configure(**backend, PipelineConfig{.fftSize = options.fftSize,
                                                         .window = *window,
                                                         .overlap = options.overlap,
                                                         .workerCount = options.workerCount,
                                                         .averageCount = options.averageCount,
                                                         .targetFrameRate = 30.0});
        !configured) {
        std::println(stderr, "sweeppp-cli: {}", configured.error().describe());
        return 1;
    }
    pipeline.setTuning(options.centerHz, options.sampleRate, options.sampleRate);

    std::signal(SIGINT, handleInterrupt);
    const std::uint64_t startedNs = monotonicNs();

    if (auto started = pipeline.start(**device, StreamConfig{.framesPerBlock = 262'144,
                                                             .blockCount = 64,
                                                             .format = (*device)->nativeFormat()});
        !started) {
        std::println(stderr, "sweeppp-cli: {}", started.error().describe());
        return 1;
    }

    if (!options.quiet) {
        std::println(stderr, "recording to {} for {}", options.outputPath,
                     formatDuration(options.durationSeconds));
    }

    while (!g_interrupted.load() &&
           nsToSeconds(monotonicNs() - startedNs) < options.durationSeconds) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        telemetry.sample();
        if ((*writer)->retentionReached()) {
            std::println(stderr, "sweeppp-cli: {}", (*writer)->retentionReason());
            break;
        }
    }

    pipeline.stop();
    if (auto closed = (*writer)->close(); !closed) {
        std::println(stderr, "sweeppp-cli: {}", closed.error().describe());
        return 1;
    }

    std::println("wrote {} ({} lines, {} segments, {})", options.outputPath,
                 (*writer)->linesWritten(), (*writer)->segmentCount(),
                 toml_util::formatBytes((*writer)->bytesWritten()));
    return 0;
}

int runReplay(const Options& options) {
    if (options.inputPath.empty()) {
        std::println(stderr, "sweeppp-cli: replay needs a .sweeps file");
        return 1;
    }

    FrameBus frameBus;
    EventBus eventBus;
    installLoggingSubscribers(eventBus);

    auto replay = SessionReplay::open(options.inputPath, frameBus, eventBus);
    if (!replay) {
        std::println(stderr, "sweeppp-cli: {}", replay.error().describe());
        return 1;
    }

    std::ofstream fileStream;
    std::ostream* out = &std::cout;
    if (!options.outputPath.empty() && options.outputPath != "-") {
        fileStream.open(options.outputPath, std::ios::trunc);
        out = &fileStream;
    }

    CsvWriter csv(*out, true);
    frameBus.subscribe(&csv);

    (*replay)->setSpeed(options.durationSeconds > 0.0 ? 16.0 : 1.0);

    std::atomic<bool> complete{false};
    (*replay)->setCompletionCallback([&complete] { complete.store(true); });

    if (auto started = (*replay)->start(); !started) {
        std::println(stderr, "sweeppp-cli: {}", started.error().describe());
        return 1;
    }

    std::signal(SIGINT, handleInterrupt);
    while (!complete.load() && !g_interrupted.load() && (*replay)->running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!options.quiet) {
            std::print(stderr, "\rreplay {} / {}", formatDuration((*replay)->positionSeconds()),
                       formatDuration((*replay)->durationSeconds()));
        }
    }
    (*replay)->stop();
    out->flush();

    std::println(stderr, "\nreplayed {} frames", csv.rows());
    return 0;
}

int runExtract(const Options& options) {
    if (options.inputPath.empty() || options.outputPath.empty()) {
        std::println(stderr, "sweeppp-cli: extract needs an input .sweeps and -o <output>");
        return 1;
    }

    sweeps::cli::ExtractOptions extract;
    extract.input = options.inputPath;
    extract.output = options.outputPath;
    extract.fromSeconds = options.fromSeconds;
    extract.toSeconds = options.toSeconds;
    extract.startHz = options.startHz;
    extract.stopHz = options.stopHz;
    extract.applicationVersion = std::string(versionString());
    return sweeps::cli::runExtract(extract, std::cout, std::cerr);
}

/// The token from `--token`, else `--token-file`, else the environment.
/// Surrounding whitespace is dropped: a file written by `echo` ends in a
/// newline nobody meant as part of the secret.
Result<std::string> resolveToken(const Options& options) {
    std::string token = options.token;
    if (token.empty() && !options.tokenFile.empty()) {
        std::ifstream in(options.tokenFile, std::ios::binary);
        if (!in) {
            return fail<std::string>(ErrorCode::NotFound, "cannot read the token file {}",
                                     options.tokenFile);
        }
        token.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (token.empty()) {
        if (const char* environment = std::getenv("SWEEPPP_REMOTE_TOKEN")) {
            token = environment;
        }
    }
    const auto first = token.find_first_not_of(" \t\r\n");
    const auto last = token.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{} : token.substr(first, last - first + 1);
}

int runServe(const Options& options) {
    // The token is the key the link is encrypted under, so it is worth as
    // much as it is hard to guess: 32 random bytes, printed as hex.
    if (options.newToken) {
        std::array<std::uint8_t, 32> secret{};
        if (auto filled = crypto::fillRandom(secret); !filled) {
            std::println(stderr, "sweeppp-cli: {}", filled.error().describe());
            return 1;
        }
        std::println("{}", crypto::toHex(secret));
        return 0;
    }

    auto token = resolveToken(options);
    if (!token) {
        std::println(stderr, "sweeppp-cli: {}", token.error().describe());
        return 1;
    }
    if (token->empty() && !net::isLoopbackAddress(options.listenAddress)) {
        std::println(stderr,
                     "sweeppp-cli: listening on {} needs a token (--token, --token-file or "
                     "SWEEPPP_REMOTE_TOKEN); without one, anyone who can reach this machine "
                     "can drive the radio",
                     options.listenAddress);
        return 1;
    }

    // Radio and transform plugins, but no frame processors: those run on the
    // desktop, against the frames it receives.
    registerBuiltinsAndPlugins();

    auto backend = FftBackendManager::instance().acquireOrDefault(options.fftBackend);
    if (!backend) {
        std::println(stderr, "sweeppp-cli: {}", backend.error().describe());
        return 1;
    }
    std::unique_ptr<ISdrDevice> device = openNamedDevice(options);
    if (!device) {
        return 1;
    }

    FrameBus output;
    EventBus events;
    Telemetry telemetry;
    LocalInstrument instrument(output, events, telemetry, InstrumentPaths::fromConfig(), **backend);
    instrument.adoptDevice(std::move(device));
    for (const InstrumentNotice& notice : instrument.takeNotices()) {
        std::println(stderr, "sweeppp-cli: {}", notice.text);
    }

    remote::RemoteServer server(
        instrument, output, events, telemetry,
        remote::ServerConfig{.listenAddress = options.listenAddress,
                             .port = options.port,
                             .token = *token,
                             .linger = std::chrono::milliseconds(
                                 static_cast<std::int64_t>(options.lingerSeconds * 1000.0)),
                             .sessionsDir = Paths::instance().sessionsDir(),
                             .recordAtStart = options.record,
                             .advertise = !options.noAdvertise &&
                                          !net::isLoopbackAddress(options.listenAddress),
                             .shared = options.shared,
                             .maxClients = options.maxClients});
    if (auto started = server.start(); !started) {
        std::println(stderr, "sweeppp-cli: {}", started.error().describe());
        return 1;
    }

    std::println("serving {} on {}:{}{}", instrument.displayLabel(), options.listenAddress,
                 server.port(), token->empty() ? " (no token: this machine only)" : "");
    std::fflush(stdout);

    std::signal(SIGINT, handleInterrupt);
    std::signal(SIGTERM, handleInterrupt);
    std::vector<remote::ConnectedClient> lastClients;
    while (!g_interrupted.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (options.quiet) {
            continue;
        }
        const std::vector<remote::ConnectedClient> clients = server.clients();
        const auto known = [](const std::vector<remote::ConnectedClient>& list, std::uint64_t id) {
            return std::ranges::find(list, id, &remote::ConnectedClient::id);
        };
        for (const remote::ConnectedClient& client : clients) {
            const auto before = known(lastClients, client.id);
            if (before == lastClients.end()) {
                std::println(stderr, "{} ({}, {}) connected{}", client.name, client.kind,
                             client.address, client.controls ? ", in control" : "");
            } else if (client.controls && !before->controls) {
                std::println(stderr, "{} has control", client.name);
            }
        }
        for (const remote::ConnectedClient& client : lastClients) {
            if (known(clients, client.id) == clients.end()) {
                std::println(stderr, "{} disconnected", client.name);
            }
        }
        lastClients = clients;
    }

    std::println(stderr, "\nstopping");
    server.stop();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    auto options = parseArguments(argc, argv);
    if (!options) {
        std::println(stderr, "sweeppp-cli: {}", options.error().describe());
        std::println(stderr, "Run `sweeppp-cli help` for usage.");
        return 1;
    }

    if (!options->configDir.empty()) {
        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(options->configDir, ec);
        Paths::setConfigDirOverride(ec ? std::filesystem::path(options->configDir) : absolute);
    }

    // Everything but help and version does real work, so it gets the log file
    // and the crash handler. Those two do not: neither should create a config
    // directory as a side effect of being asked a question.
    if (options->command != Command::Help && options->command != Command::Version) {
        installDiagnostics();
    }

    switch (options->command) {
    case Command::Help:
        printUsage();
        return 0;

    case Command::Version:
        std::println("sweeppp-cli {}", buildString());
        std::println("  built {} with {} for {}", buildDate(), buildCompiler(), buildPlatform());
        return 0;

    case Command::Info:
        // A path means "describe this session"; no path means "describe this
        // build".
        return options->inputPath.empty() ? runInfo(*options) : runSessionInfo(*options);

    case Command::Sweep:
        return runSweep(*options);
    case Command::Calibrate:
        return runCalibrate(*options);
    case Command::Record:
        return runRecord(*options);
    case Command::Replay:
        return runReplay(*options);
    case Command::Extract:
        return runExtract(*options);

    case Command::Serve:
        return runServe(*options);
    }

    return 0;
}
