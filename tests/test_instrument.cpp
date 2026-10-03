// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ReferenceFft.hpp"

#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <sweeppp/backends/sdr/SyntheticDevice.hpp>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/fft/FftBackendManager.hpp>
#include <sweeppp/instrument/LocalInstrument.hpp>
#include <sweeppp/sdr/ISdrDevice.hpp>
#include <thread>
#include <vector>

using namespace sweeppp;

namespace {

struct FrameCounter final : IFrameConsumer {
    void onFrame(const SpectrumFramePtr& frame) noexcept override {
        count.fetch_add(1, std::memory_order_relaxed);
        if (frame->passComplete) {
            passes.fetch_add(1, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::string_view consumerName() const noexcept override { return "counter"; }

    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> passes{0};
};

/// A synthetic radio behind a `LocalInstrument`, with its bench files in a
/// directory of its own so nothing reaches the real configuration.
class InstrumentFixture {
public:
    InstrumentFixture()
        : m_root(std::filesystem::temp_directory_path() /
                 std::format("sweeppp-instrument-{}", monotonicNs())) {
        registerReferenceFftBackend();
        registerBuiltinSdrDevices();
        auto backend = FftBackendManager::instance().acquire("reference");
        REQUIRE(backend.has_value());

        m_parameterSubscription = events.subscribe<ParameterChangedEvent>(
            [this](const ParameterChangedEvent& event) { parameterEvents.push_back(event); });
        m_counterSubscription = output.subscribe(&counter);

        instrument = std::make_unique<LocalInstrument>(output, events, telemetry,
                                                       InstrumentPaths::under(m_root), **backend);
    }

    ~InstrumentFixture() {
        instrument.reset();
        output.unsubscribe(m_counterSubscription);
        events.unsubscribe(m_parameterSubscription);
        std::error_code ec;
        std::filesystem::remove_all(m_root, ec);
    }

    InstrumentFixture(const InstrumentFixture&) = delete;
    InstrumentFixture& operator=(const InstrumentFixture&) = delete;
    InstrumentFixture(InstrumentFixture&&) = delete;
    InstrumentFixture& operator=(InstrumentFixture&&) = delete;

    void adoptSynthetic() const {
        auto device = SdrDeviceManager::instance().open("synthetic", "");
        REQUIRE(device.has_value());
        instrument->adoptDevice(std::move(*device));
    }

    /// A narrow plan the synthetic radio sweeps in a few milliseconds.
    [[nodiscard]] static SweepPlan quickPlan() {
        SweepPlan plan;
        plan.segments = {SweepSegment{.startHz = 100e6, .stopHz = 140e6}};
        plan.sampleRate = 8e6;
        plan.rbwHz = 100e3;
        plan.applyMode(SweepMode::Fast);
        plan.continuous = true;
        return plan;
    }

    /// Ticks the instrument, as its owner would, until `done` or a deadline.
    [[nodiscard]] bool tickUntil(const std::function<bool()>& done,
                                 std::chrono::seconds timeout = std::chrono::seconds(60)) const {
        const std::uint64_t deadline =
            monotonicNs() + secondsToNs(static_cast<double>(timeout.count()));
        while (!done()) {
            if (monotonicNs() > deadline) {
                return false;
            }
            instrument->tick(monotonicNs());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return m_root; }

    FrameBus output;
    EventBus events;
    Telemetry telemetry;
    FrameCounter counter;
    std::vector<ParameterChangedEvent> parameterEvents;
    std::unique_ptr<LocalInstrument> instrument;

private:
    std::filesystem::path m_root;
    FrameBus::SubscriptionId m_counterSubscription = 0;
    EventBus::SubscriptionId m_parameterSubscription = 0;
};

} // namespace

TEST_CASE("a radio is adopted to sweep its whole range at its fastest rate") {
    InstrumentFixture fixture;
    CHECK(fixture.instrument->device() == nullptr);
    CHECK_FALSE(fixture.instrument->start().has_value());

    fixture.adoptSynthetic();

    const DeviceDescriptor* device = fixture.instrument->device();
    REQUIRE(device != nullptr);
    CHECK(device->info.driver == "synthetic");
    CHECK_FALSE(device->parameters.empty());
    CHECK(fixture.instrument->profileDriver() == "synthetic");

    const SweepPlan& plan = fixture.instrument->sweepPlan();
    CHECK(fixture.instrument->sweeping());
    REQUIRE(plan.segments.size() == 1);
    CHECK(plan.segments.front().startHz == doctest::Approx(device->info.minFrequencyHz));
    CHECK(plan.segments.front().stopHz == doctest::Approx(device->info.maxFrequencyHz));
    CHECK(plan.sampleRate == doctest::Approx(122.88e6));
}

TEST_CASE("starting puts stitched passes on the output bus") {
    InstrumentFixture fixture;
    fixture.adoptSynthetic();
    REQUIRE(fixture.instrument->applySweepPlan(InstrumentFixture::quickPlan()).has_value());
    CHECK(fixture.instrument->schedule().stepCount > 0);

    const std::uint64_t generation = fixture.instrument->startGeneration();
    REQUIRE(fixture.instrument->start().has_value());
    CHECK(fixture.instrument->running());
    CHECK(fixture.instrument->startGeneration() == generation + 1);

    CHECK(fixture.tickUntil([&] { return fixture.counter.passes.load() >= 2; }));
    CHECK(fixture.instrument->engineStats().passCount >= 2);

    fixture.instrument->stop();
    CHECK_FALSE(fixture.instrument->running());
}

TEST_CASE("a plan applied while running restarts acquisition on it") {
    InstrumentFixture fixture;
    fixture.adoptSynthetic();
    REQUIRE(fixture.instrument->applySweepPlan(InstrumentFixture::quickPlan()).has_value());
    REQUIRE(fixture.instrument->start().has_value());
    const std::uint64_t generation = fixture.instrument->startGeneration();

    SweepPlan wider = InstrumentFixture::quickPlan();
    wider.segments = {SweepSegment{.startHz = 200e6, .stopHz = 260e6}};
    REQUIRE(fixture.instrument->applySweepPlan(wider).has_value());

    CHECK(fixture.instrument->running());
    CHECK(fixture.instrument->startGeneration() == generation + 1);
    CHECK(fixture.instrument->sweepPlan().lowestHz() == doctest::Approx(200e6));
    CHECK(fixture.instrument->schedule().gridStartHz == doctest::Approx(200e6).epsilon(0.01));
    fixture.instrument->stop();
}

TEST_CASE("an invalid plan is kept for the editor but never reaches the radio") {
    InstrumentFixture fixture;
    fixture.adoptSynthetic();
    REQUIRE(fixture.instrument->applySweepPlan(InstrumentFixture::quickPlan()).has_value());
    const std::size_t steps = fixture.instrument->schedule().stepCount;

    SweepPlan broken = InstrumentFixture::quickPlan();
    broken.segments = {SweepSegment{.startHz = 140e6, .stopHz = 100e6}};
    CHECK_FALSE(fixture.instrument->applySweepPlan(broken).has_value());
    CHECK(fixture.instrument->sweepPlan().segments.front().startHz == doctest::Approx(140e6));
    CHECK(fixture.instrument->schedule().stepCount == steps);
}

TEST_CASE("a parameter change is published once, and the sample rate goes to the plan") {
    InstrumentFixture fixture;
    fixture.adoptSynthetic();
    REQUIRE(fixture.instrument->applySweepPlan(InstrumentFixture::quickPlan()).has_value());

    REQUIRE(fixture.instrument->setDeviceParameter("gain", SdrValue{std::int64_t{30}}).has_value());
    REQUIRE(fixture.parameterEvents.size() == 1);
    CHECK(fixture.parameterEvents.back().key == "gain");
    CHECK(fixture.parameterEvents.back().calibrationAffecting);
    CHECK(asInt(fixture.instrument->parameter("gain").value_or(SdrValue{std::int64_t{0}})) == 30);

    REQUIRE(fixture.instrument->setDeviceParameter("sample_rate", SdrValue{10e6}).has_value());
    CHECK(fixture.parameterEvents.size() == 2);
    CHECK(fixture.instrument->sweepPlan().sampleRate == doctest::Approx(10e6));

    CHECK_FALSE(fixture.instrument->setDeviceParameter("no_such_key", SdrValue{true}).has_value());
    CHECK(fixture.parameterEvents.size() == 2);
}

TEST_CASE("switching to a backend that does not exist leaves the current one") {
    InstrumentFixture fixture;
    fixture.adoptSynthetic();
    const std::string before = fixture.instrument->fftBackendName();
    CHECK_FALSE(fixture.instrument->setFftBackend("no-such-backend").has_value());
    CHECK(fixture.instrument->fftBackendName() == before);
    CHECK(fixture.instrument->setFftBackend(before).has_value());
    CHECK_FALSE(fixture.instrument->fftBackends().empty());
}

TEST_CASE("a fixed-tune learn writes its file under the instrument's own directory") {
    InstrumentFixture fixture;
    fixture.adoptSynthetic();
    REQUIRE(fixture.instrument->setSweeping(false).has_value());
    CHECK_FALSE(fixture.instrument->startLearning().has_value());

    REQUIRE(fixture.instrument->start().has_value());
    REQUIRE(fixture.instrument->startLearning().has_value());
    CHECK(fixture.instrument->learning());

    REQUIRE(fixture.tickUntil([&] { return !fixture.instrument->learning(); }));

    const std::vector<InstrumentNotice> notices = fixture.instrument->takeNotices();
    CHECK(std::ranges::any_of(notices, [](const InstrumentNotice& notice) {
        return notice.kind == InstrumentNotice::Kind::Success;
    }));

    const CorrectionSummary learned = fixture.instrument->correctionSummary();
    CHECK(learned.present);
    CHECK(learned.floorPoints > 0);
    CHECK(fixture.instrument->correctionSettings().flatten);

    const auto calibration = fixture.root() / "calibration";
    REQUIRE(std::filesystem::exists(calibration));
    CHECK_FALSE(std::filesystem::is_empty(calibration));

    fixture.instrument->clearCorrections();
    CHECK_FALSE(fixture.instrument->correctionSummary().present);
    CHECK(std::filesystem::is_empty(calibration));
    fixture.instrument->stop();
}

TEST_CASE("antenna and assignment edits are written and read back") {
    InstrumentFixture fixture;
    fixture.adoptSynthetic();
    const std::uint64_t revision = fixture.instrument->revision();

    Antenna discone{.id = "discone",
                    .name = "Test discone",
                    .category = "Wideband",
                    .type = "discone",
                    .startHz = 25e6,
                    .stopHz = 1.3e9};
    REQUIRE(fixture.instrument->setUserAntennas({discone}).has_value());
    CHECK(std::filesystem::exists(fixture.root() / "antennas" / "custom.toml"));
    const Antenna* found = fixture.instrument->antennas().find("discone");
    REQUIRE(found != nullptr);
    CHECK_FALSE(found->builtin);
    CHECK(fixture.instrument->revision() > revision);

    AntennaAssignments assignments = fixture.instrument->antennaAssignments();
    const std::string device = fixture.instrument->deviceAntennaKey();
    const std::string port = fixture.instrument->device()->rxPorts.front().id;
    assignments.assign(device, port, "discone");
    REQUIRE(fixture.instrument->setAntennaAssignments(assignments).has_value());
    CHECK(std::filesystem::exists(fixture.root() / "antennas" / "assignments.toml"));

    const Antenna* onPort = fixture.instrument->antennaOnPort(port);
    REQUIRE(onPort != nullptr);
    CHECK(onPort->id == "discone");
    CHECK_FALSE(fixture.instrument->antennaCoverage().empty());

    const std::vector<RfLegView> legs = fixture.instrument->rfPath();
    REQUIRE_FALSE(legs.empty());
    CHECK(legs.front().antenna.id == "discone");
    CHECK(legs.front().live);
}

TEST_CASE("closing the radio clears what was known about it") {
    InstrumentFixture fixture;
    fixture.adoptSynthetic();
    REQUIRE(fixture.instrument->applySweepPlan(InstrumentFixture::quickPlan()).has_value());
    REQUIRE(fixture.instrument->start().has_value());

    fixture.instrument->closeDevice();
    CHECK_FALSE(fixture.instrument->running());
    CHECK(fixture.instrument->device() == nullptr);
    CHECK(fixture.instrument->parameter("gain") == std::nullopt);
    CHECK(fixture.instrument->health().empty());
}
