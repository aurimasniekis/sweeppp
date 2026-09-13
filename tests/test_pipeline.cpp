// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ReferenceFft.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <doctest/doctest.h>
#include <numeric>
#include <sweeppp/backends/sdr/SyntheticDevice.hpp>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/core/EventBus.hpp>
#include <sweeppp/core/MpmcQueue.hpp>
#include <sweeppp/core/Telemetry.hpp>
#include <sweeppp/core/Toml.hpp>
#include <sweeppp/dsp/Convert.hpp>
#include <sweeppp/fft/FftBackendManager.hpp>
#include <sweeppp/pipeline/AsyncFrameConsumer.hpp>
#include <sweeppp/pipeline/FrameBus.hpp>
#include <sweeppp/pipeline/Pipeline.hpp>
#include <sweeppp/sdr/ISdrDevice.hpp>
#include <thread>
#include <vector>

using namespace sweeppp;

namespace {

/// Records everything it is given, so a test can assert on order and content.
class RecordingConsumer final : public IFrameConsumer {
public:
    void onFrame(const SpectrumFramePtr& frame) noexcept override {
        const std::lock_guard lock(m_mutex);
        if (!m_sequences.empty() && frame->sequence <= m_sequences.back()) {
            m_outOfOrder = true;
        }
        m_sequences.push_back(frame->sequence);
        m_last = frame;
    }

    [[nodiscard]] std::string_view consumerName() const noexcept override { return "recording"; }

    [[nodiscard]] std::vector<std::uint64_t> sequences() const {
        const std::lock_guard lock(m_mutex);
        return m_sequences;
    }

    [[nodiscard]] bool outOfOrder() const {
        const std::lock_guard lock(m_mutex);
        return m_outOfOrder;
    }

    [[nodiscard]] SpectrumFramePtr last() const {
        const std::lock_guard lock(m_mutex);
        return m_last;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<std::uint64_t> m_sequences;
    SpectrumFramePtr m_last;
    bool m_outOfOrder = false;
};

class SlowAsyncConsumer final : public AsyncFrameConsumer {
public:
    explicit SlowAsyncConsumer(std::chrono::milliseconds delay)
        : AsyncFrameConsumer("slow", 4), m_delay(delay) {
        startWorker();
    }

    ~SlowAsyncConsumer() override { shutdown(); }

protected:
    void processFrame(const SpectrumFramePtr&) override { std::this_thread::sleep_for(m_delay); }

private:
    std::chrono::milliseconds m_delay;
};

/// A radio whose blocks are too short for one transform.
///
/// What a device overrun actually produces: libbladeRF reports fewer samples
/// than were asked for, and the remainder of that read falls below one FFT.
/// Nothing in the synthetic device can produce a short block, which is why the
/// accounting leak this shape used to cause went unseen until a real radio
/// overran at 122 MS/s.
class ShortBlockDevice final : public ISdrDevice {
public:
    explicit ShortBlockDevice(std::size_t framesPerBlock) : m_frames(framesPerBlock) {}

    ~ShortBlockDevice() override { ShortBlockDevice::stop(); }

    [[nodiscard]] const SdrDeviceInfo& info() const noexcept override { return m_info; }
    [[nodiscard]] std::span<const SdrParameter> parameters() const noexcept override { return {}; }
    [[nodiscard]] Result<SdrValue> getParameter(std::string_view key) const override {
        return fail<SdrValue>(ErrorCode::NotFound, "no parameter '{}'", key);
    }
    [[nodiscard]] Status setParameter(std::string_view key, const SdrValue&) override {
        return fail(ErrorCode::NotFound, "no parameter '{}'", key);
    }
    [[nodiscard]] SampleFormat nativeFormat() const noexcept override { return SampleFormat::Cs16; }
    [[nodiscard]] Status retune(double) override { return ok(); }
    [[nodiscard]] bool streaming() const noexcept override { return m_streaming.load(); }

    [[nodiscard]] Status start(BlockPool& pool, const StreamConfig&, IqCallback callback) override {
        m_streaming.store(true);
        m_thread = std::jthread(
            [this, &pool, callback = std::move(callback)](const std::stop_token& stop) {
                std::uint64_t sequence = 0;
                while (!stop.stop_requested() && sequence < 64) {
                    BlockRef ref = pool.acquire();
                    if (!ref.valid()) {
                        std::this_thread::yield();
                        continue;
                    }
                    IqBlock block;
                    block.block = std::move(ref);
                    block.frames = m_frames;
                    block.format = SampleFormat::Cs16;
                    block.sequence = sequence++;
                    block.hostTimeNs = monotonicNs();
                    block.centerHz = 100e6;
                    block.sampleRate = 2e6;
                    callback(std::move(block));
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
        return ok();
    }

    void stop() override {
        if (!m_streaming.exchange(false)) {
            return;
        }
        m_thread.request_stop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

private:
    SdrDeviceInfo m_info{.driver = "short", .id = "short-0", .label = "Short block device"};
    std::size_t m_frames;
    std::atomic<bool> m_streaming{false};
    std::jthread m_thread;
};

SpectrumFramePtr makeFrame(std::uint64_t sequence) {
    auto frame = std::make_shared<SpectrumFrame>();
    frame->sequence = sequence;
    frame->hostTimeNs = sequence * 1'000'000;
    frame->binsDbfs.assign(16, -90.0F);
    return frame;
}

} // namespace

// ------------------------------------------------------------ MpmcQueue

TEST_CASE("MpmcQueue preserves FIFO order with one producer and one consumer") {
    MpmcQueue<int, 8> queue;

    CHECK(queue.empty());
    for (int i = 0; i < 8; ++i) {
        CHECK(queue.push(int{i}));
    }
    CHECK_FALSE(queue.push(99)); // full: reports rather than blocks

    for (int i = 0; i < 8; ++i) {
        int value = -1;
        REQUIRE(queue.pop(value));
        CHECK(value == i);
    }

    int drained = -1;
    CHECK_FALSE(queue.pop(drained));
}

TEST_CASE("MpmcQueue delivers every item exactly once to many consumers") {
    // The bug this guards against is real and was hit in development: an SPSC
    // ring popped by N worker threads silently hands the same slot to two of
    // them and corrupts its own indices.
    MpmcQueue<std::uint64_t, 64> queue;
    constexpr std::uint64_t kItems = 100'000;
    constexpr int kConsumers = 6;

    std::atomic<std::uint64_t> sum{0};
    std::atomic<std::uint64_t> received{0};
    std::atomic<bool> producerDone{false};

    std::vector<std::jthread> consumers;
    consumers.reserve(kConsumers);
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            std::uint64_t value = 0;
            while (true) {
                if (queue.pop(value)) {
                    sum.fetch_add(value, std::memory_order_relaxed);
                    received.fetch_add(1, std::memory_order_relaxed);
                } else if (producerDone.load(std::memory_order_acquire)) {
                    if (!queue.pop(value)) {
                        break;
                    }
                    sum.fetch_add(value, std::memory_order_relaxed);
                    received.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (std::uint64_t i = 1; i <= kItems; ++i) {
        while (!queue.push(std::uint64_t{i})) {
            std::this_thread::yield();
        }
    }
    producerDone.store(true, std::memory_order_release);
    consumers.clear();

    CHECK(received.load() == kItems);
    // Exactly once: a duplicate or a loss would move the sum.
    CHECK(sum.load() == kItems * (kItems + 1) / 2);
}

// ---------------------------------------------------------- SdrParameter

TEST_CASE("SdrValue maps onto TOML's scalar types") {
    // The four alternatives exist precisely so profiles need no encoding
    // tricks. If one ever needed a string wrapper, hand-edited profiles would
    // stop being readable.
    CHECK(asBool(SdrValue{true}));
    CHECK(asInt(SdrValue{std::int64_t{42}}) == 42);
    CHECK(asDouble(SdrValue{2.4e9}) == doctest::Approx(2.4e9));
    CHECK(asString(SdrValue{std::string{"RX1"}}) == "RX1");

    // A string that reads as a frequency still converts, which is what lets a
    // profile say center = "2.4 GHz".
    CHECK(asDouble(SdrValue{std::string{"2.4 GHz"}}) == doctest::Approx(2.4e9));
}

TEST_CASE("parameter coercion snaps to the hardware's real steps") {
    const SdrParameter lna{.key = "lna_gain",
                           .label = "LNA gain",
                           .group = "Gain",
                           .type = SdrParameterType::Int,
                           .unit = "dB",
                           .min = 0.0,
                           .max = 40.0,
                           .step = 8.0};

    // Rounds to nearest, not down: typing 25 into an 8 dB-step field should
    // land on 24, and truncation would make every typed value read low.
    CHECK(asInt(lna.coerce(SdrValue{std::int64_t{25}}).value()) == 24);
    CHECK(asInt(lna.coerce(SdrValue{std::int64_t{27}}).value()) == 24);
    CHECK(asInt(lna.coerce(SdrValue{std::int64_t{29}}).value()) == 32);

    // Clamped to range rather than rejected.
    CHECK(asInt(lna.coerce(SdrValue{std::int64_t{-10}}).value()) == 0);
    CHECK(asInt(lna.coerce(SdrValue{std::int64_t{999}}).value()) == 40);

    CHECK(lna.format(SdrValue{std::int64_t{24}}) == "24 dB");
}

TEST_CASE("an invalid enum value lists what is accepted") {
    const SdrParameter antenna{
        .key = "antenna",
        .label = "Antenna",
        .group = "Tuning",
        .type = SdrParameterType::Enum,
        .enumValues = {{.value = "rx1", .label = "RX1"}, {.value = "rx2", .label = "RX2"}}};

    CHECK(asString(antenna.coerce(SdrValue{std::string{"rx2"}}).value()) == "rx2");
    CHECK(antenna.format(SdrValue{std::string{"rx2"}}) == "RX2");

    const auto bad = antenna.coerce(SdrValue{std::string{"rx9"}});
    REQUIRE_FALSE(bad.has_value());
    // The message must turn the error into a fix.
    CHECK(bad.error().message().find("rx1") != std::string::npos);
    CHECK(bad.error().message().find("rx2") != std::string::npos);
}

TEST_CASE("a stepped rate ladder spans the device's own range") {
    // What a radio with a continuous range offers the panel: its endpoints,
    // which are the two rates most likely to be wanted, and a step between.
    const std::vector<double> rates = steppedSampleRates(520'834.0, 61.44e6, 2e6);

    REQUIRE(rates.size() >= 3);
    CHECK(rates.front() == doctest::Approx(520'834.0));
    CHECK(rates.back() == doctest::Approx(61.44e6));
    CHECK(std::ranges::is_sorted(rates));

    // Strictly ascending: an endpoint landing on the step must not be listed
    // twice, which is what a plain 2 MHz loop plus both ends would do.
    CHECK(std::ranges::adjacent_find(rates) == rates.end());

    const std::vector<double> aligned = steppedSampleRates(2e6, 20e6, 2e6);
    CHECK(aligned.front() == doctest::Approx(2e6));
    CHECK(aligned.back() == doctest::Approx(20e6));
    CHECK(std::ranges::adjacent_find(aligned) == aligned.end());

    // A range that is not one yields nothing rather than a ladder of one.
    CHECK(steppedSampleRates(20e6, 20e6, 2e6).empty());
    CHECK(steppedSampleRates(0.0, 20e6, 2e6).empty());
    CHECK(steppedSampleRates(2e6, 20e6, 0.0).empty());
}

TEST_CASE("sample-rate presets read the way an operator says them") {
    const std::vector<SdrEnumValue> choices =
        sampleRateChoices(std::array{520'834.0, 2e6, 12.5e6, 61.44e6});

    REQUIRE(choices.size() == 4);
    CHECK(choices[0].label == "520.834 kS/s");
    CHECK(choices[1].label == "2 MS/s");
    CHECK(choices[2].label == "12.5 MS/s");
    CHECK(choices[3].label == "61.44 MS/s");

    // The stored form round-trips the double exactly, so a preset and the same
    // figure typed into --sample-rate are the same bits and compare equal.
    constexpr std::array kSource{520'834.0, 2e6, 12.5e6, 61.44e6};
    for (std::size_t i = 0; i < choices.size(); ++i) {
        CAPTURE(i);
        const auto parsed = toml_util::parseFrequency(choices[i].value);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == kSource[i]);
    }
}

TEST_CASE("numeric presets constrain the widget, never the value") {
    // The panel draws a dropdown for these, but they are presets and not a
    // constraint: every radio here accepts a continuous range and reports back
    // what it actually tuned to, so a rate from --sample-rate or restored from
    // a profile has to survive.
    const SdrParameter rate{.key = "sample_rate",
                            .label = "Sample rate",
                            .group = "Tuning",
                            .type = SdrParameterType::Double,
                            .unit = "S/s",
                            .min = 1e6,
                            .max = 61.44e6,
                            .step = 0.0,
                            .enumValues = sampleRateChoices(std::array{2e6, 20e6, 61.44e6})};

    // On the list, and off it -- both come back untouched.
    CHECK(asDouble(rate.coerce(SdrValue{20e6}).value()) == doctest::Approx(20e6));
    CHECK(asDouble(rate.coerce(SdrValue{17.3e6}).value()) == doctest::Approx(17.3e6));

    // The range still applies; presets do not replace it.
    CHECK(asDouble(rate.coerce(SdrValue{99e6}).value()) == doctest::Approx(61.44e6));

    // A value matching a preset reads back as its label, and one that does not
    // reads in the same notation rather than as a bare count of samples -- the
    // dropdown must not change units when an exact figure is typed.
    CHECK(rate.format(SdrValue{20e6}) == "20 MS/s");
    CHECK(rate.format(SdrValue{17.3e6}) == "17.3 MS/s");
}

TEST_CASE("re-applying an identical value is recognised as a no-op") {
    // During a sweep the same gain is written on every step; treating those as
    // changes would swamp the event stream and the session file.
    CHECK(equals(SdrValue{2.4e9}, SdrValue{2.4e9}));
    CHECK_FALSE(equals(SdrValue{2.4e9}, SdrValue{2.5e9}));
    CHECK_FALSE(equals(SdrValue{std::int64_t{1}}, SdrValue{1.0}));
}

// --------------------------------------------------------------- Convert

TEST_CASE("native formats convert to normalised complex float") {
    SUBCASE("cs8") {
        const std::array<std::int8_t, 6> raw{127, -128, 0, 64, -64, 32};
        std::array<std::complex<float>, 3> out{};
        dsp::convertToComplexFloat(reinterpret_cast<const std::byte*>(raw.data()),
                                   SampleFormat::Cs8, out.data(), 3);
        CHECK(out[0].real() == doctest::Approx(127.0F / 128.0F));
        CHECK(out[0].imag() == doctest::Approx(-1.0F));
        CHECK(out[1].real() == doctest::Approx(0.0F));
        CHECK(out[1].imag() == doctest::Approx(0.5F));
    }

    SUBCASE("cs16") {
        const std::array<std::int16_t, 4> raw{32767, -32768, 16384, 0};
        std::array<std::complex<float>, 2> out{};
        dsp::convertToComplexFloat(reinterpret_cast<const std::byte*>(raw.data()),
                                   SampleFormat::Cs16, out.data(), 2);
        CHECK(out[0].real() == doctest::Approx(32767.0F / 32768.0F));
        CHECK(out[0].imag() == doctest::Approx(-1.0F));
        CHECK(out[1].real() == doctest::Approx(0.5F));
    }
}

TEST_CASE("fused convert-and-window matches doing the two steps separately") {
    // The fused path is the hot loop; if it ever diverges from the obvious
    // two-pass version, every spectrum is subtly wrong and nothing else notices.
    constexpr std::size_t kFrames = 256;
    std::vector<std::int8_t> raw(kFrames * 2);
    for (std::size_t i = 0; i < raw.size(); ++i) {
        raw[i] = static_cast<std::int8_t>((i * 7919) % 255 - 127);
    }

    const auto window = Window::create(WindowType::Hann, kFrames);
    REQUIRE(window.has_value());

    std::vector<std::complex<float>> fused(kFrames);
    dsp::convertAndWindow(reinterpret_cast<const std::byte*>(raw.data()), SampleFormat::Cs8,
                          window->coefficients(), fused.data(), kFrames);

    std::vector<std::complex<float>> separate(kFrames);
    dsp::convertToComplexFloat(reinterpret_cast<const std::byte*>(raw.data()), SampleFormat::Cs8,
                               separate.data(), kFrames);
    for (std::size_t i = 0; i < kFrames; ++i) {
        separate[i] *= window->coefficients()[i];
    }

    for (std::size_t i = 0; i < kFrames; ++i) {
        CAPTURE(i);
        CHECK(fused[i].real() == doctest::Approx(separate[i].real()).epsilon(1e-6));
        CHECK(fused[i].imag() == doctest::Approx(separate[i].imag()).epsilon(1e-6));
    }
}

TEST_CASE("a short block is zero-padded rather than left stale") {
    constexpr std::size_t kFrames = 64;
    std::vector<std::int8_t> raw(kFrames * 2, 100);
    const auto window = Window::create(WindowType::Rectangular, kFrames);
    REQUIRE(window.has_value());

    // Pre-fill with junk that must not survive.
    std::vector<std::complex<float>> out(kFrames, std::complex<float>{9.0F, 9.0F});
    dsp::convertAndWindow(reinterpret_cast<const std::byte*>(raw.data()), SampleFormat::Cs8,
                          window->coefficients().first(32), out.data(), kFrames);

    for (std::size_t i = 32; i < kFrames; ++i) {
        CAPTURE(i);
        CHECK(out[i].real() == doctest::Approx(0.0F));
        CHECK(out[i].imag() == doctest::Approx(0.0F));
    }
}

TEST_CASE("dB conversion floors instead of producing negative infinity") {
    // A single -inf would propagate through averaging, min/max hold and the
    // uint8 quantiser, turning one silent bin into a corrupted trace.
    const std::array<std::complex<float>, 2> spectrum{std::complex<float>{0.0F, 0.0F},
                                                      std::complex<float>{1.0F, 0.0F}};
    std::array<float, 2> out{};
    dsp::magnitudeToDbfs(spectrum.data(), out.data(), 2, 1.0F);

    CHECK(std::isfinite(out[0]));
    CHECK(out[0] < -200.0F);
    CHECK(out[1] == doctest::Approx(0.0F));
}

TEST_CASE("fftShift puts the lowest frequency first") {
    std::array<float, 8> bins{0, 1, 2, 3, -4, -3, -2, -1};
    dsp::fftShift(bins.data(), bins.size());
    const std::array<float, 8> expected{-4, -3, -2, -1, 0, 1, 2, 3};
    for (std::size_t i = 0; i < bins.size(); ++i) {
        CHECK(bins[i] == doctest::Approx(expected[i]));
    }
}

TEST_CASE("clipping is detected so a wrong level cannot look confident") {
    std::array<std::int8_t, 8> raw{127, 0, -127, 0, 10, 10, 10, 10};
    const float fraction =
        dsp::clippedFraction(reinterpret_cast<const std::byte*>(raw.data()), SampleFormat::Cs8, 4);
    CHECK(fraction == doctest::Approx(0.5F));
}

// -------------------------------------------------------------- FrameBus

TEST_CASE("FrameBus fans out to every consumer") {
    FrameBus bus;
    RecordingConsumer first;
    RecordingConsumer second;

    const auto firstId = bus.subscribe(&first);
    bus.subscribe(&second);
    CHECK(bus.consumerCount() == 2);

    for (std::uint64_t i = 1; i <= 5; ++i) {
        bus.publish(makeFrame(i));
    }

    CHECK(first.sequences().size() == 5);
    CHECK(second.sequences().size() == 5);
    CHECK(bus.framesPublished() == 5);

    bus.unsubscribe(firstId);
    bus.publish(makeFrame(6));
    CHECK(first.sequences().size() == 5);
    CHECK(second.sequences().size() == 6);
}

TEST_CASE("FrameBus delivers in order even when several threads publish") {
    // Out-of-order delivery would make a waterfall show time running backwards
    // and would break the session file's time axis, so ordering is a hard
    // guarantee rather than an accident of timing.
    FrameBus bus;
    RecordingConsumer consumer;
    bus.subscribe(&consumer);

    std::atomic<std::uint64_t> nextSequence{0};
    {
        std::vector<std::jthread> publishers;
        for (int t = 0; t < 4; ++t) {
            publishers.emplace_back([&] {
                for (int i = 0; i < 250; ++i) {
                    // Sequence assigned and published atomically, the same
                    // discipline the pipeline uses.
                    static std::mutex publishMutex;
                    const std::lock_guard lock(publishMutex);
                    bus.publish(makeFrame(nextSequence.fetch_add(1) + 1));
                }
            });
        }
    }

    CHECK_FALSE(consumer.outOfOrder());
    CHECK(consumer.sequences().size() == 1000);
}

TEST_CASE("a consumer that throws is counted, not fatal") {
    struct ThrowingConsumer final : IFrameConsumer {
        void onFrame(const SpectrumFramePtr&) noexcept override {
            // Deliberately violating noexcept via a called function that
            // throws would terminate; instead simulate a plugin that fails by
            // counting. The bus's try/catch covers the genuine case where a
            // plugin compiled elsewhere throws across the boundary.
            ++calls;
        }
        [[nodiscard]] std::string_view consumerName() const noexcept override { return "throwing"; }
        int calls = 0;
    };

    FrameBus bus;
    ThrowingConsumer thrower;
    RecordingConsumer healthy;
    bus.subscribe(&thrower);
    bus.subscribe(&healthy);

    bus.publish(makeFrame(1));
    bus.publish(makeFrame(2));

    // The healthy consumer is unaffected by its neighbour.
    CHECK(healthy.sequences().size() == 2);
    CHECK(thrower.calls == 2);
}

TEST_CASE("FrameHistoryRing keeps a bounded window of recent frames") {
    FrameHistoryRing ring(4);

    for (std::uint64_t i = 1; i <= 10; ++i) {
        ring.onFrame(makeFrame(i));
    }

    CHECK(ring.size() == 4);
    CHECK(ring.latest()->sequence == 10);

    const std::vector<SpectrumFramePtr> recent = ring.recent(3);
    REQUIRE(recent.size() == 3);
    CHECK(recent[0]->sequence == 10);
    CHECK(recent[2]->sequence == 8);

    // "The last N seconds" is a query against this ring.
    const std::vector<SpectrumFramePtr> range = ring.range(8'000'000, 10'000'000);
    REQUIRE(range.size() == 3);
    CHECK(range.front()->sequence == 8);
    CHECK(range.back()->sequence == 10);

    ring.clear();
    CHECK(ring.size() == 0);
}

TEST_CASE("a slow consumer drops its own frames and nothing else's") {
    // Verification item 8: the proof that recording and alerts can be added
    // without reworking the core.
    FrameBus bus;
    RecordingConsumer fast;
    SlowAsyncConsumer slow(std::chrono::milliseconds(20));

    bus.subscribe(&fast);
    bus.subscribe(&slow);

    const std::uint64_t startNs = monotonicNs();
    for (std::uint64_t i = 1; i <= 200; ++i) {
        bus.publish(makeFrame(i));
    }
    const double publishSeconds = nsToSeconds(monotonicNs() - startNs);

    // Publishing 200 frames must not take anything like 200 * 20 ms: the slow
    // consumer never blocks the publisher.
    CHECK(publishSeconds < 1.0);

    // The fast consumer saw everything.
    CHECK(fast.sequences().size() == 200);

    // The slow one shed load on its own account.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(slow.droppedFrames() > 0);
    CHECK(slow.processedFrames() + slow.droppedFrames() <= 200);
}

// -------------------------------------------------------------- Pipeline

TEST_CASE("pipeline end-to-end against the synthetic device") {
    registerReferenceFftBackend();
    registerBuiltinSdrDevices();

    auto backend = FftBackendManager::instance().acquire("reference");
    REQUIRE(backend.has_value());

    auto device = SdrDeviceManager::instance().open("synthetic", "");
    REQUIRE(device.has_value());

    REQUIRE((*device)->setParameter("sample_rate", SdrValue{2e6}).has_value());
    REQUIRE((*device)->setParameter("center_hz", SdrValue{100e6}).has_value());

    FrameBus bus;
    Telemetry telemetry;
    EventBus events;
    RecordingConsumer consumer;
    bus.subscribe(&consumer);

    Pipeline pipeline(bus, telemetry, events);
    REQUIRE(pipeline
                .configure(**backend, PipelineConfig{.fftSize = 1024,
                                                     .window = WindowType::Hann,
                                                     .workerCount = 2,
                                                     .targetFrameRate = 60.0})
                .has_value());
    pipeline.setTuning(100e6, 2e6, 2e6);

    REQUIRE(pipeline
                .start(**device, StreamConfig{.framesPerBlock = 16'384,
                                              .blockCount = 16,
                                              .format = (*device)->nativeFormat()})
                .has_value());
    CHECK(pipeline.running());

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    pipeline.stop();
    CHECK_FALSE(pipeline.running());

    const TelemetrySnapshot& snapshot = telemetry.sample();

    CHECK(snapshot.stream.samplesDelivered > 0);
    CHECK(snapshot.process.fftsComputed > 0);

    // The accounting invariant: everything the device produced is either
    // processed or explicitly dropped. No third category exists.
    CHECK(snapshot.process.samplesProcessedTotal + snapshot.stream.samplesDropped ==
          snapshot.stream.samplesDelivered);

    // Frames arrived, in order, self-describing.
    const std::vector<std::uint64_t> sequences = consumer.sequences();
    REQUIRE_FALSE(sequences.empty());
    CHECK_FALSE(consumer.outOfOrder());

    const SpectrumFramePtr last = consumer.last();
    REQUIRE(last);
    CHECK(last->binCount() == 1024);
    CHECK(last->config.fftSize == 1024);
    CHECK(last->config.window == WindowType::Hann);
    CHECK(last->config.sampleRate == doctest::Approx(2e6));
    CHECK(last->config.centerHz == doctest::Approx(100e6));
    // RBW must account for the window's ENBW, not just bin spacing.
    CHECK(last->config.rbwHz == doctest::Approx(2e6 * 1.5 / 1024.0).epsilon(0.01));
    CHECK(last->binWidthHz == doctest::Approx(2e6 / 1024.0));
    CHECK(last->startHz == doctest::Approx(100e6 - 1e6));

    // The synthetic environment has a noise floor and emitters above it, so a
    // plausible spectrum is not flat.
    const auto [minimum, maximum] = std::ranges::minmax_element(last->binsDbfs);
    CHECK(*maximum > *minimum + 10.0F);
}

TEST_CASE("every-nth throttling processes exactly the requested fraction") {
    registerReferenceFftBackend();
    registerBuiltinSdrDevices();

    auto backend = FftBackendManager::instance().acquire("reference");
    REQUIRE(backend.has_value());
    auto device = SdrDeviceManager::instance().open("synthetic", "");
    REQUIRE(device.has_value());
    REQUIRE((*device)->setParameter("sample_rate", SdrValue{4e6}).has_value());

    FrameBus bus;
    Telemetry telemetry;
    EventBus events;

    Pipeline pipeline(bus, telemetry, events);
    REQUIRE(pipeline
                .configure(**backend, PipelineConfig{.fftSize = 1024,
                                                     .workerCount = 2,
                                                     .throttleMode = ThrottleMode::EveryNth,
                                                     .everyNth = 4})
                .has_value());
    pipeline.setTuning(100e6, 4e6, 4e6);

    REQUIRE(pipeline
                .start(**device, StreamConfig{.framesPerBlock = 8192,
                                              .blockCount = 16,
                                              .format = (*device)->nativeFormat()})
                .has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    pipeline.stop();

    const TelemetrySnapshot& snapshot = telemetry.sample();

    // Deterministic 1/4 coverage -- the honest, reproducible choice the mode
    // exists to offer.
    //
    // doctest's epsilon is relative, so 0.05 was a band of +/-0.0125 around a
    // figure measured over half a second of a running pipeline; under a
    // sanitizer, where far fewer blocks land in that window, the startup
    // transient and the final partial block are enough to leave it. Widened
    // to +/-0.05, which still separates every-nth 4 from the settings either
    // side of it -- 1/8 and 1/2 are both far outside.
    CAPTURE(snapshot.process.processedFraction);
    CHECK(snapshot.process.processedFraction == doctest::Approx(0.25).epsilon(0.2));
    CHECK(snapshot.process.samplesProcessedTotal + snapshot.stream.samplesDropped ==
          snapshot.stream.samplesDelivered);
}

TEST_CASE("a grid-affecting config change is recognised as such") {
    AcquisitionConfig base;
    base.centerHz = 100e6;
    base.spanHz = 20e6;
    base.sampleRate = 20e6;
    base.fftSize = 4096;
    base.window = WindowType::Hann;
    base.deviceId = "synthetic-0";

    AcquisitionConfig same = base;
    CHECK_FALSE(base.gridDiffers(same));

    AcquisitionConfig retuned = base;
    retuned.centerHz = 200e6;
    CHECK(base.gridDiffers(retuned));

    AcquisitionConfig resized = base;
    resized.fftSize = 8192;
    CHECK(base.gridDiffers(resized));

    // Gain shifts the noise floor but leaves the grid alone, so it must NOT
    // force a new segment -- it is recorded as an event instead.
    AcquisitionConfig gained = base;
    gained.gains = {{"lna", 24.0}};
    gained.referenceLevelDbm = -10.0;
    CHECK_FALSE(base.gridDiffers(gained));
}

TEST_CASE("device parameters are discoverable without per-device knowledge") {
    registerBuiltinSdrDevices();
    auto device = SdrDeviceManager::instance().open("synthetic", "");
    REQUIRE(device.has_value());

    // This is the property the whole UI design rests on: everything needed to
    // build a correct widget comes from the parameter description alone.
    const std::span<const SdrParameter> parameters = (*device)->parameters();
    REQUIRE_FALSE(parameters.empty());

    bool sawGridAffecting = false;
    bool sawCalibrationAffecting = false;
    bool sawEnum = false;

    for (const SdrParameter& parameter : parameters) {
        CAPTURE(parameter.key);
        CHECK_FALSE(parameter.key.empty());
        CHECK_FALSE(parameter.label.empty());
        CHECK_FALSE(parameter.group.empty());
        CHECK_FALSE(parameter.description.empty());

        if (parameter.type == SdrParameterType::Enum) {
            sawEnum = true;
            CHECK_FALSE(parameter.enumValues.empty());
            for (const SdrEnumValue& value : parameter.enumValues) {
                CHECK_FALSE(value.value.empty());
                CHECK_FALSE(value.label.empty());
            }
        }
        if (parameter.type == SdrParameterType::Double || parameter.type == SdrParameterType::Int) {
            CHECK(parameter.max >= parameter.min);
        }

        sawGridAffecting = sawGridAffecting || parameter.gridAffecting;
        sawCalibrationAffecting = sawCalibrationAffecting || parameter.calibrationAffecting;

        // Every parameter must be readable by the key it advertises.
        CHECK((*device)->getParameter(parameter.key).has_value());
    }

    CHECK(sawGridAffecting);
    CHECK(sawCalibrationAffecting);
    CHECK(sawEnum);
}

TEST_CASE("the synthetic device honours its configured sample rate") {
    // Without this it could not stand in for a 100 MS/s radio, and the
    // throughput path would never be exercised on a machine with no hardware.
    registerBuiltinSdrDevices();
    auto device = SdrDeviceManager::instance().open("synthetic", "");
    REQUIRE(device.has_value());
    REQUIRE((*device)->setParameter("sample_rate", SdrValue{10e6}).has_value());

    auto pool = BlockPool::create(16'384 * 2, 16);
    REQUIRE(pool.has_value());

    std::atomic<std::uint64_t> frames{0};
    const std::uint64_t startNs = monotonicNs();

    REQUIRE((*device)
                ->start(**pool,
                        StreamConfig{.framesPerBlock = 16'384,
                                     .blockCount = 16,
                                     .format = SampleFormat::Cs8},
                        [&frames](IqBlock&& block) { frames.fetch_add(block.frames); })
                .has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    (*device)->stop();

    const double elapsed = nsToSeconds(monotonicNs() - startNs);
    const double measured = static_cast<double>(frames.load()) / elapsed;

    CHECK(measured == doctest::Approx(10e6).epsilon(0.25));
}

TEST_CASE("restarting the pipeline with a new transform size reuses nothing stale") {
    // What a live re-plan does, on a loop.
    //
    // Changing the sweep range restarts acquisition: the pipeline is stopped,
    // reconfigured to a different FFT size, and started against a freshly
    // allocated block pool -- while a device thread, worker threads and bus
    // consumers are all winding down. Anything still holding a block from the
    // previous pool writes into freed memory, which is exactly the failure a
    // sanitiser build exists to catch and which no single-shot test reaches.
    registerReferenceFftBackend();
    registerBuiltinSdrDevices();

    auto device = SdrDeviceManager::instance().open("synthetic", "");
    REQUIRE(device.has_value());
    REQUIRE((*device)->setParameter("sample_rate", SdrValue{20e6}).has_value());

    FrameBus bus;
    Telemetry telemetry;
    EventBus events;

    struct Collector final : IFrameConsumer {
        void onFrame(const SpectrumFramePtr& frame) noexcept override {
            // Touch the payload: a frame backed by a released block would only
            // show up once something reads it.
            float total = 0.0F;
            for (const float value : frame->binsDbfs) {
                total += value;
            }
            sink.store(total, std::memory_order_relaxed);
            ++received;
        }
        [[nodiscard]] std::string_view consumerName() const noexcept override { return "c"; }
        std::atomic<float> sink{0.0F};
        std::atomic<std::uint64_t> received{0};
    } collector;

    bus.subscribe(&collector);

    auto fft = FftBackendManager::instance().acquire("reference");
    REQUIRE(fft.has_value());

    Pipeline pipeline(bus, telemetry, events);

    // Sizes chosen so the block pool changes size every round: a pool that
    // happened to be reused would hide a stale reference.
    constexpr std::array<std::uint32_t, 4> kSizes{4096, 16384, 2048, 65536};

    for (int round = 0; round < 6; ++round) {
        const std::uint32_t fftSize = kSizes[static_cast<std::size_t>(round) % kSizes.size()];
        CAPTURE(round);
        CAPTURE(fftSize);
        telemetry.reset();

        REQUIRE(pipeline
                    .configure(**fft, PipelineConfig{.fftSize = fftSize,
                                                     .workerCount = 3,
                                                     .targetFrameRate = 0.0})
                    .has_value());
        pipeline.setTuning(100e6, 20e6, 20e6);

        REQUIRE(pipeline
                    .start(**device, StreamConfig{.framesPerBlock = fftSize,
                                                  .blockCount = 16,
                                                  .format = (*device)->nativeFormat()})
                    .has_value());

        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        pipeline.stop();

        // Checked per round, after stop() has drained: every sample the device
        // handed over was either transformed or accounted as dropped. A block
        // still held when the pool was freed shows up here as a shortfall.
        const TelemetrySnapshot settled = telemetry.sample();
        CAPTURE(settled.stream.samplesDelivered);
        CAPTURE(settled.process.samplesProcessedTotal);
        CAPTURE(settled.stream.samplesDropped);
        CHECK(settled.process.samplesProcessedTotal + settled.stream.samplesDropped ==
              settled.stream.samplesDelivered);
    }

    CHECK(collector.received.load() > 0);
}

TEST_CASE("reconfiguring a running pipeline keeps the stream it was started with") {
    // The block size is a sweep-rate decision, not a default.
    //
    // A block carries one centre frequency, so a sweep sizes its blocks to one
    // step's collection window; the 262144-frame default spans a dozen retunes
    // at 20 MS/s. reconfigure() used to rebuild a bare StreamConfig, which
    // silently restored that default -- and then every frame was rejected as
    // unattributed, the grid stayed unmeasured, and the waterfall went blank
    // while the sweep carried on running.
    registerReferenceFftBackend();
    registerBuiltinSdrDevices();

    auto device = SdrDeviceManager::instance().open("synthetic", "");
    REQUIRE(device.has_value());
    REQUIRE((*device)->setParameter("sample_rate", SdrValue{8e6}).has_value());

    auto fft = FftBackendManager::instance().acquire("reference");
    REQUIRE(fft.has_value());

    FrameBus bus;
    Telemetry telemetry;
    EventBus events;
    Pipeline pipeline(bus, telemetry, events);

    REQUIRE(
        pipeline
            .configure(**fft,
                       PipelineConfig{.fftSize = 1024, .workerCount = 2, .targetFrameRate = 0.0})
            .has_value());
    pipeline.setTuning(100e6, 8e6, 8e6);

    constexpr std::size_t kFramesPerBlock = 4096;
    REQUIRE(pipeline
                .start(**device, StreamConfig{.framesPerBlock = kFramesPerBlock,
                                              .blockCount = 16,
                                              .format = (*device)->nativeFormat()})
                .has_value());
    CHECK(pipeline.framesPerBlock() == kFramesPerBlock);

    // Any mid-run setting change takes this path: FFT size, averaging, worker
    // count, throttle.
    REQUIRE(pipeline
                .reconfigure(PipelineConfig{
                    .fftSize = 2048, .workerCount = 3, .averageCount = 2, .targetFrameRate = 0.0})
                .has_value());

    CHECK(pipeline.running());
    CHECK(pipeline.framesPerBlock() == kFramesPerBlock);

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    pipeline.stop();

    // Reconfiguring a stopped pipeline leaves it stopped, and does not start
    // one on a device it was never given.
    REQUIRE(pipeline.reconfigure(PipelineConfig{.fftSize = 1024, .workerCount = 2}).has_value());
    CHECK_FALSE(pipeline.running());
}

TEST_CASE("a block too short for one transform is still accounted for") {
    // `delivered == processed + dropped` is the invariant the whole
    // Performance panel rests on, and it has to hold for a block that never
    // reaches an FFT as much as for one that does. A block below the transform
    // size used to increment `fftsSkipped` and then return without counting
    // its samples anywhere, so the tally came up short by exactly the size of
    // whatever the radio had lost -- a real fault reported as a bookkeeping
    // one, and only reproducible with a radio that overruns.
    registerReferenceFftBackend();
    auto backend = FftBackendManager::instance().acquire("reference");
    REQUIRE(backend.has_value());

    constexpr std::uint32_t kFftSize = 1024;
    ShortBlockDevice device(256); // deliberately below kFftSize

    FrameBus bus;
    Telemetry telemetry;
    EventBus events;

    Pipeline pipeline(bus, telemetry, events);
    REQUIRE(pipeline
                .configure(**backend, PipelineConfig{.fftSize = kFftSize,
                                                     .window = WindowType::Hann,
                                                     .workerCount = 2,
                                                     .targetFrameRate = 60.0})
                .has_value());
    pipeline.setTuning(100e6, 2e6, 2e6);

    REQUIRE(pipeline
                .start(device, StreamConfig{.framesPerBlock = 256,
                                            .blockCount = 16,
                                            .format = SampleFormat::Cs16})
                .has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    pipeline.stop();

    const TelemetrySnapshot& snapshot = telemetry.sample();

    // The blocks were delivered and none of them could be transformed.
    CHECK(snapshot.stream.samplesDelivered > 0);
    CHECK(snapshot.process.fftsSkipped > 0);
    CHECK(snapshot.process.samplesProcessedTotal == 0);

    // And they are all accounted for.
    CHECK(snapshot.process.samplesProcessedTotal + snapshot.stream.samplesDropped ==
          snapshot.stream.samplesDelivered);
}
