// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// Native PlutoSDR driver, on libiio's 0.x API.
//
// A Pluto is an AD936x behind a Zynq running Linux; the host sees it as an IIO
// context, over USB or the network. `ad9361-phy` carries every control and
// `cf-ad9361-lpc` carries the samples, which are read synchronously on a
// thread this driver owns -- the Fobos driver's arrangement, with libiio's own
// buffer standing in for the staging area.
#include "PlutoDriver.hpp"
#include "PlutoParse.hpp"
#include "sweeppp/core/Clock.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <iio.h>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace sweeppp::pluto {
namespace {

/// What a radio is listed with before it is opened: the AD9364's range, which
/// is what most Plutos are reconfigured to. Opening reads the real one, which
/// on an unmodified AD9363 is 325 MHz to 3.8 GHz.
inline constexpr double kListedMinFrequencyHz = 70e6;
inline constexpr double kListedMaxFrequencyHz = 6e9;

/// 25 MS/s / 12, the lowest rate the AD9361 reaches without loading a FIR
/// filter, which this driver does not do.
inline constexpr double kListedMinRate = 2'083'333.0;
inline constexpr double kListedMaxRate = 61.44e6;

/// Link capacity in delivered bytes per second, for the Performance panel's
/// utilisation bar. Estimates, not measurements: libiio's USB backend sustains
/// about 20 MB/s on the Pluto's USB 2.0 port, and the USB network gadget about
/// half that.
inline constexpr std::uint64_t kUsbCapacityBytesPerSec = 20'000'000ULL;
inline constexpr std::uint64_t kNetworkCapacityBytesPerSec = 10'000'000ULL;
inline constexpr const char* kUsbDescription = "USB 2.0 high speed";
inline constexpr const char* kNetworkDescription = "Network";

/// The rates offered as presets, trimmed to what the radio reports.
inline constexpr std::array kPresetRates{2.5e6, 3e6,  4e6,  5e6,     6e6,    8e6,
                                         10e6,  15e6, 20e6, 30.72e6, 61.44e6};

/// Signed 16-bit I and Q, carrying the AD936x's 12 bits sign-extended. Full
/// scale is therefore 2048 rather than the 32768 `Cs16` assumes, so levels
/// read 24 dB low -- the same as the bladeRF's SC16_Q11, which is the same
/// chip family delivering the same way.
inline constexpr SampleFormat kFormat = SampleFormat::Cs16;
inline constexpr std::size_t kBytesPerFrame = 4;

/// Buffers the kernel queues on the radio. Each is one read.
inline constexpr unsigned kKernelBuffers = 4;

/// One read is about this long, so a retune is seen within a few of them.
inline constexpr double kReadSeconds = 5e-3;
inline constexpr std::size_t kMinReadFrames = 16'384;

/// The AD9361's synthesiser locking after a frequency write. A starting
/// figure: measure it on hardware.
inline constexpr double kLoSettleSeconds = 1e-3;

/// How long a context waits on the radio before giving up. Long enough for a
/// read at the lowest rate, short enough that a radio pulled out mid-stream is
/// noticed.
inline constexpr unsigned kContextTimeoutMs = 2'000;

/// The AXI DMA controller's status register on the receive path, and its
/// overflow bit: set when the DMA had nowhere to write, that is, when samples
/// were lost on the radio before the host ever saw them. Written back to clear.
/// Not yet confirmed on hardware.
inline constexpr std::uint32_t kDmaStatusRegister = 0x80000088;
inline constexpr std::uint32_t kDmaOverflowBit = 0x4;
inline constexpr std::uint64_t kOverflowCheckIntervalNs = 250'000'000; // 4 Hz

inline constexpr std::string_view kManualGain = "manual";

[[nodiscard]] std::string iioError(int error) {
    std::array<char, 256> text{};
    iio_strerror(error < 0 ? -error : error, text.data(), text.size());
    return text.data();
}

[[nodiscard]] std::size_t readFramesFor(double sampleRate) noexcept {
    const auto frames = static_cast<std::size_t>(sampleRate * kReadSeconds);
    // A multiple of 4096 frames: libiio moves whole USB packets, and an odd
    // size costs a short transfer on every read.
    const std::size_t rounded = (std::max(frames, kMinReadFrames) + 4095) / 4096 * 4096;
    return rounded;
}

[[nodiscard]] std::string gainModeLabel(std::string_view mode) {
    if (mode == "manual") {
        return "Manual";
    }
    if (mode == "slow_attack") {
        return "Slow attack AGC";
    }
    if (mode == "fast_attack") {
        return "Fast attack AGC";
    }
    if (mode == "hybrid") {
        return "Hybrid AGC";
    }
    return std::string(mode);
}

struct ContextDeleter {
    void operator()(iio_context* context) const noexcept { iio_context_destroy(context); }
};
using ContextPtr = std::unique_ptr<iio_context, ContextDeleter>;

/// Everything open() reads from the radio before the device exists.
struct Handles {
    ContextPtr context;
    iio_device* phy = nullptr;
    iio_device* rx = nullptr;
    iio_channel* rxConfig = nullptr; ///< phy input voltage0: gain, rate, bandwidth
    iio_channel* rxLo = nullptr;     ///< phy output altvoltage0: the RX LO
    iio_channel* rxI = nullptr;
    iio_channel* rxQ = nullptr;
    iio_channel* temperature = nullptr; ///< phy input temp0, may be absent
};

/// What the radio says it can do, read once at open.
struct Capabilities {
    IioRange frequency{.min = kListedMinFrequencyHz, .step = 1.0, .max = kListedMaxFrequencyHz};
    IioRange sampleRate{.min = kListedMinRate, .step = 1.0, .max = kListedMaxRate};
    IioRange bandwidth{.min = 200e3, .step = 1.0, .max = 56e6};
    IioRange gain{.min = -3.0, .step = 1.0, .max = 71.0};
    std::vector<std::string> gainModes;
};

[[nodiscard]] std::string readAttribute(const iio_channel* channel, const char* name) {
    std::array<char, 1024> text{};
    const ssize_t read = iio_channel_attr_read(channel, name, text.data(), text.size());
    return read > 0 ? std::string(text.data()) : std::string{};
}

class PlutoDevice final : public plugin::Radio {
public:
    PlutoDevice(Handles handles, Capabilities capabilities, SdrDeviceInfo info)
        : m_handles(std::move(handles)), m_capabilities(std::move(capabilities)),
          m_info(std::move(info)) {
        buildParameters();
    }

    ~PlutoDevice() override { PlutoDevice::stop(); }

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
        if (key == "bandwidth") {
            return SdrValue{m_bandwidthAuto.load() ? 0.0 : m_bandwidthHz.load()};
        }
        if (key == "gain_mode") {
            const std::lock_guard lock(m_settingsMutex);
            return SdrValue{m_gainMode};
        }
        if (key == "gain") {
            return SdrValue{std::int64_t{m_gain.load()}};
        }
        if (key == "quadrature_tracking") {
            return SdrValue{m_quadratureTracking.load()};
        }
        if (key == "rf_dc_tracking") {
            return SdrValue{m_rfDcTracking.load()};
        }
        if (key == "bb_dc_tracking") {
            return SdrValue{m_bbDcTracking.load()};
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
        if (key == "sample_rate") {
            return setSampleRate(asDouble(*coerced));
        }
        if (key == "bandwidth") {
            const double requested = asDouble(*coerced);
            m_bandwidthAuto.store(requested <= 0.0);
            return applyBandwidth(requested <= 0.0 ? m_sampleRate.load() : requested);
        }
        if (key == "gain_mode") {
            const std::string mode = asString(*coerced);
            if (Status written = writeString(m_handles.rxConfig, "gain_control_mode", mode);
                !written) {
                return written;
            }
            {
                const std::lock_guard lock(m_settingsMutex);
                m_gainMode = mode;
            }
            // Back to the gain the panel shows, which the AGC will have moved.
            if (mode == kManualGain) {
                return applyGain(m_gain.load());
            }
            return ok();
        }
        if (key == "gain") {
            return applyGain(static_cast<int>(asInt(*coerced)));
        }
        if (key == "quadrature_tracking") {
            return setTracking("quadrature_tracking_en", asBool(*coerced), m_quadratureTracking);
        }
        if (key == "rf_dc_tracking") {
            return setTracking("rf_dc_offset_tracking_en", asBool(*coerced), m_rfDcTracking);
        }
        if (key == "bb_dc_tracking") {
            return setTracking("bb_dc_offset_tracking_en", asBool(*coerced), m_bbDcTracking);
        }

        return fail(ErrorCode::NotFound, "no parameter '{}'", key);
    }

    [[nodiscard]] SampleFormat nativeFormat() const noexcept override { return kFormat; }

    [[nodiscard]] std::vector<double> supportedSampleRates() const override {
        // Empty is "anything in range": the AD9361 synthesises its clocks, and
        // the presets are only suggestions.
        return {};
    }

    [[nodiscard]] Status start(plugin::Stream& stream, const StreamConfig& config) override {
        if (m_streaming.load()) {
            return fail(ErrorCode::AlreadyExists, "already streaming");
        }

        const std::size_t readFrames = readFramesFor(m_sampleRate.load());
        {
            const std::lock_guard lock(m_controlMutex);
            iio_channel_enable(m_handles.rxI);
            iio_channel_enable(m_handles.rxQ);

            if (const int result =
                    iio_device_set_kernel_buffers_count(m_handles.rx, kKernelBuffers);
                result < 0) {
                log().debug("could not set the kernel buffer count: {}", iioError(result));
            }

            m_buffer = iio_device_create_buffer(m_handles.rx, readFrames, false);
            if (m_buffer == nullptr) {
                const int error = errno;
                disableChannels();
                return fail(ErrorCode::DeviceError, "could not start receiving: {}",
                            iioError(error));
            }
        }

        // Two enabled 16-bit channels, interleaved. Anything else is a radio
        // this driver has misread, and its samples would be noise.
        if (const std::ptrdiff_t step = iio_buffer_step(m_buffer);
            step != static_cast<std::ptrdiff_t>(kBytesPerFrame)) {
            destroyBuffer();
            return fail(ErrorCode::DeviceError,
                        "the radio delivers {} bytes per sample where 4 were expected", step);
        }

        m_readFrames.store(readFrames);
        m_staged = nullptr;
        m_stagedFrames = 0;
        m_stagedTaken = 0;
        m_readFailures = 0;
        m_nextOverflowCheckNs = 0;
        m_overflowCheck = true;

        m_stream = &stream;
        m_framesPerBlock = config.framesPerBlock;
        m_sequence = 0;
        m_streaming.store(true);

        m_thread = std::jthread([this](const std::stop_token& stop) { receiveLoop(stop); });
        return ok();
    }

    void stop() override {
        if (!m_streaming.exchange(false)) {
            return;
        }
        // Cancelled first, so a refill waiting on the radio returns now rather
        // than at the context's timeout; then joined, because destroying the
        // buffer frees what that refill was filling.
        m_thread.request_stop();
        iio_buffer_cancel(m_buffer);
        if (m_thread.joinable()) {
            m_thread.join();
        }
        destroyBuffer();
        m_stream = nullptr;
    }

    [[nodiscard]] bool streaming() const noexcept override { return m_streaming.load(); }

    [[nodiscard]] Status retune(double centerHz) override {
        const std::lock_guard lock(m_controlMutex);
        const int result = iio_channel_attr_write_longlong(m_handles.rxLo, "frequency",
                                                           static_cast<long long>(centerHz));
        if (result < 0) {
            return fail(ErrorCode::DeviceError, "set frequency: {}", iioError(result));
        }
        m_centerHz.store(centerHz);
        return ok();
    }

    [[nodiscard]] double deliveryGranularitySeconds(double sampleRate) const noexcept override {
        // One read is one transfer with one tuning attached.
        return sampleRate > 0.0 ? static_cast<double>(currentReadFrames()) / sampleRate : 0.0;
    }

    [[nodiscard]] double retuneSettleSeconds() const noexcept override {
        // The synthesiser locking, plus every buffer the kernel had already
        // queued when the frequency changed. Those hold samples from the old
        // frequency, but they are read -- and so stamped -- after the retune,
        // and nothing can tell them apart from the new ones except time.
        const double rate = m_sampleRate.load();
        const double queued =
            rate > 0.0 ? static_cast<double>(kKernelBuffers * currentReadFrames()) / rate : 0.0;
        return kLoSettleSeconds + queued;
    }

    [[nodiscard]] std::vector<SdrHealthReading> healthReadings() const override {
        std::vector<SdrHealthReading> readings;
        const std::lock_guard lock(m_controlMutex);

        long long milliCelsius = 0;
        if (m_handles.temperature != nullptr &&
            iio_channel_attr_read_longlong(m_handles.temperature, "input", &milliCelsius) == 0) {
            const auto celsius = static_cast<float>(milliCelsius) / 1000.0F;
            // The AD9361 is specified to 85 C.
            readings.push_back({.label = "RFIC temperature",
                                .value = std::format("{:.1f} C", celsius),
                                .numeric = celsius,
                                .minimum = 0.0F,
                                .maximum = 100.0F,
                                .alarm = celsius > 85.0F});
        }

        // Reported as a positive figure below full scale.
        double rssi = 0.0;
        if (iio_channel_attr_read_double(m_handles.rxConfig, "rssi", &rssi) == 0) {
            const auto level = static_cast<float>(-rssi);
            readings.push_back({.label = "RSSI",
                                .value = std::format("{:.2f} dB", level),
                                .numeric = level,
                                .minimum = -120.0F,
                                .maximum = 0.0F,
                                .alarm = false});
        }
        return readings;
    }

private:
    [[nodiscard]] std::size_t currentReadFrames() const noexcept {
        return m_streaming.load() ? m_readFrames.load() : readFramesFor(m_sampleRate.load());
    }

    [[nodiscard]] Status writeString(iio_channel* channel, const char* attribute,
                                     const std::string& value) {
        const std::lock_guard lock(m_controlMutex);
        const ssize_t result = iio_channel_attr_write(channel, attribute, value.c_str());
        if (result < 0) {
            return fail(ErrorCode::DeviceError, "set {}: {}", attribute,
                        iioError(static_cast<int>(result)));
        }
        return ok();
    }

    [[nodiscard]] Status setSampleRate(double rate) {
        double actual = rate;
        {
            const std::lock_guard lock(m_controlMutex);
            const int result = iio_channel_attr_write_longlong(
                m_handles.rxConfig, "sampling_frequency", static_cast<long long>(rate));
            if (result < 0) {
                return fail(ErrorCode::DeviceError, "set sample rate: {}", iioError(result));
            }
            // What the radio is running at rather than what was asked: every
            // derived figure -- RBW, span, the frequency axis -- follows it.
            long long readBack = 0;
            if (iio_channel_attr_read_longlong(m_handles.rxConfig, "sampling_frequency",
                                               &readBack) == 0 &&
                readBack > 0) {
                actual = static_cast<double>(readBack);
            }
        }
        m_sampleRate.store(actual);

        // The analogue filter follows the rate unless the operator pinned it;
        // a filter narrower than the span silently loses its edges.
        if (m_bandwidthAuto.load()) {
            return applyBandwidth(actual);
        }
        return ok();
    }

    [[nodiscard]] Status applyBandwidth(double width) {
        const double clamped =
            std::clamp(width, m_capabilities.bandwidth.min, m_capabilities.bandwidth.max);
        const std::lock_guard lock(m_controlMutex);
        const int result = iio_channel_attr_write_longlong(m_handles.rxConfig, "rf_bandwidth",
                                                           static_cast<long long>(clamped));
        if (result < 0) {
            return fail(ErrorCode::DeviceError, "set filter bandwidth: {}", iioError(result));
        }
        long long readBack = 0;
        const bool read =
            iio_channel_attr_read_longlong(m_handles.rxConfig, "rf_bandwidth", &readBack) == 0;
        m_bandwidthHz.store(read ? static_cast<double>(readBack) : clamped);
        return ok();
    }

    /// Writes the manual gain, remembering it whether or not the AGC is in
    /// charge right now, so switching back to manual restores it.
    ///
    /// The AD9361's gain table depends on the LO band, and so does its highest
    /// gain: 71 dB below 1.3 GHz, less above. A gain the current band cannot
    /// reach is lowered to its maximum rather than refused.
    [[nodiscard]] Status applyGain(int gain) {
        m_gain.store(gain);
        {
            const std::lock_guard lock(m_settingsMutex);
            if (m_gainMode != kManualGain) {
                return ok();
            }
        }

        const std::lock_guard lock(m_controlMutex);
        int result = iio_channel_attr_write_longlong(m_handles.rxConfig, "hardwaregain", gain);
        if (result == -EINVAL) {
            if (const auto band =
                    parseRange(readAttribute(m_handles.rxConfig, "hardwaregain_available"))) {
                const auto reachable = static_cast<long long>(
                    std::clamp(static_cast<double>(gain), band->min, band->max));
                result =
                    iio_channel_attr_write_longlong(m_handles.rxConfig, "hardwaregain", reachable);
            }
        }
        if (result < 0) {
            return fail(ErrorCode::DeviceError, "set gain: {}", iioError(result));
        }
        return ok();
    }

    [[nodiscard]] Status setTracking(const char* attribute, bool enabled,
                                     std::atomic<bool>& state) {
        const std::lock_guard lock(m_controlMutex);
        const int result = iio_channel_attr_write_bool(m_handles.rxConfig, attribute, enabled);
        if (result < 0) {
            return fail(ErrorCode::DeviceError, "set {}: {}", attribute, iioError(result));
        }
        state.store(enabled);
        return ok();
    }

    void disableChannels() noexcept {
        iio_channel_disable(m_handles.rxI);
        iio_channel_disable(m_handles.rxQ);
    }

    void destroyBuffer() noexcept {
        const std::lock_guard lock(m_controlMutex);
        if (m_buffer != nullptr) {
            iio_buffer_destroy(m_buffer);
            m_buffer = nullptr;
        }
        disableChannels();
    }

    /// A block, waiting briefly for one rather than giving up at once.
    ///
    /// The pool empties in bursts, when a retune's control transfer holds up
    /// the link and the backlog behind it arrives in a rush. Waiting those out
    /// loses nothing; exceeding the budget means the consumer is genuinely
    /// behind, and the caller drains and drops instead.
    [[nodiscard]] plugin::Block acquireWithinBudget(const std::stop_token& stop) {
        constexpr std::uint64_t kBudgetNs = 1'000'000; // 1 ms

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

    /// Throws away what is staged, reading a buffer first if nothing is, to
    /// keep the radio draining when there is nowhere to put the data. Returns
    /// the frames lost.
    ///
    /// Not reading is not a way of dropping: the kernel's queue fills and the
    /// DMA overflows instead.
    [[nodiscard]] std::size_t drainAndDiscard(const std::stop_token& stop) {
        if (m_stagedTaken >= m_stagedFrames && !refill(stop)) {
            return 0;
        }
        const std::size_t dropped = m_stagedFrames - m_stagedTaken;
        m_stagedTaken = m_stagedFrames;
        // A gap the host caused: the samples after it are not continuous with
        // the ones before, and the sequence number says so.
        ++m_sequence;
        return dropped;
    }

    /// Reads one buffer from the radio into libiio's own memory, which is
    /// where blocks are filled from. False when nothing arrived.
    ///
    /// Stamped before the call, because the samples were captured before it
    /// returned; a retune landing during it, or still queued behind it, is
    /// what the settle time covers.
    [[nodiscard]] bool refill(const std::stop_token& stop) {
        m_stagedStartNs = monotonicNs();
        m_stagedCenterHz = m_centerHz.load();
        m_stagedSampleRate = m_sampleRate.load();
        m_stagedNsPerFrame = m_stagedSampleRate > 0.0 ? 1e9 / m_stagedSampleRate : 0.0;

        const ssize_t read = iio_buffer_refill(m_buffer);
        if (read < 0) {
            reportReadFailure(static_cast<int>(read), stop);
            m_staged = nullptr;
            m_stagedFrames = 0;
            m_stagedTaken = 0;
            return false;
        }
        m_staged = static_cast<const std::byte*>(iio_buffer_start(m_buffer));
        m_stagedFrames = static_cast<std::size_t>(read) / kBytesPerFrame;
        m_stagedTaken = 0;
        return m_stagedFrames > 0;
    }

    /// One read failure, logged well enough to act on, and then counted. The
    /// pause keeps an unplugged radio, which fails every read at once, from
    /// spinning a core and flooding the log.
    void reportReadFailure(int error, const std::stop_token& stop) {
        if (stop.stop_requested()) {
            return;
        }
        const std::uint64_t count = ++m_readFailures;
        if (count == 1 || count % 50 == 0) {
            log().warn("read failed: {} -- {} failure(s) so far", iioError(error), count);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    /// Asks the radio whether it dropped samples, a few times a second.
    ///
    /// The overflow bit is a sticky flag, not a count, so one set bit is
    /// reported as one read's worth of samples lost. A radio without access to
    /// the register -- debugfs not mounted, or an older firmware -- is asked
    /// once and then left alone.
    void checkOverflow() {
        const std::uint64_t now = monotonicNs();
        if (!m_overflowCheck || now < m_nextOverflowCheckNs) {
            return;
        }
        m_nextOverflowCheckNs = now + kOverflowCheckIntervalNs;

        const std::lock_guard lock(m_controlMutex);
        std::uint32_t status = 0;
        if (const int result = iio_device_reg_read(m_handles.rx, kDmaStatusRegister, &status);
            result < 0) {
            log().debug("no overflow reporting: {}", iioError(result));
            m_overflowCheck = false;
            return;
        }
        if ((status & kDmaOverflowBit) != 0) {
            (void)iio_device_reg_write(m_handles.rx, kDmaStatusRegister, status);
            m_stream->reportDeviceOverrun(m_readFrames.load());
        }
    }

    void receiveLoop(const std::stop_token& stop) {
        while (!stop.stop_requested()) {
            plugin::Block block = acquireWithinBudget(stop);
            if (!block.valid()) {
                if (stop.stop_requested()) {
                    break;
                }
                m_stream->reportPoolExhausted(drainAndDiscard(stop));
                continue;
            }

            const std::size_t want =
                std::min(m_framesPerBlock, block.capacityBytes() / kBytesPerFrame);

            std::size_t filled = 0;
            std::uint64_t blockStartNs = 0;
            double blockCenterHz = 0.0;
            double blockSampleRate = 0.0;

            // Filled COMPLETELY, across as many reads as it takes: a block
            // short of one transform is a block the pipeline can only drop.
            while (filled < want && !stop.stop_requested()) {
                if (m_stagedTaken >= m_stagedFrames && !refill(stop)) {
                    break;
                }

                if (filled == 0) {
                    // Stamped from the read its first sample came out of.
                    blockStartNs = m_stagedStartNs +
                                   static_cast<std::uint64_t>(static_cast<double>(m_stagedTaken) *
                                                              m_stagedNsPerFrame);
                    blockCenterHz = m_stagedCenterHz;
                    blockSampleRate = m_stagedSampleRate;
                }

                const std::size_t take = std::min(want - filled, m_stagedFrames - m_stagedTaken);
                std::memcpy(block.data() + (filled * kBytesPerFrame),
                            m_staged + (m_stagedTaken * kBytesPerFrame), take * kBytesPerFrame);
                filled += take;
                m_stagedTaken += take;
            }

            if (filled < want) {
                // A failed read or a stop part-way through. The block goes
                // back unpublished, and the sequence moves so the gap is
                // visible downstream.
                if (filled > 0) {
                    ++m_sequence;
                }
                continue;
            }

            sweeppp_sdr_delivery_t delivery{};
            delivery.frames = filled;
            delivery.format = plugin::toAbiFormat(kFormat);
            delivery.sequence = m_sequence++;
            delivery.host_time_ns = blockStartNs;
            delivery.center_hz = blockCenterHz;
            delivery.sample_rate = blockSampleRate;

            m_stream->publish(std::move(block), delivery);
            checkOverflow();
        }
    }

    void buildParameters() {
        const Capabilities& caps = m_capabilities;

        std::vector<double> presets;
        for (const double rate : kPresetRates) {
            if (rate >= caps.sampleRate.min && rate <= caps.sampleRate.max) {
                presets.push_back(rate);
            }
        }

        m_parameters = {
            SdrParameter{
                .key = "center_hz",
                .label = "Center frequency",
                .group = "Tuning",
                .type = SdrParameterType::Double,
                .unit = "Hz",
                .min = caps.frequency.min,
                .max = caps.frequency.max,
                .step = 1.0,
                .enumValues = {},
                .readOnly = false,
                .gridAffecting = true,
                .calibrationAffecting = false,
                .requiresStop = false,
                .description = std::format("{:g} MHz to {:g} MHz, as this radio reports.",
                                           caps.frequency.min / 1e6, caps.frequency.max / 1e6),
                .defaultValue = std::clamp(433.92e6, caps.frequency.min, caps.frequency.max)},

            SdrParameter{.key = "sample_rate",
                         .label = "Sample rate",
                         .group = "Tuning",
                         .type = SdrParameterType::Double,
                         .unit = "S/s",
                         .min = caps.sampleRate.min,
                         .max = caps.sampleRate.max,
                         .step = 0.0,
                         .enumValues = sampleRateChoices(presets),
                         .readOnly = false,
                         .gridAffecting = true,
                         .calibrationAffecting = false,
                         .requiresStop = false,
                         .description = "USB 2.0 carries about 5 MS/s without dropping samples; "
                                        "above that the Performance window shows overruns.",
                         .defaultValue = std::clamp(4e6, caps.sampleRate.min, caps.sampleRate.max)},

            SdrParameter{.key = "bandwidth",
                         .label = "Filter bandwidth",
                         .group = "Filters",
                         .type = SdrParameterType::Double,
                         .unit = "Hz",
                         .min = 0.0,
                         .max = caps.bandwidth.max,
                         .step = 0.0,
                         .enumValues = {},
                         .readOnly = false,
                         .gridAffecting = false,
                         .calibrationAffecting = false,
                         .requiresStop = false,
                         .description = "Analogue filter width. 0 follows the sample rate, which "
                                        "is almost always what you want -- a filter narrower "
                                        "than the span silently loses its edges.",
                         .defaultValue = 0.0},
        };

        if (!caps.gainModes.empty()) {
            std::vector<SdrEnumValue> modes;
            modes.reserve(caps.gainModes.size());
            for (const std::string& mode : caps.gainModes) {
                modes.push_back({.value = mode, .label = gainModeLabel(mode), .description = {}});
            }
            const bool hasManual =
                std::ranges::find(caps.gainModes, kManualGain) != caps.gainModes.end();
            m_parameters.push_back(SdrParameter{
                .key = "gain_mode",
                .label = "Gain mode",
                .group = "Gain",
                .type = SdrParameterType::Enum,
                .unit = {},
                .min = 0.0,
                .max = 0.0,
                .step = 0.0,
                .enumValues = std::move(modes),
                .readOnly = false,
                .gridAffecting = false,
                .calibrationAffecting = true,
                .requiresStop = false,
                .description = "Manual by default: an AGC sets each sweep step's gain on its "
                               "own, so neighbouring steps stop agreeing on levels.",
                .defaultValue = hasManual ? std::string(kManualGain) : caps.gainModes.front()});
        }

        m_parameters.push_back(SdrParameter{
            .key = "gain",
            .label = "Manual gain",
            .group = "Gain",
            .type = SdrParameterType::Int,
            .unit = "dB",
            .min = caps.gain.min,
            .max = caps.gain.max,
            .step = 1.0,
            .enumValues = {},
            .readOnly = false,
            .gridAffecting = false,
            .calibrationAffecting = true,
            .requiresStop = false,
            .description = "Overall receive gain. The highest gain depends on the frequency: "
                           "above 1.3 GHz the radio stops lower, and a higher setting is "
                           "lowered to fit.",
            .defaultValue =
                static_cast<std::int64_t>(std::clamp(40.0, caps.gain.min, caps.gain.max)),
            .appliesWhenKey = caps.gainModes.empty() ? std::string{} : std::string("gain_mode"),
            .appliesWhenValues = caps.gainModes.empty()
                                     ? std::vector<std::string>{}
                                     : std::vector<std::string>{std::string(kManualGain)}});

        const auto tracking = [](std::string key, std::string label, std::string description) {
            return SdrParameter{.key = std::move(key),
                                .label = std::move(label),
                                .group = "Corrections",
                                .type = SdrParameterType::Bool,
                                .unit = {},
                                .min = 0.0,
                                .max = 0.0,
                                .step = 0.0,
                                .enumValues = {},
                                .readOnly = false,
                                .gridAffecting = false,
                                .calibrationAffecting = true,
                                .requiresStop = false,
                                .description = std::move(description),
                                .defaultValue = true};
        };
        m_parameters.push_back(tracking("quadrature_tracking", "Quadrature tracking",
                                        "Corrects I/Q imbalance continuously, which keeps the "
                                        "mirror image of a strong signal down."));
        m_parameters.push_back(tracking("rf_dc_tracking", "RF DC tracking",
                                        "Removes the LO leakage spike at the centre of each "
                                        "step."));
        m_parameters.push_back(tracking("bb_dc_tracking", "Baseband DC tracking",
                                        "Removes the baseband DC offset continuously."));
    }

    /// Declared first so it is destroyed last, after the receive thread that
    /// reads through it has been joined.
    Handles m_handles;
    Capabilities m_capabilities;
    SdrDeviceInfo m_info;
    std::vector<SdrParameter> m_parameters;

    /// Serialises control traffic. libiio's client takes its own lock per
    /// request, but a register read is two requests -- an address written,
    /// then a value read -- and nothing else may land between them.
    mutable std::mutex m_controlMutex;

    std::atomic<double> m_centerHz{433.92e6};
    std::atomic<double> m_sampleRate{4e6};
    std::atomic<double> m_bandwidthHz{0.0};
    std::atomic<bool> m_bandwidthAuto{true};
    std::atomic<int> m_gain{40};
    std::atomic<bool> m_quadratureTracking{true};
    std::atomic<bool> m_rfDcTracking{true};
    std::atomic<bool> m_bbDcTracking{true};

    mutable std::mutex m_settingsMutex;
    std::string m_gainMode{kManualGain};

    std::atomic<bool> m_streaming{false};
    std::jthread m_thread;
    iio_buffer* m_buffer = nullptr;
    plugin::Stream* m_stream = nullptr;
    std::size_t m_framesPerBlock = kMinReadFrames;
    std::atomic<std::size_t> m_readFrames{kMinReadFrames};
    std::uint64_t m_sequence = 0;

    /// The last read, in libiio's buffer. Touched only by the receive thread.
    const std::byte* m_staged = nullptr;
    std::size_t m_stagedFrames = 0;
    std::size_t m_stagedTaken = 0;
    std::uint64_t m_stagedStartNs = 0;
    double m_stagedCenterHz = 0.0;
    double m_stagedSampleRate = 0.0;
    double m_stagedNsPerFrame = 0.0;
    std::uint64_t m_readFailures = 0;
    std::uint64_t m_nextOverflowCheckNs = 0;
    bool m_overflowCheck = true;
};

/// A Pluto the USB scan found.
struct ScannedRadio {
    std::string uri;
    std::string serial;
};

[[nodiscard]] std::vector<ScannedRadio> scanUsb() {
    std::vector<ScannedRadio> radios;

    iio_scan_context* scan = iio_create_scan_context(std::string(kUsbScan).c_str(), 0);
    if (scan == nullptr) {
        log().debug("no USB scan: {}", iioError(errno));
        return radios;
    }

    iio_context_info** infos = nullptr;
    const ssize_t count = iio_scan_context_get_info_list(scan, &infos);
    if (count < 0) {
        log().debug("USB scan failed: {}", iioError(static_cast<int>(count)));
    }
    for (ssize_t i = 0; i < count; ++i) {
        const char* uri = iio_context_info_get_uri(infos[i]);
        const char* description = iio_context_info_get_description(infos[i]);
        if (uri == nullptr) {
            continue;
        }
        radios.push_back({.uri = uri,
                          .serial = description != nullptr ? serialFromDescription(description)
                                                           : std::string{}});
    }

    if (infos != nullptr) {
        iio_context_info_list_free(infos);
    }
    iio_scan_context_destroy(scan);
    return radios;
}

[[nodiscard]] Capabilities readCapabilities(const Handles& handles) {
    Capabilities caps;
    if (const auto range = parseRange(readAttribute(handles.rxLo, "frequency_available"))) {
        caps.frequency = *range;
    }
    if (const auto range =
            parseRange(readAttribute(handles.rxConfig, "sampling_frequency_available"))) {
        caps.sampleRate = *range;
    }
    if (const auto range = parseRange(readAttribute(handles.rxConfig, "rf_bandwidth_available"))) {
        caps.bandwidth = *range;
    }
    if (const auto range = parseRange(readAttribute(handles.rxConfig, "hardwaregain_available"))) {
        caps.gain = *range;
    }
    caps.gainModes = splitWords(readAttribute(handles.rxConfig, "gain_control_mode_available"));
    return caps;
}

[[nodiscard]] std::string contextAttribute(iio_context* context, const char* key) {
    const char* value = iio_context_get_attr_value(context, key);
    return value != nullptr ? std::string(value) : std::string{};
}

/// The radio an id names: a URI as given, or a serial -- or nothing, for the
/// first -- among the radios on USB.
[[nodiscard]] Result<ScannedRadio> resolve(std::string_view id) {
    if (isUri(id)) {
        return ScannedRadio{.uri = std::string(id), .serial = {}};
    }

    const std::vector<ScannedRadio> radios = scanUsb();
    const auto match =
        id.empty() ? radios.begin() : std::ranges::find_if(radios, [id](const ScannedRadio& radio) {
            return radio.serial == id;
        });
    if (match == radios.end()) {
        return fail<ScannedRadio>(ErrorCode::NotFound,
                                  "{} is attached over USB (a PlutoSDR on the network is opened "
                                  "by address, as pluto:ip:192.168.2.1)",
                                  id.empty() ? std::string("no PlutoSDR")
                                             : std::format("no PlutoSDR '{}'", id));
    }
    return *match;
}

/// A context on the radio, and the devices and channels this driver uses.
[[nodiscard]] Result<Handles> connect(const std::string& uri) {
    Handles handles;
    handles.context.reset(iio_create_context_from_uri(uri.c_str()));
    if (!handles.context) {
        const int error = errno;
        return fail<Handles>(
            ErrorCode::DeviceError, "could not open PlutoSDR at {}: {}{}", uri, iioError(error),
            isNetworkUri(uri)
                ? " (check that the address is reachable)"
                : " (on Linux this is usually a missing udev rule; otherwise another program "
                  "already has the radio open)");
    }
    (void)iio_context_set_timeout(handles.context.get(), kContextTimeoutMs);

    iio_context* context = handles.context.get();
    handles.phy = iio_context_find_device(context, "ad9361-phy");
    handles.rx = iio_context_find_device(context, "cf-ad9361-lpc");
    if (handles.phy != nullptr) {
        handles.rxConfig = iio_device_find_channel(handles.phy, "voltage0", false);
        handles.rxLo = iio_device_find_channel(handles.phy, "altvoltage0", true);
        handles.temperature = iio_device_find_channel(handles.phy, "temp0", false);
    }
    if (handles.rx != nullptr) {
        handles.rxI = iio_device_find_channel(handles.rx, "voltage0", false);
        handles.rxQ = iio_device_find_channel(handles.rx, "voltage1", false);
    }
    if (handles.rxConfig == nullptr || handles.rxLo == nullptr || handles.rxI == nullptr ||
        handles.rxQ == nullptr) {
        return fail<Handles>(ErrorCode::DeviceError,
                             "{} is not a PlutoSDR: it has no AD9361 receive path", uri);
    }
    return handles;
}

class PlutoFactory final : public plugin::Driver {
public:
    /// The USB scan alone. It reads each radio's descriptors and opens no
    /// context -- creating one downloads the radio's whole description, which
    /// takes the better part of a second. A Pluto on the network is not
    /// listed; it is opened by its address.
    [[nodiscard]] std::vector<SdrDeviceInfo> enumerate() const override {
        std::vector<SdrDeviceInfo> devices;
        for (const ScannedRadio& radio : scanUsb()) {
            devices.push_back(SdrDeviceInfo{.driver = "pluto",
                                            .id = radio.serial.empty() ? radio.uri : radio.serial,
                                            .label = "PlutoSDR",
                                            .serial = radio.serial,
                                            .hardwareRevision = {},
                                            .firmware = {},
                                            .fpga = {},
                                            .minFrequencyHz = kListedMinFrequencyHz,
                                            .maxFrequencyHz = kListedMaxFrequencyHz,
                                            .minSampleRate = kListedMinRate,
                                            .maxSampleRate = kListedMaxRate,
                                            .linkCapacityBytesPerSec = kUsbCapacityBytesPerSec,
                                            .linkDescription = kUsbDescription});
        }
        return devices;
    }

    /// An id is a serial from the scan, or a libiio URI -- `ip:192.168.2.1`,
    /// `usb:1.5.5` -- used as given. Empty opens the first radio on USB.
    [[nodiscard]] Result<std::unique_ptr<plugin::Radio>> open(std::string_view id) override {
        Result<ScannedRadio> target = resolve(id);
        if (!target) {
            return std::unexpected(target.error());
        }
        const std::string& uri = target->uri;

        Result<Handles> connected = connect(uri);
        if (!connected) {
            return std::unexpected(connected.error());
        }
        Handles handles = *std::move(connected);
        iio_context* context = handles.context.get();

        const HardwareModel model = parseHardwareModel(contextAttribute(context, "hw_model"));
        std::string serial = contextAttribute(context, "hw_serial");
        if (serial.empty()) {
            serial = target->serial;
        }
        // The id it was asked for, so a profile reopens it the same way; when
        // asked for none, the one a profile should store.
        std::string openedId(id);
        if (openedId.empty()) {
            openedId = serial.empty() ? uri : serial;
        }
        const Capabilities caps = readCapabilities(handles);
        const bool network = isNetworkUri(uri);

        SdrDeviceInfo info{.driver = "pluto",
                           .id = std::move(openedId),
                           .label = model.revision.empty()
                                        ? std::string("PlutoSDR")
                                        : std::format("PlutoSDR {}", model.revision),
                           .serial = serial,
                           .hardwareRevision = model.revision,
                           .firmware = {.version = contextAttribute(context, "fw_version"),
                                        .knownLatest = {},
                                        .aheadOfDriver = false},
                           .fpga = {},
                           .minFrequencyHz = caps.frequency.min,
                           .maxFrequencyHz = caps.frequency.max,
                           .minSampleRate = caps.sampleRate.min,
                           .maxSampleRate = caps.sampleRate.max,
                           .linkCapacityBytesPerSec =
                               network ? kNetworkCapacityBytesPerSec : kUsbCapacityBytesPerSec,
                           .linkDescription = network ? kNetworkDescription : kUsbDescription};

        // What the AD936x driver runs as, which on a reconfigured Pluto is
        // not the chip printed in the hardware model.
        std::string chip = contextAttribute(context, "ad9361-phy,model");
        if (chip.empty()) {
            chip = model.chip;
        }
        auto device = std::make_unique<PlutoDevice>(std::move(handles), caps, std::move(info));

        // Applied so the radio's state matches what the panel reports the
        // moment it opens, rather than whatever the last program left behind.
        for (const SdrParameter& parameter : device->parameters()) {
            if (!parameter.readOnly) {
                if (Status applied = device->setParameter(parameter.key, parameter.defaultValue);
                    !applied) {
                    log().debug("default for {} not applied: {}", parameter.key,
                                applied.error().message());
                }
            }
        }

        const SdrDeviceInfo& opened = device->info();
        log().info("opened {} at {} (serial {}, firmware {}, {}, {:g} - {:g} MHz)", opened.label,
                   uri, opened.serial.empty() ? "-" : opened.serial,
                   opened.firmware.version.empty() ? "-" : opened.firmware.version,
                   chip.empty() ? "-" : chip, opened.minFrequencyHz / 1e6,
                   opened.maxFrequencyHz / 1e6);

        return device;
    }
};

} // namespace

plugin::Logger& log() noexcept {
    static plugin::Logger logger;
    return logger;
}

plugin::Driver& driver() noexcept {
    static PlutoFactory factory;
    return factory;
}

} // namespace sweeppp::pluto
