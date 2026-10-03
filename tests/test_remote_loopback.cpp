// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ReferenceFft.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <sweeppp/backends/sdr/SyntheticDevice.hpp>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/fft/FftBackendManager.hpp>
#include <sweeppp/instrument/LocalInstrument.hpp>
#include <sweeppp/remote/RemoteInstrument.hpp>
#include <sweeppp/remote/RemoteServer.hpp>
#include <sweeppp/remote/ServerList.hpp>
#include <sweeppp/sdr/ISdrDevice.hpp>
#include <thread>
#include <vector>

using namespace sweeppp;
using namespace sweeppp::remote;
using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

struct FrameCounter final : IFrameConsumer {
    void onFrame(const SpectrumFramePtr& frame) noexcept override {
        const std::lock_guard lock(mutex);
        ++frames;
        passes += frame->passComplete ? 1 : 0;
        bins = frame->binCount();
        startHz = frame->startHz;
        lastHostNs = frame->hostTimeNs;
    }
    [[nodiscard]] std::string_view consumerName() const noexcept override { return "counter"; }

    std::mutex mutex;
    std::uint64_t frames = 0;
    std::uint64_t passes = 0;
    std::size_t bins = 0;
    double startHz = 0.0;
    std::uint64_t lastHostNs = 0;
};

/// A synthetic radio behind a server on an ephemeral port, and a desktop's
/// buses to connect a remote instrument to.
class Loopback {
public:
    Loopback()
        : m_root(std::filesystem::temp_directory_path() /
                 std::format("sweeppp-loopback-{}", monotonicNs())) {
        registerReferenceFftBackend();
        registerBuiltinSdrDevices();
        auto backend = FftBackendManager::instance().acquire("reference");
        REQUIRE(backend.has_value());
        local = std::make_unique<LocalInstrument>(serverOutput, serverEvents, serverTelemetry,
                                                  InstrumentPaths::under(m_root), **backend);
        auto device = SdrDeviceManager::instance().open("synthetic", "");
        REQUIRE(device.has_value());
        local->adoptDevice(std::move(*device));
        REQUIRE(local->applySweepPlan(quickPlan()).has_value());

        server = std::make_unique<RemoteServer>(
            *local, serverOutput, serverEvents, serverTelemetry,
            ServerConfig{.port = 0, .token = "secret", .serverName = "bench"});
        REQUIRE(server->start().has_value());
        counterSubscription = output.subscribe(&counter);
    }

    ~Loopback() {
        remote.reset();
        output.unsubscribe(counterSubscription);
        server.reset();
        local.reset();
        std::error_code ec;
        std::filesystem::remove_all(m_root, ec);
    }

    Loopback(const Loopback&) = delete;
    Loopback& operator=(const Loopback&) = delete;
    Loopback(Loopback&&) = delete;
    Loopback& operator=(Loopback&&) = delete;

    static SweepPlan quickPlan() {
        SweepPlan plan;
        plan.segments = {SweepSegment{.startHz = 100e6, .stopHz = 140e6}};
        plan.sampleRate = 8e6;
        plan.rbwHz = 100e3;
        plan.applyMode(SweepMode::Fast);
        return plan;
    }

    [[nodiscard]] RemoteEndpoint endpoint() const {
        return RemoteEndpoint{.host = "127.0.0.1", .port = server->port(), .token = "secret"};
    }

    void connect() {
        auto connected = RemoteInstrument::connect(endpoint(), output, events, 3000ms);
        REQUIRE(connected.has_value());
        remote = std::move(*connected);
        remote->begin();
    }

    /// Ticks the remote instrument, as the UI would, until `done`.
    bool tickUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout = 5000ms) {
        const auto deadline = Clock::now() + timeout;
        while (!done()) {
            if (Clock::now() > deadline) {
                return false;
            }
            if (remote) {
                remote->tick(monotonicNs());
            }
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

    void tickFor(std::chrono::milliseconds duration) {
        (void)tickUntil([] { return false; }, duration);
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return m_root; }

    // The server's side.
    FrameBus serverOutput;
    EventBus serverEvents;
    Telemetry serverTelemetry;
    std::unique_ptr<LocalInstrument> local;
    std::unique_ptr<RemoteServer> server;

    // The desktop's side.
    FrameBus output;
    EventBus events;
    FrameCounter counter;
    FrameBus::SubscriptionId counterSubscription = 0;
    std::unique_ptr<RemoteInstrument> remote;

private:
    std::filesystem::path m_root;
};

} // namespace

TEST_CASE("an endpoint is read and written as host and port") {
    auto plain = RemoteEndpoint::parse("pi.local:7400");
    REQUIRE(plain.has_value());
    CHECK(plain->host == "pi.local");
    CHECK(plain->port == 7400);
    CHECK(plain->address() == "pi.local:7400");

    auto bare = RemoteEndpoint::parse("pi.local");
    REQUIRE(bare.has_value());
    CHECK(bare->port == kDefaultPort);

    auto v6 = RemoteEndpoint::parse("[fe80::1]:7332");
    REQUIRE(v6.has_value());
    CHECK(v6->host == "fe80::1");
    CHECK(v6->address() == "[fe80::1]:7332");

    auto bareV6 = RemoteEndpoint::parse("::1");
    REQUIRE(bareV6.has_value());
    CHECK(bareV6->host == "::1");

    CHECK_FALSE(RemoteEndpoint::parse("").has_value());
    CHECK_FALSE(RemoteEndpoint::parse("host:0").has_value());
    CHECK_FALSE(RemoteEndpoint::parse("host:70000").has_value());
    CHECK_FALSE(RemoteEndpoint::parse("host:abc").has_value());
    CHECK_FALSE(RemoteEndpoint::parse("[::1").has_value());
}

TEST_CASE("a remote radio arrives with the server's state") {
    Loopback loop;
    loop.connect();
    RemoteInstrument& remote = *loop.remote;

    CHECK(remote.linkUp());
    REQUIRE(remote.device() != nullptr);
    CHECK(remote.device()->info.driver == "synthetic");
    CHECK(remote.profileDriver() == "remote");
    CHECK(remote.profileId() == loop.endpoint().address());
    CHECK(remote.displayLabel().ends_with(" on bench"));
    CHECK_FALSE(remote.canBenchmark());
    CHECK(remote.sweeping());
    CHECK(remote.sweepPlan().lowestHz() == doctest::Approx(100e6));
    CHECK(remote.schedule().stepCount > 0);
    CHECK(remote.parameter("gain").has_value());
    CHECK_FALSE(remote.fftBackends().empty());
    CHECK(remote.fftBackendName() == "reference");
}

TEST_CASE("frames arrive on the server's grid, timed on this machine's clock") {
    Loopback loop;
    loop.connect();
    std::vector<std::uint64_t> passTimes;
    std::mutex passMutex;
    const auto subscription = loop.events.subscribe<SweepPassEvent>([&](const SweepPassEvent& e) {
        const std::lock_guard lock(passMutex);
        passTimes.push_back(e.monotonicNs);
    });

    REQUIRE(loop.remote->start().has_value());
    REQUIRE(loop.tickUntil([&] {
        const std::lock_guard lock(loop.counter.mutex);
        return loop.counter.passes >= 5;
    }));
    CHECK(loop.tickUntil([&] { return loop.remote->running(); }));
    CHECK(loop.remote->startGeneration() > 0);
    loop.tickFor(300ms);

    {
        const std::lock_guard lock(loop.counter.mutex);
        CHECK(loop.counter.bins == loop.remote->schedule().gridBinCount);
        CHECK(loop.counter.startHz == doctest::Approx(loop.remote->schedule().gridStartHz));
        CHECK(loop.counter.lastHostNs <= monotonicNs());
    }
    {
        const std::lock_guard lock(passMutex);
        REQUIRE(passTimes.size() >= 2);
        CHECK(std::ranges::is_sorted(passTimes));
        CHECK(passTimes.back() <= monotonicNs());
    }
    CHECK(loop.remote->engineTelemetry() != nullptr);
    CHECK(loop.remote->link().bytesReceived > 0);
    loop.events.unsubscribe(subscription);
}

TEST_CASE("a parameter set remotely is reported here exactly once") {
    Loopback loop;
    loop.connect();
    std::atomic<int> changes{0};
    const auto subscription =
        loop.events.subscribe<ParameterChangedEvent>([&](const ParameterChangedEvent& e) {
            if (e.key == "gain") {
                changes.fetch_add(1);
            }
        });

    REQUIRE(loop.remote->setDeviceParameter("gain", SdrValue{std::int64_t{18}}).has_value());
    CHECK(asInt(*loop.remote->parameter("gain")) == 18);
    CHECK(loop.tickUntil([&] { return changes.load() >= 1; }));
    loop.tickFor(300ms);
    CHECK(changes.load() == 1);
    CHECK(asInt(*loop.local->parameter("gain")) == 18);
    loop.events.unsubscribe(subscription);
}

TEST_CASE("an edit the server changes or refuses is put right here") {
    Loopback loop;
    loop.connect();

    SUBCASE("refused: reported, and gone again") {
        REQUIRE(loop.remote->setDeviceParameter("no_such_key", SdrValue{true}).has_value());
        CHECK(loop.remote->parameter("no_such_key").has_value());

        std::vector<InstrumentNotice> notices;
        CHECK(loop.tickUntil([&] {
            for (InstrumentNotice& notice : loop.remote->takeNotices()) {
                notices.push_back(std::move(notice));
            }
            return !notices.empty();
        }));
        REQUIRE_FALSE(notices.empty());
        CHECK(notices.front().kind == InstrumentNotice::Kind::Error);
        CHECK(loop.tickUntil([&] { return !loop.remote->parameter("no_such_key").has_value(); }));
    }

    SUBCASE("clamped: what the radio took replaces what was asked") {
        REQUIRE(loop.remote->setDeviceParameter("gain", SdrValue{std::int64_t{9999}}).has_value());
        CHECK(asInt(*loop.remote->parameter("gain")) == 9999);
        CHECK(loop.tickUntil([&] { return asInt(*loop.remote->parameter("gain")) != 9999; }));
        CHECK(asInt(*loop.remote->parameter("gain")) == asInt(*loop.local->parameter("gain")));
    }
}

TEST_CASE("a burst of plans settles on the last") {
    Loopback loop;
    loop.connect();
    for (int i = 0; i < 30; ++i) {
        SweepPlan plan = Loopback::quickPlan();
        plan.segments = {SweepSegment{.startHz = 300e6 + (i * 1e6), .stopHz = 340e6 + (i * 1e6)}};
        REQUIRE(loop.remote->applySweepPlan(plan).has_value());
    }
    CHECK(loop.remote->sweepPlan().lowestHz() == doctest::Approx(329e6));
    loop.tickFor(500ms);
    CHECK(loop.remote->sweepPlan().lowestHz() == doctest::Approx(329e6));
    CHECK(loop.local->sweepPlan().lowestHz() == doctest::Approx(329e6));
    CHECK(loop.remote->schedule().gridStartHz == doctest::Approx(329e6).epsilon(0.01));
}

TEST_CASE("bench edits made here are kept on the server") {
    Loopback loop;
    loop.connect();
    const Antenna whip{.id = "whip", .name = "Whip", .startHz = 100e6, .stopHz = 1e9};
    REQUIRE(loop.remote->setUserAntennas({whip}).has_value());
    REQUIRE(loop.remote->antennas().find("whip") != nullptr);

    AntennaAssignments assignments = loop.remote->antennaAssignments();
    const std::string port = loop.remote->device()->rxPorts.front().id;
    assignments.assign(loop.remote->deviceAntennaKey(), port, "whip");
    REQUIRE(loop.remote->setAntennaAssignments(assignments).has_value());

    CHECK(loop.tickUntil([&] {
        const Antenna* onPort = loop.remote->antennaOnPort(port);
        return onPort != nullptr && onPort->id == "whip" && !loop.remote->rfPath().empty();
    }));
    loop.tickFor(200ms);
    CHECK(std::filesystem::exists(loop.root() / "antennas" / "custom.toml"));
    CHECK(std::filesystem::exists(loop.root() / "antennas" / "assignments.toml"));
    CHECK(loop.local->antennaOnPort(port) != nullptr);
    CHECK(std::ranges::any_of(loop.remote->antennas().entries(), [](const Antenna& a) {
              return a.builtin;
          }) == std::ranges::any_of(loop.local->antennas().entries(), [](const Antenna& a) {
              return a.builtin;
          }));
}

TEST_CASE("a learn run on the server is summarised here") {
    Loopback loop;
    loop.connect();
    REQUIRE(loop.remote->setSweeping(false).has_value());
    REQUIRE(loop.remote->start().has_value());
    REQUIRE(loop.tickUntil([&] { return !loop.remote->sweeping(); }));
    REQUIRE(loop.remote->startLearning().has_value());
    CHECK(loop.tickUntil([&] { return loop.remote->correctionSummary().present; }, 30000ms));
    CHECK(loop.remote->correctionSummary().floorPoints > 0);
    CHECK_FALSE(loop.remote->learning());
}

TEST_CASE("the link going down closes the radio and says so") {
    Loopback loop;
    loop.connect();
    std::atomic<int> closed{0};
    const auto subscription =
        loop.events.subscribe<DeviceClosedEvent>([&](const DeviceClosedEvent&) { closed++; });
    REQUIRE(loop.remote->start().has_value());
    loop.tickFor(200ms);

    loop.server->stop();
    CHECK(loop.tickUntil([&] { return !loop.remote->linkUp(); }));
    CHECK(loop.remote->device() == nullptr);
    CHECK_FALSE(loop.remote->running());
    CHECK(closed.load() == 1);
    const std::vector<InstrumentNotice> notices = loop.remote->takeNotices();
    CHECK(std::ranges::any_of(notices, [](const InstrumentNotice& notice) {
        return notice.kind == InstrumentNotice::Kind::Condition &&
               notice.text.find("shut down") != std::string::npos;
    }));
    loop.events.unsubscribe(subscription);
}

TEST_CASE("disconnecting frees the server for the next desktop") {
    Loopback loop;
    loop.connect();

    FrameBus otherOutput;
    EventBus otherEvents;
    auto refused = RemoteInstrument::connect(loop.endpoint(), otherOutput, otherEvents, 3000ms);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code() == ErrorCode::Unavailable);

    RemoteEndpoint wrong = loop.endpoint();
    wrong.token = "guess";
    auto denied = RemoteInstrument::connect(wrong, otherOutput, otherEvents, 3000ms);
    REQUIRE_FALSE(denied.has_value());
    CHECK(denied.error().code() == ErrorCode::PermissionDenied);

    loop.remote->disconnect();
    CHECK_FALSE(loop.remote->linkUp());
    REQUIRE(Loopback::quickPlan().validate().has_value());
    const auto deadline = Clock::now() + 5s;
    while (loop.server->clientConnected() && Clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    auto next = RemoteInstrument::connect(loop.endpoint(), otherOutput, otherEvents, 3000ms);
    REQUIRE(next.has_value());
    CHECK((*next)->device() != nullptr);
}

// A report rather than a check, and seconds of full load: run it by name with
// --no-skip.
TEST_CASE("throughput over loopback at bladeRF-class settings" * doctest::skip()) {
    Loopback loop;
    loop.connect();

    SweepPlan wide;
    wide.segments = {SweepSegment{.startHz = 70e6, .stopHz = 6e9}};
    wide.sampleRate = 61.44e6;
    wide.rbwHz = 5625.0;
    wide.applyMode(SweepMode::Fast);
    REQUIRE(loop.remote->sweepRange(wide).has_value());
    REQUIRE(loop.tickUntil([&] { return loop.remote->schedule().fftSize >= 16384; }));
    REQUIRE(loop.remote->start().has_value());

    const std::uint64_t startNs = monotonicNs();
    loop.tickFor(3000ms);
    const double seconds = nsToSeconds(monotonicNs() - startNs);
    const LinkStats link = loop.remote->link();
    std::uint64_t frames = 0;
    std::uint64_t passes = 0;
    {
        const std::lock_guard lock(loop.counter.mutex);
        frames = loop.counter.frames;
        passes = loop.counter.passes;
    }

    MESSAGE(std::format(
        "{} bins, FFT {}, {} steps: {:.1f} MB/s, {:.1f} frames/s, {:.1f} passes/s; "
        "server coalesced {} passes and {} partials, sent {} frames",
        loop.remote->schedule().gridBinCount, loop.remote->schedule().fftSize,
        loop.remote->schedule().stepCount, static_cast<double>(link.bytesReceived) / seconds / 1e6,
        static_cast<double>(frames) / seconds, static_cast<double>(passes) / seconds,
        link.passesCoalesced, link.partialsCoalesced, link.framesSent));
    CHECK(frames > 0);
}

TEST_CASE("saved servers keep their tokens, privately") {
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       std::format("sweeppp-servers-{}.toml", monotonicNs());

    ServerList list;
    list.put(SavedServer{.name = "Roof Pi",
                         .endpoint = RemoteEndpoint{.host = "pi.local", .token = "s3cret"}});
    list.put(SavedServer{.name = "Lab", .endpoint = RemoteEndpoint{.host = "::1", .port = 7400}});
    list.put(SavedServer{.name = "Roof Pi (renamed)",
                         .endpoint = RemoteEndpoint{.host = "pi.local", .token = "n3w"}});
    REQUIRE(list.save(path).has_value());

    const ServerList back = ServerList::load(path);
    REQUIRE(back.entries().size() == 2);
    const SavedServer* roof = back.find("pi.local:7332");
    REQUIRE(roof != nullptr);
    CHECK(roof->name == "Roof Pi (renamed)");
    CHECK(roof->endpoint.token == "n3w");
    const SavedServer* lab = back.find("[::1]:7400");
    REQUIRE(lab != nullptr);
    CHECK(lab->endpoint.token.empty());

#if !defined(_WIN32)
    const auto perms = std::filesystem::status(path).permissions();
    CHECK((perms & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
          std::filesystem::perms::none);
#endif

    ServerList edited = back;
    edited.remove("pi.local:7332");
    CHECK(edited.entries().size() == 1);
    CHECK(ServerList::load(path.string() + ".missing").entries().empty());
    std::filesystem::remove(path);
}
