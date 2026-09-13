// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "GoldenSession.hpp"

#include <algorithm>
#include <atomic>
#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/core/EventBus.hpp>
#include <sweeppp/fft/Window.hpp>
#include <sweeppp/history/IFrameSource.hpp>
#include <sweeppp/history/SessionReader.hpp>
#include <sweeppp/history/SessionRecorder.hpp>
#include <sweeppp/pipeline/FrameBus.hpp>
#include <thread>

using namespace sweeppp;
using namespace sweeppp::session;

// The format itself is tested in lib/libsweepsfile/tests/, against the library
// directly. What remains here is Sweep++'s glue -- the recorder's threading and
// replay -- both of which libsweepsfile deliberately does not contain.

namespace {

class ScopedTempDir {
public:
    ScopedTempDir()
        : m_path(std::filesystem::temp_directory_path() /
                 std::format("sweeppp-session-{}", monotonicNs())) {
        std::filesystem::create_directories(m_path);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    [[nodiscard]] std::filesystem::path file(std::string_view name) const { return m_path / name; }

private:
    std::filesystem::path m_path;
};

SpectrumFramePtr makeFrame(std::uint64_t sequence, std::uint64_t timeNs, double startHz,
                           double binWidthHz, std::size_t bins, float floorDb,
                           std::size_t burstBin = SIZE_MAX, float burstDb = 0.0F) {
    auto frame = std::make_shared<SpectrumFrame>();
    frame->sequence = sequence;
    frame->hostTimeNs = timeNs;
    frame->wallTimeNs = timeNs;
    frame->startHz = startHz;
    frame->binWidthHz = binWidthHz;
    frame->binsDbfs.assign(bins, floorDb);

    if (burstBin < bins) {
        frame->binsDbfs[burstBin] = burstDb;
    }

    frame->config.centerHz = startHz + binWidthHz * static_cast<double>(bins) * 0.5;
    frame->config.spanHz = binWidthHz * static_cast<double>(bins);
    frame->config.sampleRate = 20e6;
    frame->config.fftSize = 4096;
    frame->config.window = WindowType::Hann;
    frame->config.windowEnbw = 1.5;
    frame->config.rbwHz = 20e6 * 1.5 / 4096.0;
    frame->config.deviceId = "test-0";
    frame->config.deviceLabel = "Test device";
    return frame;
}

struct RecordedSession {
    std::uint64_t firstNs = 0;
    std::uint64_t lastNs = 0;
    std::uint64_t burstNs = 0;
};

/// Records a session through the recorder, which is what these tests are about.
RecordedSession recordSession(const std::filesystem::path& path, std::uint32_t lines,
                              bool injectBurst) {
    RecorderConfig config;
    config.binsPerLine = 2048;

    auto recorder = SessionRecorder::create(path, config);
    REQUIRE(recorder.has_value());

    RecordedSession written;
    constexpr std::uint64_t kLineIntervalNs = 20'000'000; // 50 lines/s
    const std::uint64_t start = 1'000'000'000;
    const std::uint32_t burstLine = lines / 2;

    for (std::uint32_t i = 0; i < lines; ++i) {
        const std::uint64_t timeNs = start + i * kLineIntervalNs;
        const bool burst = injectBurst && i == burstLine;
        if (burst) {
            written.burstNs = timeNs;
        }

        (*recorder)->onFrame(
            makeFrame(i + 1, timeNs, 90e6, 9765.625, 2048, -90.0F, burst ? 900 : SIZE_MAX, -20.0F));
        if (i == 0) {
            written.firstNs = timeNs;
        }
        written.lastNs = timeNs;

        // The recorder is intentionally drop-aware: it sheds frames rather than
        // blocking the publisher. A test feeding it flat out would therefore
        // lose most of them and prove nothing, so it is paced here. A live
        // recorder is paced by the frame rate instead.
        if ((i % 64) == 63) {
            (*recorder)->flush();
        }
    }

    (*recorder)->flush();
    REQUIRE((*recorder)->close().has_value());
    REQUIRE((*recorder)->droppedFrames() == 0);
    return written;
}

} // namespace

TEST_CASE("events published from another thread never touch the writer") {
    // The shape of a live sweep, and the line this test holds.
    //
    // The sweep thread publishes a retune event per step -- thousands a second
    // on a wide plan -- while the recorder's own thread is writing tiles. If
    // both reached std::ofstream directly, two threads driving one filebuf would
    // walk its put pointers past the end of its buffer: a heap overflow that
    // surfaces anywhere but here, as a corrupted allocation somewhere else
    // entirely. Events are queued and serialised by the recorder thread instead.
    //
    // It is also why the library's writer is synchronous and owns no queue:
    // this behaviour is the application's, and building it into the library
    // would mean putting threading there too.
    const ScopedTempDir temp;
    const std::filesystem::path path = temp.file("concurrent-events.sweeps");

    constexpr int kEvents = 4000;

    {
        // Declared first so it outlives the recorder: the recorder unsubscribes
        // from the bus as it is destroyed.
        EventBus events;

        RecorderConfig config;
        config.binsPerLine = 1024;
        auto recorder = SessionRecorder::create(path, config);
        REQUIRE(recorder.has_value());

        (*recorder)->attachEvents(events);

        std::atomic<bool> go{false};

        std::thread publisher([&events, &go] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kEvents; ++i) {
                events.publish(
                    RetuneEvent{.monotonicNs = 1'000'000'000 + static_cast<std::uint64_t>(i) * 1000,
                                .centerHz = 100e6 + i,
                                .stepIndex = static_cast<std::uint32_t>(i)});
            }
        });

        go.store(true, std::memory_order_release);

        for (std::uint32_t i = 0; i < 400; ++i) {
            (*recorder)->onFrame(
                makeFrame(i + 1, 1'000'000'000 + i * 20'000'000, 90e6, 9765.625, 2048, -90.0F));
        }

        publisher.join();
        (*recorder)->flush();
        REQUIRE((*recorder)->close().has_value());
    }

    // Intact rather than merely surviving: a file damaged by the race reads
    // back short or fails its record checksums, and verify() checks every one.
    auto reader = SessionReader::open(path);
    REQUIRE(reader.has_value());
    CHECK((*reader)->verify().has_value());

    const auto retunes = std::ranges::count_if((*reader)->events(), [](const SessionEvent& event) {
        return event.kindEnum() == SessionEvent::Kind::Retune;
    });
    CHECK(retunes > 0);
    CHECK(retunes <= kEvents);
}

TEST_CASE("a parameter change is recorded with both of its flags") {
    // The two flags are independent and both have to survive the trip from bus
    // to file. Losing one is silent: nothing downstream has anywhere to notice a
    // parameter change that arrives claiming it left the grid alone.
    const ScopedTempDir temp;
    const std::filesystem::path path = temp.file("parameter-flags.sweeps");

    {
        EventBus events;

        RecorderConfig config;
        config.binsPerLine = 1024;
        auto recorder = SessionRecorder::create(path, config);
        REQUIRE(recorder.has_value());
        (*recorder)->attachEvents(events);

        events.publish(ParameterChangedEvent{.monotonicNs = 1'000'000'000,
                                             .key = "fft_size",
                                             .value = "8192",
                                             .gridAffecting = true,
                                             .calibrationAffecting = false});
        events.publish(ParameterChangedEvent{.monotonicNs = 1'000'000'001,
                                             .key = "lna_gain",
                                             .value = "24",
                                             .gridAffecting = false,
                                             .calibrationAffecting = true});

        for (std::uint32_t i = 0; i < 40; ++i) {
            (*recorder)->onFrame(
                makeFrame(i + 1, 1'000'000'000 + i * 20'000'000, 90e6, 9765.625, 1024, -90.0F));
        }
        (*recorder)->flush();
        REQUIRE((*recorder)->close().has_value());
    }

    auto reader = SessionReader::open(path);
    REQUIRE(reader.has_value());

    const auto find = [&reader](std::string_view key) -> const ParameterChangedData* {
        for (const SessionEvent& event : (*reader)->events()) {
            const auto* body = event.as<ParameterChangedData>();
            if (body != nullptr && body->key == key) {
                return body;
            }
        }
        return nullptr;
    };

    const ParameterChangedData* fftSize = find("fft_size");
    REQUIRE(fftSize != nullptr);
    CHECK(fftSize->value == "8192");
    CHECK(fftSize->gridAffecting);
    CHECK_FALSE(fftSize->calibrationAffecting);

    const ParameterChangedData* gain = find("lna_gain");
    REQUIRE(gain != nullptr);
    CHECK(gain->value == "24");
    CHECK_FALSE(gain->gridAffecting);
    CHECK(gain->calibrationAffecting);
}

TEST_CASE("replay reproduces the session onto a frame bus") {
    const ScopedTempDir temp;
    const std::filesystem::path path = temp.file("replay.sweeps");

    const RecordedSession written = recordSession(path, 400, true);

    FrameBus bus;
    EventBus events;

    struct Collector final : IFrameConsumer {
        void onFrame(const SpectrumFramePtr& frame) noexcept override {
            const std::lock_guard lock(mutex);
            frames.push_back(frame);
        }
        [[nodiscard]] std::string_view consumerName() const noexcept override { return "collect"; }

        mutable std::mutex mutex;
        std::vector<SpectrumFramePtr> frames;
    } collector;

    bus.subscribe(&collector);

    auto replay = SessionReplay::open(path, bus, events);
    REQUIRE(replay.has_value());

    CHECK((*replay)->seekable());
    CHECK((*replay)->durationSeconds() > 0.0);

    // Fast-forward so the test does not run in real time.
    (*replay)->setSpeed(16.0);

    std::atomic<bool> complete{false};
    (*replay)->setCompletionCallback([&complete] { complete.store(true); });

    REQUIRE((*replay)->start().has_value());

    const std::uint64_t deadline = monotonicNs() + 10'000'000'000ULL;
    while (!complete.load() && monotonicNs() < deadline && (*replay)->running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    (*replay)->stop();

    const std::lock_guard lock(collector.mutex);
    REQUIRE_FALSE(collector.frames.empty());

    // A replayed frame carries the recorded configuration, which is exactly
    // why consumers cannot tell replay from live.
    const SpectrumFramePtr& first = collector.frames.front();
    CHECK(first->config.fftSize == 4096);
    CHECK(first->config.window == WindowType::Hann);
    CHECK(first->startHz == doctest::Approx(90e6));
    CHECK(first->binCount() == 2048);

    // Time moves forward.
    for (std::size_t i = 1; i < collector.frames.size(); ++i) {
        CHECK(collector.frames[i]->hostTimeNs >= collector.frames[i - 1]->hostTimeNs);
    }

    CHECK(written.burstNs > 0);
}

TEST_CASE("replay republishes a recorded event with its fields intact") {
    // The other half of what makes replay faithful: frames reproduce the
    // waterfall, events reproduce everything around it. Round-tripping a marker
    // through the file and back onto a live bus is what says a body survives
    // both directions with each field still in its own place.
    const ScopedTempDir temp;
    const std::filesystem::path path = temp.file("replay-events.sweeps");

    constexpr std::uint64_t kFirstNs = 1'000'000'000;
    constexpr double kMarkerHz = 433.92e6;
    constexpr double kMarkerDbm = -42.25;

    {
        EventBus recording;

        RecorderConfig config;
        config.binsPerLine = 1024;
        auto recorder = SessionRecorder::create(path, config);
        REQUIRE(recorder.has_value());
        (*recorder)->attachEvents(recording);

        (*recorder)->onFrame(makeFrame(1, kFirstNs, 90e6, 9765.625, 1024, -90.0F));
        (*recorder)->flush();

        recording.publish(MarkerEvent{.monotonicNs = kFirstNs + 20'000'000,
                                      .label = "birdie",
                                      .frequencyHz = kMarkerHz,
                                      .levelDbm = kMarkerDbm});

        for (std::uint32_t i = 1; i < 200; ++i) {
            (*recorder)->onFrame(
                makeFrame(i + 1, kFirstNs + i * 20'000'000, 90e6, 9765.625, 1024, -90.0F));
        }
        (*recorder)->flush();
        REQUIRE((*recorder)->close().has_value());
    }

    FrameBus bus;
    EventBus events;

    std::mutex mutex;
    std::vector<MarkerEvent> seen;
    events.subscribe<MarkerEvent>([&mutex, &seen](const MarkerEvent& event) {
        const std::lock_guard lock(mutex);
        seen.push_back(event);
    });

    auto replay = SessionReplay::open(path, bus, events);
    REQUIRE(replay.has_value());
    (*replay)->setSpeed(16.0);

    std::atomic<bool> complete{false};
    (*replay)->setCompletionCallback([&complete] { complete.store(true); });
    REQUIRE((*replay)->start().has_value());

    const std::uint64_t deadline = monotonicNs() + 10'000'000'000ULL;
    while (!complete.load() && monotonicNs() < deadline && (*replay)->running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    (*replay)->stop();

    const std::lock_guard lock(mutex);
    REQUIRE_FALSE(seen.empty());
    CHECK(seen.front().label == "birdie");
    CHECK(seen.front().frequencyHz == doctest::Approx(kMarkerHz));
    CHECK(seen.front().levelDbm == doctest::Approx(kMarkerDbm));
}

TEST_CASE("replay transport controls behave") {
    const ScopedTempDir temp;
    const std::filesystem::path path = temp.file("transport.sweeps");
    recordSession(path, 500, false);

    FrameBus bus;
    EventBus events;
    auto replay = SessionReplay::open(path, bus, events);
    REQUIRE(replay.has_value());

    // Speed is clamped to the documented range rather than accepted blindly.
    (*replay)->setSpeed(100.0);
    CHECK((*replay)->speed() == doctest::Approx(SessionReplay::kMaxSpeed));
    (*replay)->setSpeed(0.001);
    CHECK((*replay)->speed() == doctest::Approx(SessionReplay::kMinSpeed));
    (*replay)->setSpeed(2.0);
    CHECK((*replay)->speed() == doctest::Approx(2.0));

    (*replay)->setPaused(true);
    CHECK((*replay)->paused());
    (*replay)->setPaused(false);
    CHECK_FALSE((*replay)->paused());

    REQUIRE((*replay)->start().has_value());
    (*replay)->setPaused(true);
    (*replay)->seek(2.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK((*replay)->positionSeconds() == doctest::Approx(2.0).epsilon(0.5));
    (*replay)->stop();
}
