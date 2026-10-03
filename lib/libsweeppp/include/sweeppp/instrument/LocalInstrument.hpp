// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/EventBus.hpp"
#include "sweeppp/correction/CorrectionLearner.hpp"
#include "sweeppp/fft/IFftBackend.hpp"
#include "sweeppp/instrument/Instrument.hpp"
#include "sweeppp/pipeline/FrameBus.hpp"
#include "sweeppp/pipeline/Pipeline.hpp"
#include "sweeppp/sdr/ISdrDevice.hpp"
#include "sweeppp/sweep/SweepEngine.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sweeppp {

/// The instrument in this process: a radio opened here, transformed here.
///
/// The orchestration that used to live in the desktop's state object -- how a
/// plan reaches a running sweep, how a learn advances, how antennas re-plan --
/// lifted out so a server can run exactly the same code for a remote desktop.
class LocalInstrument final : public Instrument {
public:
    /// `output` receives what the engine publishes: stitched passes when
    /// sweeping, transforms when tuned. `events` and `telemetry` are shared
    /// with whoever displays them.
    LocalInstrument(FrameBus& output, EventBus& events, Telemetry& telemetry, InstrumentPaths paths,
                    IFftBackend& backend);
    ~LocalInstrument() override;

    LocalInstrument(const LocalInstrument&) = delete;
    LocalInstrument& operator=(const LocalInstrument&) = delete;
    LocalInstrument(LocalInstrument&&) = delete;
    LocalInstrument& operator=(LocalInstrument&&) = delete;

    /// Installs an opened radio: closes any previous one, sweeps the new one's
    /// whole range at its fastest rate, and loads what was learned about it.
    void adoptDevice(std::unique_ptr<ISdrDevice> device);

    /// The driver and id of the open radio, for "is this already open".
    [[nodiscard]] bool holds(std::string_view driver, std::string_view id) const noexcept;

    /// Takes a configuration without a radio to apply it to, as when a remote
    /// one goes away and this takes over its settings.
    void adoptConfiguration(const SweepPlan& plan, const PipelineConfig& config, bool sweeping,
                            const CorrectionSettings& corrections);

    /// The stored plan, unchecked, for a caller that edits it without wanting
    /// the radio to follow yet.
    void setStoredPlan(const SweepPlan& plan) { m_sweepPlan = plan; }

    /// Learning a fixed tune takes this many frames.
    static constexpr std::size_t kLearnFrames = 200;

    /// Passes a swept learn takes: one for the floor and the LO-offset spurs,
    /// the rest through them, plus the one under way at the start.
    [[nodiscard]] static std::size_t learnPasses() noexcept;

    // ---- Instrument ------------------------------------------------------

    [[nodiscard]] std::string profileDriver() const override;
    [[nodiscard]] std::string profileId() const override;
    [[nodiscard]] std::string displayLabel() const override;
    [[nodiscard]] bool canBenchmark() const noexcept override { return true; }

    [[nodiscard]] const DeviceDescriptor* device() const noexcept override;
    [[nodiscard]] std::optional<SdrValue> parameter(std::string_view key) const override;
    [[nodiscard]] std::string selectedRxPort() const override;
    Status setDeviceParameter(const std::string& key, const SdrValue& value) override;
    [[nodiscard]] bool parameterNeedsStop(const SdrParameter& parameter) const noexcept override;
    void closeDevice() override;
    [[nodiscard]] std::vector<SdrHealthReading> health() const override;

    Status start() override;
    void stop() override;
    [[nodiscard]] bool running() const noexcept override;
    Status restart() override;
    [[nodiscard]] std::uint64_t startGeneration() const noexcept override {
        return m_startGeneration;
    }

    [[nodiscard]] bool sweeping() const noexcept override { return m_sweeping; }
    Status setSweeping(bool enabled) override;
    Status applySweepPlan(const SweepPlan& plan) override;
    Status sweepRange(const SweepPlan& plan) override;
    [[nodiscard]] const SweepPlan& sweepPlan() const noexcept override { return m_sweepPlan; }
    [[nodiscard]] const ScheduleSummary& schedule() const noexcept override { return m_schedule; }
    [[nodiscard]] EngineStats engineStats() const override;

    Status applyPipelineConfig(const PipelineConfig& config) override;
    [[nodiscard]] const PipelineConfig& pipelineConfig() const noexcept override {
        return m_pipelineConfig;
    }

    [[nodiscard]] std::vector<FftBackendInfo> fftBackends() const override;
    [[nodiscard]] std::string fftBackendName() const override;
    Status setFftBackend(std::string_view name) override;

    [[nodiscard]] const CorrectionSettings& correctionSettings() const noexcept override {
        return m_correctionSettings;
    }
    void setCorrectionSettings(const CorrectionSettings& settings) override;
    [[nodiscard]] CorrectionSummary correctionSummary() const override;
    Status startLearning() override;
    [[nodiscard]] bool learning() const override;
    [[nodiscard]] std::string learningLabel() const override;
    void cancelLearning() override;
    void clearAutoSpurs() override;
    void clearCorrections() override;

    [[nodiscard]] const AntennaLibrary& antennas() const noexcept override { return m_antennas; }
    Status setUserAntennas(std::vector<Antenna> entries) override;
    [[nodiscard]] const AntennaAssignments& antennaAssignments() const noexcept override {
        return m_assignments;
    }
    Status setAntennaAssignments(AntennaAssignments assignments) override;
    [[nodiscard]] std::string deviceAntennaKey() const override;
    [[nodiscard]] const Antenna* antennaOnPort(std::string_view portId) const override;
    [[nodiscard]] std::vector<RfLegView> rfPath() const override;
    [[nodiscard]] std::vector<std::pair<double, double>> antennaCoverage() const override;
    [[nodiscard]] std::span<const SwitcherView> openSwitchers() const noexcept override {
        return m_switcherViews;
    }
    [[nodiscard]] const SwitcherView* switcher(std::string_view key) const override;
    [[nodiscard]] std::span<const RfPathInfo> availableSwitchers() const noexcept override {
        return m_availableSwitchers;
    }
    void rescanSwitchers() override;

    void resetTelemetry() override { m_telemetry.reset(); }
    void tick(std::uint64_t nowNs) override;
    [[nodiscard]] std::vector<InstrumentNotice> takeNotices() override;

    /// Bumped whenever something a remote copy mirrors changes -- the device,
    /// the antenna library, the assignments, the switchers -- so a server can
    /// tell when to send it again without comparing it.
    [[nodiscard]] std::uint64_t revision() const noexcept { return m_revision; }

private:
    /// What a learn takes from the output bus: at a fixed tune, every frame;
    /// while sweeping, completed passes measured through the LO-offset set.
    class LearnTap final : public IFrameConsumer {
    public:
        explicit LearnTap(LocalInstrument& owner) : m_owner(owner) {}
        void onFrame(const SpectrumFramePtr& frame) noexcept override;
        [[nodiscard]] std::string_view consumerName() const noexcept override {
            return "instrument-learning";
        }

    private:
        LocalInstrument& m_owner;
    };

    /// Republishes pipeline frames onto the output bus when not sweeping.
    ///
    /// In sweep mode the SweepEngine occupies this position, stitching steps
    /// into a whole-span frame. Fixed tune needs no stitching, but everything
    /// downstream still binds to the output bus -- so the two modes differ
    /// only in what sits between the buses, not in what consumers see.
    class DisplayForwarder final : public IFrameConsumer {
    public:
        explicit DisplayForwarder(FrameBus& target) : m_target(target) {}
        void onFrame(const SpectrumFramePtr& frame) noexcept override { m_target.publish(frame); }
        [[nodiscard]] std::string_view consumerName() const noexcept override {
            return "display-forwarder";
        }

    private:
        FrameBus& m_target;
    };

    struct LearnRun {
        /// Sweep: one pass for the floor and the LO-offset spurs, then
        /// several through them for whatever stands at a fixed frequency.
        /// Fixed tune: one phase, everything absolute.
        enum class Phase : std::uint8_t { LoOffsets, Absolute, Fixed };
        Phase phase = Phase::LoOffsets;
        CorrectionSettings savedSettings;
        CalibrationContext context;
        CorrectionLearner learner;
        FloorShape floor;
        std::vector<SpurEntry> loSpurs;
        std::uint64_t passesHandled = 0;
        /// The pass in progress when the LO-offset set went in; only a pass
        /// completed after it was measured through the set throughout.
        std::uint64_t installedAtPass = 0;
        /// The stitched grid's own learner, one frame per completed pass.
        CorrectionLearner gridLearner;
    };

    void notify(InstrumentNotice::Kind kind, std::string text);

    void adoptEffectivePlan();
    void reportUnroutedRanges();
    void refreshDescriptor();
    void sampleHealth();

    void reloadAntennas();
    void applyAntennaChange();
    void refreshSwitchers();
    void refreshSwitcherViews();

    [[nodiscard]] CalibrationContext currentContext() const;
    [[nodiscard]] CorrectionSettings effectiveCorrectionSettings() const noexcept;
    void pushCorrectionSettings();
    void loadCalibration();
    void installCorrections();
    void refreshFloorStaleness();

    void observeStep(const SpectrumFrame& frame) noexcept;
    void advanceLearning();
    void finishLearning(const std::vector<SpurEntry>& absoluteSpurs);
    void abortLearning();
    void beginAutoSpurs();
    void endAutoSpurs();
    void updateAutoSpurs();

    FrameBus& m_output;
    EventBus& m_events;
    Telemetry& m_telemetry;
    InstrumentPaths m_paths;
    IFftBackend* m_backend = nullptr;

    FrameBus m_pipelineBus; ///< raw per-step frames from the pipeline
    DisplayForwarder m_displayForwarder{m_output};
    FrameBus::SubscriptionId m_pipelineSubscription = 0;
    LearnTap m_learnTap{*this};
    FrameBus::SubscriptionId m_learnTapSubscription = 0;
    EventBus::SubscriptionId m_passSubscription = 0;

    std::unique_ptr<Pipeline> m_pipeline;
    std::unique_ptr<SweepEngine> m_sweepEngine;
    std::unique_ptr<ISdrDevice> m_device;
    std::optional<DeviceDescriptor> m_descriptor;

    PipelineConfig m_pipelineConfig;
    SweepPlan m_sweepPlan;
    ScheduleSummary m_schedule;
    bool m_sweeping = false;
    std::uint64_t m_startGeneration = 0;
    std::uint64_t m_revision = 0;

    AntennaLibrary m_antennas;
    AntennaAssignments m_assignments;
    /// Owned; `m_openPaths` borrows from these, so the two are only ever
    /// rebuilt together.
    std::vector<std::unique_ptr<IRfPath>> m_switchers;
    std::vector<OpenRfPath> m_openPaths;
    std::vector<SwitcherView> m_switcherViews;
    std::vector<RfPathInfo> m_availableSwitchers;

    /// What the last warning said, so a re-plan that changes nothing does not
    /// raise it again.
    std::vector<std::pair<double, double>> m_reportedUnroutedHz;

    std::vector<SdrHealthReading> m_health;
    std::uint64_t m_lastSlowTickNs = 0;

    std::vector<InstrumentNotice> m_notices;

    CorrectionSettings m_correctionSettings;
    std::optional<CorrectionSet> m_corrections;
    std::string m_floorStaleReason;

    /// Guards the two learners and `m_learn` itself, which the bus thread
    /// reads and the owner's thread replaces.
    mutable std::mutex m_learnMutex;
    std::optional<LearnRun> m_learn;
    std::optional<CorrectionLearner> m_autoLearner;
    std::uint64_t m_autoPassesHandled = 0;

    /// Sweep passes completed, counted on the sweep thread and polled on the
    /// owner's, so a phase change never runs on the thread that raised it.
    std::atomic<std::uint64_t> m_passesSeen{0};
};

} // namespace sweeppp
