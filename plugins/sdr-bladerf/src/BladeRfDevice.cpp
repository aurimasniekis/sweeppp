// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// Native BladeRF driver, on libbladeRF.
//
// Differs from HackRF in two ways that matter to the pipeline:
//
//  * Samples are CS16, not CS8 -- twice the bytes per sample, so the link
//    budget is tighter at the same rate.
//  * The device reports overruns explicitly through stream metadata. That is
//    strictly better than HackRF, where the only signal is a host-side gap in
//    block sequence numbers, and it is wired straight into the same telemetry
//    counter so the Performance panel reads identically for both.
#include "BladeRfDriver.hpp"
#include "BladeRfVersions.hpp"
#include "sweeppp/core/Clock.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <optional>
#include <thread>
#include <tuple>
#include <utility>

namespace sweeppp::blade {
namespace {

// The version gates these use are in BladeRfVersions.hpp, which needs them for
// the FPGA-size table.
inline constexpr bool kPackedLinkSupported = SWEEPPP_BLADERF_HAS_PACKED != 0;

/// bladerf_enable_feature() where the library has it, and a refusal where it
/// does not.
///
/// Only ever reached to turn the feature *off*, or after
/// probeOversampleMaxRate() reported a range -- which it cannot do on a
/// library without the call, so the refusal is unreachable rather than a
/// behaviour change.
[[nodiscard]] int enableOversample([[maybe_unused]] bladerf* device, [[maybe_unused]] bool enable) {
#if SWEEPPP_BLADERF_HAS_OVERSAMPLE
    return bladerf_enable_feature(device, BLADERF_FEATURE_OVERSAMPLE, enable);
#else
    return BLADERF_ERR_UNSUPPORTED;
#endif
}

[[nodiscard]] std::string bladeError(int status) {
    return bladerf_strerror(status);
}

/// Practical throughput ceilings per link speed, in bytes per second.
///
/// Both are reachable: a bladeRF plugged into a USB 2.0 port still enumerates
/// and still reports a 61.44 MS/s ceiling, and CS16 at even a tenth of that
/// rate does not fit through high speed. Which one applies has to be asked of
/// the device, or the Performance panel's utilisation bar reads a comfortable
/// 10% while the radio overruns continuously.
inline constexpr std::uint64_t kSuperSpeedBytesPerSec = 400'000'000ULL;
inline constexpr std::uint64_t kHighSpeedBytesPerSec = 35'000'000ULL;

/// The top of the AD9361's ordinary range. Above it the radio only runs in the
/// oversampling mode, and libbladeRF refuses the rate until the feature is on.
inline constexpr double kNormalMaxRate = 61.44e6;

/// The fewest frames worth asking libbladeRF for in one call.
///
/// Its synchronous interface is not built for tiny reads. A sweep sizes its
/// blocks to one FFT so that a block cannot span a retune, and at a fine RBW
/// that bottoms out at the host's 2048-frame floor -- which at 122.88 MS/s is
/// 17 us, or sixty thousand calls a second. libbladeRF answers that with
/// "An unexpected error occurred" and then stops answering at all, taking the
/// stream down with it. It also rounds its own buffer up and says so
/// ("Requested buffer size 2048 samples is invalid. Adjusting to 4096"), so
/// reads below that are partial-buffer reads on every single call.
///
/// 16384 frames is 133 us at 122.88 MS/s and 267 us at 61.44 -- a few thousand
/// calls a second, which the interface handles without complaint.
inline constexpr std::size_t kMinReadFrames = 16'384;

/// The unit libbladeRF sizes its synchronous buffer in, in samples.
///
/// Ask for anything else and it rounds UP and says so -- "Requested buffer size
/// 18432 samples is invalid. Adjusting to 20480" -- which leaves its buffer
/// bigger than the reads we then issue. Every read is a partial-buffer read
/// from that point on, and libbladeRF eventually answers one with "An
/// unexpected error occurred" and stops delivering, taking the stream down.
///
/// The failure is intermittent, which is what made it confusing: it depends on
/// where the reads happen to fall against the buffer boundary. 4096 is also
/// what SC16_Q11_PACKED documents for itself ("buffer length must be a multiple
/// of 4096 samples"); that rule is not specific to packing.
///
/// The host's block size is `fftSize * averageCount` and lands wherever that
/// product falls -- 18451 for a 10 kHz RBW at 122.88 MS/s -- so it can never be
/// relied on to be a multiple of anything.
inline constexpr std::size_t kSyncBufferMultiple = 4096;

/// The largest legal read that fits in `frames`, never below the minimum.
///
/// Rounded DOWN, so the buffer libbladeRF ends up with is exactly the one asked
/// for and every read consumes a whole one.
[[nodiscard]] constexpr std::size_t alignedReadFrames(std::size_t frames) noexcept {
    const std::size_t rounded = frames / kSyncBufferMultiple * kSyncBufferMultiple;
    return std::max(rounded, kMinReadFrames);
}

/// The top of this board's oversampling range, or 0 when it has none.
///
/// Asked of the hardware rather than assumed. The feature belongs to the
/// AD9361, so it is a bladeRF 2.0 thing -- but whether a given FPGA image
/// implements it is not something the board name settles, and libbladeRF only
/// reports the extended range while the feature is on. Two calls at open beat
/// a constant that is wrong for somebody.
///
/// Turned back off before returning: the device comes up in its ordinary mode,
/// and the rate that is about to be applied decides whether it stays there.
[[nodiscard]] double probeOversampleMaxRate(bladerf* device, bool micro) {
    if (!micro) {
        return 0.0;
    }
    if (enableOversample(device, true) != 0) {
        return 0.0;
    }

    double maximum = 0.0;
    const bladerf_range* range = nullptr;
    if (bladerf_get_sample_rate_range(device, BLADERF_CHANNEL_RX(0), &range) == 0 &&
        range != nullptr) {
        maximum = static_cast<double>(range->max);
    }

    (void)enableOversample(device, false);
    return maximum;
}

/// How the samples reach the host, which the sample rate selects.
///
/// Not a preference: each is the only arrangement that works at its rate. The
/// AD9361 tops out at 61.44 MS/s in its ordinary mode, and the oversampling
/// mode that goes past that *is* 8-bit -- libbladeRF calls the feature
/// "Enforces AD9361 OC and 8bit mode", and asking for SC16 there is not an
/// option to weigh up, it is a configuration the radio rejects.
enum class LinkMode {
    /// SC16_Q11_META. Metadata carries the overrun flag and the hardware
    /// timestamp, so this is the default wherever it fits.
    Normal,
    /// SC16_Q11_PACKED. 12-bit on the wire, SC16 by the time it reaches us --
    /// libbladeRF unpacks. Buys 33% more headroom on a bandwidth-limited link
    /// and costs the metadata, because there is no packed META variant.
    Packed,
    /// SC8_Q7_META, above 61.44 MS/s. Half the bytes per sample, so twice the
    /// rate over the same link, at 8 bits of dynamic range.
    Oversampled,
};

class BladeRfDevice final : public plugin::Radio {
public:
    BladeRfDevice(bladerf* device, SdrDeviceInfo info, double oversampleMaxRate)
        : m_device(device), m_info(std::move(info)), m_oversampleMaxRate(oversampleMaxRate) {
        buildPorts();
        buildParameters();
    }

    ~BladeRfDevice() override {
        BladeRfDevice::stop();
        if (m_device != nullptr) {
            bladerf_close(m_device);
        }
    }

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
        if (key == "packed_link") {
            return SdrValue{m_packedRequested.load()};
        }
        if (key == "bandwidth") {
            return SdrValue{m_bandwidthHz.load()};
        }
        if (key == "gain") {
            return SdrValue{std::int64_t{m_gain.load()}};
        }
        if (key == "gain_mode") {
            return SdrValue{gainModeName(m_gainMode.load())};
        }
        if (key == "bias_tee") {
            return SdrValue{m_biasTee.load()};
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
            return retune(asDouble(*coerced));
        }
        if (key == "packed_link") {
            if constexpr (!kPackedLinkSupported) {
                return fail(ErrorCode::InvalidArgument,
                            "the packed link needs libbladeRF 2.6.0 or newer; this build links "
                            "API {:#010x}",
                            static_cast<unsigned>(LIBBLADERF_API_VERSION));
            }
            m_packedRequested.store(asBool(*coerced));
            return ok();
        }
        if (key == "sample_rate") {
            const double wanted = asDouble(*coerced);

            // The oversampling feature has to be on *before* a rate above the
            // ordinary range is accepted -- libbladeRF refuses the rate
            // outright otherwise, naming both ranges in its own log.
            if (const Status switched = applyOversample(wanted > kNormalMaxRate); !switched) {
                return switched;
            }

            bladerf_sample_rate actual = 0;
            const int status = bladerf_set_sample_rate(
                m_device, rxChannel(), static_cast<bladerf_sample_rate>(wanted), &actual);
            if (status != 0) {
                // Back to whatever the rate that is still set needs, so a
                // refused request does not leave the radio in a mode nothing
                // is going to use.
                (void)applyOversample(m_sampleRate.load() > kNormalMaxRate);
                return fail(ErrorCode::DeviceError, "set sample rate: {}", bladeError(status));
            }
            // The radio reports what it actually achieved, which is not always
            // what was asked for; storing the request would put every derived
            // figure -- RBW, span, the frequency axis -- slightly out.
            m_sampleRate.store(static_cast<double>(actual));
            if (m_bandwidthAuto.load()) {
                applyAutoBandwidth(static_cast<double>(actual));
            }
            return ok();
        }
        if (key == "bandwidth") {
            const double requested = asDouble(*coerced);
            m_bandwidthAuto.store(requested <= 0.0);
            if (requested <= 0.0) {
                applyAutoBandwidth(m_sampleRate.load());
                return ok();
            }
            bladerf_bandwidth actual = 0;
            const int status = bladerf_set_bandwidth(
                m_device, rxChannel(), static_cast<bladerf_bandwidth>(requested), &actual);
            if (status != 0) {
                return fail(ErrorCode::DeviceError, "set bandwidth: {}", bladeError(status));
            }
            m_bandwidthHz.store(static_cast<double>(actual));
            return ok();
        }
        if (key == "gain") {
            // Naming a gain is asking for that gain, which no automatic mode
            // will honour -- it moves the gain again on the next burst. So the
            // radio is taken off automatic here rather than accepting a number
            // it is about to overwrite, and the mode control follows.
            if (m_gainMode.load() != static_cast<int>(BLADERF_GAIN_MGC)) {
                const int mode = bladerf_set_gain_mode(m_device, rxChannel(), BLADERF_GAIN_MGC);
                if (mode != 0) {
                    return fail(ErrorCode::DeviceError, "set gain mode: {}", bladeError(mode));
                }
                m_gainMode.store(static_cast<int>(BLADERF_GAIN_MGC));
            }

            const int status =
                bladerf_set_gain(m_device, rxChannel(), static_cast<bladerf_gain>(asInt(*coerced)));
            if (status != 0) {
                return fail(ErrorCode::DeviceError, "set gain: {}", bladeError(status));
            }
            m_gain.store(static_cast<int>(asInt(*coerced)));
            return ok();
        }
        if (key == "gain_mode") {
            const std::string name = asString(*coerced);
            const auto match = std::ranges::find_if(
                m_gainModes, [&name](const auto& candidate) { return candidate.first == name; });
            if (match == m_gainModes.end()) {
                return fail(ErrorCode::InvalidArgument, "no gain mode '{}'", name);
            }

            const int status = bladerf_set_gain_mode(m_device, rxChannel(), match->second);
            if (status != 0) {
                return fail(ErrorCode::DeviceError, "set gain mode: {}", bladeError(status));
            }
            m_gainMode.store(static_cast<int>(match->second));

            // The gain an automatic mode settles on is the radio's own and it
            // moves, so the manual control shows where the gain actually is
            // rather than where it was last put by hand.
            bladerf_gain gain = 0;
            if (bladerf_get_gain(m_device, rxChannel(), &gain) == 0) {
                m_gain.store(static_cast<int>(gain));
            }
            return ok();
        }
        if (key == "bias_tee") {
            const bool enabled = asBool(*coerced);
            const int status = bladerf_set_bias_tee(m_device, rxChannel(), enabled);
            if (status != 0) {
                return fail(ErrorCode::DeviceError, "set bias tee: {}", bladeError(status));
            }
            m_biasTee.store(enabled);
            return ok();
        }

        return fail(ErrorCode::NotFound, "no parameter '{}'", key);
    }

    /// Cs16 normally, Cs8 in the oversampling mode -- which is the whole
    /// reason the ABI carries a format per block rather than per driver.
    [[nodiscard]] SampleFormat nativeFormat() const noexcept override {
        return linkMode() == LinkMode::Oversampled ? SampleFormat::Cs8 : SampleFormat::Cs16;
    }

    [[nodiscard]] Status start(plugin::Stream& stream, const StreamConfig& config) override {
        if (m_streaming.load()) {
            return fail(ErrorCode::AlreadyExists, "already streaming");
        }

        // Synchronous interface with metadata wherever the format has a META
        // variant: metadata is what carries the overrun flag and the device
        // timestamp, both of which the session format records. SC16_Q11_PACKED
        // has no such variant, which is the whole cost of choosing it.
        const LinkMode mode = linkMode();

        // Aligned to libbladeRF's metadata block and never below what its
        // sync interface will service. A sweep with a fine RBW asks for blocks
        // far smaller than this; those runs read into a staging buffer and copy
        // out, and everything else still fills the pool block directly with no
        // copy at all.
        std::size_t buffered =
            std::clamp<std::size_t>(alignedReadFrames(config.framesPerBlock), 1024, 1U << 20U);
        if (mode == LinkMode::Packed) {
            // A multiple of 4096 samples, as the packed format requires.
            buffered =
                std::max(kSyncBufferMultiple, buffered / kSyncBufferMultiple * kSyncBufferMultiple);
        }

        // Written as statements rather than a conditional chain because two of
        // the three enumerators are absent on an old libbladeRF, and a #if
        // inside a ternary reads as neither one thing nor the other. The modes
        // they serve are unreachable there anyway: both are gated on a sample
        // rate the library will not accept without the oversampling feature.
        bladerf_format format = BLADERF_FORMAT_SC16_Q11_META;
#if SWEEPPP_BLADERF_HAS_OVERSAMPLE
        if (mode == LinkMode::Oversampled) {
            format = BLADERF_FORMAT_SC8_Q7_META;
        }
#endif
#if SWEEPPP_BLADERF_HAS_PACKED
        if (mode == LinkMode::Packed) {
            format = BLADERF_FORMAT_SC16_Q11_PACKED;
        }
#endif

        m_activeStreamFormat = format;
        m_readFrames = buffered;

        if (const int status = configureStream(); status != 0) {
            return fail(ErrorCode::DeviceError, "sync config: {}", bladeError(status));
        }

        int status = 0;

        // Latched for the run: the receive thread must not see the mode change
        // under it, and the rate that selects it cannot move while streaming.
        m_activeMode = mode;
        m_activeFormat = nativeFormat();

        // Somewhere to put a read that has nowhere else to go. Always
        // allocated: the whole point is that it is available at the moment the
        // pool is not.
        m_discard.assign(m_readFrames * bytesPerFrame(m_activeFormat), std::byte{0});

        m_staging.clear();
        m_stagedFrames = 0;
        m_stagedTaken = 0;
        m_readFailures = 0;
        m_consecutiveFailures = 0;
        // Zero-copy only when one read fills exactly one block. Anything else
        // is staged and copied, because a block has to be handed over FULL:
        // the pipeline needs at least one FFT in it, and the block size is one
        // FFT, so a block short by even a frame is a block it can only drop.
        if (config.framesPerBlock != m_readFrames) {
            m_staging.resize(m_readFrames * bytesPerFrame(m_activeFormat));
            log().debug("blocks of {} frames do not match the {}-frame read, so reads are "
                        "staged and blocks filled from them",
                        config.framesPerBlock, m_readFrames);
        }

        if (mode == LinkMode::Oversampled && m_packedRequested.load()) {
            // Not a refusal, but not silent either: the oversampling mode
            // dictates SC8_Q7, so there is nothing left for packing to do.
            log().info("packed link ignored above {:g} MS/s -- the 8-bit mode sets the format",
                       kNormalMaxRate / 1e6);
        }

        if (mode == LinkMode::Packed) {
            // Said out loud, because the Performance panel will otherwise show
            // a confident "0 device overruns" that means "none reported" and
            // reads as "none happened".
            log().info("packed link: samples are 12-bit over USB, and this format carries no "
                       "metadata -- device overruns and hardware timestamps are unavailable "
                       "for this run");
        }

        status = bladerf_enable_module(m_device, rxChannel(), true);
        if (status != 0) {
            return fail(ErrorCode::DeviceError, "enable RX: {}", bladeError(status));
        }

        m_stream = &stream;
        m_framesPerBlock = config.framesPerBlock;
        m_sequence = 0;
        m_streaming.store(true);

        m_thread = std::jthread([this](std::stop_token stop) { receiveLoop(stop); });
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
        if (m_device != nullptr) {
            bladerf_enable_module(m_device, rxChannel(), false);
        }
        // The receive thread is joined above, so nothing of ours is still
        // touching the stream -- which is what the host requires before it
        // tears it down.
        m_stream = nullptr;
    }

    [[nodiscard]] bool streaming() const noexcept override { return m_streaming.load(); }

    [[nodiscard]] Status retune(double centerHz) override {
        const int status =
            bladerf_set_frequency(m_device, rxChannel(), static_cast<bladerf_frequency>(centerHz));
        if (status != 0) {
            return fail(ErrorCode::DeviceError, "set frequency: {}", bladeError(status));
        }
        m_centerHz.store(centerHz);
        return ok();
    }

    [[nodiscard]] double deliveryGranularitySeconds(double sampleRate) const noexcept override {
        // What one read covers, which is the finest a step can be resolved.
        //
        // This was missing entirely, and the base class reads a missing answer
        // as "delivers finely enough not to constrain the sweep" -- which for
        // libbladeRF is not true at any rate and is badly untrue at the top of
        // the range. The engine then retuned faster than the driver could
        // attribute samples, so a read spanned several steps and most of them
        // were never measured.
        return sampleRate > 0.0 ? static_cast<double>(kMinReadFrames) / sampleRate : 0.0;
    }

    [[nodiscard]] double retuneSettleSeconds() const noexcept override {
        // Roughly an order of magnitude faster to settle than a HackRF, which
        // is why this figure is per-device rather than a shared constant: using
        // HackRF's here would throw away most of the BladeRF's sweep rate.
        return 30e-6;
    }

private:
    /// The rates the panel offers.
    ///
    /// The ordinary range laddered at 2 MHz, then the two 8-bit rates if this
    /// board has an oversampling mode. Only two of those rather than a ladder:
    /// past 61.44 MS/s the interesting question is "the most this can do" and
    /// not "which of thirty steps", and every entry there costs half the
    /// dynamic range.
    [[nodiscard]] std::vector<SdrEnumValue> buildRateChoices() const {
        std::vector<SdrEnumValue> choices =
            sampleRateChoices(steppedSampleRates(m_info.minSampleRate, m_info.maxSampleRate, 2e6));

        if (m_oversampleMaxRate <= kNormalMaxRate) {
            return choices;
        }

        // Labelled so the trade is visible in the closed box rather than
        // discovered in the noise floor: these rates are 8 bits.
        for (const double rate : {110e6, m_oversampleMaxRate}) {
            if (rate <= kNormalMaxRate || rate > m_oversampleMaxRate) {
                continue;
            }
            std::vector<SdrEnumValue> one = sampleRateChoices(std::array{rate});
            one.front().label += " (8-bit)";
            one.front().description =
                "Above 61.44 MS/s the radio runs its oversampling mode, which is 8-bit: twice "
                "the rate through the same link, half the dynamic range.";
            choices.push_back(std::move(one.front()));
        }
        return choices;
    }

    static constexpr unsigned int kStreamTimeoutMs = 3'000;

    /// How much USB buffering to ask libbladeRF for, in bytes.
    ///
    /// Sized in bytes rather than in buffers because a buffer is one read, and
    /// a read is 32 KB when sweeping at a fine RBW but 1 MB at a fixed tune --
    /// a fixed count of 256 would be 8 MB of one and 268 MB of the other.
    ///
    /// It is worth spending. A retune is a control transfer on the same bus
    /// that is carrying 245 MB/s of samples, and the stall it causes is what
    /// the device FIFO has to ride out. Over a 70 MHz - 6 GHz sweep at
    /// 122.88 MS/s, three runs of each: 2 MB of buffering gave 202, 102 and 314
    /// device overruns per 20 s, and 8 MB gave 43, 70 and 56. The count is
    /// noisy enough that a single run proves nothing, which is why those are
    /// written out. 16 MB was no better than 8.
    static constexpr std::size_t kTargetBufferBytes = 8U << 20U;

    /// What one receive-port change costs, end to end.
    ///
    /// Not a settle time: the switch disables the module, reconfigures the sync
    /// interface -- which reallocates and re-submits the USB buffers sized by
    /// `kTargetBufferBytes` above -- and enables it again. The planner charges
    /// this per transition and the Analysis panel prints the result, so an
    /// operator can see before pressing Start that a plan whose antennas
    /// interleave twenty times spends four seconds a pass switching.
    ///
    /// A conservative figure rather than a measured one, and deliberately so
    /// until it is measured on hardware: over-estimating makes the predicted
    /// pass time pessimistic, which is the direction that does not surprise
    /// anybody. Measure it on a 2.0 micro and put the number here.
    static constexpr double kPortSwitchSeconds = 0.2;

    /// (Re)configures the synchronous stream from the latched settings.
    [[nodiscard]] int configureStream() const {
        const std::size_t bufferBytes = m_readFrames * bytesPerFrame(m_activeFormat);
        const auto buffers = static_cast<unsigned int>(std::clamp<std::size_t>(
            kTargetBufferBytes / std::max<std::size_t>(bufferBytes, 1), 16, 256));

        // libbladeRF requires fewer transfers than buffers; a quarter keeps
        // enough requests in flight to cover a stall without starving it of
        // buffers for them to land in.
        const unsigned int transfers = std::max(8U, buffers / 4U);

        return bladerf_sync_config(m_device, BLADERF_RX_X1, m_activeStreamFormat, buffers,
                                   static_cast<unsigned int>(m_readFrames), transfers,
                                   kStreamTimeoutMs);
    }

    /// A block, waiting briefly for one rather than giving up at once.
    ///
    /// The pool empties in bursts: a retune is a control transfer on the bus
    /// carrying the samples, and when it clears libbladeRF hands over the
    /// backlog in a rush that outruns the workers for a moment. Those moments
    /// are hundreds of microseconds and cost nothing to wait out, because
    /// libbladeRF is still buffering behind us -- 64 buffers is some
    /// milliseconds of cushion at any rate this radio reaches.
    ///
    /// The budget is what separates that from a real backlog. Spending it
    /// loses nothing; exceeding it means the consumer is genuinely behind, and
    /// then the caller drains and drops rather than letting the FIFO overrun.
    [[nodiscard]] plugin::Block acquireWithinBudget(const std::stop_token& stop) {
        constexpr std::uint64_t kBudgetNs = 1'000'000; // 1 ms, well inside the cushion

        const std::uint64_t deadline = monotonicNs() + kBudgetNs;
        while (!stop.stop_requested()) {
            plugin::Block block = m_stream->acquire();
            if (block.valid() || monotonicNs() >= deadline) {
                return block;
            }
            std::this_thread::yield();
        }
        return {};
    }

    /// Reads one buffer and throws it away, to keep the radio draining when
    /// there is nowhere to put the data. Returns the frames lost.
    [[nodiscard]] std::size_t drainAndDiscard(const std::stop_token& stop) {
        const bool metadataCarried = m_activeMode != LinkMode::Packed;
        bladerf_metadata metadata{};
        metadata.flags = static_cast<std::uint32_t>(BLADERF_META_FLAG_RX_NOW);

        const int status =
            bladerf_sync_rx(m_device, m_discard.data(), static_cast<unsigned int>(m_readFrames),
                            metadataCarried ? &metadata : nullptr, 3'000);
        if (status != 0) {
            if (!stop.stop_requested()) {
                reportReadFailure(status, m_readFrames);
                recoverIfWedged(stop);
            }
            return 0;
        }

        // A gap the host caused, so the sequence number moves with it: the
        // samples after it are not continuous with the ones before, and
        // pretending otherwise would hide the loss from gap detection.
        ++m_sequence;
        return metadataCarried && metadata.actual_count > 0
                   ? std::min<std::size_t>(metadata.actual_count, m_readFrames)
                   : m_readFrames;
    }

    /// Rebuilds the stream after libbladeRF has given up on it.
    ///
    /// "Transfer callback occurred out of order" followed by "Timed out while
    /// stopping worker. Canceling thread." is its USB layer losing its place at
    /// full rate. Once that has happened the worker is gone and every later
    /// read fails, so without this the sweep shows nothing at all while the log
    /// repeats the same warning every three seconds -- which is the shape the
    /// operator sees as "it just stopped working".
    ///
    /// Rare, and not caused by how the interface is being driven: the read that
    /// fails is a whole, correctly aligned buffer.
    [[nodiscard]] bool restartStream() {
        (void)bladerf_enable_module(m_device, rxChannel(), false);

        if (const int status = configureStream(); status != 0) {
            log().error("could not rebuild the stream: {}", bladeError(status));
            return false;
        }
        if (const int status = bladerf_enable_module(m_device, rxChannel(), true); status != 0) {
            log().error("could not re-enable RX: {}", bladeError(status));
            return false;
        }

        // Whatever was half-consumed belonged to the stream that just died.
        m_stagedFrames = 0;
        m_stagedTaken = 0;
        log().info("stream rebuilt after libbladeRF dropped it");
        return true;
    }

    /// Rebuilds the stream once libbladeRF has failed twice running.
    ///
    /// Two rather than one because a single failure can be a timeout with the
    /// radio simply quiet; two in a row is the worker having gone.
    void recoverIfWedged(const std::stop_token& stop) {
        if (++m_consecutiveFailures < 2 || stop.stop_requested()) {
            return;
        }
        m_consecutiveFailures = 0;
        (void)restartStream();
    }

    /// One read failure, described well enough to act on.
    ///
    /// "An unexpected error occurred" on its own says nothing, and it repeats
    /// every timeout until somebody stops the sweep -- so the first one is
    /// logged in full and the rest are counted. What matters is the read size
    /// and whether it was aligned to libbladeRF's metadata block, because a
    /// partial-block read is what provokes this, and which mode was in force.
    void reportReadFailure(int status, std::size_t requested) {
        const std::uint64_t count = ++m_readFailures;
        if (count != 1 && count % 50 != 0) {
            return;
        }

        log().warn("sync_rx failed: {} -- {} frames requested ({} to the {}-frame sync buffer), "
                   "{}, {}, {}; {} failure(s) so far",
                   bladeError(status), requested,
                   requested % kSyncBufferMultiple == 0 ? "aligned" : "NOT ALIGNED",
                   kSyncBufferMultiple,
                   m_activeMode == LinkMode::Oversampled ? "oversampled 8-bit"
                   : m_activeMode == LinkMode::Packed    ? "packed 12-bit link"
                                                         : "normal 16-bit",
                   toString(m_activeFormat),
                   m_staging.empty() ? "read straight into the block" : "staged", count);
    }

    /// Captures what the read about to be issued will be describing.
    ///
    /// Taken before the call rather than after, the way the HackRF driver
    /// stamps a block when it starts filling: the samples arrive during the
    /// call, so the tuning in force as it begins is the one they belong to. A
    /// retune landing mid-read is covered by the sweep engine's settle
    /// discard, which is what that mechanism is for.
    void beginRead() noexcept {
        m_stagedStartNs = monotonicNs();
        m_stagedCenterHz = m_centerHz.load();
        m_stagedSampleRate = m_sampleRate.load();
        m_stagedNsPerFrame = m_stagedSampleRate > 0.0 ? 1e9 / m_stagedSampleRate : 0.0;
    }

    /// Which arrangement the current rate and settings call for.
    [[nodiscard]] LinkMode linkMode() const noexcept {
        if (m_sampleRate.load() > kNormalMaxRate) {
            return LinkMode::Oversampled;
        }
        if constexpr (kPackedLinkSupported) {
            return m_packedRequested.load() ? LinkMode::Packed : LinkMode::Normal;
        }
        return LinkMode::Normal;
    }

    /// Turns the oversampling feature on or off, and does nothing when it is
    /// already where it needs to be -- the call is not free and the rate is
    /// re-applied on every sweep configuration.
    [[nodiscard]] Status applyOversample(bool wanted) {
        if (wanted == m_oversampleOn.load()) {
            return ok();
        }
        if (wanted && m_oversampleMaxRate <= 0.0) {
            return fail(ErrorCode::Unsupported,
                        "this board has no oversampling mode, so it stops at {:g} MS/s",
                        kNormalMaxRate / 1e6);
        }

        const int status = enableOversample(m_device, wanted);
        if (status != 0) {
            return fail(ErrorCode::DeviceError, "{} oversampling: {}",
                        wanted ? "enable" : "disable", bladeError(status));
        }

        m_oversampleOn.store(wanted);
        log().debug("oversampling {}; samples are now {}", wanted ? "on" : "off",
                    wanted ? "8-bit" : "16-bit");
        return ok();
    }

    void applyAutoBandwidth(double sampleRate) {
        // Left alone while oversampling. libbladeRF warns that "bandwidth
        // assignements with oversample feature enabled yields unkown results",
        // and a filter width nobody can predict is worse than the one the
        // radio chose for itself -- it would silently shape the very span the
        // 8-bit mode was selected to see.
        if (m_oversampleOn.load()) {
            return;
        }

        bladerf_bandwidth actual = 0;
        if (bladerf_set_bandwidth(m_device, rxChannel(),
                                  static_cast<bladerf_bandwidth>(sampleRate * 0.75),
                                  &actual) == 0) {
            m_bandwidthHz.store(static_cast<double>(actual));
        }
    }

    /// Reads one buffer into the staging area. False when nothing arrived.
    [[nodiscard]] bool fillStaging(const std::stop_token& stop) {
        const bool metadataCarried = m_activeMode != LinkMode::Packed;
        bladerf_metadata metadata{};
        metadata.flags = static_cast<std::uint32_t>(BLADERF_META_FLAG_RX_NOW);

        beginRead();
        const int status =
            bladerf_sync_rx(m_device, m_staging.data(), static_cast<unsigned int>(m_readFrames),
                            metadataCarried ? &metadata : nullptr, 3'000);
        if (status != 0) {
            if (!stop.stop_requested()) {
                reportReadFailure(status, m_readFrames);
                recoverIfWedged(stop);
            }
            return false;
        }

        m_consecutiveFailures = 0;
        m_stagedFrames = metadataCarried && metadata.actual_count > 0
                             ? std::min<std::size_t>(metadata.actual_count, m_readFrames)
                             : m_readFrames;
        m_stagedTaken = 0;
        m_stagedTimestamp = metadata.timestamp;

        // Reported once per read rather than once per block filled from it.
        if (metadataCarried && (metadata.status & BLADERF_META_STATUS_OVERRUN) != 0) {
            m_stream->reportDeviceOverrun(0);
        }
        return m_stagedFrames > 0;
    }

    void receiveLoop(std::stop_token stop) {
        while (!stop.stop_requested()) {
            plugin::Block ref = acquireWithinBudget(stop);
            if (!ref.valid()) {
                // Exhausted: the consumer is behind, and there is nowhere to
                // put the next read.
                //
                // Read it anyway and throw it away. Spinning here without
                // reading is what "not waiting" would actually be, and to the
                // radio it is exactly waiting: a synchronous interface only
                // drains when it is called, so a pool that is empty for a
                // moment stops the USB stream and the device FIFO overruns.
                // A counted host-side drop beats an uncounted device-side one.
                m_stream->reportPoolExhausted(drainAndDiscard(stop));
                continue;
            }

            const std::size_t bytesPer = bytesPerFrame(m_activeFormat);
            const std::size_t want = std::min(m_framesPerBlock, ref.capacityBytes() / bytesPer);

            std::size_t filled = 0;
            std::uint64_t blockStartNs = 0;
            std::uint64_t blockTimestamp = 0;
            double blockCenterHz = 0.0;
            double blockSampleRate = 0.0;

            if (m_staging.empty()) {
                // Zero-copy: the read is exactly the block, so libbladeRF
                // writes into the pool directly. Only reachable when the two
                // sizes agree, which start() checked.
                if (!fillDirect(ref, want, stop, filled)) {
                    continue;
                }
                blockStartNs = m_stagedStartNs;
                blockTimestamp = m_stagedTimestamp;
                blockCenterHz = m_stagedCenterHz;
                blockSampleRate = m_stagedSampleRate;
            } else {
                // Filled COMPLETELY, across as many reads as it takes.
                //
                // The read size is libbladeRF's business -- a multiple of 4096
                // frames, never below 16384 -- and the block size is the
                // pipeline's, which is one FFT. Neither divides the other in
                // general, so publishing whatever a single read happened to
                // leave over hands the pipeline a block shorter than one
                // transform, and it can only throw it away. At a 15 kHz RBW
                // that was a quarter of every sample the radio produced.
                while (filled < want && !stop.stop_requested()) {
                    if (m_stagedTaken >= m_stagedFrames && !fillStaging(stop)) {
                        break;
                    }

                    if (filled == 0) {
                        // Stamped when the block starts filling, from the read
                        // its first sample came out of.
                        blockStartNs = m_stagedStartNs +
                                       static_cast<std::uint64_t>(
                                           static_cast<double>(m_stagedTaken) * m_stagedNsPerFrame);
                        blockTimestamp = m_stagedTimestamp + m_stagedTaken;
                        blockCenterHz = m_stagedCenterHz;
                        blockSampleRate = m_stagedSampleRate;
                    }

                    const std::size_t take =
                        std::min(want - filled, m_stagedFrames - m_stagedTaken);
                    std::memcpy(ref.data() + (filled * bytesPer),
                                m_staging.data() + (m_stagedTaken * bytesPer), take * bytesPer);
                    filled += take;
                    m_stagedTaken += take;
                }
            }

            if (filled == 0) {
                continue;
            }

            sweeppp_sdr_delivery_t delivery{};
            delivery.frames = filled;
            delivery.format = plugin::toAbiFormat(m_activeFormat);
            delivery.sequence = m_sequence++;

            // Every field below describes where these samples came from, not
            // the moment the block finished being assembled.
            delivery.host_time_ns = blockStartNs;
            delivery.device_time_ns =
                (m_activeMode != LinkMode::Packed && blockSampleRate > 0.0)
                    ? static_cast<std::uint64_t>(static_cast<double>(blockTimestamp) * 1e9 /
                                                 blockSampleRate)
                    : 0;
            delivery.center_hz = blockCenterHz;
            delivery.sample_rate = blockSampleRate;

            m_stream->publish(std::move(ref), delivery);
        }
    }

    /// One read straight into the pool block, for the case where the two sizes
    /// are the same. Returns false when nothing usable arrived.
    [[nodiscard]] bool fillDirect(plugin::Block& ref, std::size_t want, const std::stop_token& stop,
                                  std::size_t& filled) {
        const bool metadataCarried = m_activeMode != LinkMode::Packed;
        bladerf_metadata metadata{};
        metadata.flags = static_cast<std::uint32_t>(BLADERF_META_FLAG_RX_NOW);

        beginRead();
        const int status =
            bladerf_sync_rx(m_device, ref.as<void>(), static_cast<unsigned int>(want),
                            metadataCarried ? &metadata : nullptr, 3'000);
        if (status != 0) {
            if (!stop.stop_requested()) {
                reportReadFailure(status, want);
                recoverIfWedged(stop);
            }
            return false;
        }

        m_consecutiveFailures = 0;
        filled = metadataCarried && metadata.actual_count > 0
                     ? std::min<std::size_t>(metadata.actual_count, want)
                     : want;
        m_stagedTimestamp = metadata.timestamp;

        if (metadataCarried && (metadata.status & BLADERF_META_STATUS_OVERRUN) != 0) {
            m_stream->reportDeviceOverrun(0);
        }
        return filled > 0;
    }

    void buildParameters() {
        m_gainModes = readGainModes();
        const std::string manualMode = gainModeName(static_cast<int>(BLADERF_GAIN_MGC));

        m_parameters = {
            SdrParameter{.key = "center_hz",
                         .label = "Center frequency",
                         .group = "Tuning",
                         .type = SdrParameterType::Double,
                         .unit = "Hz",
                         .min = m_info.minFrequencyHz,
                         .max = m_info.maxFrequencyHz,
                         .step = 1.0,
                         .enumValues = {},
                         .readOnly = false,
                         .gridAffecting = true,
                         .calibrationAffecting = false,
                         .requiresStop = false,
                         .description = "Tuner centre frequency.",
                         .defaultValue = 100e6},

            SdrParameter{.key = "sample_rate",
                         .label = "Sample rate",
                         .group = "Tuning",
                         .type = SdrParameterType::Double,
                         .unit = "S/s",
                         .min = m_info.minSampleRate,
                         .max = std::max(m_info.maxSampleRate, m_oversampleMaxRate),
                         .step = 0.0,
                         .enumValues = buildRateChoices(),
                         .readOnly = false,
                         .gridAffecting = true,
                         .calibrationAffecting = false,
                         .requiresStop = true,
                         .description = "Samples are 16-bit here, so a given rate costs twice "
                                        "the USB bandwidth of an 8-bit radio -- except above "
                                        "61.44 MS/s, where the radio switches to 8-bit and they "
                                        "do not. Watch the link utilisation bar in the "
                                        "Performance panel.",
                         .defaultValue = 20e6},

            SdrParameter{.key = "packed_link",
                         .label = "Packed link",
                         .group = "Filters",
                         .type = SdrParameterType::Bool,
                         .unit = {},
                         .min = 0.0,
                         .max = 0.0,
                         .step = 0.0,
                         .enumValues = {},
                         .readOnly = false,
                         .gridAffecting = false,
                         .calibrationAffecting = false,
                         .requiresStop = true,
                         .description =
                             "Sends 12-bit samples over the link and unpacks them here, for 33% "
                             "more headroom when the link is the limit -- a USB 2.0 port, "
                             "typically. Samples still arrive 16-bit. Off by default because "
                             "this format carries no metadata, so it costs the "
                             "device-reported overruns and the hardware timestamps, and on a "
                             "SuperSpeed link it buys nothing.",
                         .defaultValue = false},

            SdrParameter{.key = "bandwidth",
                         .label = "Filter bandwidth",
                         .group = "Filters",
                         .type = SdrParameterType::Double,
                         .unit = "Hz",
                         .min = 0.0,
                         .max = 56e6,
                         .step = 0.0,
                         .enumValues = {},
                         .readOnly = false,
                         .gridAffecting = false,
                         .calibrationAffecting = false,
                         .requiresStop = false,
                         .description = "0 follows the sample rate automatically. The radio "
                                        "snaps the request to a width it supports and reports "
                                        "what it actually set.",
                         .defaultValue = 0.0},

            SdrParameter{.key = "gain",
                         .label = "Manual gain",
                         .group = "Gain",
                         .type = SdrParameterType::Int,
                         .unit = "dB",
                         .min = -15.0,
                         .max = 60.0,
                         .step = 1.0,
                         .enumValues = {},
                         .readOnly = false,
                         .gridAffecting = false,
                         .calibrationAffecting = true,
                         .requiresStop = false,
                         .description = "Overall receive gain.",
                         .defaultValue = std::int64_t{30},
                         .appliesWhenKey = "gain_mode",
                         .appliesWhenValues = {manualMode}},

            SdrParameter{.key = "bias_tee",
                         .label = "Bias tee",
                         .group = "Antenna",
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
                         .description = "Powers an external LNA up the antenna port. Leave off "
                                        "unless the attached hardware expects it.",
                         .defaultValue = false},
        };

        // Asked of the board rather than assumed. libbladeRF suggests
        // presenting a plain on/off, which is what this was -- and that hides
        // three of the five modes a bladeRF 2.0 has, while a 1 series really
        // does offer only two.
        if (m_gainModes.size() > 1) {
            std::vector<SdrEnumValue> values;
            values.reserve(m_gainModes.size());
            for (const auto& [name, mode] : m_gainModes) {
                values.push_back({.value = name,
                                  .label = gainModeLabel(mode, name),
                                  .description = gainModeDescription(mode)});
            }

            const auto gain = std::ranges::find_if(
                m_parameters, [](const SdrParameter& entry) { return entry.key == "gain"; });

            m_parameters.insert(
                gain,
                SdrParameter{.key = "gain_mode",
                             .label = "Gain mode",
                             .group = "Gain",
                             .type = SdrParameterType::Enum,
                             .unit = {},
                             .min = 0.0,
                             .max = 0.0,
                             .step = 0.0,
                             .enumValues = values,
                             .readOnly = false,
                             .gridAffecting = false,
                             .calibrationAffecting = true,
                             .requiresStop = false,
                             .description =
                                 "Manual is the only one that measures: every automatic mode is "
                                 "free to move the gain under you, so the level scale stops "
                                 "meaning anything fixed and two sweeps stop being comparable. "
                                 "They are for listening and for finding things.",
                             .defaultValue = gainModeName(m_gainMode.load())});
        }
    }

    /// The receive connectors, asked of the hardware rather than hard-coded:
    /// the 1 and 2 series differ in how many they have.
    ///
    /// There is deliberately no `rx_channel` parameter beside this. Two
    /// controls writing one piece of state is the "second opinion" this
    /// codebase avoids elsewhere -- and this one would have been the worse
    /// kind, because the parameter would be greyed out as `requiresStop` while
    /// a routed sweep moved the thing it mirrors several times a pass.
    void buildPorts() {
        const std::size_t channels = bladerf_get_channel_count(m_device, BLADERF_RX);
        if (channels < 2) {
            // One connector: the host's default -- an implicit single input and
            // no port chooser at all -- is exactly right, and saying "RX1" adds
            // a control with nothing on the other side of it.
            return;
        }

        m_ports.reserve(channels);
        for (std::size_t i = 0; i < channels; ++i) {
            m_ports.push_back(SdrRxPort{
                .id = std::format("rx{}", i + 1),
                .label = std::format("RX{}", i + 1),
                .connector = std::format("SMA (RX{})", i + 1),
                // The whole tuning range on both: the AD9361's two receive
                // chains are identical, and only what is screwed onto them
                // differs.
                .minHz = 0.0,
                .maxHz = 0.0,
                .biasTee = true,
                // The sync interface is configured for one channel at
                // `bladerf_sync_config` time, so switching means rebuilding it.
                // The alternative -- streaming BLADERF_RX_X2 and discarding
                // half -- doubles the link bandwidth to throw it away, on the
                // radio whose whole appeal is the top sample rate.
                .requiresStop = true,
                .switchSeconds = kPortSwitchSeconds,
            });
        }
    }

    /// Names come from libbladeRF because a profile stores them.
    [[nodiscard]] std::vector<std::pair<std::string, bladerf_gain_mode>> readGainModes() const {
        const bladerf_gain_modes* modes = nullptr;
        const int count = bladerf_get_gain_modes(m_device, rxChannel(), &modes);
        if (count <= 0 || modes == nullptr) {
            return {{"manual", BLADERF_GAIN_MGC}};
        }

        std::vector<std::pair<std::string, bladerf_gain_mode>> found;
        found.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            if (modes[i].name != nullptr) {
                found.emplace_back(modes[i].name, modes[i].mode);
            }
        }
        return found.empty() ? decltype(found){{"manual", BLADERF_GAIN_MGC}} : found;
    }

    [[nodiscard]] std::string gainModeName(int mode) const {
        const auto match = std::ranges::find_if(m_gainModes, [mode](const auto& candidate) {
            return static_cast<int>(candidate.second) == mode;
        });
        return match != m_gainModes.end()
                   ? match->first
                   : (m_gainModes.empty() ? std::string("manual") : m_gainModes.front().first);
    }

    [[nodiscard]] static std::string gainModeLabel(bladerf_gain_mode mode,
                                                   const std::string& fallback) {
        switch (mode) {
        case BLADERF_GAIN_MGC:
            return "Manual";
        case BLADERF_GAIN_DEFAULT:
            return "Automatic (device default)";
        case BLADERF_GAIN_FASTATTACK_AGC:
            return "Automatic, fast attack";
        case BLADERF_GAIN_SLOWATTACK_AGC:
            return "Automatic, slow attack";
        case BLADERF_GAIN_HYBRID_AGC:
            return "Automatic, hybrid attack";
        }
        return fallback;
    }

    [[nodiscard]] static std::string gainModeDescription(bladerf_gain_mode mode) {
        switch (mode) {
        case BLADERF_GAIN_MGC:
            return "The gain stays where you put it. The only mode in which the level scale is "
                   "fixed, so it is the one to measure in.";
        case BLADERF_GAIN_DEFAULT:
            return "Whatever the board calls automatic -- slow attack on a 2.0 micro.";
        case BLADERF_GAIN_FASTATTACK_AGC:
            return "Backs off quickly when a strong signal appears. Useful sweeping past a "
                   "transmitter that would otherwise swamp the front end, at the cost of "
                   "chasing every burst.";
        case BLADERF_GAIN_SLOWATTACK_AGC:
            return "Settles on a level and holds it, riding over short bursts. The steadier "
                   "picture of a quiet band.";
        case BLADERF_GAIN_HYBRID_AGC:
            return "Holds the analogue gain and corrects digitally, changing the analogue "
                   "stages only when it must. Between the other two.";
        }
        return {};
    }

    /// Re-reads the per-channel settings after the channel has changed.
    void refreshFromHardware() {
        bladerf_sample_rate rate = 0;
        if (bladerf_get_sample_rate(m_device, rxChannel(), &rate) == 0) {
            m_sampleRate.store(static_cast<double>(rate));
        }

        bladerf_bandwidth bandwidth = 0;
        if (bladerf_get_bandwidth(m_device, rxChannel(), &bandwidth) == 0) {
            m_bandwidthHz.store(static_cast<double>(bandwidth));
        }

        bladerf_gain gain = 0;
        if (bladerf_get_gain(m_device, rxChannel(), &gain) == 0) {
            m_gain.store(static_cast<int>(gain));
        }

        bladerf_gain_mode mode = BLADERF_GAIN_DEFAULT;
        if (bladerf_get_gain_mode(m_device, rxChannel(), &mode) == 0) {
            m_gainMode.store(static_cast<int>(mode));
        }

        bladerf_frequency frequency = 0;
        if (bladerf_get_frequency(m_device, rxChannel(), &frequency) == 0) {
            m_centerHz.store(static_cast<double>(frequency));
        }
    }

    [[nodiscard]] std::span<const SdrRxPort> rxPorts() const noexcept override { return m_ports; }

    [[nodiscard]] std::string_view selectedRxPort() const noexcept override {
        const auto index = static_cast<std::size_t>(m_rxIndex.load(std::memory_order_relaxed));
        return index < m_ports.size() ? std::string_view(m_ports[index].id) : std::string_view{};
    }

    [[nodiscard]] Status selectRxPort(std::string_view id) override {
        const auto match =
            std::ranges::find_if(m_ports, [id](const SdrRxPort& port) { return port.id == id; });
        if (match == m_ports.end()) {
            return fail(ErrorCode::InvalidArgument, "no receive port '{}'", id);
        }

        const auto index = static_cast<int>(match - m_ports.begin());
        if (index == m_rxIndex.load(std::memory_order_relaxed)) {
            return ok();
        }
        if (m_streaming.load()) {
            // Every port here declares `requiresStop`, so the caller has
            // already cycled the stream. Reaching this means something drove
            // the driver directly, and switching under a live sync interface
            // configured for the other channel would deliver silence that
            // looks exactly like a quiet band.
            return fail(ErrorCode::Unavailable, "stop the stream before changing receive port");
        }

        m_rxIndex.store(index, std::memory_order_relaxed);

        // Gain, bandwidth and bias tee are per-channel on this hardware, so
        // the cached values now describe a channel nobody is listening to.
        refreshFromHardware();
        return ok();
    }

    [[nodiscard]] std::vector<SdrHealthReading> healthReadings() const override {
        std::vector<SdrHealthReading> readings;

        float temperature = 0.0F;
        if (bladerf_get_rfic_temperature(m_device, &temperature) == 0) {
            // The AD9361 is specified to 85 C; past that it still runs but its
            // gain and frequency accuracy drift, so it is worth flagging rather
            // than merely displaying.
            readings.push_back({.label = "RFIC temperature",
                                .value = std::format("{:.1f} C", temperature),
                                .numeric = temperature,
                                .minimum = 0.0F,
                                .maximum = 100.0F,
                                .alarm = temperature > 85.0F});
        }

        bladerf_power_sources source = BLADERF_UNKNOWN;
        if (bladerf_get_power_source(m_device, &source) == 0) {
            const char* name = source == BLADERF_PS_DC         ? "DC barrel"
                               : source == BLADERF_PS_USB_VBUS ? "USB bus"
                                                               : "unknown";
            readings.push_back({.label = "Power source", .value = name});
        }

        std::int32_t preRssi = 0;
        std::int32_t postRssi = 0;
        if (bladerf_get_rfic_rssi(m_device, rxChannel(), &preRssi, &postRssi) == 0) {
            readings.push_back({.label = "RSSI (pre / post)",
                                .value = std::format("{} / {} dB", preRssi, postRssi),
                                .numeric = static_cast<float>(postRssi),
                                .minimum = -120.0F,
                                .maximum = 0.0F});
        }

        // The INA219 on the supply rail. Bus voltage and load current say
        // whether a brownout is behind an overrun, which is otherwise
        // indistinguishable from the host being too slow.
        const auto pmic = [this](bladerf_pmic_register reg) -> std::optional<float> {
            float value = 0.0F;
            if (bladerf_get_pmic_register(m_device, reg, &value) != 0) {
                return std::nullopt;
            }
            return value;
        };

        if (const auto volts = pmic(BLADERF_PMIC_VOLTAGE_BUS)) {
            // The board runs from a nominal 5 V rail whichever source feeds it,
            // and USB sagging under load is a real failure mode.
            readings.push_back({.label = "Bus voltage",
                                .value = std::format("{:.2f} V", *volts),
                                .numeric = *volts,
                                .minimum = 4.0F,
                                .maximum = 5.5F,
                                .alarm = *volts < 4.5F});
        }
        if (const auto amps = pmic(BLADERF_PMIC_CURRENT)) {
            readings.push_back({.label = "Load current",
                                .value = std::format("{:.3f} A", *amps),
                                .numeric = *amps,
                                .minimum = 0.0F,
                                .maximum = 2.0F});
        }
        if (const auto watts = pmic(BLADERF_PMIC_POWER)) {
            readings.push_back({.label = "Load power",
                                .value = std::format("{:.2f} W", *watts),
                                .numeric = *watts,
                                .minimum = 0.0F,
                                .maximum = 10.0F});
        }

        std::uint16_t trim = 0;
        if (bladerf_get_vctcxo_trim(m_device, &trim) == 0) {
            readings.push_back({.label = "VCTCXO trim", .value = std::format("{}", trim)});
        }

        return readings;
    }

    /// Which RX channel the driver is using. A bladeRF 2.0 has two, fed by
    /// separate connectors, and every per-channel call below has to name one.
    ///
    /// Atomic because the receive thread reads it through here while the sweep
    /// thread may be switching. "Safe because switching is forbidden while
    /// streaming" stopped being true the moment the engine learned to cycle
    /// the stream around a switch.
    [[nodiscard]] bladerf_channel rxChannel() const noexcept {
        return BLADERF_CHANNEL_RX(m_rxIndex.load(std::memory_order_relaxed));
    }

    bladerf* m_device = nullptr;
    std::atomic<int> m_rxIndex{0};

    /// Fixed after construction, so `selectedRxPort` hands out a view into it
    /// while a switch is in flight. Empty on a one-connector board.
    std::vector<SdrRxPort> m_ports;

    SdrDeviceInfo m_info;
    std::vector<SdrParameter> m_parameters;

    std::atomic<double> m_centerHz{100e6};
    std::atomic<double> m_sampleRate{20e6};

    /// The top of the oversampling range this board actually offers, probed at
    /// open, or 0 when it has none. Asked of the hardware rather than assumed:
    /// it is a feature of the FPGA image as much as of the board.
    double m_oversampleMaxRate = 0.0;
    std::atomic<bool> m_oversampleOn{false};
    std::atomic<bool> m_packedRequested{false};

    /// Latched by start() so the receive thread reads one consistent mode.
    LinkMode m_activeMode = LinkMode::Normal;
    SampleFormat m_activeFormat = SampleFormat::Cs16;

    /// One read, when the host's blocks are smaller than libbladeRF will
    /// service. Empty on every other run, and then nothing is copied at all.
    /// Touched only by the receive thread.
    std::size_t m_readFrames = 0;
    std::vector<std::byte> m_staging;
    std::vector<std::byte> m_discard;
    std::size_t m_stagedFrames = 0;
    std::size_t m_stagedTaken = 0;
    std::uint64_t m_stagedTimestamp = 0;
    std::uint64_t m_stagedStartNs = 0;
    double m_stagedCenterHz = 0.0;
    double m_stagedSampleRate = 0.0;
    double m_stagedNsPerFrame = 0.0;
    bladerf_format m_activeStreamFormat = BLADERF_FORMAT_SC16_Q11_META;
    std::uint64_t m_readFailures = 0;
    std::uint32_t m_consecutiveFailures = 0;
    std::atomic<double> m_bandwidthHz{0.0};
    std::atomic<bool> m_bandwidthAuto{true};
    std::atomic<int> m_gain{30};
    std::vector<std::pair<std::string, bladerf_gain_mode>> m_gainModes;
    std::atomic<int> m_gainMode{static_cast<int>(BLADERF_GAIN_MGC)};
    std::atomic<bool> m_biasTee{false};

    std::atomic<bool> m_streaming{false};
    std::jthread m_thread;
    plugin::Stream* m_stream = nullptr;
    std::size_t m_framesPerBlock = 262'144;
    std::uint64_t m_sequence = 0;
};

class BladeRfFactory final : public plugin::Driver {
public:
    [[nodiscard]] std::vector<SdrDeviceInfo> enumerate() const override {
        std::vector<SdrDeviceInfo> devices;

        bladerf_devinfo* list = nullptr;
        const int count = bladerf_get_device_list(&list);
        if (count < 0 || list == nullptr) {
            // A negative count is "none found", not a failure worth surfacing.
            return devices;
        }

        for (int i = 0; i < count; ++i) {
            // The USB product string is all enumeration can see, and it stops
            // at "bladeRF 2.0": the FPGA size that names the variant is in
            // flash, and the link speed needs a handle, so both wait until the
            // radio is open.
            const std::string_view product = list[i].product;
            devices.push_back(SdrDeviceInfo{.driver = "bladerf",
                                            .id = list[i].serial,
                                            .label = product.contains("2.0") ? "bladeRF 2.0 micro"
                                                     : product.empty()       ? "bladeRF"
                                                                             : std::string(product),
                                            .serial = list[i].serial,
                                            .hardwareRevision = {},
                                            .firmware = {},
                                            .fpga = {},
                                            .minFrequencyHz = 47e6,
                                            .maxFrequencyHz = 6e9,
                                            .minSampleRate = 520e3,
                                            .maxSampleRate = 61.44e6,
                                            .linkCapacityBytesPerSec = 0,
                                            .linkDescription = {}});
        }

        bladerf_free_device_list(list);
        return devices;
    }

    [[nodiscard]] Result<std::unique_ptr<plugin::Radio>> open(std::string_view id) override {
        bladerf* handle = nullptr;
        const std::string identifier(id);

        // libbladeRF takes a device identifier string, not a bare serial.
        const bool qualified =
            identifier.find('=') != std::string::npos || identifier.find(':') != std::string::npos;
        const std::string deviceString =
            qualified ? identifier : std::format("*:serial={}", identifier);

        const int status =
            bladerf_open(&handle, identifier.empty() ? nullptr : deviceString.c_str());
        if (status != 0 || handle == nullptr) {
            return fail<std::unique_ptr<plugin::Radio>>(
                ErrorCode::DeviceError, "could not open BladeRF{}: {}",
                identifier.empty() ? "" : std::format(" '{}'", identifier), bladeError(status));
        }

        SdrDeviceInfo info{.driver = "bladerf",
                           .id = identifier,
                           .label = "bladeRF",
                           .serial = identifier,
                           .hardwareRevision = {},
                           .firmware = {},
                           .fpga = {},
                           .minFrequencyHz = 47e6,
                           .maxFrequencyHz = 6e9,
                           .minSampleRate = 520e3,
                           .maxSampleRate = 61.44e6,
                           .linkCapacityBytesPerSec = 0,
                           .linkDescription = {}};

        // Limits read from the hardware rather than assumed: the 1 and 2
        // series differ in both tuning range and sample rate, and a panel
        // offering a range the radio cannot reach is worse than no panel.
        const bladerf_range* frequencyRange = nullptr;
        if (bladerf_get_frequency_range(handle, BLADERF_CHANNEL_RX(0), &frequencyRange) == 0 &&
            frequencyRange != nullptr) {
            info.minFrequencyHz = static_cast<double>(frequencyRange->min);
            info.maxFrequencyHz = static_cast<double>(frequencyRange->max);
        }

        const bladerf_range* rateRange = nullptr;
        if (bladerf_get_sample_rate_range(handle, BLADERF_CHANNEL_RX(0), &rateRange) == 0 &&
            rateRange != nullptr) {
            info.minSampleRate = static_cast<double>(rateRange->min);
            info.maxSampleRate = static_cast<double>(rateRange->max);
        }

        bladerf_fpga_size fpgaSize = BLADERF_FPGA_UNKNOWN;
        (void)bladerf_get_fpga_size(handle, &fpgaSize);

        const char* boardName = bladerf_get_board_name(handle);
        const bool micro = boardName != nullptr && std::string_view(boardName) == "bladerf2";

        info.label = modelName(boardName, fpgaSize);
        if (const char* variant = fpgaVariant(fpgaSize); variant != nullptr) {
            info.hardwareRevision = variant;
        }

        // The one part of this that can stop working without saying so: a
        // libbladeRF that does not export its tables leaves both blank.
        const KnownLatest latest = knownLatest(micro);
        log().debug("libbladeRF knows firmware up to {}, FPGA up to {}",
                    latest.firmware ? versionText(*latest.firmware) : "-",
                    latest.fpga ? versionText(*latest.fpga) : "-");

        // `struct` is required: libbladeRF declares a *function* named
        // bladerf_version as well as the struct, and in C++ the function
        // declaration hides the type. C does not care; C++ does.
        struct bladerf_version firmware{};
        if (bladerf_fw_version(handle, &firmware) == 0) {
            info.firmware = describeVersion(firmware, latest.firmware);
        }

        // Only meaningful with an image loaded -- asked otherwise it reports
        // whatever was left in the version registers.
        if (bladerf_is_fpga_configured(handle) == 1) {
            struct bladerf_version fpga{};
            if (bladerf_fpga_version(handle, &fpga) == 0) {
                info.fpga = describeVersion(fpga, latest.fpga);
            }
        }

        // Read back rather than trusted from the identifier: opening by
        // instance, by bus address, or with no identifier at all leaves the
        // serial unknown, and the panel then has nothing to name the radio by.
        struct bladerf_serial serial{};
        if (bladerf_get_serial_struct(handle, &serial) == 0) {
            info.serial = serial.serial;
            if (info.id.empty()) {
                info.id = info.serial;
            }
        }

        switch (bladerf_device_speed(handle)) {
        case BLADERF_DEVICE_SPEED_SUPER:
            info.linkCapacityBytesPerSec = kSuperSpeedBytesPerSec;
            info.linkDescription = "USB 3.0 SuperSpeed";
            break;
        case BLADERF_DEVICE_SPEED_HIGH:
            info.linkCapacityBytesPerSec = kHighSpeedBytesPerSec;
            info.linkDescription = "USB 2.0 high speed";
            break;
        case BLADERF_DEVICE_SPEED_UNKNOWN:
        default:
            break;
        }

        auto device = std::make_unique<BladeRfDevice>(handle, std::move(info),
                                                      probeOversampleMaxRate(handle, micro));

        for (const SdrParameter& parameter : device->parameters()) {
            if (!parameter.readOnly) {
                (void)device->setParameter(parameter.key, parameter.defaultValue);
            }
        }

        const SdrDeviceInfo& opened = device->info();
        log().info("opened {} (serial {}, firmware {}, FPGA {}, {})", opened.label,
                   opened.serial.empty() ? "-" : opened.serial,
                   opened.firmware.version.empty() ? "-" : opened.firmware.version,
                   opened.fpga.version.empty() ? "not loaded" : opened.fpga.version,
                   opened.linkDescription.empty() ? "link speed unknown" : opened.linkDescription);

        // A radio past the end of libbladeRF's table means everything logged
        // above about it came from a library that predates it.
        for (const auto& [what, report] :
             {std::pair{"firmware", opened.firmware}, std::pair{"FPGA", opened.fpga}}) {
            if (report.aheadOfDriver) {
                log().warn("{} v{} is newer than libbladeRF knows of; update libbladeRF "
                           "if problems arise",
                           what, report.version);
            } else if (!report.knownLatest.empty() && report.knownLatest != report.version) {
                log().info("libbladeRF supports {} up to v{}; v{} is loaded", what,
                           report.knownLatest, report.version);
            }
        }

        return device;
    }
};

} // namespace

plugin::Logger& log() noexcept {
    static plugin::Logger logger;
    return logger;
}

plugin::Driver& driver() noexcept {
    static BladeRfFactory factory;
    return factory;
}

} // namespace sweeppp::blade
