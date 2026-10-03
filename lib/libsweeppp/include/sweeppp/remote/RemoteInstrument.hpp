// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/EventBus.hpp"
#include "sweeppp/core/Result.hpp"
#include "sweeppp/instrument/Instrument.hpp"
#include "sweeppp/pipeline/FrameBus.hpp"
#include "sweeppp/remote/Protocol.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sweeps/Metadata.hpp>
#include <vector>

namespace sweeppp::remote {

/// Where a server is, and what it takes to be let in.
struct RemoteEndpoint {
    std::string host;
    std::uint16_t port = kDefaultPort;
    std::string token;

    /// "pi.local:7332", "[fe80::1]:7332": what a profile stores.
    [[nodiscard]] std::string address() const;

    /// "host", "host:port" or "[v6]:port"; the port defaults.
    [[nodiscard]] static Result<RemoteEndpoint> parse(std::string_view address);
};

/// An instrument on another machine, served by `sweeppp-cli serve`.
///
/// Everything is read from a copy of the server's state that the link keeps
/// up to date. An edit lands in the copy at once and goes to the server as a
/// command; the server's word on that part of the state is taken again only
/// once it has acknowledged the command, so a late update cannot undo an edit
/// the operator just made. A command the server refuses comes back as a
/// notice.
///
/// Frames and events arrive on the link's own thread and go straight to the
/// buses, timed on this machine's clocks.
class RemoteInstrument final : public Instrument {
public:
    /// Connects, authenticates and waits for the server's state, for up to
    /// `timeout`. Blocks: call it off the UI thread. Nothing reaches the buses
    /// until `begin()`.
    [[nodiscard]] static Result<std::unique_ptr<RemoteInstrument>>
    connect(const RemoteEndpoint& endpoint, FrameBus& output, EventBus& events,
            std::chrono::milliseconds timeout = std::chrono::seconds(5));

    ~RemoteInstrument() override;

    RemoteInstrument(const RemoteInstrument&) = delete;
    RemoteInstrument& operator=(const RemoteInstrument&) = delete;
    RemoteInstrument(RemoteInstrument&&) = delete;
    RemoteInstrument& operator=(RemoteInstrument&&) = delete;

    /// Starts the link: frames and events flow from here on. Once, on the
    /// owner's thread.
    void begin();

    /// Says goodbye and closes the link. The instrument then reads as having
    /// no radio.
    void disconnect();

    /// False once the link has gone, by `disconnect()` or otherwise. A link
    /// that drops is noticed by `tick()`, which raises the notice and the
    /// closed event before this turns false.
    [[nodiscard]] bool linkUp() const noexcept;

    /// Why the link went, when it went on its own.
    [[nodiscard]] const std::string& linkError() const noexcept { return m_linkError; }

    [[nodiscard]] const RemoteEndpoint& endpoint() const noexcept { return m_endpoint; }

    /// The server's name for itself, which is what the window calls it.
    [[nodiscard]] const std::string& serverName() const noexcept { return m_serverName; }

    [[nodiscard]] LinkStats link() const noexcept { return m_link; }

    // ---- Instrument ------------------------------------------------------

    [[nodiscard]] std::string profileDriver() const override { return "remote"; }
    [[nodiscard]] std::string profileId() const override { return m_endpoint.address(); }
    [[nodiscard]] std::string displayLabel() const override;
    [[nodiscard]] bool canBenchmark() const noexcept override { return false; }

    [[nodiscard]] const DeviceDescriptor* device() const noexcept override;
    [[nodiscard]] std::optional<SdrValue> parameter(std::string_view key) const override;
    [[nodiscard]] std::string selectedRxPort() const override { return m_selectedRxPort; }
    Status setDeviceParameter(const std::string& key, const SdrValue& value) override;
    [[nodiscard]] bool parameterNeedsStop(const SdrParameter& parameter) const noexcept override;
    void closeDevice() override { disconnect(); }
    [[nodiscard]] std::vector<SdrHealthReading> health() const override { return m_health; }

    Status start() override;
    void stop() override;
    [[nodiscard]] bool running() const noexcept override { return m_running; }
    Status restart() override;
    [[nodiscard]] std::uint64_t startGeneration() const noexcept override {
        return m_startGeneration;
    }

    [[nodiscard]] bool sweeping() const noexcept override { return m_sweeping; }
    Status setSweeping(bool enabled) override;
    Status applySweepPlan(const SweepPlan& plan) override;
    Status sweepRange(const SweepPlan& plan) override;
    [[nodiscard]] const SweepPlan& sweepPlan() const noexcept override { return m_plan; }
    [[nodiscard]] const ScheduleSummary& schedule() const noexcept override { return m_schedule; }
    [[nodiscard]] EngineStats engineStats() const override { return m_engine; }

    Status applyPipelineConfig(const PipelineConfig& config) override;
    [[nodiscard]] const PipelineConfig& pipelineConfig() const noexcept override {
        return m_pipeline;
    }

    [[nodiscard]] std::vector<FftBackendInfo> fftBackends() const override { return m_backends; }
    [[nodiscard]] std::string fftBackendName() const override { return m_backendName; }
    Status setFftBackend(std::string_view name) override;

    [[nodiscard]] const CorrectionSettings& correctionSettings() const noexcept override {
        return m_correctionSettings;
    }
    void setCorrectionSettings(const CorrectionSettings& settings) override;
    [[nodiscard]] CorrectionSummary correctionSummary() const override {
        return m_correctionSummary;
    }
    Status startLearning() override;
    [[nodiscard]] bool learning() const override { return m_learning; }
    [[nodiscard]] std::string learningLabel() const override { return m_learningLabel; }
    void cancelLearning() override;
    void clearAutoSpurs() override;
    void clearCorrections() override;

    [[nodiscard]] const AntennaLibrary& antennas() const noexcept override { return m_antennas; }
    Status setUserAntennas(std::vector<Antenna> entries) override;
    [[nodiscard]] const AntennaAssignments& antennaAssignments() const noexcept override {
        return m_assignments;
    }
    Status setAntennaAssignments(AntennaAssignments assignments) override;
    [[nodiscard]] std::string deviceAntennaKey() const override { return m_antennaKey; }
    [[nodiscard]] const Antenna* antennaOnPort(std::string_view portId) const override;
    [[nodiscard]] std::vector<RfLegView> rfPath() const override { return m_rfLegs; }
    [[nodiscard]] std::vector<std::pair<double, double>> antennaCoverage() const override {
        return m_coverage;
    }
    [[nodiscard]] std::span<const SwitcherView> openSwitchers() const noexcept override {
        return m_switchers;
    }
    [[nodiscard]] const SwitcherView* switcher(std::string_view key) const override;
    [[nodiscard]] std::span<const RfPathInfo> availableSwitchers() const noexcept override {
        return m_availableSwitchers;
    }
    void rescanSwitchers() override;

    [[nodiscard]] const TelemetrySnapshot* engineTelemetry() const noexcept override;
    void resetTelemetry() override;
    void tick(std::uint64_t nowNs) override;
    [[nodiscard]] std::vector<InstrumentNotice> takeNotices() override;

private:
    struct Link;

    RemoteInstrument(RemoteEndpoint endpoint, FrameBus& output, EventBus& events);

    /// Queues `op`, holding the sections it touches until it is answered.
    void send(std::string_view op, sweeps::Metadata args);

    void applyState(std::uint64_t ackSeq, const sweeps::Metadata& sections);
    void applySection(std::string_view name, const sweeps::Metadata& body);
    void linkLost(std::string reason);

    RemoteEndpoint m_endpoint;
    FrameBus& m_output;
    EventBus& m_events;
    std::unique_ptr<Link> m_linkThreads;
    std::string m_serverName;
    std::string m_linkError;
    bool m_closed = false;

    std::uint64_t m_seq = 0;
    std::map<std::string, std::uint64_t, std::less<>> m_pending;

    std::optional<DeviceDescriptor> m_device;
    std::string m_deviceLabel;
    std::map<std::string, SdrValue, std::less<>> m_values;
    std::string m_selectedRxPort;

    bool m_running = false;
    bool m_sweeping = false;
    std::uint64_t m_startGeneration = 0;
    EngineStats m_engine;
    SweepPlan m_plan;
    ScheduleSummary m_schedule;
    PipelineConfig m_pipeline;
    std::vector<FftBackendInfo> m_backends;
    std::string m_backendName;

    CorrectionSettings m_correctionSettings;
    CorrectionSummary m_correctionSummary;
    bool m_learning = false;
    std::string m_learningLabel;

    AntennaLibrary m_antennas;
    AntennaAssignments m_assignments;
    std::string m_antennaKey;
    std::vector<SwitcherView> m_switchers;
    std::vector<RfPathInfo> m_availableSwitchers;
    std::vector<RfLegView> m_rfLegs;
    std::vector<std::pair<double, double>> m_coverage;

    std::vector<SdrHealthReading> m_health;
    TelemetrySnapshot m_engineTelemetry;
    bool m_haveTelemetry = false;
    LinkStats m_link;
    std::uint64_t m_rateWindowNs = 0;
    std::uint64_t m_rateWindowBytes = 0;

    std::vector<InstrumentNotice> m_notices;
};

} // namespace sweeppp::remote
