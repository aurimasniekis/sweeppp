// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/pipeline/Pipeline.hpp"

#include "sweeppp/core/Clock.hpp"
#include "sweeppp/core/Log.hpp"
#include "sweeppp/core/Toml.hpp"
#include "sweeppp/dsp/Convert.hpp"

#include <algorithm>
#include <cmath>

namespace sweeppp {

std::uint32_t defaultWorkerCount() {
    const unsigned int hardware = std::thread::hardware_concurrency();
    if (hardware <= 3) {
        return 1;
    }
    return hardware - 2;
}

std::string_view toString(ThrottleMode mode) noexcept {
    switch (mode) {
    case ThrottleMode::EveryNth:
        return "every-nth";
    case ThrottleMode::Auto:
        return "auto";
    case ThrottleMode::AllSamples:
        return "all-samples";
    }
    return "auto";
}

Pipeline::Pipeline(FrameBus& frameBus, Telemetry& telemetry, EventBus& eventBus)
    : m_frameBus(frameBus), m_telemetry(telemetry), m_eventBus(eventBus) {
}

Pipeline::~Pipeline() {
    stop();
}

Status Pipeline::configure(IFftBackend& backend, const PipelineConfig& config) {
    if (running()) {
        return fail(ErrorCode::Unavailable, "cannot reconfigure the pipeline while it is running");
    }

    PipelineConfig resolved = config;
    if (resolved.workerCount == 0) {
        resolved.workerCount = defaultWorkerCount();
    }
    if (resolved.everyNth == 0) {
        resolved.everyNth = 1;
    }
    if (resolved.averageCount == 0) {
        resolved.averageCount = 1;
    }

    if (!backend.supportsSize(resolved.fftSize)) {
        const std::size_t snapped = backend.snapSize(resolved.fftSize);
        logInfo("pipeline", "FFT size {} is not supported by {}; using {}", resolved.fftSize,
                backend.name(), snapped);
        resolved.fftSize = static_cast<std::uint32_t>(snapped);
    }

    auto window = Window::create(resolved.window, resolved.fftSize, resolved.windowBeta);
    if (!window) {
        return std::unexpected(window.error());
    }

    // Serialised, because the FFT benchmark plans on a thread of its own and
    // an operator can change FFT size while it runs. Uncontended in every
    // other case, which is every case there used to be.
    auto plan = createPlanSerialised(backend, {.size = resolved.fftSize,
                                               .batchCount = 1,
                                               .inverse = false,
                                               .inPlace = false,
                                               .quality = resolved.planQuality});
    if (!plan) {
        return std::unexpected(plan.error());
    }

    m_backend = &backend;
    m_config = resolved;
    m_window = std::move(*window);
    // shared_ptr, not unique_ptr: all workers execute this one plan
    // concurrently, which FFTW's fftwf_execute_dft explicitly permits and
    // which is the reason planning cost does not scale with worker count.
    m_plan = std::shared_ptr<IFftPlan>(std::move(*plan));

    m_scratch.assign(resolved.workerCount, Worker{});
    for (Worker& worker : m_scratch) {
        worker.input.resize(resolved.fftSize);
        worker.output.resize(resolved.fftSize);
        worker.bins.resize(resolved.fftSize);
        worker.accumulator.assign(resolved.fftSize, 0.0F);
        worker.accumulated = 0;
    }

    {
        const std::lock_guard lock(m_configMutex);
        m_acquisitionConfig.fftSize = resolved.fftSize;
        m_acquisitionConfig.window = resolved.window;
        m_acquisitionConfig.windowBeta = resolved.windowBeta;
        m_acquisitionConfig.windowEnbw = m_window.properties().enbw;
        m_acquisitionConfig.overlap = resolved.overlap;
        m_acquisitionConfig.dbfsToDbmOffset = resolved.dbfsToDbmOffset;
        if (m_acquisitionConfig.sampleRate > 0.0) {
            m_acquisitionConfig.rbwHz =
                m_window.resolutionBandwidth(m_acquisitionConfig.sampleRate);
        }
    }

    m_telemetry.process().workerCount.store(resolved.workerCount, std::memory_order_relaxed);

    logInfo("pipeline", "configured: {} point {} window, {} workers, throttle {}", resolved.fftSize,
            toString(resolved.window), resolved.workerCount, toString(resolved.throttleMode));

    return ok();
}

Status Pipeline::reconfigure(const PipelineConfig& config) {
    if (m_backend == nullptr) {
        return fail(ErrorCode::Unavailable, "pipeline has never been configured");
    }

    const bool wasRunning = running();
    ISdrDevice* device = m_device;

    // The stream the caller started, not a default one.
    //
    // m_streamConfig is stored by start() precisely so a restart need not be
    // told again, and the block size in it is a decision: a sweep sizes its
    // blocks to one step's collection window, because a block carries one
    // centre frequency and must not span a retune. Rebuilding a bare
    // StreamConfig here restored the 262144-frame default -- 13 ms at 20 MS/s,
    // a dozen retunes per block -- so every frame after any mid-sweep setting
    // change was rejected as unsettled or unattributed and the waterfall went
    // blank while the sweep carried on running.
    StreamConfig streamConfig = m_streamConfig;
    if (wasRunning) {
        streamConfig.format = device->nativeFormat();
        stop();
    }

    // An FFT size or window change redefines the frequency grid, so the
    // session must close its current segment and open a new one. Publishing
    // the event is what makes that happen without the pipeline knowing the
    // history store exists.
    if (config.fftSize != m_config.fftSize) {
        m_eventBus.publish(ParameterChangedEvent{.monotonicNs = monotonicNs(),
                                                 .key = "fft_size",
                                                 .value = std::format("{}", config.fftSize),
                                                 .gridAffecting = true,
                                                 .calibrationAffecting = false});
    }
    if (config.window != m_config.window) {
        m_eventBus.publish(ParameterChangedEvent{.monotonicNs = monotonicNs(),
                                                 .key = "window",
                                                 .value = std::string(toString(config.window)),
                                                 .gridAffecting = true,
                                                 .calibrationAffecting = false});
    }

    auto configured = configure(*m_backend, config);
    if (!configured) {
        return configured;
    }

    if (wasRunning && device != nullptr) {
        return start(*device, streamConfig);
    }
    return ok();
}

Status Pipeline::start(ISdrDevice& device, const StreamConfig& streamConfig) {
    if (running()) {
        return fail(ErrorCode::AlreadyExists, "pipeline is already running");
    }
    if (!m_plan) {
        return fail(ErrorCode::Unavailable, "pipeline must be configured before starting");
    }

    // Pool sized from the stream configuration, allocated once. Nothing on the
    // acquisition path allocates after this point.
    const std::size_t blockBytes =
        streamConfig.framesPerBlock * bytesPerFrame(device.nativeFormat());
    auto pool = BlockPool::create(blockBytes, static_cast<std::uint32_t>(streamConfig.blockCount));
    if (!pool) {
        return std::unexpected(pool.error().withContext("allocating the acquisition block pool"));
    }
    m_ownedPool = std::move(*pool);
    m_pool = m_ownedPool.get();
    m_device = &device;
    m_framesPerBlock = streamConfig.framesPerBlock;
    m_streamConfig = streamConfig;

    logInfo("pipeline", "block pool: {} x {} = {}", streamConfig.blockCount, blockBytes,
            toml_util::formatBytes(m_pool->totalBytes()));

    m_queue.clear();
    m_blockSequence.store(0, std::memory_order_relaxed);
    m_lastBlockSequence.store(0, std::memory_order_relaxed);
    m_frameSequence.store(0, std::memory_order_relaxed);
    m_everyNthCounter.store(0, std::memory_order_relaxed);
    m_lastPublishNs.store(0, std::memory_order_relaxed);

    m_running.store(true, std::memory_order_release);

    m_workers.clear();
    m_workers.reserve(m_config.workerCount);
    for (std::uint32_t i = 0; i < m_config.workerCount; ++i) {
        m_workers.emplace_back([this, i](std::stop_token stop) { workerLoop(stop, i); });
    }

    device.attachTelemetry(&m_telemetry.stream());

    auto started =
        device.start(*m_pool, streamConfig, [this](IqBlock&& block) { onBlock(std::move(block)); });
    if (!started) {
        stop();
        return started;
    }

    updateThrottleReason(m_config.throttleMode == ThrottleMode::EveryNth && m_config.everyNth > 1
                             ? ThrottleReason::EveryNth
                             : ThrottleReason::None);

    return ok();
}

Status Pipeline::cycleDeviceStream(const std::function<Status()>& between) {
    if (!running() || m_device == nullptr || m_pool == nullptr) {
        return fail(ErrorCode::Unavailable, "no stream to cycle");
    }

    ISdrDevice& device = *m_device;

    // Not `stop()`. The workers keep running and the queue keeps draining, so
    // whatever the old port had already delivered is still processed and
    // published -- which is what the sweep engine's per-step settle window is
    // there to sort out. Tearing the pool down here would free blocks the
    // workers are reading from.
    device.stop();
    device.attachTelemetry(nullptr);

    Status result = between ? between() : ok();

    // The format is asked again rather than reused: a driver is entitled to
    // deliver a different one now that whatever `between` did has happened,
    // and the pool was sized from the widest it declared.
    m_streamConfig.format = device.nativeFormat();

    device.attachTelemetry(&m_telemetry.stream());
    auto started = device.start(*m_pool, m_streamConfig,
                                [this](IqBlock&& block) { onBlock(std::move(block)); });
    if (!started) {
        device.attachTelemetry(nullptr);
        return std::unexpected(started.error().withContext("restarting the stream"));
    }

    return result;
}

void Pipeline::stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    if (m_device != nullptr) {
        // Stop the source first: no new blocks can arrive while the workers
        // wind down, so the in-flight table drains cleanly.
        m_device->stop();
        m_device->attachTelemetry(nullptr);
    }

    for (std::jthread& worker : m_workers) {
        worker.request_stop();
    }
    m_workers.clear();

    // Release anything still queued so the pool is fully returned. A leaked
    // block here would shrink the pool on the next start and look like an
    // unexplained drop rate. Safe now: producers and consumers have stopped.
    //
    // Discarded blocks are counted as dropped rather than silently forgotten,
    // so that delivered == processed + dropped still reconciles exactly after
    // a stop. Without this the tally is short by whatever was in flight, which
    // reads as an accounting leak when it is really just shutdown.
    IqBlock discarded;
    while (m_queue.pop(discarded)) {
        if (discarded.valid()) {
            m_telemetry.stream().samplesDropped.fetch_add(discarded.frames,
                                                          std::memory_order_relaxed);
            m_telemetry.stream().blocksDropped.fetch_add(1, std::memory_order_relaxed);
        }
        discarded = IqBlock{};
    }

    m_ownedPool.reset();
    m_pool = nullptr;
    m_device = nullptr;

    updateThrottleReason(ThrottleReason::Stopped);
}

void Pipeline::onBlock(IqBlock&& block) noexcept {
    // Runs on the device's USB transfer thread. Every path through this
    // function is bounded and allocation-free; blocking here would cause
    // device-side overruns, which is the failure this telemetry exists to
    // report rather than to cause.
    if (!m_running.load(std::memory_order_acquire) || !block.valid()) {
        return;
    }

    StreamCounters& stream = m_telemetry.stream();

    const std::uint64_t sequence = m_blockSequence.fetch_add(1, std::memory_order_relaxed);
    block.sequence = sequence;

    // Gap detection: on a device with no overrun reporting of its own (HackRF)
    // this is the only signal that the radio dropped data before we saw it.
    const std::uint64_t previous =
        m_lastBlockSequence.exchange(sequence, std::memory_order_relaxed);
    if (sequence > previous + 1) {
        stream.sequenceGaps.fetch_add(1, std::memory_order_relaxed);
    }

    stream.samplesDelivered.fetch_add(block.frames, std::memory_order_relaxed);
    stream.bytesDelivered.fetch_add(block.bytes(), std::memory_order_relaxed);
    stream.blocksDelivered.fetch_add(1, std::memory_order_relaxed);
    stream.ringFillFraction.store(m_queue.fillFraction(), std::memory_order_relaxed);

    // Settle blocks are counted as delivered but never processed -- the
    // radio's output during a retune is not trustworthy. They are accounted as
    // dropped so that delivered = processed + dropped still reconciles.
    if (block.settling) {
        stream.samplesDropped.fetch_add(block.frames, std::memory_order_relaxed);
        return;
    }

    if (!shouldProcess(sequence)) {
        // Deliberately skipped, not lost. Counted as dropped samples because
        // from the operator's point of view the data is equally not in the
        // display; the throttle reason is what distinguishes the two.
        m_telemetry.process().fftsSkipped.fetch_add(1, std::memory_order_relaxed);
        stream.samplesDropped.fetch_add(block.frames, std::memory_order_relaxed);
        return;
    }

    const std::size_t frames = block.frames;
    if (!m_queue.push(std::move(block))) {
        // Queue full: the workers are behind. Never wait -- drop, count, and
        // return immediately, because this is the device's transfer thread.
        stream.ringFullEvents.fetch_add(1, std::memory_order_relaxed);
        stream.samplesDropped.fetch_add(frames, std::memory_order_relaxed);
        stream.blocksDropped.fetch_add(1, std::memory_order_relaxed);
        updateThrottleReason(ThrottleReason::RingFull);
    }
}

bool Pipeline::shouldProcess(std::uint64_t blockSequence) noexcept {
    switch (m_config.throttleMode) {
    case ThrottleMode::AllSamples:
        return true;

    case ThrottleMode::EveryNth:
        // Deterministic 1/N coverage: reproducible, and exactly what the
        // operator asked for.
        return m_config.everyNth <= 1 || (blockSequence % m_config.everyNth) == 0;

    case ThrottleMode::Auto: {
        // Take what the workers can absorb. A mostly-full queue means they are
        // falling behind, so start skipping before blocks are lost outright --
        // a skipped block is accounted as "not processed", a dropped one as
        // "lost", and the distinction matters to the operator.
        const float fill = m_queue.fillFraction();
        if (fill > 0.75F) {
            updateThrottleReason(ThrottleReason::CpuLimited);
            return false;
        }
        return true;
    }
    }
    return true;
}

void Pipeline::workerLoop(std::stop_token stop, std::uint32_t workerIndex) {
    ProcessCounters& process = m_telemetry.process();

    while (!stop.stop_requested()) {
        IqBlock block;
        if (!m_queue.pop(block)) {
            // Nothing queued. Sleep briefly rather than spin: a busy-wait
            // across every worker would starve the device's transfer thread of
            // CPU, causing exactly the overruns this pipeline exists to avoid.
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }

        if (!block.valid()) {
            continue;
        }

        process.activeWorkers.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t busyFrom = monotonicNs();
        processBlock(block, workerIndex);
        process.workerBusyNs.fetch_add(monotonicNs() - busyFrom, std::memory_order_relaxed);
        process.activeWorkers.fetch_sub(1, std::memory_order_relaxed);
    }
}

void Pipeline::processBlock(IqBlock& block, std::uint32_t workerIndex) {
    Worker& worker = m_scratch[workerIndex];
    const std::uint32_t fftSize = m_config.fftSize;

    if (block.frames < fftSize) {
        // Not enough for one transform. Counted as skipped rather than
        // silently ignored -- and its samples counted as dropped, because
        // `delivered == processed + dropped` is the invariant the whole
        // accounting rests on and this block is not going to be processed.
        //
        // Only reachable when a block comes back short, which is what a device
        // overrun does: libbladeRF reports fewer samples than were asked for,
        // and the remainder of that read is below one FFT. Leaving the samples
        // uncounted read as an accounting leak of exactly the size of the
        // overrun -- a real fault reported as a bookkeeping one.
        m_telemetry.process().fftsSkipped.fetch_add(1, std::memory_order_relaxed);
        m_telemetry.stream().samplesDropped.fetch_add(block.frames, std::memory_order_relaxed);
        return;
    }

    const std::uint64_t started = monotonicNs();

    // Overlap turns into a hop size: 0.5 means each transform advances half a
    // window, doubling the FFT count for the same block.
    const auto hop = static_cast<std::size_t>(std::max(
        1.0, static_cast<double>(fftSize) * (1.0 - std::clamp(m_config.overlap, 0.0, 0.95))));

    const float clipped = dsp::clippedFraction(block.data(), block.format, block.frames);

    std::uint32_t transforms = 0;
    for (std::size_t offset = 0; offset + fftSize <= block.frames; offset += hop) {
        const std::byte* input = block.data() + offset * bytesPerFrame(block.format);

        // Convert and window in one pass -- see dsp/Convert.hpp for why these
        // are fused rather than sequential.
        dsp::convertAndWindow(input, block.format, m_window.coefficients(), worker.input.data(),
                              fftSize);

        m_plan->execute(worker.input.data(), worker.output.data());

        dsp::magnitudeToDbfs(worker.output.data(), worker.bins.data(), fftSize,
                             m_window.amplitudeScale());
        dsp::fftShift(worker.bins.data(), fftSize);

        if (worker.accumulated == 0) {
            worker.accumulator.assign(worker.bins.begin(), worker.bins.end());
        } else {
            dsp::accumulateMean(worker.accumulator.data(), worker.bins.data(), fftSize,
                                1.0F / static_cast<float>(worker.accumulated + 1));
        }
        ++worker.accumulated;
        ++transforms;

        if (worker.accumulated >= m_config.averageCount) {
            publishFrame(block, worker.accumulator, worker.accumulated, clipped);
            worker.accumulated = 0;
        }
    }

    ProcessCounters& process = m_telemetry.process();
    process.fftsComputed.fetch_add(transforms, std::memory_order_relaxed);
    process.samplesProcessed.fetch_add(block.frames, std::memory_order_relaxed);

    // Per *transform*, not per block. One block holds tens of transforms, so
    // timing the block would report a figure tens of times too large and make
    // the FFT look like the bottleneck when it is not. Timing each transform
    // individually would cost two clock reads per FFT on the hot path, so the
    // block is timed once and divided.
    if (transforms > 0) {
        const double blockUs = static_cast<double>(monotonicNs() - started) / 1000.0;
        process.fftLatency.record(static_cast<float>(blockUs / transforms));
    }
}

void Pipeline::publishFrame(const IqBlock& block, const std::vector<float>& bins,
                            std::uint32_t averaged, float clipped) {
    // One critical section covers the rate-limit check, the sequence
    // assignment and the publish. Splitting them would let two workers take
    // sequences 5 and 6 and then deliver 6 first -- consumers would see time
    // run backwards, and the per-consumer gap detector would report drops that
    // never happened. Contention is negligible: publication is capped at the
    // target frame rate, tens per second.
    const std::lock_guard publishLock(m_publishMutex);

    const std::uint64_t now = monotonicNs();
    const auto minimumInterval = static_cast<std::uint64_t>(
        m_config.targetFrameRate > 0.0 ? 1e9 / m_config.targetFrameRate : 0.0);

    const std::uint64_t last = m_lastPublishNs.load(std::memory_order_relaxed);
    if (minimumInterval > 0 && last != 0 && now - last < minimumInterval) {
        return;
    }
    m_lastPublishNs.store(now, std::memory_order_relaxed);

    auto frame = std::make_shared<SpectrumFrame>();
    frame->sequence = m_frameSequence.fetch_add(1, std::memory_order_relaxed) + 1;

    // The block's *capture* time, not now.
    //
    // Publish time can be milliseconds later -- past the queue, the workers and
    // the averaging -- and by then the radio may have retuned several times.
    // Anything that reasons about when a sample was taken (the sweep engine's
    // settle-window discard, the session file's time axis, replay pacing) needs
    // the moment it left the antenna, not the moment it reached a consumer.
    frame->hostTimeNs = block.hostTimeNs != 0 ? block.hostTimeNs : now;
    frame->wallTimeNs = wallClockNs();
    frame->deviceTimeNs = block.deviceTimeNs;
    frame->sweepPass =
        block.sweepPass != 0 ? block.sweepPass : m_sweepPass.load(std::memory_order_relaxed);
    frame->sweepStep =
        block.sweepStep != 0 ? block.sweepStep : m_sweepStep.load(std::memory_order_relaxed);
    frame->passComplete = m_passComplete.exchange(false, std::memory_order_relaxed);
    frame->binsDbfs = bins;
    frame->averageCount = averaged;
    frame->clippedFraction = clipped;

    {
        const std::lock_guard lock(m_configMutex);
        frame->config = m_acquisitionConfig;
    }

    // Prefer the block's own tuning: during a sweep the centre changes every
    // few milliseconds, and the block records what was true when it was
    // captured rather than what is true now.
    const double centerHz = block.centerHz != 0.0 ? block.centerHz : frame->config.centerHz;
    const double sampleRate = block.sampleRate != 0.0 ? block.sampleRate : frame->config.sampleRate;

    frame->config.centerHz = centerHz;
    frame->config.sampleRate = sampleRate;
    frame->config.spanHz = sampleRate;
    frame->config.rbwHz = sampleRate > 0.0 ? sampleRate * frame->config.windowEnbw /
                                                 static_cast<double>(m_config.fftSize)
                                           : 0.0;

    frame->binWidthHz = sampleRate / static_cast<double>(bins.size());
    frame->startHz = centerHz - sampleRate * 0.5;

    m_telemetry.process().framesPublished.fetch_add(1, std::memory_order_relaxed);
    m_frameBus.publish(frame);
}

void Pipeline::setTuning(double centerHz, double spanHz, double sampleRate) noexcept {
    const std::lock_guard lock(m_configMutex);
    m_acquisitionConfig.centerHz = centerHz;
    m_acquisitionConfig.spanHz = spanHz;
    m_acquisitionConfig.sampleRate = sampleRate;
    m_acquisitionConfig.rbwHz = sampleRate > 0.0 ? sampleRate * m_acquisitionConfig.windowEnbw /
                                                       static_cast<double>(m_config.fftSize)
                                                 : 0.0;
    m_telemetry.stream().configuredSps.store(sampleRate, std::memory_order_relaxed);
}

void Pipeline::setGains(std::vector<std::pair<std::string, double>> gains) {
    const std::lock_guard lock(m_configMutex);
    m_acquisitionConfig.gains = std::move(gains);
}

void Pipeline::setSweepPosition(std::uint64_t pass, std::uint32_t step,
                                bool passComplete) noexcept {
    m_sweepPass.store(pass, std::memory_order_relaxed);
    m_sweepStep.store(step, std::memory_order_relaxed);
    if (passComplete) {
        m_passComplete.store(true, std::memory_order_relaxed);
        m_telemetry.process().sweepPassesCompleted.fetch_add(1, std::memory_order_relaxed);
    }
}

void Pipeline::updateThrottleReason(ThrottleReason reason) noexcept {
    const ThrottleReason previous =
        m_lastThrottleReason.exchange(reason, std::memory_order_relaxed);
    m_telemetry.process().throttleReason.store(reason, std::memory_order_relaxed);

    if (previous == reason) {
        return;
    }

    // Publishing on change only: the reason is evaluated per block, and an
    // event per block would swamp the event stream and the session file.
    m_eventBus.publish(ThrottleChangedEvent{.monotonicNs = monotonicNs(),
                                            .reason = std::string(toString(reason)),
                                            .processedFraction =
                                                m_telemetry.snapshot().process.processedFraction});
}

} // namespace sweeppp
