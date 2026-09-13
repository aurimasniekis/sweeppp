// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/backends/sdr/SyntheticDevice.hpp"

#include "sweeppp/core/BlockPool.hpp"
#include "sweeppp/core/Clock.hpp"
#include "sweeppp/core/Log.hpp"
#include "sweeppp/core/Telemetry.hpp"
#include "sweeppp/core/Version.hpp"
#include "sweeppp/dsp/Convert.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>
#include <random>
#include <thread>

namespace sweeppp {
namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

/// How far ahead of real time the generator is allowed to run.
///
/// It reads the tuning as it produces each block, so this is also how finely
/// it can resolve a retune -- the stand-in for a real radio's USB transfer
/// size. It has to stay well under a sweep step, or the generator emits a
/// burst of blocks at one tuning and sleeps past whole steps unobserved.
constexpr double kMaxLookAheadSeconds = 50e-6;

/// The rates the panel offers, spanning what a real radio does: a narrow FM
/// channel at one end, a wideband capture at the other.
///
/// Named once because it is both the preset list and what
/// `supportedSampleRates()` reports. Presets only -- the generator honours any
/// rate it is given, which is what lets it reproduce a specific radio's load.
constexpr std::array kOfferedSampleRates{2e6,     8e6,  10e6,  20e6,    40e6,
                                         61.44e6, 80e6, 100e6, 122.88e6};

/// An emitter in the synthetic RF environment.
///
/// The mix is chosen so the device exercises the things that actually break
/// spectrum software: a steady carrier (baseline), a drifting one (does the
/// waterfall track it), a pulsed one (does max-hold decimation preserve a
/// short burst -- the property §3.8's LOD pyramid depends on), and a wideband
/// one (does per-pixel envelope decimation keep its shape).
struct Emitter {
    enum class Kind { Continuous, Drifting, Pulsed, Wideband };

    Kind kind = Kind::Continuous;
    double frequencyHz = 0.0;
    double amplitude = 0.1;
    double bandwidthHz = 0.0;

    /// Drift in Hz/s.
    double driftRate = 0.0;
    double driftSpanHz = 0.0;

    /// Pulse timing, in seconds.
    double pulsePeriod = 1.0;
    double pulseWidth = 0.2;

    double phase = 0.0;
    double driftPhase = 0.0;
};

class SyntheticDevice final : public ISdrDevice {
public:
    explicit SyntheticDevice(SdrDeviceInfo info) : m_info(std::move(info)) {
        buildParameters();
        buildPorts();
        buildEmitters();
    }

    ~SyntheticDevice() override { SyntheticDevice::stop(); }

    [[nodiscard]] const SdrDeviceInfo& info() const noexcept override { return m_info; }

    [[nodiscard]] std::span<const SdrParameter> parameters() const noexcept override {
        return m_parameters;
    }

    [[nodiscard]] Result<SdrValue> getParameter(std::string_view key) const override {
        if (key == "center_hz") {
            return SdrValue{m_centerHz.load()};
        }
        if (key == "sample_rate") {
            return SdrValue{m_sampleRate.load()};
        }
        if (key == "gain") {
            return SdrValue{std::int64_t{m_gainDb.load()}};
        }
        if (key == "noise_floor") {
            return SdrValue{m_noiseFloorDb.load()};
        }
        if (key == "emitters") {
            return SdrValue{std::int64_t{m_emitterCount.load()}};
        }
        if (key == "sample_format") {
            return SdrValue{std::string(toString(m_format))};
        }
        if (key == "simulate_overruns") {
            return SdrValue{m_simulateOverruns.load()};
        }
        return fail<SdrValue>(ErrorCode::NotFound, "no parameter '{}'", key);
    }

    [[nodiscard]] Status setParameter(std::string_view key, const SdrValue& value) override {
        const auto parameter = std::ranges::find_if(
            m_parameters, [key](const SdrParameter& p) { return p.key == key; });
        if (parameter == m_parameters.end()) {
            return fail(ErrorCode::NotFound, "no parameter '{}'", key);
        }

        auto coerced = parameter->coerce(value);
        if (!coerced) {
            return std::unexpected(coerced.error());
        }

        if (key == "center_hz") {
            m_centerHz.store(asDouble(*coerced));
        } else if (key == "sample_rate") {
            m_sampleRate.store(asDouble(*coerced));
        } else if (key == "gain") {
            m_gainDb.store(static_cast<int>(asInt(*coerced)));
        } else if (key == "noise_floor") {
            m_noiseFloorDb.store(asDouble(*coerced));
        } else if (key == "emitters") {
            m_emitterCount.store(static_cast<int>(asInt(*coerced)));
            buildEmitters();
        } else if (key == "sample_format") {
            auto format = sampleFormatFromString(asString(*coerced));
            if (!format) {
                return std::unexpected(format.error());
            }
            m_format = *format;
        } else if (key == "simulate_overruns") {
            m_simulateOverruns.store(asBool(*coerced));
        }

        return ok();
    }

    [[nodiscard]] SampleFormat nativeFormat() const noexcept override { return m_format; }

    [[nodiscard]] Status start(BlockPool& pool, const StreamConfig& config,
                               IqCallback callback) override {
        if (m_streaming.load()) {
            return fail(ErrorCode::AlreadyExists, "already streaming");
        }

        m_pool = &pool;
        m_callback = std::move(callback);
        m_framesPerBlock = config.framesPerBlock;
        m_sequence = 0;
        m_streaming.store(true);

        m_thread = std::jthread([this](std::stop_token stop) { generate(stop); });
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
        m_callback = nullptr;
        m_pool = nullptr;
    }

    [[nodiscard]] bool streaming() const noexcept override { return m_streaming.load(); }

    [[nodiscard]] Status retune(double centerHz) override {
        m_centerHz.store(centerHz);
        return ok();
    }

    [[nodiscard]] double deliveryGranularitySeconds(double sampleRate) const noexcept override {
        // The generator's pacing window, not its block size: the block size
        // is only known once streaming starts, and a sweep is planned before
        // that. This is the honest figure anyway -- it is how far the
        // generator can run ahead, and therefore how stale the tuning it
        // stamps on a block can be.
        (void)sampleRate;
        return kMaxLookAheadSeconds;
    }

    [[nodiscard]] double retuneSettleSeconds() const noexcept override {
        // Small but non-zero, so the sweep engine's settle-discard path is
        // exercised rather than optimised away during development.
        return 50e-6;
    }

    void attachTelemetry(StreamCounters* counters) noexcept override {
        m_telemetry.store(counters);
        if (counters != nullptr) {
            counters->linkCapacityBytesPerSec.store(m_info.linkCapacityBytesPerSec,
                                                    std::memory_order_relaxed);
        }
    }

    [[nodiscard]] std::vector<double> supportedSampleRates() const override {
        return {kOfferedSampleRates.begin(), kOfferedSampleRates.end()};
    }

    [[nodiscard]] std::span<const SdrRxPort> rxPorts() const noexcept override { return m_ports; }

    [[nodiscard]] std::string_view selectedRxPort() const noexcept override {
        return m_ports[m_rxPort.load(std::memory_order_relaxed)].id;
    }

    [[nodiscard]] Status selectRxPort(std::string_view id) override {
        const auto match =
            std::ranges::find_if(m_ports, [id](const SdrRxPort& port) { return port.id == id; });
        if (match == m_ports.end()) {
            return fail(ErrorCode::InvalidArgument, "no receive port '{}'", id);
        }

        m_rxPort.store(static_cast<std::size_t>(match - m_ports.begin()),
                       std::memory_order_relaxed);
        return ok();
    }

private:
    /// Two ports with different limits, which is not a toy.
    ///
    /// `tests/test_sweep.cpp` plans against this device, so these are what make
    /// antenna routing testable in CI with nothing plugged in -- the same
    /// reason the synthetic device already simulates sample formats and
    /// overruns. The ranges deliberately overlap in the middle: a planner that
    /// alternates ports across a band both can hear is the failure hysteresis
    /// exists to prevent, and there has to be somewhere it could happen.
    void buildPorts() {
        m_ports = {
            SdrRxPort{.id = "rx1",
                      .label = "RX1",
                      .connector = "SMA (J1)",
                      .minHz = 1e6,
                      .maxHz = 2e9,
                      .biasTee = true,
                      .requiresStop = false,
                      .switchSeconds = 1e-3},
            SdrRxPort{.id = "rx2",
                      .label = "RX2",
                      .connector = "SMA (J2)",
                      .minHz = 1.5e9,
                      .maxHz = 6e9,
                      .biasTee = false,
                      .requiresStop = true,
                      .switchSeconds = 5e-3},
        };
    }

    void buildParameters() {
        // Built from the format list rather than restated, so a format added
        // there is one this device can immediately produce. That matters more
        // than it looks: most of these have no hardware in this tree that
        // emits them, and a format nothing can produce is a format whose
        // conversion has never actually run.
        std::vector<SdrEnumValue> formats;
        formats.reserve(allSampleFormats().size());
        for (const SampleFormat format : allSampleFormats()) {
            formats.push_back(
                {.value = std::string(toString(format)),
                 .label = std::string(displayName(format)),
                 .description = std::format("{} bytes per sample", bytesPerFrame(format))});
        }

        m_parameters = {
            SdrParameter{.key = "center_hz",
                         .label = "Center frequency",
                         .group = "Tuning",
                         .type = SdrParameterType::Double,
                         .unit = "Hz",
                         .min = 1e6,
                         .max = 6e9,
                         .step = 1.0,
                         .enumValues = {},
                         .readOnly = false,
                         .gridAffecting = true,
                         .calibrationAffecting = false,
                         .requiresStop = false,
                         .description = "Centre of the simulated tuner.",
                         .defaultValue = 100e6},
            SdrParameter{.key = "sample_rate",
                         .label = "Sample rate",
                         .group = "Tuning",
                         .type = SdrParameterType::Double,
                         .unit = "S/s",
                         .min = 1e6,
                         .max = 200e6,
                         .step = 0.0,
                         .enumValues = sampleRateChoices(kOfferedSampleRates),
                         .readOnly = false,
                         .gridAffecting = true,
                         .calibrationAffecting = false,
                         .requiresStop = true,
                         .description = "Generated rate. The generator honours this, so it can "
                                        "reproduce a genuine 100 MS/s load -- including a rate "
                                        "that is not one of the presets.",
                         .defaultValue = 20e6},
            SdrParameter{.key = "gain",
                         .label = "Gain",
                         .group = "Gain",
                         .type = SdrParameterType::Int,
                         .unit = "dB",
                         .min = 0.0,
                         .max = 60.0,
                         .step = 1.0,
                         .enumValues = {},
                         .readOnly = false,
                         .gridAffecting = false,
                         .calibrationAffecting = true,
                         .requiresStop = false,
                         .description = "Simulated front-end gain. Scales both signal and noise.",
                         .defaultValue = std::int64_t{20}},
            SdrParameter{.key = "noise_floor",
                         .label = "Noise floor",
                         .group = "Simulation",
                         .type = SdrParameterType::Double,
                         .unit = "dBFS",
                         .min = -120.0,
                         .max = -20.0,
                         .step = 0.5,
                         .enumValues = {},
                         .readOnly = false,
                         .gridAffecting = false,
                         .calibrationAffecting = true,
                         .requiresStop = false,
                         .description = "Thermal noise level of the simulated receiver.",
                         .defaultValue = -85.0},
            SdrParameter{.key = "emitters",
                         .label = "Emitters",
                         .group = "Simulation",
                         .type = SdrParameterType::Int,
                         .unit = {},
                         .min = 0.0,
                         .max = 32.0,
                         .step = 1.0,
                         .enumValues = {},
                         .readOnly = false,
                         .gridAffecting = false,
                         .calibrationAffecting = false,
                         .requiresStop = false,
                         .description = "Number of simulated transmitters: continuous, drifting, "
                                        "pulsed and wideband in rotation.",
                         .defaultValue = std::int64_t{6}},
            SdrParameter{.key = "sample_format",
                         .label = "Sample format",
                         .group = "Simulation",
                         .type = SdrParameterType::Enum,
                         .unit = {},
                         .min = 0.0,
                         .max = 0.0,
                         .step = 0.0,
                         .enumValues = std::move(formats),
                         .readOnly = false,
                         .gridAffecting = false,
                         .calibrationAffecting = false,
                         .requiresStop = true,
                         .description = "Native delivery format, so the conversion paths of real "
                                        "radios can be exercised without one attached.",
                         .defaultValue = std::string{"cs8"}},
            SdrParameter{.key = "simulate_overruns",
                         .label = "Simulate overruns",
                         .group = "Simulation",
                         .type = SdrParameterType::Bool,
                         .unit = {},
                         .min = 0.0,
                         .max = 0.0,
                         .step = 0.0,
                         .enumValues = {},
                         .readOnly = false,
                         .gridAffecting = false,
                         .calibrationAffecting = false,
                         .requiresStop = false,
                         .description = "Periodically skip a block, so the drop-accounting and "
                                        "amber-badge paths can be exercised deliberately.",
                         .defaultValue = false},
        };
    }

    void buildEmitters() {
        const int count = m_emitterCount.load();
        std::vector<Emitter> emitters;
        emitters.reserve(static_cast<std::size_t>(std::max(count, 0)));

        // Deterministic: the same seed every time, so a regression that
        // depends on emitter placement reproduces exactly.
        std::mt19937 rng(0x5EEDU);
        std::uniform_real_distribution<double> offsetFraction(-0.45, 0.45);
        std::uniform_real_distribution<double> amplitude(0.02, 0.35);

        for (int i = 0; i < count; ++i) {
            Emitter emitter;
            emitter.kind = static_cast<Emitter::Kind>(i % 4);
            emitter.frequencyHz = offsetFraction(rng);
            emitter.amplitude = amplitude(rng);

            switch (emitter.kind) {
            case Emitter::Kind::Drifting:
                emitter.driftRate = 0.05 + 0.1 * static_cast<double>(i % 3);
                emitter.driftSpanHz = 0.05;
                break;
            case Emitter::Kind::Pulsed:
                // 200 ms burst every 2 s: long enough to be real, short enough
                // that mean-based time decimation would erase it. Exactly the
                // case the LOD pyramid must preserve.
                emitter.pulsePeriod = 2.0;
                emitter.pulseWidth = 0.2;
                break;
            case Emitter::Kind::Wideband:
                emitter.bandwidthHz = 0.02 + 0.01 * static_cast<double>(i % 4);
                break;
            case Emitter::Kind::Continuous:
                break;
            }

            emitters.push_back(emitter);
        }

        const std::lock_guard lock(m_emitterMutex);
        m_emitters = std::move(emitters);
    }

    void generate(std::stop_token stop) {
        std::mt19937 rng(0xC0FFEEU);
        std::normal_distribution<float> noise(0.0F, 1.0F);

        const std::uint64_t startNs = monotonicNs();
        std::uint64_t framesGenerated = 0;
        std::uint64_t blockIndex = 0;
        double lastRefreshTime = -1.0;

        std::vector<Emitter> emitters;
        {
            const std::lock_guard lock(m_emitterMutex);
            emitters = m_emitters;
        }

        while (!stop.stop_requested()) {
            const double sampleRate = m_sampleRate.load();
            const std::size_t frames = m_framesPerBlock;

            // Pace to the configured sample rate. Without this the generator
            // would run flat out and every rate would look identical, which
            // would make it useless for exercising the throughput path.
            //
            // The tolerance has to stay well under a sweep step. Each block
            // reads the tuning as it is produced, so running far enough ahead
            // to emit a burst of blocks and then sleep means the tuning is
            // only sampled once per sleep -- and a sweep retuning every few
            // hundred microseconds advances past whole steps unobserved. The
            // device then looks like it is skipping bands when it is only
            // failing to notice them, which is exactly the artefact this
            // device exists to help rule out.
            const double elapsed = nsToSeconds(monotonicNs() - startNs);
            const double due = static_cast<double>(framesGenerated) / sampleRate;
            if (due > elapsed + kMaxLookAheadSeconds) {
                std::this_thread::sleep_for(
                    std::chrono::duration<double>(std::min(due - elapsed, 0.05)));
                continue;
            }

            BlockRef ref = m_pool->acquire();
            if (!ref.valid()) {
                // The consumer is behind. Count it and keep generating, so the
                // reported rate stays honest rather than throttling silently
                // to whatever the consumer can take.
                if (StreamCounters* counters = m_telemetry.load(); counters != nullptr) {
                    counters->poolExhaustedEvents.fetch_add(1, std::memory_order_relaxed);
                    counters->samplesLostAtSource.fetch_add(frames, std::memory_order_relaxed);
                }
                framesGenerated += frames;
                std::this_thread::yield();
                continue;
            }

            ++blockIndex;
            if (m_simulateOverruns.load() && (blockIndex % 97) == 0) {
                if (StreamCounters* counters = m_telemetry.load(); counters != nullptr) {
                    counters->deviceOverruns.fetch_add(1, std::memory_order_relaxed);
                    counters->samplesLostAtSource.fetch_add(frames, std::memory_order_relaxed);
                }
                framesGenerated += frames;
                continue;
            }

            const double centerHz = m_centerHz.load();
            const double gainLinear = std::pow(10.0, m_gainDb.load() / 20.0) / 10.0;
            const double noiseAmplitude = std::pow(10.0, m_noiseFloorDb.load() / 20.0) * gainLinear;
            const double timeBase = static_cast<double>(framesGenerated) / sampleRate;

            // Synthesis is regenerated on a slow cadence and memcpy'd in
            // between.
            //
            // This is what makes a genuine 100 MS/s load reachable. Generating
            // every sample would need ~200M Gaussian draws and ~1.2G sin/cos
            // calls per second -- two orders of magnitude beyond any CPU. A
            // memcpy at 200 MB/s is nothing. The cost is that the noise
            // realisation repeats within a refresh window, which is invisible
            // in a spectrum display and irrelevant to a load generator.
            //
            // 50 ms keeps a 200 ms pulsed burst clearly resolved, so the
            // transient the LOD pyramid must preserve is still generated
            // faithfully.
            constexpr double kRefreshSeconds = 0.05;
            if (lastRefreshTime < 0.0 || timeBase - lastRefreshTime >= kRefreshSeconds ||
                m_templateFrames != frames || m_templateFormat != m_format) {
                fillBlock(frames, emitters, centerHz, sampleRate, timeBase, gainLinear,
                          noiseAmplitude, rng, noise);
                lastRefreshTime = timeBase;
            }

            std::memcpy(ref.data(), m_template.data(),
                        std::min(ref.capacityBytes(), m_template.size()));

            IqBlock block;
            block.block = std::move(ref);
            block.frames = frames;
            block.format = m_format;
            block.sequence = m_sequence++;
            // The centre in force when this block's samples were generated.
            block.hostTimeNs = monotonicNs();
            block.centerHz = centerHz;
            block.sampleRate = sampleRate;

            if (m_callback) {
                m_callback(std::move(block));
            }

            framesGenerated += frames;
        }
    }

    void fillBlock(std::size_t frames, std::vector<Emitter>& emitters, double centerHz,
                   double sampleRate, double timeBase, double gainLinear, double noiseAmplitude,
                   std::mt19937& rng, std::normal_distribution<float>& noise) {
        (void)centerHz;

        m_scratch.resize(frames * 2);

        for (std::size_t i = 0; i < frames; ++i) {
            m_scratch[2 * i] = noise(rng) * static_cast<float>(noiseAmplitude);
            m_scratch[2 * i + 1] = noise(rng) * static_cast<float>(noiseAmplitude);
        }

        for (Emitter& emitter : emitters) {
            // Frequencies are stored as a fraction of the sample rate, so an
            // emitter stays in view when the rate changes.
            double normalisedFrequency = emitter.frequencyHz;

            if (emitter.kind == Emitter::Kind::Drifting) {
                emitter.driftPhase += emitter.driftRate * static_cast<double>(frames) / sampleRate;
                normalisedFrequency += emitter.driftSpanHz * std::sin(emitter.driftPhase);
            }

            const double amplitude = emitter.amplitude * gainLinear;
            if (emitter.kind == Emitter::Kind::Pulsed) {
                const double phaseInPeriod = std::fmod(timeBase, emitter.pulsePeriod);
                if (phaseInPeriod > emitter.pulseWidth) {
                    continue; // silent between bursts
                }
            }

            // Incremental phasor rotation rather than sin/cos per sample: one
            // complex multiply instead of two transcendentals, roughly 20x
            // faster, and this loop runs over millions of samples per refresh.
            const double omega = kTwoPi * normalisedFrequency;
            const double stepReal = std::cos(omega);
            const double stepImag = std::sin(omega);

            double phasorReal = amplitude * std::cos(emitter.phase);
            double phasorImag = amplitude * std::sin(emitter.phase);

            for (std::size_t i = 0; i < frames; ++i) {
                m_scratch[2 * i] += static_cast<float>(phasorReal);
                m_scratch[2 * i + 1] += static_cast<float>(phasorImag);

                const double nextReal = phasorReal * stepReal - phasorImag * stepImag;
                phasorImag = phasorReal * stepImag + phasorImag * stepReal;
                phasorReal = nextReal;

                // Repeated complex multiplication slowly loses magnitude to
                // rounding. Renormalising periodically keeps the emitter's
                // level constant instead of fading over a long block.
                if ((i & 0xFFFU) == 0xFFFU) {
                    const double magnitude =
                        std::sqrt(phasorReal * phasorReal + phasorImag * phasorImag);
                    if (magnitude > 1e-12) {
                        const double correction = amplitude / magnitude;
                        phasorReal *= correction;
                        phasorImag *= correction;
                    }
                }
            }

            emitter.phase = std::fmod(emitter.phase + omega * static_cast<double>(frames), kTwoPi);
        }

        writeTemplate(frames);
    }

    /// Converts the float scratch into the native format once, into a template
    /// buffer that subsequent blocks memcpy from.
    void writeTemplate(std::size_t frames) {
        m_template.resize(frames * bytesPerFrame(m_format));
        m_templateFrames = frames;
        m_templateFormat = m_format;

        dsp::convertFromComplexFloat(m_scratch.data(), m_format, m_template.data(), frames);
    }

    SdrDeviceInfo m_info;
    std::vector<SdrParameter> m_parameters;

    /// Fixed after construction, so `selectedRxPort` hands out a view into it
    /// while the sweep thread is switching.
    std::vector<SdrRxPort> m_ports;
    std::atomic<std::size_t> m_rxPort{0};

    std::atomic<double> m_centerHz{100e6};
    std::atomic<double> m_sampleRate{20e6};
    std::atomic<int> m_gainDb{20};
    std::atomic<double> m_noiseFloorDb{-85.0};
    std::atomic<int> m_emitterCount{6};
    std::atomic<bool> m_simulateOverruns{false};
    SampleFormat m_format = SampleFormat::Cs8;

    mutable std::mutex m_emitterMutex;
    std::vector<Emitter> m_emitters;

    std::atomic<bool> m_streaming{false};
    std::jthread m_thread;
    BlockPool* m_pool = nullptr;
    IqCallback m_callback;
    std::size_t m_framesPerBlock = 262'144;
    std::uint64_t m_sequence = 0;

    std::vector<float> m_scratch;
    /// Native-format block reused between refreshes -- see generate().
    std::vector<std::byte> m_template;
    std::size_t m_templateFrames = 0;
    SampleFormat m_templateFormat = SampleFormat::Cs8;

    std::atomic<StreamCounters*> m_telemetry{nullptr};
};

class SyntheticFactory final : public ISdrDeviceFactory {
public:
    [[nodiscard]] std::string_view driver() const noexcept override { return "synthetic"; }
    [[nodiscard]] std::string displayName() const override { return "Synthetic signal generator"; }

    [[nodiscard]] std::vector<SdrDeviceInfo> enumerate() const override {
        return {SdrDeviceInfo{.driver = "synthetic",
                              .id = "synthetic-0",
                              .label = "Synthetic signal generator",
                              .serial = "SYNTH0000",
                              .hardwareRevision = "virtual",
                              .firmware = {.version = std::string(versionString())},
                              .minFrequencyHz = 1e6,
                              .maxFrequencyHz = 6e9,
                              .minSampleRate = 1e6,
                              .maxSampleRate = 200e6,
                              // Reports a USB 3.0-class budget so the link
                              // utilisation bar shows something meaningful
                              // when simulating high rates.
                              .linkCapacityBytesPerSec = 400'000'000,
                              .linkDescription = "virtual (USB 3.0 class)"}};
    }

    [[nodiscard]] Result<std::unique_ptr<ISdrDevice>> open(std::string_view id) override {
        std::vector<SdrDeviceInfo> devices = enumerate();
        if (!id.empty() && id != devices.front().id) {
            return fail<std::unique_ptr<ISdrDevice>>(ErrorCode::NotFound,
                                                     "no synthetic device '{}'", id);
        }
        return std::make_unique<SyntheticDevice>(devices.front());
    }
};

} // namespace

void registerSyntheticDevice() {
    SdrDeviceManager::instance().registerFactory(std::make_unique<SyntheticFactory>());
}

} // namespace sweeppp
