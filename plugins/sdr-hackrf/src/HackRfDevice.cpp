// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// Native HackRF driver, on libhackrf.
//
// Sweeping is done by retuning per step in software rather than with
// hackrf_init_sweep(). That is a deliberate decision, not an oversight: one
// code path serves every device, the settle and dwell behaviour stays explicit
// and measurable, and the sweep engine keeps full control of the schedule.
#include "HackRfDriver.hpp"
#include "sweeppp/core/Clock.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <hackrf.h>
#include <mutex>

namespace sweeppp::hackrf {
namespace {

// BOARD_ID_UNDETECTED came in with the board-revision API in libhackrf
// 2022.09.1; see this plugin's CMakeLists for the probe. Only ever a sentinel
// for a read that has not happened yet, so the fallback just needs to be the
// same out-of-range byte the header uses.
#if SWEEPPP_HACKRF_HAS_BOARD_REV
inline constexpr std::uint8_t kBoardIdUndetected = BOARD_ID_UNDETECTED;
#else
inline constexpr std::uint8_t kBoardIdUndetected = 0xFF;
#endif

/// libhackrf keeps process-wide state, so init/exit are reference counted
/// across every open device rather than done per device.
class HackRfLibrary {
public:
    [[nodiscard]] static Status acquire() {
        const std::lock_guard lock(mutex());
        if (refCount() == 0) {
            const int result = hackrf_init();
            if (result != HACKRF_SUCCESS) {
                return fail(ErrorCode::DeviceError, "hackrf_init failed: {}",
                            hackrf_error_name(static_cast<hackrf_error>(result)));
            }
        }
        ++refCount();
        return ok();
    }

    static void release() {
        const std::lock_guard lock(mutex());
        if (refCount() > 0 && --refCount() == 0) {
            hackrf_exit();
        }
    }

private:
    static std::mutex& mutex() {
        static std::mutex instance;
        return instance;
    }
    static int& refCount() {
        static int instance = 0;
        return instance;
    }
};

[[nodiscard]] std::string hackrfError(int result) {
    return hackrf_error_name(static_cast<hackrf_error>(result));
}

/// Practical USB throughput ceiling, in bytes per second.
///
/// A constant rather than a query: libhackrf does not expose its libusb
/// handle, so libusb_get_device_speed() is not reachable -- and it would not
/// tell us anything new anyway, because the HackRF One has no SuperSpeed PHY
/// and is always USB 2.0 high speed. 35 MB/s is the realistic sustained figure
/// rather than the 60 MB/s the bus nominally offers.
///
/// This feeds the Performance panel's utilisation bar. At 20 MS/s the radio
/// produces 40 MB/s of CS8, which is *over* this ceiling -- and showing that
/// is exactly the point: it turns "why am I dropping samples" into "the link
/// cannot carry this rate".
inline constexpr std::uint64_t kUsbCapacityBytesPerSec = 35'000'000ULL;
inline constexpr const char* kUsbDescription = "USB 2.0 high speed";

/// The rates worth offering, in practice.
///
/// Below 8 MS/s the filter roll-off eats most of the span; above 20 MS/s the
/// USB link starts dropping. Named once because it is both the panel's preset
/// list and what `supportedSampleRates()` reports, and two copies of it would
/// eventually disagree about which rates a HackRF has.
inline constexpr std::array kUsableSampleRates{2e6, 4e6, 8e6, 10e6, 12.5e6, 16e6, 20e6};

class HackRfDevice final : public plugin::Radio {
public:
    HackRfDevice(hackrf_device* device, SdrDeviceInfo info)
        : m_device(device), m_info(std::move(info)) {
        buildParameters();
    }

    ~HackRfDevice() override {
        HackRfDevice::stop();
        if (m_device != nullptr) {
            hackrf_close(m_device);
        }
        HackRfLibrary::release();
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
        if (key == "bandwidth") {
            return SdrValue{m_bandwidthHz.load()};
        }
        if (key == "lna_gain") {
            return SdrValue{std::int64_t{m_lnaGain.load()}};
        }
        if (key == "vga_gain") {
            return SdrValue{std::int64_t{m_vgaGain.load()}};
        }
        if (key == "amp_enable") {
            return SdrValue{m_ampEnabled.load()};
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
        if (key == "sample_rate") {
            const double rate = asDouble(*coerced);
            const int result = hackrf_set_sample_rate(m_device, rate);
            if (result != HACKRF_SUCCESS) {
                return fail(ErrorCode::DeviceError, "set sample rate: {}", hackrfError(result));
            }
            m_sampleRate.store(rate);

            // The baseband filter follows the sample rate unless the operator
            // has pinned it. Leaving a narrow filter across a wide rate is a
            // classic way to lose the edges of the span without noticing.
            if (m_bandwidthAuto.load()) {
                applyAutoBandwidth(rate);
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
            // The radio only has a fixed ladder of filter widths, so the
            // request is snapped to one and the snapped value reported back.
            const std::uint32_t actual =
                hackrf_compute_baseband_filter_bw(static_cast<std::uint32_t>(requested));
            const int result = hackrf_set_baseband_filter_bandwidth(m_device, actual);
            if (result != HACKRF_SUCCESS) {
                return fail(ErrorCode::DeviceError, "set filter bandwidth: {}",
                            hackrfError(result));
            }
            m_bandwidthHz.store(actual);
            return ok();
        }
        if (key == "lna_gain") {
            const auto gain = static_cast<std::uint32_t>(asInt(*coerced));
            const int result = hackrf_set_lna_gain(m_device, gain);
            if (result != HACKRF_SUCCESS) {
                return fail(ErrorCode::DeviceError, "set LNA gain: {}", hackrfError(result));
            }
            m_lnaGain.store(static_cast<int>(gain));
            return ok();
        }
        if (key == "vga_gain") {
            const auto gain = static_cast<std::uint32_t>(asInt(*coerced));
            const int result = hackrf_set_vga_gain(m_device, gain);
            if (result != HACKRF_SUCCESS) {
                return fail(ErrorCode::DeviceError, "set VGA gain: {}", hackrfError(result));
            }
            m_vgaGain.store(static_cast<int>(gain));
            return ok();
        }
        if (key == "amp_enable") {
            const bool enabled = asBool(*coerced);
            const int result = hackrf_set_amp_enable(m_device, enabled ? 1 : 0);
            if (result != HACKRF_SUCCESS) {
                return fail(ErrorCode::DeviceError, "set amp: {}", hackrfError(result));
            }
            m_ampEnabled.store(enabled);
            return ok();
        }
        if (key == "bias_tee") {
            const bool enabled = asBool(*coerced);
            const int result = hackrf_set_antenna_enable(m_device, enabled ? 1 : 0);
            if (result != HACKRF_SUCCESS) {
                return fail(ErrorCode::DeviceError, "set bias tee: {}", hackrfError(result));
            }
            m_biasTee.store(enabled);
            return ok();
        }

        return fail(ErrorCode::NotFound, "no parameter '{}'", key);
    }

    // The radio delivers signed 8-bit I/Q. At 20 MS/s that is 40 MB/s -- most
    // of a USB 2.0 link, and the reason nothing on the acquisition path is
    // allowed to convert to float.
    [[nodiscard]] SampleFormat nativeFormat() const noexcept override { return SampleFormat::Cs8; }

    [[nodiscard]] std::vector<double> supportedSampleRates() const override {
        return {kUsableSampleRates.begin(), kUsableSampleRates.end()};
    }

    [[nodiscard]] Status start(plugin::Stream& stream, const StreamConfig& config) override {
        if (m_streaming.load()) {
            return fail(ErrorCode::AlreadyExists, "already streaming");
        }

        m_stream = &stream;
        m_framesPerBlock = config.framesPerBlock;
        m_sequence = 0;
        m_partialFrames = 0;
        m_streaming.store(true);

        const int result = hackrf_start_rx(m_device, &HackRfDevice::rxCallbackTrampoline, this);
        if (result != HACKRF_SUCCESS) {
            m_streaming.store(false);
            return fail(ErrorCode::DeviceError, "hackrf_start_rx: {}", hackrfError(result));
        }
        return ok();
    }

    void stop() override {
        if (!m_streaming.exchange(false)) {
            return;
        }
        if (m_device != nullptr) {
            // Returns once libhackrf's transfer thread has stopped, which is
            // what makes dropping the stream below safe: the host tears it
            // down as soon as this returns.
            hackrf_stop_rx(m_device);
        }
        m_partial.release();
        m_stream = nullptr;
    }

    [[nodiscard]] bool streaming() const noexcept override { return m_streaming.load(); }

    [[nodiscard]] Status retune(double centerHz) override {
        const int result = hackrf_set_freq(m_device, static_cast<std::uint64_t>(centerHz));
        if (result != HACKRF_SUCCESS) {
            return fail(ErrorCode::DeviceError, "set frequency: {}", hackrfError(result));
        }
        m_centerHz.store(centerHz);
        return ok();
    }

    [[nodiscard]] double deliveryGranularitySeconds(double sampleRate) const noexcept override {
        // libhackrf hands over fixed 262144-byte USB transfers, and its
        // callback is the only place the host learns anything about timing.
        // Whatever we cut those bytes into afterwards, the samples inside one
        // transfer cannot be told apart in time, so this is the finest a step
        // can be resolved. Retuning faster does not sweep faster -- it just
        // stops most steps from being measured at all.
        //
        // Hardware sweep mode is the way past this, and is deliberately not
        // used here.
        constexpr double kTransferBytes = 262144.0;
        return sampleRate > 0.0 ? kTransferBytes / (sampleRate * bytesPerFrame(SampleFormat::Cs8))
                                : 0.0;
    }

    [[nodiscard]] double retuneSettleSeconds() const noexcept override {
        // The synthesiser needs on the order of a few hundred microseconds to
        // settle after a retune, and samples taken during it are not
        // trustworthy. Measured conservatively -- discarding slightly too much
        // costs sweep rate, whereas keeping unsettled samples smears one step's
        // signal across the next step's band.
        return 300e-6;
    }

private:
    void applyAutoBandwidth(double sampleRate) {
        // 75% of the sample rate: wide enough to keep the usable span, narrow
        // enough to reject the alias energy just outside it.
        const std::uint32_t width =
            hackrf_compute_baseband_filter_bw(static_cast<std::uint32_t>(sampleRate * 0.75));
        if (hackrf_set_baseband_filter_bandwidth(m_device, width) == HACKRF_SUCCESS) {
            m_bandwidthHz.store(width);
        }
    }

    static int rxCallbackTrampoline(hackrf_transfer* transfer) {
        return static_cast<HackRfDevice*>(transfer->rx_ctx)->onTransfer(transfer);
    }

    /// Runs on libhackrf's USB transfer thread.
    ///
    /// Nothing here may block, allocate or take a contended lock: this thread
    /// also services the USB transfers, and delaying it causes device-side
    /// overruns -- the failure the telemetry exists to report rather than to
    /// cause.
    int onTransfer(hackrf_transfer* transfer) {
        if (!m_streaming.load(std::memory_order_acquire)) {
            return 0;
        }

        const auto* source = reinterpret_cast<const std::byte*>(transfer->buffer);
        auto remaining = static_cast<std::size_t>(transfer->valid_length);

        constexpr std::size_t kBytesPerFrame = 2; // CS8

        while (remaining > 0) {
            if (!m_partial.valid()) {
                m_partial = m_stream->acquire();
                m_partialFrames = 0;
                // Stamped when the block *starts* filling, not when it is
                // published. Its first samples were captured now; if a retune
                // lands mid-block the later ones belong to the next step, and
                // recording the end frequency would mislabel the whole block.
                // The sweep engine's settle discard covers the boundary case.
                m_partialCenterHz = m_centerHz.load(std::memory_order_relaxed);
                m_partialSampleRate = m_sampleRate.load(std::memory_order_relaxed);
                m_partialStartNs = monotonicNs();
                if (!m_partial.valid()) {
                    // Pool exhausted: the consumer is behind. Drop the rest of
                    // this transfer and account for it, rather than waiting --
                    // waiting on this thread would turn a host-side backlog
                    // into a device-side overrun.
                    m_stream->reportPoolExhausted(remaining / kBytesPerFrame);
                    return 0;
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

            std::memcpy(m_partial.data() + m_partialFrames * kBytesPerFrame, source,
                        take * kBytesPerFrame);
            m_partialFrames += take;
            source += take * kBytesPerFrame;
            remaining -= take * kBytesPerFrame;

            if (m_partialFrames >= capacityFrames) {
                sweeppp_sdr_delivery_t delivery{};
                delivery.frames = m_partialFrames;
                delivery.format = plugin::toAbiFormat(SampleFormat::Cs8);
                delivery.sequence = m_sequence++;
                // The values captured when this block started filling.
                delivery.host_time_ns = m_partialStartNs;
                delivery.center_hz = m_partialCenterHz;
                delivery.sample_rate = m_partialSampleRate;

                m_partialFrames = 0;
                m_stream->publish(std::move(m_partial), delivery);
            }
        }

        return 0;
    }

    void buildParameters() {
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
                         .description = "1 MHz to 6 GHz.",
                         .defaultValue = 100e6},

            SdrParameter{.key = "sample_rate",
                         .label = "Sample rate",
                         .group = "Tuning",
                         .type = SdrParameterType::Double,
                         .unit = "S/s",
                         .min = 2e6,
                         .max = 20e6,
                         .step = 0.0,
                         // The same usable set `supportedSampleRates()` names,
                         // offered as presets. `hackrf_set_sample_rate` accepts
                         // anything in range -- it computes a fractional
                         // divider -- so a rate from `--sample-rate` or a
                         // profile is still honoured off-list.
                         .enumValues = sampleRateChoices(kUsableSampleRates),
                         .readOnly = false,
                         .gridAffecting = true,
                         .calibrationAffecting = false,
                         .requiresStop = false,
                         .description = "8 to 20 MS/s is the useful range. Above 20 MS/s the "
                                        "USB 2.0 link cannot keep up and samples are dropped.",
                         .defaultValue = 10e6},

            SdrParameter{.key = "bandwidth",
                         .label = "Filter bandwidth",
                         .group = "Filters",
                         .type = SdrParameterType::Double,
                         .unit = "Hz",
                         .min = 0.0,
                         .max = 28e6,
                         .step = 0.0,
                         .enumValues = {},
                         .readOnly = false,
                         .gridAffecting = false,
                         .calibrationAffecting = false,
                         .requiresStop = false,
                         .description = "Baseband filter width. 0 follows the sample rate "
                                        "automatically, which is almost always what you want -- "
                                        "a filter narrower than the span silently loses its "
                                        "edges. Snapped to the radio's fixed ladder of widths.",
                         .defaultValue = 0.0},

            // The three gain stages, exposed separately because they are not
            // interchangeable: the amp and LNA set the noise figure, the VGA
            // only scales what already came through.
            SdrParameter{.key = "amp_enable",
                         .label = "RF amp (+14 dB)",
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
                         .description = "Front-end amplifier. Improves sensitivity for weak "
                                        "signals, but overloads easily near a strong "
                                        "transmitter. Try it off first.",
                         .defaultValue = false},

            SdrParameter{.key = "lna_gain",
                         .label = "LNA (IF) gain",
                         .group = "Gain",
                         .type = SdrParameterType::Int,
                         .unit = "dB",
                         .min = 0.0,
                         .max = 40.0,
                         // The hardware only accepts multiples of 8. A
                         // continuous control would let the operator pick a
                         // value the radio silently rounds, then disbelieve
                         // the readout.
                         .step = 8.0,
                         .enumValues = {},
                         .readOnly = false,
                         .gridAffecting = false,
                         .calibrationAffecting = true,
                         .requiresStop = false,
                         .description = "0 to 40 dB in 8 dB steps. Sets the noise figure -- "
                                        "raise this before the VGA.",
                         .defaultValue = std::int64_t{16}},

            SdrParameter{.key = "vga_gain",
                         .label = "VGA (baseband) gain",
                         .group = "Gain",
                         .type = SdrParameterType::Int,
                         .unit = "dB",
                         .min = 0.0,
                         .max = 62.0,
                         .step = 2.0,
                         .enumValues = {},
                         .readOnly = false,
                         .gridAffecting = false,
                         .calibrationAffecting = true,
                         .requiresStop = false,
                         .description = "0 to 62 dB in 2 dB steps. Amplifies signal and noise "
                                        "equally, so it sets the display level rather than the "
                                        "sensitivity.",
                         .defaultValue = std::int64_t{20}},

            SdrParameter{.key = "bias_tee",
                         .label = "Bias tee (+3.3 V)",
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
                         .description = "Feeds 3.3 V up the antenna port for a powered LNA. "
                                        "Leave off unless the attached hardware expects it -- "
                                        "it can damage equipment that does not.",
                         .defaultValue = false},
        };
    }

    hackrf_device* m_device = nullptr;
    SdrDeviceInfo m_info;
    std::vector<SdrParameter> m_parameters;

    std::atomic<double> m_centerHz{100e6};
    std::atomic<double> m_sampleRate{10e6};
    std::atomic<double> m_bandwidthHz{0.0};
    std::atomic<bool> m_bandwidthAuto{true};
    std::atomic<int> m_lnaGain{16};
    std::atomic<int> m_vgaGain{20};
    std::atomic<bool> m_ampEnabled{false};
    std::atomic<bool> m_biasTee{false};

    std::atomic<bool> m_streaming{false};
    plugin::Stream* m_stream = nullptr;
    std::size_t m_framesPerBlock = 262'144;
    std::uint64_t m_sequence = 0;

    /// A USB transfer is far smaller than one pool block, so transfers are
    /// accumulated into a block and published when it fills. Touched only on
    /// the transfer thread.
    plugin::Block m_partial;
    std::size_t m_partialFrames = 0;
    double m_partialCenterHz = 0.0;
    double m_partialSampleRate = 0.0;
    std::uint64_t m_partialStartNs = 0;
};

class HackRfFactory final : public plugin::Driver {
public:
    [[nodiscard]] std::vector<SdrDeviceInfo> enumerate() const override {
        if (auto acquired = HackRfLibrary::acquire(); !acquired) {
            log().debug("{}", acquired.error().describe());
            return {};
        }

        std::vector<SdrDeviceInfo> devices;

        hackrf_device_list_t* list = hackrf_device_list();
        if (list != nullptr) {
            for (int i = 0; i < list->devicecount; ++i) {
                const char* serial = list->serial_numbers[i];

                devices.push_back(
                    SdrDeviceInfo{.driver = "hackrf",
                                  .id = serial != nullptr ? serial : std::format("index-{}", i),
                                  .label = boardName(list->usb_board_ids[i]),
                                  .serial = serial != nullptr ? serial : "",
                                  .hardwareRevision = boardName(list->usb_board_ids[i]),
                                  .firmware = {},
                                  .minFrequencyHz = 1e6,
                                  .maxFrequencyHz = 6e9,
                                  .minSampleRate = 2e6,
                                  .maxSampleRate = 20e6,
                                  .linkCapacityBytesPerSec = 35'000'000ULL,
                                  .linkDescription = "USB 2.0 high speed"});
            }
            hackrf_device_list_free(list);
        }

        HackRfLibrary::release();
        return devices;
    }

    [[nodiscard]] Result<std::unique_ptr<plugin::Radio>> open(std::string_view id) override {
        if (auto acquired = HackRfLibrary::acquire(); !acquired) {
            return std::unexpected(acquired.error());
        }

        hackrf_device* handle = nullptr;
        const int result = id.empty() ? hackrf_open(&handle)
                                      : hackrf_open_by_serial(std::string(id).c_str(), &handle);

        if (result != HACKRF_SUCCESS || handle == nullptr) {
            HackRfLibrary::release();
            // The overwhelmingly common cause on Linux is a missing udev rule,
            // and "permission denied" alone sends people looking in the wrong
            // place entirely.
            return fail<std::unique_ptr<plugin::Radio>>(
                ErrorCode::DeviceError, "could not open HackRF{}: {}{}",
                id.empty() ? "" : std::format(" '{}'", id), hackrfError(result),
                result == HACKRF_ERROR_LIBUSB
                    ? " (on Linux this is usually a missing udev rule; on macOS, another "
                      "process already holding the device)"
                    : "");
        }

        SdrDeviceInfo info{.driver = "hackrf",
                           .id = std::string(id),
                           .label = "HackRF",
                           .serial = std::string(id),
                           .hardwareRevision = {},
                           .firmware = {},
                           .minFrequencyHz = 1e6,
                           .maxFrequencyHz = 6e9,
                           .minSampleRate = 2e6,
                           .maxSampleRate = 20e6,
                           .linkCapacityBytesPerSec = 0,
                           .linkDescription = {}};

        // Identity is read back from the hardware rather than assumed, so the
        // device chip shows what is actually attached.
        std::array<char, 256> version{};
        if (hackrf_version_string_read(handle, version.data(),
                                       static_cast<std::uint8_t>(version.size() - 1)) ==
            HACKRF_SUCCESS) {
            info.firmware.version = version.data();
        }

        std::uint8_t boardId = kBoardIdUndetected;
        if (hackrf_board_id_read(handle, &boardId) == HACKRF_SUCCESS) {
            info.label = hackrf_board_id_name(static_cast<hackrf_board_id>(boardId));
        }

#if SWEEPPP_HACKRF_HAS_BOARD_REV
        // "HackRF One" spans r1 to r10, and r9 onwards is a different RF front
        // end, so the revision is part of the model rather than a detail.
        // Firmware older than 2021.03.1 has no such command and answers
        // undetected; "older than r6" is not a revision either, and neither is
        // worth widening the label with.
        std::uint8_t boardRev = BOARD_REV_UNDETECTED;
        if (hackrf_board_rev_read(handle, &boardRev) == HACKRF_SUCCESS &&
            boardRev != BOARD_REV_UNDETECTED && boardRev != BOARD_REV_UNRECOGNIZED &&
            (boardRev & ~HACKRF_BOARD_REV_GSG) != BOARD_REV_HACKRF1_OLD) {
            const char* revision = hackrf_board_rev_name(static_cast<hackrf_board_rev>(boardRev));
            if (revision != nullptr) {
                info.hardwareRevision = revision;
                info.label = std::format("{} {}", info.label, revision);
            }
        }
#endif

        if (info.serial.empty()) {
            read_partid_serialno_t serialNumber{};
            if (hackrf_board_partid_serialno_read(handle, &serialNumber) == HACKRF_SUCCESS) {
                info.serial = std::format("{:08x}{:08x}{:08x}{:08x}", serialNumber.serial_no[0],
                                          serialNumber.serial_no[1], serialNumber.serial_no[2],
                                          serialNumber.serial_no[3]);
                info.id = info.serial;
            }
        }

        info.linkCapacityBytesPerSec = kUsbCapacityBytesPerSec;
        info.linkDescription = kUsbDescription;

        auto device = std::make_unique<HackRfDevice>(handle, std::move(info));

        // Applied so the radio's state matches what the panel reports the
        // moment it opens, rather than whatever the last user left behind.
        for (const SdrParameter& parameter : device->parameters()) {
            if (!parameter.readOnly) {
                (void)device->setParameter(parameter.key, parameter.defaultValue);
            }
        }

        log().info("opened {} (serial {}, firmware {})", device->info().label,
                   device->info().serial.empty() ? "-" : device->info().serial,
                   device->info().firmware.version.empty() ? "-" : device->info().firmware.version);

        return device;
    }

private:
    [[nodiscard]] static std::string boardName(hackrf_usb_board_id usbBoardId) {
        const char* name = hackrf_usb_board_id_name(usbBoardId);
        return name != nullptr ? name : "HackRF";
    }
};

} // namespace

plugin::Logger& log() noexcept {
    static plugin::Logger logger;
    return logger;
}

plugin::Driver& driver() noexcept {
    static HackRfFactory factory;
    return factory;
}

} // namespace sweeppp::hackrf
