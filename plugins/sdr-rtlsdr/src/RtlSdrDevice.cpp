// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// Native RTL-SDR driver, on librtlsdr.
//
// The same shape as the HackRF driver: fixed USB transfers arrive on a thread
// the library drives, and are accumulated into pool blocks right there. The
// difference is whose thread it is. rtlsdr_read_async() does not start one; it
// runs libusb's event loop on the caller until cancelled, so this driver owns
// the thread and lends it to the library.
//
// Sweeping retunes per step in software, as every driver here does.
#include "RtlSdrDriver.hpp"
#include "RtlSdrTuners.hpp"
#include "sweeppp/core/Clock.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <thread>

namespace sweeppp::rtl {
namespace {

inline constexpr bool kHasBiasTee = SWEEPPP_RTLSDR_HAS_BIAS_TEE != 0;
inline constexpr bool kHasBandwidth = SWEEPPP_RTLSDR_HAS_BANDWIDTH != 0;

/// The RTL2832U's own ceiling, in bytes per second: 3.2 MS/s of CU8.
///
/// Not the bus's. The chip sits on USB 2.0 high speed, which would carry five
/// times this, but it cannot produce more -- so against the bus the utilisation
/// bar would read 20% at the very rate where dongles start losing samples.
/// Against this it reads full there, which is the fact worth showing.
inline constexpr std::uint64_t kLinkCapacityBytesPerSec = 6'400'000ULL;
inline constexpr const char* kUsbDescription = "USB 2.0 high speed";

/// The rates worth offering.
///
/// The chip takes anything in 225 - 300 kS/s and 900 kS/s - 3.2 MS/s, but
/// these are the ones whose resampler ratio comes out exact or close to it.
/// 2.4 MS/s is the top of what runs without dropped samples on most hosts;
/// the three above it work on some and not others. Named once because it is
/// both the panel's preset list and what `supportedSampleRates()` reports.
inline constexpr std::array kUsableSampleRates{0.25e6,  1.024e6, 1.4e6,  1.8e6,  1.92e6,
                                               2.048e6, 2.4e6,   2.56e6, 2.88e6, 3.2e6};
inline constexpr double kDefaultSampleRate = 2.4e6;

/// One USB transfer, in bytes -- librtlsdr asks for a multiple of 16384, the
/// size of one of its bulk requests. The smallest legal size, because every
/// sample in a transfer carries the one tuning in force when it arrived.
inline constexpr std::uint32_t kTransferBytes = 16'384;

/// How long after a retune samples are not trusted.
///
/// A starting figure rather than a measured one. It covers the R82xx PLL
/// locking, plus one transfer libusb had already queued before the retune
/// reached the tuner. Measure it against a real dongle and put the number
/// here; too long costs sweep rate, too short smears one step into the next.
inline constexpr double kRetuneSettleSeconds = 20e-3;

/// What switching between the tuner and direct sampling costs, end to end.
///
/// Both ports need the stream cycled, and leaving direct sampling runs the
/// tuner's whole init again. Conservative until measured, so the planner's
/// predicted pass time errs slow rather than fast.
inline constexpr double kPortSwitchSeconds = 0.1;

inline constexpr std::string_view kGainAuto = "auto";
inline constexpr std::string_view kGainManual = "manual";

/// libusb's codes, which rtlsdr_open() returns unchanged. Two integers are not
/// worth including libusb.h for.
inline constexpr int kLibusbErrorAccess = -3;
inline constexpr int kLibusbErrorBusy = -6;

/// The bytes librtlsdr's string readers want room for, per string.
inline constexpr std::size_t kUsbStringBytes = 256;

struct UsbStrings {
    std::string manufacturer;
    std::string product;
    std::string serial;
};

[[nodiscard]] std::string positionalId(std::uint32_t index) {
    return std::format("index-{}", index);
}

class RtlSdrDevice final : public plugin::Radio {
public:
    RtlSdrDevice(rtlsdr_dev_t* device, SdrDeviceInfo info, TunerSpec tuner)
        : m_device(device), m_info(std::move(info)), m_tuner(tuner) {
        readGains();
        buildPorts();
        buildParameters();
    }

    ~RtlSdrDevice() override {
        RtlSdrDevice::stop();
        if (m_device != nullptr) {
            rtlsdr_close(m_device);
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
        if (key == "gain_mode") {
            return SdrValue{std::string(m_gainManual.load() ? kGainManual : kGainAuto)};
        }
        if (key == "gain") {
            return SdrValue{m_gainTenths.load() / 10.0};
        }
        if (key == "rtl_agc") {
            return SdrValue{m_rtlAgc.load()};
        }
        if (key == "ppm") {
            return SdrValue{std::int64_t{m_ppm.load()}};
        }
        if (key == "bandwidth" && kHasBandwidth) {
            return SdrValue{m_bandwidthHz.load()};
        }
        if (key == "bias_tee" && kHasBiasTee) {
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
        if (key == "sample_rate") {
            const auto wanted = static_cast<std::uint32_t>(std::lround(asDouble(*coerced)));
            if (const int result = rtlsdr_set_sample_rate(m_device, wanted); result != 0) {
                // The one refusal an operator can provoke from the panel: the
                // gap between the chip's two ranges is inside [min, max].
                return fail(ErrorCode::DeviceError,
                            "set sample rate: librtlsdr refused {:g} S/s ({}); the chip takes "
                            "225 - 300 kS/s and 900 kS/s - 3.2 MS/s",
                            static_cast<double>(wanted), result);
            }
            // The resampler ratio is quantised, and librtlsdr keeps the rate
            // it actually achieved. Storing the request instead would put the
            // frequency axis of every sweep slightly out.
            const std::uint32_t actual = rtlsdr_get_sample_rate(m_device);
            m_sampleRate.store(static_cast<double>(actual != 0 ? actual : wanted));
            return ok();
        }
        if (key == "gain_mode") {
            const bool manual = asString(*coerced) == kGainManual;
            if (const int result = rtlsdr_set_tuner_gain_mode(m_device, manual ? 1 : 0);
                result != 0) {
                return fail(ErrorCode::DeviceError, "set gain mode: librtlsdr error {}", result);
            }
            m_gainManual.store(manual);
            // Entering manual mode resets the tuner to its lowest gain, so the
            // gain the panel shows is applied again rather than left a lie.
            return manual ? applyGain(m_gainTenths.load()) : ok();
        }
        if (key == "gain") {
            // Naming a gain is asking for that gain, which automatic mode will
            // not honour, so the radio is taken off automatic here and the
            // mode control follows.
            if (!m_gainManual.load()) {
                if (const int result = rtlsdr_set_tuner_gain_mode(m_device, 1); result != 0) {
                    return fail(ErrorCode::DeviceError, "set gain mode: librtlsdr error {}",
                                result);
                }
                m_gainManual.store(true);
            }
            return applyGain(nearestGain(static_cast<int>(std::lround(asDouble(*coerced) * 10.0))));
        }
        if (key == "rtl_agc") {
            const bool enabled = asBool(*coerced);
            if (const int result = rtlsdr_set_agc_mode(m_device, enabled ? 1 : 0); result != 0) {
                return fail(ErrorCode::DeviceError, "set RTL2832 AGC: librtlsdr error {}", result);
            }
            m_rtlAgc.store(enabled);
            return ok();
        }
        if (key == "ppm") {
            const auto ppm = static_cast<int>(asInt(*coerced));
            // -2 is librtlsdr declining to apply the correction already in
            // force, which is success from here.
            if (const int result = rtlsdr_set_freq_correction(m_device, ppm);
                result != 0 && result != -2) {
                return fail(ErrorCode::DeviceError, "set frequency correction: librtlsdr error {}",
                            result);
            }
            m_ppm.store(ppm);
            return ok();
        }
#if SWEEPPP_RTLSDR_HAS_BANDWIDTH
        if (key == "bandwidth") {
            const auto width = static_cast<std::uint32_t>(std::lround(asDouble(*coerced)));
            if (const int result = rtlsdr_set_tuner_bandwidth(m_device, width); result != 0) {
                return fail(ErrorCode::DeviceError, "set tuner bandwidth: librtlsdr error {}",
                            result);
            }
            m_bandwidthHz.store(static_cast<double>(width));
            return ok();
        }
#endif
#if SWEEPPP_RTLSDR_HAS_BIAS_TEE
        if (key == "bias_tee") {
            const bool enabled = asBool(*coerced);
            if (const int result = rtlsdr_set_bias_tee(m_device, enabled ? 1 : 0); result != 0) {
                return fail(ErrorCode::DeviceError, "set bias tee: librtlsdr error {}", result);
            }
            m_biasTee.store(enabled);
            return ok();
        }
#endif

        return fail(ErrorCode::NotFound, "no parameter '{}'", key);
    }

    // Unsigned 8-bit I/Q, the RTL2832U's own output. The pipeline converts it;
    // at 2 bytes a frame there is nothing to be gained doing so here.
    [[nodiscard]] SampleFormat nativeFormat() const noexcept override { return SampleFormat::Cu8; }

    [[nodiscard]] std::vector<double> supportedSampleRates() const override {
        return {kUsableSampleRates.begin(), kUsableSampleRates.end()};
    }

    [[nodiscard]] Status start(plugin::Stream& stream, const StreamConfig& config) override {
        if (m_streaming.load()) {
            return fail(ErrorCode::AlreadyExists, "already streaming");
        }

        // Whatever the dongle buffered while nobody was reading predates this
        // run, and may belong to another frequency altogether.
        if (const int result = rtlsdr_reset_buffer(m_device); result != 0) {
            return fail(ErrorCode::DeviceError, "rtlsdr_reset_buffer: librtlsdr error {}", result);
        }

        m_stream = &stream;
        m_framesPerBlock = config.framesPerBlock;
        m_sequence = 0;
        m_partialFrames = 0;
        m_streaming.store(true);
        m_readerActive.store(true);

        m_reader = std::jthread([this] { readLoop(); });
        return ok();
    }

    void stop() override {
        if (!m_streaming.exchange(false)) {
            return;
        }

        // rtlsdr_cancel_async() only flags a loop that is already running. One
        // issued before the reader has got that far is refused and forgotten,
        // and the loop then runs for ever -- so it is repeated until the
        // reader is seen to have left, which after a real cancel is within one
        // transfer.
        while (m_readerActive.load()) {
            (void)rtlsdr_cancel_async(m_device);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (m_reader.joinable()) {
            m_reader.join();
        }

        // The reader is joined, so nothing of ours is still touching the
        // stream -- which is what the host requires before it tears it down.
        m_partial.release();
        m_stream = nullptr;
    }

    [[nodiscard]] bool streaming() const noexcept override { return m_streaming.load(); }

    [[nodiscard]] Status retune(double centerHz) override {
        if (!(centerHz >= 0.0 && centerHz <= 4.0e9)) {
            return fail(ErrorCode::InvalidArgument, "{:g} Hz is outside what librtlsdr can tune",
                        centerHz);
        }
        const int result =
            rtlsdr_set_center_freq(m_device, static_cast<std::uint32_t>(std::llround(centerHz)));
        if (result != 0) {
            return fail(ErrorCode::DeviceError, "set frequency {:g} MHz: librtlsdr error {}",
                        centerHz / 1e6, result);
        }
        m_centerHz.store(centerHz);
        return ok();
    }

    [[nodiscard]] double deliveryGranularitySeconds(double sampleRate) const noexcept override {
        // One transfer is the finest a step can be resolved: its samples all
        // arrive at once, with a single tuning attached.
        return sampleRate > 0.0 ? static_cast<double>(kTransferBytes) /
                                      (sampleRate * bytesPerFrame(SampleFormat::Cu8))
                                : 0.0;
    }

    [[nodiscard]] double retuneSettleSeconds() const noexcept override {
        return kRetuneSettleSeconds;
    }

    [[nodiscard]] std::span<const SdrRxPort> rxPorts() const noexcept override { return m_ports; }

    [[nodiscard]] std::string_view selectedRxPort() const noexcept override {
        const auto index = static_cast<std::size_t>(m_portIndex.load(std::memory_order_relaxed));
        return index < m_ports.size() ? std::string_view(m_ports[index].id) : std::string_view{};
    }

    [[nodiscard]] Status selectRxPort(std::string_view id) override {
        const auto match =
            std::ranges::find_if(m_ports, [id](const SdrRxPort& port) { return port.id == id; });
        if (match == m_ports.end()) {
            return fail(ErrorCode::InvalidArgument, "no receive port '{}'", id);
        }

        const auto index = static_cast<int>(match - m_ports.begin());
        if (index == m_portIndex.load(std::memory_order_relaxed)) {
            return ok();
        }
        if (m_streaming.load()) {
            // Every port declares `requiresStop`, so the caller has already
            // cycled the stream. Reaching this means something drove the
            // driver directly.
            return fail(ErrorCode::Unavailable, "stop the stream before changing receive port");
        }

        const bool direct = match->id == "hf";
        // 2 is the Q branch, which is where the dongles that sample HF
        // directly at all have it wired.
        if (const int result = rtlsdr_set_direct_sampling(m_device, direct ? 2 : 0); result != 0) {
            return fail(ErrorCode::DeviceError, "{} direct sampling: librtlsdr error {}",
                        direct ? "enable" : "disable", result);
        }
        m_portIndex.store(index, std::memory_order_relaxed);

        if (!direct) {
            // Leaving direct sampling runs the tuner's init again, which puts
            // its gain and filter back to power-on values the panel does not
            // show.
            reapplyTunerSettings();
        }

        // librtlsdr retunes to its last frequency on the way through, which
        // now means something else entirely; the engine retunes next anyway,
        // and until then the readout says what the radio holds.
        m_centerHz.store(static_cast<double>(rtlsdr_get_center_freq(m_device)));
        return ok();
    }

private:
    static void transferTrampoline(unsigned char* buffer, std::uint32_t length, void* context) {
        static_cast<RtlSdrDevice*>(context)->onTransfer(buffer, length);
    }

    /// The reader thread's whole life: librtlsdr's event loop, until stop()
    /// cancels it.
    void readLoop() {
        const int result =
            rtlsdr_read_async(m_device, &RtlSdrDevice::transferTrampoline, this, 0, kTransferBytes);
        if (result != 0 && m_streaming.load()) {
            // Not cancelled, so the dongle went away or libusb gave up on it.
            // Nothing arrives from here on, and the log is the only place that
            // can say why.
            log().error("the sample stream ended: librtlsdr error {}", result);
        }
        m_readerActive.store(false);
    }

    /// Runs on the reader thread, inside libusb's event handling.
    ///
    /// Nothing here may block, allocate or take a contended lock: this thread
    /// also services the USB transfers, and delaying it causes device-side
    /// overruns -- the failure the telemetry exists to report rather than to
    /// cause.
    void onTransfer(const unsigned char* buffer, std::uint32_t length) {
        if (!m_streaming.load(std::memory_order_acquire)) {
            return;
        }

        const auto* source = reinterpret_cast<const std::byte*>(buffer);
        auto remaining = static_cast<std::size_t>(length);

        constexpr std::size_t kBytesPerFrame = 2; // CU8

        while (remaining > 0) {
            if (!m_partial.valid()) {
                m_partial = m_stream->acquire();
                m_partialFrames = 0;
                // Stamped when the block *starts* filling, not when it is
                // published: if a retune lands mid-block, its later samples
                // belong to the next step, and the sweep engine's settle
                // discard covers that boundary.
                m_partialCenterHz = m_centerHz.load(std::memory_order_relaxed);
                m_partialSampleRate = m_sampleRate.load(std::memory_order_relaxed);
                m_partialStartNs = monotonicNs();
                if (!m_partial.valid()) {
                    // Pool exhausted: the consumer is behind. Drop the rest of
                    // this transfer and account for it rather than waiting --
                    // waiting on this thread would turn a host-side backlog
                    // into a device-side overrun.
                    m_stream->reportPoolExhausted(remaining / kBytesPerFrame);
                    return;
                }
            }

            const std::size_t capacityFrames =
                std::min(m_framesPerBlock, m_partial.capacityBytes() / kBytesPerFrame);
            const std::size_t wantedFrames = capacityFrames - m_partialFrames;
            const std::size_t availableFrames = remaining / kBytesPerFrame;
            const std::size_t take = std::min(wantedFrames, availableFrames);

            if (take == 0) {
                break;
            }

            std::memcpy(m_partial.data() + (m_partialFrames * kBytesPerFrame), source,
                        take * kBytesPerFrame);
            m_partialFrames += take;
            source += take * kBytesPerFrame;
            remaining -= take * kBytesPerFrame;

            if (m_partialFrames >= capacityFrames) {
                sweeppp_sdr_delivery_t delivery{};
                delivery.frames = m_partialFrames;
                delivery.format = plugin::toAbiFormat(SampleFormat::Cu8);
                delivery.sequence = m_sequence++;
                delivery.host_time_ns = m_partialStartNs;
                delivery.center_hz = m_partialCenterHz;
                delivery.sample_rate = m_partialSampleRate;

                m_partialFrames = 0;
                m_stream->publish(std::move(m_partial), delivery);
            }
        }
    }

    /// The tuner's gain steps, in tenths of a dB, as librtlsdr lists them.
    void readGains() {
        const int count = rtlsdr_get_tuner_gains(m_device, nullptr);
        if (count <= 0) {
            return;
        }
        m_gains.resize(static_cast<std::size_t>(count));
        const int filled = rtlsdr_get_tuner_gains(m_device, m_gains.data());
        m_gains.resize(static_cast<std::size_t>(std::clamp(filled, 0, count)));
        std::ranges::sort(m_gains);

        if (!m_gains.empty()) {
            // The middle of the ladder: well clear of both the noise floor and
            // the overload an antenna near anything strong reaches at the top.
            m_gainTenths.store(m_gains[m_gains.size() / 2]);
        }
    }

    [[nodiscard]] int nearestGain(int tenths) const {
        if (m_gains.empty()) {
            return tenths;
        }
        return *std::ranges::min_element(m_gains, [tenths](int a, int b) {
            return std::abs(a - tenths) < std::abs(b - tenths);
        });
    }

    [[nodiscard]] Status applyGain(int tenths) {
        if (const int result = rtlsdr_set_tuner_gain(m_device, tenths); result != 0) {
            return fail(ErrorCode::DeviceError, "set gain {:g} dB: librtlsdr error {}",
                        tenths / 10.0, result);
        }
        m_gainTenths.store(tenths);
        return ok();
    }

    void reapplyTunerSettings() {
        (void)rtlsdr_set_tuner_gain_mode(m_device, m_gainManual.load() ? 1 : 0);
        if (m_gainManual.load()) {
            (void)applyGain(m_gainTenths.load());
        }
#if SWEEPPP_RTLSDR_HAS_BANDWIDTH
        (void)rtlsdr_set_tuner_bandwidth(
            m_device, static_cast<std::uint32_t>(std::lround(m_bandwidthHz.load())));
#endif
    }

    /// Two inputs when the tuner leaves the ADC's Q branch free for direct
    /// sampling, and none otherwise: one input is the host's default, and
    /// saying "Tuner" alone adds a chooser with nothing on the other side.
    void buildPorts() {
        if (!m_tuner.directSampling) {
            return;
        }

        m_ports = {
            SdrRxPort{
                .id = "tuner",
                .label = std::format("Tuner ({})", m_tuner.name),
                .connector = "SMA",
                .minHz = m_tuner.minHz,
                .maxHz = m_tuner.maxHz,
                .biasTee = kHasBiasTee,
                .requiresStop = true,
                .switchSeconds = kPortSwitchSeconds,
            },
            SdrRxPort{
                .id = "hf",
                .label = "HF (direct sampling)",
                // The SMA on dongles built for it; solder pads on the Q
                // branch on one modified for it.
                .connector = "Q-branch input",
                .minHz = kDirectSamplingMinHz,
                .maxHz = kDirectSamplingMaxHz,
                .biasTee = kHasBiasTee,
                .requiresStop = true,
                .switchSeconds = kPortSwitchSeconds,
            },
        };
    }

    void buildParameters() {
        const double defaultCenter =
            std::clamp(100e6, m_info.minFrequencyHz, m_info.maxFrequencyHz);

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
                         .description =
                             m_tuner.directSampling
                                 ? std::format("{} tuner: {:g} - {:g} MHz, and {:g} - {:g} MHz "
                                               "through direct sampling.",
                                               m_tuner.name, m_tuner.minHz / 1e6,
                                               m_tuner.maxHz / 1e6, kDirectSamplingMinHz / 1e6,
                                               kDirectSamplingMaxHz / 1e6)
                                 : std::format("{} tuner: {:g} - {:g} MHz.", m_tuner.name,
                                               m_tuner.minHz / 1e6, m_tuner.maxHz / 1e6),
                         .defaultValue = defaultCenter},

            SdrParameter{.key = "sample_rate",
                         .label = "Sample rate",
                         .group = "Tuning",
                         .type = SdrParameterType::Double,
                         .unit = "S/s",
                         .min = kUsableSampleRates.front(),
                         .max = kUsableSampleRates.back(),
                         .step = 0.0,
                         .enumValues = sampleRateChoices(kUsableSampleRates),
                         .readOnly = false,
                         .gridAffecting = true,
                         .calibrationAffecting = false,
                         .requiresStop = false,
                         .description = "2.4 MS/s is the highest rate most dongles deliver "
                                        "without dropping samples; above it, whether they keep "
                                        "up depends on the host. Rates between 300 kS/s and "
                                        "900 kS/s do not exist on this chip.",
                         .defaultValue = kDefaultSampleRate},
        };

        if (kHasBandwidth) {
            m_parameters.push_back(SdrParameter{
                .key = "bandwidth",
                .label = "Tuner bandwidth",
                .group = "Filters",
                .type = SdrParameterType::Double,
                .unit = "Hz",
                .min = 0.0,
                .max = 8e6,
                .step = 0.0,
                .enumValues = {},
                .readOnly = false,
                .gridAffecting = false,
                .calibrationAffecting = false,
                .requiresStop = false,
                .description = "The tuner's IF filter. 0 follows the sample rate, which is "
                               "almost always what you want -- a filter narrower than the span "
                               "silently loses its edges. The tuner rounds to a width it has.",
                .defaultValue = 0.0});
        }

        // Omitted rather than shown empty when librtlsdr lists no steps: a
        // manual gain with nothing to pick from is a control that cannot work.
        if (!m_gains.empty()) {
            std::vector<SdrEnumValue> steps;
            steps.reserve(m_gains.size());
            for (const int tenths : m_gains) {
                steps.push_back({.value = toString(SdrValue{tenths / 10.0}),
                                 .label = std::format("{:g} dB", tenths / 10.0),
                                 .description = {}});
            }

            m_parameters.push_back(SdrParameter{
                .key = "gain_mode",
                .label = "Gain mode",
                .group = "Gain",
                .type = SdrParameterType::Enum,
                .unit = {},
                .min = 0.0,
                .max = 0.0,
                .step = 0.0,
                .enumValues = {{.value = std::string(kGainManual),
                                .label = "Manual",
                                .description = "The gain stays where you put it. The only mode "
                                               "in which the level scale is fixed, so it is "
                                               "the one to measure in."},
                               {.value = std::string(kGainAuto),
                                .label = "Automatic",
                                .description = "The tuner's own AGC. Free to move the gain "
                                               "between sweeps, so two of them stop being "
                                               "comparable."}},
                .readOnly = false,
                .gridAffecting = false,
                .calibrationAffecting = true,
                .requiresStop = false,
                .description = "Manual is the only one that measures.",
                .defaultValue = std::string(kGainManual)});

            m_parameters.push_back(
                SdrParameter{.key = "gain",
                             .label = "Tuner gain",
                             .group = "Gain",
                             .type = SdrParameterType::Double,
                             .unit = "dB",
                             .min = m_gains.front() / 10.0,
                             .max = m_gains.back() / 10.0,
                             .step = 0.0,
                             // The tuner's own steps, offered as presets. They are not
                             // evenly spaced, so no `step` could describe them; anything
                             // off-list is rounded to the nearest one and reported back.
                             .enumValues = std::move(steps),
                             .readOnly = false,
                             .gridAffecting = false,
                             .calibrationAffecting = true,
                             .requiresStop = false,
                             .description = std::format("The {} tuner's gain, in the steps it has.",
                                                        m_tuner.name),
                             .defaultValue = m_gainTenths.load() / 10.0,
                             .appliesWhenKey = "gain_mode",
                             .appliesWhenValues = {std::string(kGainManual)}});
        }

        m_parameters.push_back(SdrParameter{
            .key = "rtl_agc",
            .label = "RTL2832 AGC",
            .group = "Gain",
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
            .description = "The demodulator chip's digital AGC, separate from the tuner's gain. "
                           "It scales samples after the ADC, so it moves the level scale "
                           "without adding any sensitivity. Leave off to measure.",
            .defaultValue = false});

        m_parameters.push_back(SdrParameter{
            .key = "ppm",
            .label = "Frequency correction",
            .group = "Tuning",
            .type = SdrParameterType::Int,
            .unit = "ppm",
            .min = -100.0,
            .max = 100.0,
            .step = 1.0,
            .enumValues = {},
            .readOnly = false,
            .gridAffecting = false,
            .calibrationAffecting = false,
            .requiresStop = false,
            .description = "Corrects the crystal's error. A dongle without a TCXO is commonly "
                           "tens of ppm out, which at 1 GHz is tens of kHz.",
            .defaultValue = std::int64_t{0}});

        if (kHasBiasTee) {
            m_parameters.push_back(SdrParameter{
                .key = "bias_tee",
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
                .description = "Feeds DC up the antenna port for a powered LNA, on dongles "
                               "built with one. Leave off unless the attached hardware "
                               "expects it -- it can damage equipment that does not.",
                .defaultValue = false});
        }
    }

    rtlsdr_dev_t* m_device = nullptr;
    SdrDeviceInfo m_info;
    TunerSpec m_tuner;
    std::vector<SdrParameter> m_parameters;

    /// Empty when the tuner offers no direct sampling. Fixed after
    /// construction, so `selectedRxPort` can hand out a view into it.
    std::vector<SdrRxPort> m_ports;
    std::atomic<int> m_portIndex{0};

    /// Sorted ascending, in tenths of a dB.
    std::vector<int> m_gains;

    std::atomic<double> m_centerHz{100e6};
    std::atomic<double> m_sampleRate{kDefaultSampleRate};
    std::atomic<double> m_bandwidthHz{0.0};
    std::atomic<bool> m_gainManual{true};
    std::atomic<int> m_gainTenths{0};
    std::atomic<bool> m_rtlAgc{false};
    std::atomic<int> m_ppm{0};
    std::atomic<bool> m_biasTee{false};

    std::atomic<bool> m_streaming{false};
    /// Set before the reader starts and cleared as it leaves librtlsdr's loop,
    /// so stop() knows when its cancel has actually been heard.
    std::atomic<bool> m_readerActive{false};
    std::jthread m_reader;
    plugin::Stream* m_stream = nullptr;
    std::size_t m_framesPerBlock = 262'144;
    std::uint64_t m_sequence = 0;

    /// A transfer is far smaller than one pool block, so transfers are
    /// accumulated into a block and published when it fills. Touched only on
    /// the reader thread.
    plugin::Block m_partial;
    std::size_t m_partialFrames = 0;
    double m_partialCenterHz = 0.0;
    double m_partialSampleRate = 0.0;
    std::uint64_t m_partialStartNs = 0;
};

class RtlSdrFactory final : public plugin::Driver {
public:
    /// Reads each dongle's USB strings, which opens a libusb handle for a
    /// moment but claims nothing -- a dongle already streaming in another
    /// process still lists.
    [[nodiscard]] std::vector<SdrDeviceInfo> enumerate() const override {
        const std::uint32_t count = rtlsdr_get_device_count();

        std::vector<UsbStrings> strings;
        strings.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            strings.push_back(deviceStrings(i));
        }

        std::vector<SdrDeviceInfo> devices;
        devices.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::string& serial = strings[i].serial;
            // Many dongles leave the factory with the same serial, commonly
            // 00000001, and a profile that stored it would reopen whichever
            // came first. A position is at least unambiguous for as long as
            // nothing is plugged in or out.
            const bool unique =
                !serial.empty() && std::ranges::count_if(strings, [&serial](const UsbStrings& s) {
                                       return s.serial == serial;
                                   }) == 1;

            devices.push_back(SdrDeviceInfo{.driver = "rtlsdr",
                                            .id = unique ? serial : positionalId(i),
                                            .label = labelFor(strings[i], i),
                                            .serial = serial,
                                            .hardwareRevision = {},
                                            .firmware = {},
                                            .fpga = {},
                                            // The tuner is not known until the
                                            // dongle is opened, so this is the
                                            // commonest one's range.
                                            .minFrequencyHz = 24e6,
                                            .maxFrequencyHz = 1766e6,
                                            .minSampleRate = kUsableSampleRates.front(),
                                            .maxSampleRate = kUsableSampleRates.back(),
                                            .linkCapacityBytesPerSec = kLinkCapacityBytesPerSec,
                                            .linkDescription = kUsbDescription});
        }
        return devices;
    }

    [[nodiscard]] Result<std::unique_ptr<plugin::Radio>> open(std::string_view id) override {
        const std::uint32_t count = rtlsdr_get_device_count();
        if (count == 0) {
            return fail<std::unique_ptr<plugin::Radio>>(ErrorCode::NotFound,
                                                        "no RTL-SDR is attached");
        }

        const std::optional<std::uint32_t> index = resolveIndex(id, count);
        if (!index) {
            return fail<std::unique_ptr<plugin::Radio>>(
                ErrorCode::NotFound, "no RTL-SDR '{}' among the {} attached", id, count);
        }

        rtlsdr_dev_t* handle = nullptr;
        const int result = rtlsdr_open(&handle, *index);
        if (result != 0 || handle == nullptr) {
            return fail<std::unique_ptr<plugin::Radio>>(
                result == kLibusbErrorAccess ? ErrorCode::PermissionDenied : ErrorCode::DeviceError,
                "could not open RTL-SDR{}: error {}{}", id.empty() ? "" : std::format(" '{}'", id),
                result, openHint(result));
        }

        UsbStrings strings;
        {
            std::array<char, kUsbStringBytes> manufacturer{};
            std::array<char, kUsbStringBytes> product{};
            std::array<char, kUsbStringBytes> serial{};
            if (rtlsdr_get_usb_strings(handle, manufacturer.data(), product.data(),
                                       serial.data()) == 0) {
                strings = {.manufacturer = manufacturer.data(),
                           .product = product.data(),
                           .serial = serial.data()};
            }
        }

        const TunerSpec tuner = tunerSpec(rtlsdr_get_tuner_type(handle));

        SdrDeviceInfo info{.driver = "rtlsdr",
                           .id = !id.empty()               ? std::string(id)
                                 : !strings.serial.empty() ? strings.serial
                                                           : positionalId(*index),
                           .label = labelFor(strings, *index),
                           .serial = strings.serial,
                           .hardwareRevision = std::string(tuner.name),
                           .firmware = {},
                           .fpga = {},
                           // The union across both ports; each port says which part it reaches.
                           .minFrequencyHz =
                               tuner.directSampling ? kDirectSamplingMinHz : tuner.minHz,
                           .maxFrequencyHz = tuner.maxHz,
                           .minSampleRate = kUsableSampleRates.front(),
                           .maxSampleRate = kUsableSampleRates.back(),
                           .linkCapacityBytesPerSec = kLinkCapacityBytesPerSec,
                           .linkDescription = kUsbDescription};

        auto device = std::make_unique<RtlSdrDevice>(handle, std::move(info), tuner);

        // Applied so the radio's state matches what the panel reports the
        // moment it opens, rather than whatever the last user left behind.
        for (const SdrParameter& parameter : device->parameters()) {
            if (!parameter.readOnly) {
                (void)device->setParameter(parameter.key, parameter.defaultValue);
            }
        }

        log().info("opened {} (serial {}, {} tuner)", device->info().label,
                   device->info().serial.empty() ? "-" : device->info().serial, tuner.name);

        return device;
    }

private:
    [[nodiscard]] static UsbStrings deviceStrings(std::uint32_t index) {
        std::array<char, kUsbStringBytes> manufacturer{};
        std::array<char, kUsbStringBytes> product{};
        std::array<char, kUsbStringBytes> serial{};
        if (rtlsdr_get_device_usb_strings(index, manufacturer.data(), product.data(),
                                          serial.data()) != 0) {
            return {};
        }
        return {.manufacturer = manufacturer.data(),
                .product = product.data(),
                .serial = serial.data()};
    }

    /// The product string where the dongle has one, else librtlsdr's name
    /// for its USB id.
    [[nodiscard]] static std::string labelFor(const UsbStrings& strings, std::uint32_t index) {
        if (!strings.product.empty()) {
            return strings.product;
        }
        const char* name = rtlsdr_get_device_name(index);
        return name != nullptr && *name != '\0' ? std::string(name) : std::string("RTL-SDR");
    }

    /// An id back to a position in librtlsdr's list: a serial first, then the
    /// positional form enumerate() falls back to.
    [[nodiscard]] static std::optional<std::uint32_t> resolveIndex(std::string_view id,
                                                                   std::uint32_t count) {
        if (id.empty()) {
            return 0U;
        }

        const std::string serial(id);
        if (const int found = rtlsdr_get_index_by_serial(serial.c_str()); found >= 0) {
            return static_cast<std::uint32_t>(found);
        }

        constexpr std::string_view kPrefix = "index-";
        if (id.starts_with(kPrefix)) {
            std::uint32_t position = 0;
            const std::string_view digits = id.substr(kPrefix.size());
            const auto [end, error] =
                std::from_chars(digits.data(), digits.data() + digits.size(), position);
            if (error == std::errc{} && end == digits.data() + digits.size() && position < count) {
                return position;
            }
        }
        return std::nullopt;
    }

    /// The sentence that sends someone to the actual cause. "Error -3" alone
    /// sends them nowhere.
    [[nodiscard]] static std::string_view openHint(int result) {
        if (result == kLibusbErrorAccess) {
            return " (permission denied: on Linux, install librtlsdr's udev rules and replug "
                   "the dongle)";
        }
        if (result == kLibusbErrorBusy) {
            return " (the device is busy: on Linux the dvb_usb_rtl28xxu kernel driver claims "
                   "these dongles unless it is blacklisted; otherwise another program has it "
                   "open)";
        }
        return " (on Linux this is usually a missing udev rule, or the dvb_usb_rtl28xxu kernel "
               "driver holding the device)";
    }
};

} // namespace

plugin::Logger& log() noexcept {
    static plugin::Logger logger;
    return logger;
}

plugin::Driver& driver() noexcept {
    static RtlSdrFactory factory;
    return factory;
}

} // namespace sweeppp::rtl
