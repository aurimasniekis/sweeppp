// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/core/Telemetry.hpp"
#include "sweeppp/correction/Corrections.hpp"
#include "sweeppp/fft/FftBackendManager.hpp"
#include "sweeppp/fft/FftBenchmark.hpp"
#include "sweeppp/pipeline/Pipeline.hpp"
#include "sweeppp/rf/Antenna.hpp"
#include "sweeppp/rf/AntennaAssignments.hpp"
#include "sweeppp/rf/IRfPath.hpp"
#include "sweeppp/sdr/SdrDeviceInfo.hpp"
#include "sweeppp/sdr/SdrParameter.hpp"
#include "sweeppp/sdr/SdrRxPort.hpp"
#include "sweeppp/sweep/SweepPlan.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sweeppp {

/// What a radio is and what it offers, as values.
///
/// The interface never hands out the `ISdrDevice` itself: the radio may be on
/// another machine, and a pointer to it would be a promise nothing remote can
/// keep.
struct DeviceDescriptor {
    SdrDeviceInfo info;
    std::vector<SdrParameter> parameters;
    std::vector<SdrRxPort> rxPorts;
    std::vector<double> supportedSampleRates;
};

/// The parts of a sweep schedule the panels read.
struct ScheduleSummary {
    std::size_t stepCount = 0;
    std::uint32_t fftSize = 0;
    double actualRbwHz = 0.0;
    double gridStartHz = 0.0;
    double gridBinWidthHz = 0.0;
    std::size_t gridBinCount = 0;
    double estimatedPassSeconds = 0.0;
    double estimatedSweepRateHzPerSec = 0.0;
    double retuneOverheadFraction = 0.0;
    std::vector<std::pair<double, double>> unroutedHz;
    std::uint32_t portSwitches = 0;

    [[nodiscard]] static ScheduleSummary of(const SweepSchedule& schedule);
};

/// How the stitcher is doing, for the Performance panel.
struct EngineStats {
    std::uint64_t stitched = 0;
    std::uint64_t unsettled = 0;
    std::uint64_t unattributed = 0;
    std::uint64_t tooShort = 0;
    double lastPassCoverage = 0.0;
    double measuredSweepRateHzPerSec = 0.0;
    std::uint64_t passCount = 0;
};

/// What has been learned about the open radio.
struct CorrectionSummary {
    bool present = false;
    std::size_t floorPoints = 0;
    std::size_t spurs = 0;
    std::size_t automaticSpurs = 0;
    std::string learnedAt;
    /// Why the learned floor is not being applied, or empty while it is.
    std::string floorStaleReason;
};

/// An open antenna switcher, as values.
struct SwitcherView {
    std::string key;
    RfPathInfo info;
    std::vector<RfPathInput> inputs;
    std::uint32_t selectedInput = 0;
};

/// One antenna the radio can hear through, as values.
struct RfLegView {
    RoutePort route;
    std::string switcherKey; ///< Empty when the antenna is on the connector
    Antenna antenna;
    std::string portLabel; ///< "RX1", or "RX1 via J3"
    /// On the selected connector and, behind a switcher, its selected input:
    /// the antenna being measured through right now.
    bool live = false;
};

/// The network between a remote instrument and this process, as both ends
/// see it. All zero for a local one.
struct LinkStats {
    double roundTripMs = 0.0;
    double bytesPerSec = 0.0; ///< Arriving here
    std::uint64_t bytesReceived = 0;
    /// The server's side: what it sent, and what it had to merge into a later
    /// frame because the link had not taken the earlier one yet.
    std::uint64_t framesSent = 0;
    std::uint64_t passesCoalesced = 0;
    std::uint64_t partialsCoalesced = 0;
    std::uint64_t eventsDropped = 0;
    /// Time the server spent turning frames into records, in all.
    std::uint64_t encodeNs = 0;
};

/// An FFT benchmark on the machine the instrument computes on.
struct BenchmarkStatus {
    bool running = false;
    bool complete = false;
    std::size_t stepsDone = 0;
    std::size_t stepsTotal = 0;
    std::string currentStep;
    double elapsedSeconds = 0.0;
    std::vector<FftBenchmarkEntry> results;
};

/// Something the operator should be told, raised where toasts are not.
struct InstrumentNotice {
    enum class Kind : std::uint8_t {
        Info,
        Success,
        Warning,
        Error,
        /// A latched condition: replaces any earlier one until cleared.
        Condition,
        ClearCondition,
    };
    Kind kind = Kind::Info;
    std::string text;
};

/// Where an instrument keeps what belongs to its bench: the antenna library,
/// which antenna is on which connector, and what was learned about each radio.
///
/// Passed in rather than read from `Paths`, so a test, or a server given its
/// own config directory, never writes into the operator's.
struct InstrumentPaths {
    std::vector<std::filesystem::path> antennaSearchPath;
    std::filesystem::path antennasDir;
    std::filesystem::path calibrationDir;
    /// One calibration file for whichever radio is open, read and written in
    /// place of the radio's own under `calibrationDir`. Empty uses those.
    std::filesystem::path calibrationFile;

    /// From `Paths`, the application's own configuration.
    [[nodiscard]] static InstrumentPaths fromConfig();

    /// Everything under one directory, for a test or a server.
    [[nodiscard]] static InstrumentPaths under(const std::filesystem::path& root);
};

/// The radio, the FFT pipeline and the sweep engine, driven as one.
///
/// Implemented here, in this process, and over the network, against a server
/// running the same local implementation -- which is what makes a remote radio
/// behave exactly like one plugged in.
///
/// Every method is called from one thread, the owner's. Frames and events go
/// out on the buses the owner lent; anything that needs saying goes through
/// `takeNotices()`.
class Instrument {
public:
    virtual ~Instrument() = default;

    Instrument(const Instrument&) = delete;
    Instrument& operator=(const Instrument&) = delete;
    Instrument(Instrument&&) = delete;
    Instrument& operator=(Instrument&&) = delete;

    // ---- identity --------------------------------------------------------

    /// How a profile names this instrument, so loading it opens it again.
    [[nodiscard]] virtual std::string profileDriver() const = 0;
    [[nodiscard]] virtual std::string profileId() const = 0;

    /// What the window calls the radio: "HackRF One", or "HackRF One on pi".
    [[nodiscard]] virtual std::string displayLabel() const = 0;

    /// The machine the transforms run on, by name; empty for this one.
    [[nodiscard]] virtual std::string computeHost() const = 0;

    // ---- control ---------------------------------------------------------

    /// Whether this process may change anything: always, for a radio of its
    /// own; for one shared with others, only while it holds control.
    [[nodiscard]] virtual bool canControl() const noexcept { return true; }

    /// Takes control from whoever has it.
    virtual Status takeControl() { return ok(); }

    // ---- the radio -------------------------------------------------------

    /// Null when no radio is open.
    [[nodiscard]] virtual const DeviceDescriptor* device() const noexcept = 0;
    [[nodiscard]] virtual std::optional<SdrValue> parameter(std::string_view key) const = 0;
    [[nodiscard]] virtual std::string selectedRxPort() const = 0;

    /// Publishes `ParameterChangedEvent` when it succeeds. A sample rate while
    /// sweeping goes to the plan instead, which owns it.
    virtual Status setDeviceParameter(const std::string& key, const SdrValue& value) = 0;

    /// Whether the panel must hold this parameter until acquisition stops.
    [[nodiscard]] virtual bool parameterNeedsStop(const SdrParameter& parameter) const noexcept = 0;

    virtual void closeDevice() = 0;

    [[nodiscard]] virtual std::vector<SdrHealthReading> health() const = 0;

    // ---- running ---------------------------------------------------------

    virtual Status start() = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual bool running() const noexcept = 0;
    virtual Status restart() = 0;

    /// Bumped each time acquisition starts, restarts included. A new run
    /// invalidates what was accumulated from the last one.
    [[nodiscard]] virtual std::uint64_t startGeneration() const noexcept = 0;

    [[nodiscard]] virtual bool sweeping() const noexcept = 0;
    virtual Status setSweeping(bool enabled) = 0;

    /// Kept whatever it says; only a valid plan reaches the radio.
    virtual Status applySweepPlan(const SweepPlan& plan) = 0;

    /// Sweeps `plan`, whether or not the radio was sweeping. One restart.
    virtual Status sweepRange(const SweepPlan& plan) = 0;

    /// The plan the engine is running, clamped by what the radio accepted.
    [[nodiscard]] virtual const SweepPlan& sweepPlan() const noexcept = 0;
    [[nodiscard]] virtual const ScheduleSummary& schedule() const noexcept = 0;
    [[nodiscard]] virtual EngineStats engineStats() const = 0;

    virtual Status applyPipelineConfig(const PipelineConfig& config) = 0;
    [[nodiscard]] virtual const PipelineConfig& pipelineConfig() const noexcept = 0;

    [[nodiscard]] virtual std::vector<FftBackendInfo> fftBackends() const = 0;

    /// Times every available backend where the transforms run, on its own
    /// thread there. Results come in through `benchmark()`.
    virtual Status startBenchmark(const FftBenchmarkConfig& config) = 0;
    virtual void cancelBenchmark() = 0;
    [[nodiscard]] virtual BenchmarkStatus benchmark() const = 0;
    [[nodiscard]] virtual std::string fftBackendName() const = 0;
    virtual Status setFftBackend(std::string_view name) = 0;

    // ---- receiver corrections --------------------------------------------

    [[nodiscard]] virtual const CorrectionSettings& correctionSettings() const noexcept = 0;
    virtual void setCorrectionSettings(const CorrectionSettings& settings) = 0;
    [[nodiscard]] virtual CorrectionSummary correctionSummary() const = 0;

    virtual Status startLearning() = 0;
    [[nodiscard]] virtual bool learning() const = 0;
    [[nodiscard]] virtual std::string learningLabel() const = 0;
    virtual void cancelLearning() = 0;
    virtual void clearAutoSpurs() = 0;
    virtual void clearCorrections() = 0;

    // ---- antennas --------------------------------------------------------

    [[nodiscard]] virtual const AntennaLibrary& antennas() const noexcept = 0;

    /// Replaces the operator's own entries; the shipped ones stay.
    virtual Status setUserAntennas(std::vector<Antenna> entries) = 0;

    [[nodiscard]] virtual const AntennaAssignments& antennaAssignments() const noexcept = 0;
    virtual Status setAntennaAssignments(AntennaAssignments assignments) = 0;

    /// The key the open radio's assignments are filed under; empty with none.
    [[nodiscard]] virtual std::string deviceAntennaKey() const = 0;
    [[nodiscard]] virtual const Antenna* antennaOnPort(std::string_view portId) const = 0;
    [[nodiscard]] virtual std::vector<RfLegView> rfPath() const = 0;
    [[nodiscard]] virtual std::vector<std::pair<double, double>> antennaCoverage() const = 0;

    [[nodiscard]] virtual std::span<const SwitcherView> openSwitchers() const noexcept = 0;
    [[nodiscard]] virtual const SwitcherView* switcher(std::string_view key) const = 0;

    /// The switchers connected to the machine the radio is on, as of the last
    /// `rescanSwitchers()`.
    [[nodiscard]] virtual std::span<const RfPathInfo> availableSwitchers() const noexcept = 0;
    virtual void rescanSwitchers() = 0;

    // ---- telemetry and housekeeping ---------------------------------------

    /// Stream and processing figures measured elsewhere, or null when they are
    /// this process's own and already in the shared `Telemetry`.
    [[nodiscard]] virtual const TelemetrySnapshot* engineTelemetry() const noexcept {
        return nullptr;
    }
    virtual void resetTelemetry() = 0;

    /// Learning, automatic spurs, staleness and health: work that runs on the
    /// owner's thread, a few times a second or more.
    virtual void tick(std::uint64_t nowNs) = 0;

    [[nodiscard]] virtual std::vector<InstrumentNotice> takeNotices() = 0;

protected:
    Instrument() = default;
};

} // namespace sweeppp
