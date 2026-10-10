// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/EventBus.hpp"
#include "sweeppp/core/Result.hpp"
#include "sweeppp/instrument/Instrument.hpp"
#include "sweeppp/pipeline/FrameBus.hpp"
#include "sweeppp/remote/Messages.hpp"
#include "sweeppp/remote/Protocol.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
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

/// How a desktop introduces itself to a server.
struct ClientIdentity {
    std::string name;     ///< This machine's name when empty
    std::string clientId; ///< Kept across runs, so a reconnect is known as one
    /// Reconnecting: the time of the last frame before the link went. Frames
    /// the server kept meanwhile are placed after it.
    std::uint64_t resumeAfterNs = 0;
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
            std::chrono::milliseconds timeout = std::chrono::seconds(5),
            const ClientIdentity& identity = {});

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

    /// What the instrument was doing when its link went on its own, for a
    /// reconnect to put back: the settable parameters, and whether it ran.
    struct LostState {
        std::vector<std::pair<std::string, SdrValue>> parameters;
        bool running = false;
    };
    [[nodiscard]] const LostState& lostState() const noexcept { return m_lost; }

    /// Drops the connection without a goodbye, as a network failure would:
    /// the next `tick()` finds the link gone. For tests of what follows.
    void abandon();

    [[nodiscard]] const RemoteEndpoint& endpoint() const noexcept { return m_endpoint; }

    /// The server's name for itself, which is what the window calls it.
    [[nodiscard]] const std::string& serverName() const noexcept { return m_serverName; }

    [[nodiscard]] LinkStats link() const noexcept { return m_link; }
    /// The server machine's CPU, memory and disk, once it has said.
    [[nodiscard]] const std::optional<HostStats>& serverHost() const noexcept {
        return m_serverHost;
    }

    // ---- sharing ------------------------------------------------------------

    /// Whether the server lets others watch alongside.
    [[nodiscard]] bool shared() const noexcept { return m_controlState.shared; }
    [[nodiscard]] const ControlState& control() const noexcept { return m_controlState; }
    /// Everyone connected, this desktop included.
    [[nodiscard]] const std::vector<ConnectedClient>& clients() const noexcept { return m_clients; }
    /// Gives control up, leaving the radio as it is for whoever takes it.
    void releaseControl();

    /// How many bins frames are reduced to before they cross the network;
    /// zero for whole. Set per connection, and lost with it.
    void setLinkResolution(std::uint32_t maxBins);
    [[nodiscard]] std::uint32_t linkResolution() const noexcept { return m_linkMaxBins; }

    // ---- recordings made on the server ------------------------------------

    [[nodiscard]] const ServerRecordings& recordings() const noexcept { return m_recordings; }

    /// Records on the server, at up to `maxBins` a line, completed passes
    /// only while sweeping.
    void startRecording(std::uint32_t maxBins);
    void stopRecording();
    void deleteRecording(const std::string& name);

    /// A recording coming over from the server.
    struct Download {
        std::string name;
        std::filesystem::path path; ///< Where it lands once complete
        std::uint64_t received = 0;
        std::uint64_t totalBytes = 0;
        bool done = false;
        std::string error;
    };

    /// Fetches `name` into `directory`, a few pieces in flight at a time,
    /// through `<name>.part` -- which a later download of the same name picks
    /// up from, after a dropped link.
    Status beginDownload(const std::string& name, const std::filesystem::path& directory);
    /// Stops it and deletes what had arrived.
    void cancelDownload(const std::string& name);
    [[nodiscard]] std::vector<Download> downloads() const;

    // ---- Instrument ------------------------------------------------------

    [[nodiscard]] std::string profileDriver() const override { return "remote"; }
    [[nodiscard]] std::string profileId() const override { return m_endpoint.address(); }
    [[nodiscard]] std::string displayLabel() const override;
    [[nodiscard]] std::string computeHost() const override { return m_serverName; }

    [[nodiscard]] bool canControl() const noexcept override { return m_controlState.you; }
    Status takeControl() override;

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
    Status startBenchmark(const FftBenchmarkConfig& config) override;
    void cancelBenchmark() override;
    [[nodiscard]] BenchmarkStatus benchmark() const override { return m_benchmark; }
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

    RemoteInstrument(RemoteEndpoint endpoint, EventBus& events);

    /// Refused with the reason while another client controls the server:
    /// nothing is changed here or asked of it.
    [[nodiscard]] Status mayChange() const;

    /// Queues `op`, holding the sections it touches until it is answered.
    /// Its sequence number, or zero when there is no link to send it on.
    std::uint64_t send(std::string_view op, sweeps::Metadata args);

    struct DownloadState;
    void requestMoreOf(DownloadState& download);
    void receiveChunk(const Chunk& chunk);
    void failDownload(DownloadState& download, std::string why);

    void applyState(std::uint64_t ackSeq, const sweeps::Metadata& sections);
    void applySection(std::string_view name, const sweeps::Metadata& body);
    void linkLost(std::string reason);

    RemoteEndpoint m_endpoint;
    EventBus& m_events;
    std::unique_ptr<Link> m_linkThreads;
    std::string m_serverName;
    std::string m_linkError;
    LostState m_lost;
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

    BenchmarkStatus m_benchmark;
    ServerRecordings m_recordings;
    /// Until the server says otherwise, this desktop is all there is.
    ControlState m_controlState{.you = true};
    std::vector<ConnectedClient> m_clients;
    std::vector<std::unique_ptr<DownloadState>> m_downloads;
    /// Which download each outstanding fetch belongs to, so a refused one
    /// fails the right download.
    std::map<std::uint64_t, std::string> m_fetches;
    std::vector<SdrHealthReading> m_health;
    TelemetrySnapshot m_engineTelemetry;
    std::optional<HostStats> m_serverHost;
    bool m_haveTelemetry = false;
    LinkStats m_link;
    std::uint32_t m_linkMaxBins = 0;
    std::uint64_t m_rateWindowNs = 0;
    std::uint64_t m_rateWindowBytes = 0;

    std::vector<InstrumentNotice> m_notices;
};

} // namespace sweeppp::remote
