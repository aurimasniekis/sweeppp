// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cstring>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sweeppp/core/BlockPool.hpp>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/core/EventBus.hpp>
#include <sweeppp/core/Log.hpp>
#include <sweeppp/core/Result.hpp>
#include <sweeppp/core/SpscRing.hpp>
#include <sweeppp/core/Telemetry.hpp>
#include <sweeppp/core/Version.hpp>
#include <system_error>
#include <thread>
#include <vector>

using namespace sweeppp;

TEST_CASE("Result carries a code and a message") {
    const Result<int> ok = 42;
    REQUIRE(ok.has_value());
    CHECK(*ok == 42);

    const Result<int> bad = fail<int>(ErrorCode::InvalidArgument, "size {} is not valid", 7);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code() == ErrorCode::InvalidArgument);
    CHECK(bad.error().message() == "size 7 is not valid");
    CHECK(bad.error().describe() == "InvalidArgument: size 7 is not valid");

    // Context prepends without losing the code -- a failure deep in the
    // session reader must still say which file it was reading.
    const Error contextual = bad.error().withContext("reading session.sweeps");
    CHECK(contextual.code() == ErrorCode::InvalidArgument);
    CHECK(contextual.message() == "reading session.sweeps: size 7 is not valid");
}

TEST_CASE("SpscRing preserves order and reports fullness") {
    SpscRing<int, 8> ring;

    CHECK(ring.empty());
    CHECK(ring.sizeApprox() == 0);

    for (int i = 0; i < 8; ++i) {
        CHECK(ring.push(i));
    }
    CHECK(ring.sizeApprox() == 8);
    CHECK(ring.fillFraction() == doctest::Approx(1.0F));

    // Full means push fails rather than blocks. On the acquisition path this
    // is the difference between dropping a block and stalling the USB thread.
    CHECK_FALSE(ring.push(99));

    for (int i = 0; i < 8; ++i) {
        int value = -1;
        REQUIRE(ring.pop(value));
        CHECK(value == i);
    }

    int drained = -1;
    CHECK_FALSE(ring.pop(drained));
    CHECK(ring.empty());
}

TEST_CASE("SpscRing survives a concurrent producer and consumer") {
    // Sized deliberately small relative to the item count so the ring fills
    // and empties many times, exercising both wrap paths.
    SpscRing<std::uint64_t, 64> ring;
    constexpr std::uint64_t kItems = 200'000;

    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> received{0};
    std::atomic<bool> orderViolated{false};

    std::jthread producer([&] {
        for (std::uint64_t i = 0; i < kItems; ++i) {
            while (!ring.push(i)) {
                // Retry rather than drop, so the consumer must see every value
                // exactly once and in order.
                std::this_thread::yield();
            }
        }
    });

    std::jthread consumer([&] {
        std::uint64_t expected = 0;
        std::uint64_t value = 0;
        while (expected < kItems) {
            if (ring.pop(value)) {
                if (value != expected) {
                    orderViolated.store(true);
                }
                ++expected;
                received.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    CHECK_FALSE(orderViolated.load());
    CHECK(received.load() == kItems);
    CHECK(dropped.load() == 0);
}

TEST_CASE("BlockPool hands out distinct, aligned, recyclable blocks") {
    auto pool = BlockPool::create(4096, 16);
    REQUIRE(pool.has_value());

    CHECK((*pool)->blockCount() == 16);
    CHECK((*pool)->blockBytes() >= 4096);
    CHECK((*pool)->inUse() == 0);

    std::vector<BlockRef> refs;
    std::vector<std::byte*> pointers;
    for (int i = 0; i < 16; ++i) {
        BlockRef ref = (*pool)->acquire();
        REQUIRE(ref.valid());
        // Cache-line aligned: two workers must never share a line.
        CHECK(reinterpret_cast<std::uintptr_t>(ref.data()) % kCacheLineSize == 0);
        pointers.push_back(ref.data());
        refs.push_back(std::move(ref));
    }

    CHECK((*pool)->inUse() == 16);
    CHECK((*pool)->utilisation() == doctest::Approx(1.0F));

    // Exhaustion is reported, not fatal, and is counted separately from a full
    // ring so the two failure modes stay distinguishable.
    const BlockRef overflow = (*pool)->acquire();
    CHECK_FALSE(overflow.valid());
    CHECK((*pool)->exhaustedCount() == 1);

    // All distinct.
    std::ranges::sort(pointers);
    CHECK(std::ranges::adjacent_find(pointers) == pointers.end());

    refs.clear();
    CHECK((*pool)->inUse() == 0);

    // Recycled rather than leaked.
    const BlockRef reacquired = (*pool)->acquire();
    CHECK(reacquired.valid());
}

TEST_CASE("BlockPool reference counting keeps a block alive until the last holder") {
    auto pool = BlockPool::create(256, 4);
    REQUIRE(pool.has_value());

    {
        BlockRef first = (*pool)->acquire();
        REQUIRE(first.valid());
        CHECK((*pool)->inUse() == 1);

        // A frame handed to several consumers shares one block; it must not be
        // recycled while any of them still holds it.
        BlockRef second = first;
        BlockRef third = first;
        CHECK((*pool)->inUse() == 1);
        CHECK(second.data() == first.data());
        CHECK(third.data() == first.data());

        second.reset();
        CHECK((*pool)->inUse() == 1);
    }

    CHECK((*pool)->inUse() == 0);
}

TEST_CASE("BlockPool is safe under concurrent acquire and release") {
    auto pool = BlockPool::create(512, 64);
    REQUIRE(pool.has_value());

    constexpr int kThreads = 8;
    constexpr int kIterations = 20'000;
    std::atomic<int> failures{0};

    std::vector<std::jthread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&pool, &failures] {
            for (int i = 0; i < kIterations; ++i) {
                BlockRef ref = (*pool)->acquire();
                if (!ref.valid()) {
                    continue; // legitimate transient exhaustion
                }
                // Write through the block so ASan/TSan see real accesses and
                // would flag two threads handed the same block.
                std::memset(ref.data(), i & 0xFF, ref.capacityBytes());
                if (ref.bytes().size() != (*pool)->blockBytes()) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    threads.clear();

    CHECK(failures.load() == 0);
    // Every block returned: the ABA-safe free list did not lose or duplicate
    // any index.
    CHECK((*pool)->inUse() == 0);

    std::vector<BlockRef> all;
    for (std::uint32_t i = 0; i < (*pool)->blockCount(); ++i) {
        BlockRef ref = (*pool)->acquire();
        CHECK(ref.valid());
        all.push_back(std::move(ref));
    }
    CHECK((*pool)->inUse() == (*pool)->blockCount());
}

TEST_CASE("BlockPool rejects impossible configurations") {
    CHECK_FALSE(BlockPool::create(0, 16).has_value());
    CHECK_FALSE(BlockPool::create(1024, 0).has_value());
}

TEST_CASE("EventBus delivers to typed subscribers only") {
    EventBus bus;

    int retunes = 0;
    double lastCenter = 0.0;
    int passes = 0;

    const auto retuneId = bus.subscribe<RetuneEvent>([&](const RetuneEvent& event) {
        ++retunes;
        lastCenter = event.centerHz;
    });
    bus.subscribe<SweepPassEvent>([&](const SweepPassEvent&) { ++passes; });

    bus.publish(RetuneEvent{.monotonicNs = 1, .centerHz = 2.4e9, .stepIndex = 3});
    CHECK(retunes == 1);
    CHECK(lastCenter == doctest::Approx(2.4e9));
    CHECK(passes == 0);

    bus.publish(SweepPassEvent{.monotonicNs = 2, .passId = 1});
    CHECK(retunes == 1);
    CHECK(passes == 1);

    CHECK(bus.subscriberCount() == 2);
    bus.unsubscribe(retuneId);
    CHECK(bus.subscriberCount() == 1);

    bus.publish(RetuneEvent{.monotonicNs = 3, .centerHz = 5.8e9});
    CHECK(retunes == 1);
}

TEST_CASE("publishing an event with no subscribers is harmless") {
    const EventBus bus;
    bus.publish(DeviceErrorEvent{.monotonicNs = 0, .deviceId = "x", .message = "y"});
    CHECK(bus.subscriberCount() == 0);
}

TEST_CASE("Telemetry turns counters into rates") {
    Telemetry telemetry;
    telemetry.reset();

    telemetry.stream().configuredSps.store(100e6);
    telemetry.stream().linkCapacityBytesPerSec.store(400'000'000);

    telemetry.sample(); // establish a baseline

    telemetry.stream().samplesDelivered.fetch_add(1'000'000);
    telemetry.stream().samplesDropped.fetch_add(250'000);
    telemetry.stream().bytesDelivered.fetch_add(2'000'000);
    telemetry.process().samplesProcessed.fetch_add(500'000);
    telemetry.process().fftsComputed.fetch_add(100);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const TelemetrySnapshot& snapshot = telemetry.sample();

    CHECK(snapshot.stream.samplesDelivered == 1'000'000);
    CHECK(snapshot.stream.samplesDropped == 250'000);
    CHECK(snapshot.stream.measuredSps > 0.0);

    // 250k discarded out of the 1M the radio produced.
    //
    // Not 1.25M: a dropped sample was delivered first, so counting it in the
    // denominator as well would count it twice and flatter the figure.
    CHECK(snapshot.stream.dropFraction == doctest::Approx(0.25).epsilon(1e-6));

    // Half of what arrived reached an FFT -- the "am I seeing everything?"
    // number.
    CHECK(snapshot.process.processedFraction == doctest::Approx(0.5).epsilon(1e-6));

    CHECK(snapshot.stream.linkUtilisation > 0.0);
    CHECK(telemetry.history().measuredSps.count() == 2);
}

TEST_CASE("samples never handed over are counted apart from samples discarded") {
    // The two failures the operator must be able to tell apart: the host threw
    // away what it was given, against the radio produced data the host could
    // not accept. Only the first is a subset of what was delivered, which is
    // what makes delivered == processed + dropped an identity rather than an
    // approximation.
    Telemetry telemetry;
    telemetry.reset();
    telemetry.sample();

    telemetry.stream().samplesDelivered.fetch_add(800'000);
    telemetry.stream().samplesDropped.fetch_add(200'000);
    telemetry.stream().samplesLostAtSource.fetch_add(200'000);
    telemetry.process().samplesProcessed.fetch_add(600'000);

    const TelemetrySnapshot& snapshot = telemetry.sample();

    CHECK(snapshot.stream.samplesLostAtSource == 200'000);

    // Everything delivered is either transformed or discarded, exactly.
    CHECK(snapshot.process.samplesProcessedTotal + snapshot.stream.samplesDropped ==
          snapshot.stream.samplesDelivered);

    // 400k lost of the 1M the radio produced.
    CHECK(snapshot.stream.dropFraction == doctest::Approx(0.4).epsilon(1e-6));
}

TEST_CASE("Telemetry reset clears run counters but keeps device description") {
    Telemetry telemetry;
    telemetry.stream().configuredSps.store(20e6);
    telemetry.stream().samplesDelivered.store(12345);

    telemetry.reset();

    CHECK(telemetry.stream().samplesDelivered.load() == 0);
    // The configured rate describes the radio, not the run.
    CHECK(telemetry.stream().configuredSps.load() == doctest::Approx(20e6));
    CHECK(telemetry.history().measuredSps.count() == 0);
}

TEST_CASE("LatencyTracker reports ordered percentiles") {
    LatencyTracker tracker;
    for (int i = 1; i <= 1000; ++i) {
        tracker.record(static_cast<float>(i));
    }

    const LatencyTracker::Percentiles percentiles = tracker.percentiles();
    CHECK(percentiles.p50 > 0.0F);
    CHECK(percentiles.p99 >= percentiles.p50);
    CHECK(percentiles.max >= percentiles.p99);
}

TEST_CASE("RollingHistory reads oldest-first after wrapping") {
    RollingHistory<4> history;
    for (int i = 0; i < 6; ++i) {
        history.push(static_cast<float>(i));
    }

    REQUIRE(history.count() == 4);
    // The four most recent values, in chronological order.
    CHECK(history.at(0) == doctest::Approx(2.0F));
    CHECK(history.at(3) == doctest::Approx(5.0F));
    CHECK(history.latest() == doctest::Approx(5.0F));
    CHECK(history.max() == doctest::Approx(5.0F));
    CHECK(history.mean() == doctest::Approx(3.5F));
}

TEST_CASE("throttle reasons all have names") {
    const ThrottleReason reasons[] = {
        ThrottleReason::None,     ThrottleReason::EveryNth,      ThrottleReason::CpuLimited,
        ThrottleReason::RingFull, ThrottleReason::PoolExhausted, ThrottleReason::DeviceOverrun,
        ThrottleReason::Stopped,
    };
    for (const ThrottleReason reason : reasons) {
        CHECK_FALSE(toString(reason).empty());
        CHECK(toString(reason) != "unknown");
    }
}

TEST_CASE("clock formatting is stable and readable") {
    CHECK(formatWallClockIso8601(0) == "1970-01-01T00:00:00Z");
    CHECK(formatWallClockCompact(0) == "19700101-000000");

    CHECK(formatDuration(0.000'001) == "1.0 us");
    CHECK(formatDuration(0.0125) == "12.50 ms");
    CHECK(formatDuration(1.5) == "1.500 s");
    CHECK(formatDuration(90.0) == "1:30");
    CHECK(formatDuration(3725.0) == "1:02:05");

    // Monotonic time never goes backwards.
    const std::uint64_t first = monotonicNs();
    const std::uint64_t second = monotonicNs();
    CHECK(second >= first);
}

namespace {

/// A directory that removes itself, so a rotation test leaves nothing behind
/// even when it fails.
class ScopedLogDir {
public:
    ScopedLogDir()
        : m_path(std::filesystem::temp_directory_path() /
                 ("sweeppp-log-" + std::to_string(monotonicNs()))) {
        std::filesystem::create_directories(m_path);
    }

    ~ScopedLogDir() {
        // Detached before the directory goes: the log is process-wide, and a
        // later test emitting into a deleted path is not a failure anyone
        // would enjoy diagnosing.
        Log::setLogFile("");
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    ScopedLogDir(const ScopedLogDir&) = delete;
    ScopedLogDir& operator=(const ScopedLogDir&) = delete;

    [[nodiscard]] std::filesystem::path file(const std::string& name) const {
        return m_path / name;
    }

private:
    std::filesystem::path m_path;
};

[[nodiscard]] std::string readAll(const std::filesystem::path& path) {
    const std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace

TEST_CASE("the log file rotates one generation when it is already too large") {
    const ScopedLogDir directory;
    const std::filesystem::path path = directory.file("sweeppp.log");
    const std::filesystem::path previous = directory.file("sweeppp.log.1");

    {
        std::ofstream out(path);
        out << std::string(4096, 'x') << '\n';
    }

    // Under the cap: the run continues the existing file rather than starting
    // a new one, which is what makes a log across a restart still readable.
    Log::setLogFile(path.string(), 1'000'000);
    Log::emit(LogLevel::Error, "test", "appended");
    Log::setLogFile("");

    CHECK_FALSE(std::filesystem::exists(previous));
    CHECK(readAll(path).find("appended") != std::string::npos);

    // Over it: the old file is moved aside intact and the new one starts empty.
    Log::setLogFile(path.string(), 1024);
    Log::emit(LogLevel::Error, "test", "after rotation");
    Log::setLogFile("");

    REQUIRE(std::filesystem::exists(previous));
    CHECK(readAll(previous).find("appended") != std::string::npos);

    const std::string rotated = readAll(path);
    CHECK(rotated.find("after rotation") != std::string::npos);
    CHECK(rotated.find("appended") == std::string::npos);

    // One generation, not a growing pile: rotating again overwrites .1 rather
    // than creating .2, and there is never a third file.
    Log::setLogFile(path.string(), 1);
    Log::emit(LogLevel::Error, "test", "third run");
    Log::setLogFile("");

    CHECK(readAll(previous).find("after rotation") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(directory.file("sweeppp.log.1.1")));
    CHECK_FALSE(std::filesystem::exists(directory.file("sweeppp.log.2")));
}

TEST_CASE("a log file that does not exist yet is created rather than rotated") {
    const ScopedLogDir directory;
    const std::filesystem::path path = directory.file("fresh.log");

    Log::setLogFile(path.string(), 16);
    Log::emit(LogLevel::Warn, "test", "first line");
    Log::setLogFile("");

    CHECK(readAll(path).find("first line") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(directory.file("fresh.log.1")));
}

TEST_CASE("the build string extends the version rather than replacing it") {
    // versionString() reaches session manifests and the plugin ABI's host
    // version check, so it must stay a bare version whatever the build knows
    // about itself. buildString() is the one a person is shown.
    const std::string_view version = versionString();
    const std::string_view build = buildString();

    REQUIRE_FALSE(version.empty());
    CHECK(build.starts_with(version));
    CHECK(version.find('+') == std::string_view::npos);

    // Either exactly the version -- no repository to ask -- or the version and
    // a suffix that begins with '+'. Never a dangling separator.
    if (build.size() > version.size()) {
        CHECK(build[version.size()] == '+');
        CHECK(build.size() > version.size() + 1);
    }

    CHECK_FALSE(buildDate().empty());
    CHECK_FALSE(buildCompiler().empty());
    CHECK_FALSE(buildPlatform().empty());
}
