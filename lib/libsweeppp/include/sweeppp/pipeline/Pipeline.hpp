// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/BlockPool.hpp"
#include "sweeppp/core/EventBus.hpp"
#include "sweeppp/core/MpmcQueue.hpp"
#include "sweeppp/core/Telemetry.hpp"
#include "sweeppp/correction/Corrections.hpp"
#include "sweeppp/fft/IFftBackend.hpp"
#include "sweeppp/fft/Window.hpp"
#include "sweeppp/pipeline/FrameBus.hpp"
#include "sweeppp/sdr/ISdrDevice.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace sweeppp {

/// What to do when the workers cannot process every sample.
///
/// At 100 MS/s with a 4096-point FFT you would need ~24k FFTs per second per
/// full coverage; you cannot have all of it and a responsive display. The
/// design makes that an operator decision with a visible consequence rather
/// than silent loss.
enum class ThrottleMode : std::uint8_t {
    /// Process every Nth block, deterministically. Predictable duty cycle,
    /// and the resulting coverage is exactly 1/N -- the honest choice when a
    /// measurement needs to be reproducible.
    EveryNth,
    /// Process whatever the CPU allows, skipping when workers are busy.
    /// Maximum coverage, variable duty cycle. The default.
    Auto,
    /// Never skip. Back-pressures until blocks are dropped at the ring, which
    /// makes the loss visible in the drop counters instead of the FFT count.
    /// Only realistic at low sample rates.
    AllSamples,
};

[[nodiscard]] std::string_view toString(ThrottleMode mode) noexcept;

/// Workers the pipeline uses when `PipelineConfig::workerCount` is left at 0:
/// `hardware_concurrency - 2`, leaving one core for the UI and one for the
/// device's transfer thread.
///
/// Saturating every core with FFT workers reliably makes throughput *worse*:
/// the transfer thread gets scheduled late, USB transfers back up, and the
/// device overruns -- which shows up as dropped samples rather than as CPU
/// contention, so it is easy to misdiagnose.
///
/// Exposed because the FFT benchmark needs it. Comparing backends at one
/// thread answers a question nobody asked; the number that predicts a sweep is
/// the one measured at the worker count the sweep will actually use.
[[nodiscard]] std::uint32_t defaultWorkerCount();

struct PipelineConfig {
    std::uint32_t fftSize = 4096;
    WindowType window = WindowType::Hann;
    double windowBeta = 8.6;
    double overlap = 0.0;

    /// 0 means `defaultWorkerCount()`, leaving a core for the UI and one for
    /// the USB transfer thread. Oversubscribing here costs more in contention
    /// than it gains in throughput.
    std::uint32_t workerCount = 0;

    ThrottleMode throttleMode = ThrottleMode::Auto;
    std::uint32_t everyNth = 1;

    /// How many FFTs to average into one published frame. Averaging happens
    /// before publication so every consumer sees the same frame.
    std::uint32_t averageCount = 1;

    FftPlanQuality planQuality = FftPlanQuality::Balanced;

    /// Frames per second the pipeline aims to publish. Beyond this the display
    /// gains nothing and the extra frames only cost consumers work.
    double targetFrameRate = 60.0;

    /// Correction from dBFS to dBm, carried into every frame's config.
    double dbfsToDbmOffset = 0.0;
};

/// Turns native-format IQ blocks into published spectrum frames.
///
/// Threading model, and why:
///
///   device transfer thread -> enqueue block index -> MpmcQueue<IqBlock, 256> -> N workers
///
/// The transfer thread only ever pushes an index and returns. It never
/// converts, never allocates, never waits. Workers own the expensive work
/// (convert+window fused, FFT, dB conversion, averaging) and publish to the
/// FrameBus. Every place a block can be lost increments a distinct counter, so
/// "where did my samples go" always has an answer.
class Pipeline {
public:
    Pipeline(FrameBus& frameBus, Telemetry& telemetry, EventBus& eventBus);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    /// Prepares plans, windows and workers. Does not touch the device.
    [[nodiscard]] Status configure(IFftBackend& backend, const PipelineConfig& config);

    /// Starts the workers and begins streaming from `device`.
    [[nodiscard]] Status start(ISdrDevice& device, const StreamConfig& streamConfig);
    void stop();
    [[nodiscard]] bool running() const noexcept {
        return m_running.load(std::memory_order_acquire);
    }

    [[nodiscard]] const PipelineConfig& config() const noexcept { return m_config; }

    /// Frames per acquisition block, as passed to the last `start()`.
    ///
    /// Exposed because a sweep step has to stay tuned long enough for a whole
    /// block to be captured within it -- a block carries a single centre
    /// frequency, so one that straddles a retune is not attributable to either
    /// step. The sweep engine cannot compute this itself: the caller chooses
    /// the block size, and clamps it.
    [[nodiscard]] std::size_t framesPerBlock() const noexcept { return m_framesPerBlock; }

    /// Applies a new FFT size, window or throttle policy without restarting
    /// the device. Grid-affecting changes publish a ParameterChangedEvent so
    /// the session opens a new segment.
    [[nodiscard]] Status reconfigure(const PipelineConfig& config);

    /// Stops the device's stream, runs `between`, and starts it again.
    ///
    /// For a receive port that cannot be selected on a live stream. Workers,
    /// queue, block pool, FFT plan and the session all survive -- this is
    /// deliberately *not* stop()/start(), which drops the pool and would have
    /// the sweep engine reallocating megabytes mid-pass, nor
    /// `AppState::stop()/start()`, which opens a new session segment.
    ///
    /// A callback rather than a stop and a start the caller pairs up, so the
    /// stream cannot be left down by an early return in between. `between`
    /// failing is reported, but the stream is restarted either way.
    ///
    /// The caller is responsible for the samples in flight: blocks captured
    /// before this returns carry the *old* port's spectrum, and nothing here
    /// marks them.
    [[nodiscard]] Status cycleDeviceStream(const std::function<Status()>& between);

    /// Sets the tuning context stamped into each frame. The sweep engine calls
    /// this per step; a fixed-tune session calls it once.
    void setTuning(double centerHz, double spanHz, double sampleRate) noexcept;

    /// Gain stages recorded in every frame's config, for later interpretation.
    void setGains(std::vector<std::pair<std::string, double>> gains);

    /// Marks the current sweep pass and step, stamped into published frames.
    void setSweepPosition(std::uint64_t pass, std::uint32_t step, bool passComplete) noexcept;

    /// Which receiver corrections apply, effective from the next block. Held
    /// as one atomic byte, so switching one costs the hot path nothing and
    /// needs no restart -- unlike a `PipelineConfig` change.
    void setCorrectionSettings(CorrectionSettings settings) noexcept;
    [[nodiscard]] CorrectionSettings correctionSettings() const noexcept;

    /// The learned floor and spur list the flatten and mask switches apply.
    /// Null clears it. Survives `configure()` and `reconfigure()`, which only
    /// rebuild the workers.
    void setCorrections(std::shared_ptr<const CorrectionSet> set);
    [[nodiscard]] std::shared_ptr<const CorrectionSet> corrections() const;

    [[nodiscard]] std::uint32_t workerCount() const noexcept {
        return static_cast<std::uint32_t>(m_workers.size());
    }

private:
    /// Called from the device's transfer thread. Must not block.
    void onBlock(IqBlock&& block) noexcept;

    void workerLoop(std::stop_token stop, std::uint32_t workerIndex);
    void processBlock(IqBlock& block, std::uint32_t workerIndex);
    [[nodiscard]] bool shouldProcess(std::uint64_t blockSequence) noexcept;
    void publishFrame(const IqBlock& block, const std::vector<float>& bins, std::uint32_t averaged,
                      float clipped);
    void updateThrottleReason(ThrottleReason reason) noexcept;

    /// Per-worker scratch. Separate allocations per worker so no two workers
    /// share a cache line, and so one FFTW plan can serve all of them.
    struct Worker {
        std::vector<std::complex<float>> input;
        std::vector<std::complex<float>> output;
        std::vector<float> bins;
        std::vector<float> accumulator;
        std::uint32_t accumulated = 0;
    };

    FrameBus& m_frameBus;
    Telemetry& m_telemetry;
    EventBus& m_eventBus;

    PipelineConfig m_config;
    std::size_t m_framesPerBlock = 0;

    /// What the running stream was started with, so it can be started again
    /// without the caller having to remember.
    StreamConfig m_streamConfig;

    IFftBackend* m_backend = nullptr;
    std::shared_ptr<IFftPlan> m_plan;
    Window m_window;

    // Sized so a burst of blocks during a scheduling hiccup does not
    // immediately drop. Power of two, as the queue requires.
    //
    // Multi-consumer, not SPSC: one producer (the device transfer thread) but
    // N worker threads pulling. The queue carries the IqBlock itself rather
    // than an index into a side table -- a side table would need its own
    // "still in use" flag, and that flag would be the race.
    static constexpr std::size_t kQueueCapacity = 256;
    MpmcQueue<IqBlock, kQueueCapacity> m_queue;

    std::vector<std::jthread> m_workers;
    std::vector<Worker> m_scratch;

    std::atomic<bool> m_running{false};
    std::atomic<std::uint64_t> m_blockSequence{0};
    std::atomic<std::uint64_t> m_lastBlockSequence{0};
    std::atomic<std::uint64_t> m_frameSequence{0};
    std::atomic<std::uint64_t> m_lastPublishNs{0};
    std::atomic<std::uint32_t> m_everyNthCounter{0};
    std::atomic<ThrottleReason> m_lastThrottleReason{ThrottleReason::Stopped};

    mutable std::mutex m_configMutex;
    /// Held across the rate-limit check, sequence assignment and publish, so
    /// frame sequences are both gap-free and emitted in order. Several workers
    /// publish; without this they would allocate sequences in one order and
    /// deliver them in another, which would corrupt any consumer that assumes
    /// time moves forward -- the waterfall and the session file both do.
    mutable std::mutex m_publishMutex;

    /// `CorrectionSettings::toBits()`, read once per block and once per frame.
    std::atomic<std::uint8_t> m_correctionBits{0};
    /// The set and its per-grid scratch, both guarded by m_publishMutex: the
    /// corrections run inside the publish critical section, on the frame's
    /// own bins, so one scratch serves every worker.
    std::shared_ptr<const CorrectionSet> m_corrections;
    CorrectionScratch m_correctionScratch;
    AcquisitionConfig m_acquisitionConfig;
    std::atomic<std::uint64_t> m_sweepPass{0};
    std::atomic<std::uint32_t> m_sweepStep{0};
    std::atomic<bool> m_passComplete{false};

    ISdrDevice* m_device = nullptr;
    BlockPool* m_pool = nullptr;
    std::unique_ptr<BlockPool> m_ownedPool;
};

} // namespace sweeppp
