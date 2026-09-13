// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// Native Fobos SDR driver, on RigExpert's two libraries.
//
// A Fobos SDR runs one of two firmwares, and each has a library of its own
// that does not see radios running the other. So enumeration asks both, and a
// radio is opened through whichever library listed it. Nobody chooses: the
// firmware decides, and the label says which one was found.
//
// Samples arrive through the libraries' synchronous interface, already
// converted to floats, scaled, and DC- and IQ-corrected, on a receive thread
// this driver owns -- the bladeRF driver's arrangement, for the same reasons.
//
// Both firmwares sweep by retuning per step. The agile firmware's own
// hardware scan is not used.
#include "FobosDriver.hpp"
#include "FobosLibrary.hpp"
#include "sweeppp/core/Clock.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace sweeppp::fobos {
namespace {

/// The RF input's reach, from the standard library's band table -- whose
/// first row starts at 50 MHz and last ends at 6900. The agile library has no
/// such table because its firmware tunes, but the front end is the same one.
inline constexpr double kMinFrequencyHz = 50e6;
inline constexpr double kMaxFrequencyHz = 6900e6;

/// Frames per synchronous read.
///
/// A multiple of both libraries' units: 128 frames for the standard one, and
/// 8192 for the agile one, which silently truncates anything else. It is also
/// the smallest read the agile firmware's hardware scan runs with, so the
/// same reads will serve that when it is used.
inline constexpr std::uint32_t kReadFrames = 65'536;

/// How long after a retune samples are not trusted.
///
/// A starting figure rather than a measured one, meant to cover the RFFC507x
/// and MAX2830 synthesisers locking and the library's DC filter recovering
/// from the step. Measure it on hardware and put the number here; too long
/// costs sweep rate, too short smears one step into the next.
inline constexpr double kRetuneSettleSeconds = 2e-3;

/// Link capacity as the Performance panel compares it: against bytes
/// delivered, not bytes on the wire.
///
/// The wire carries 4 bytes a frame, two 16-bit samples, and the library hands
/// over 8, two floats. SuperSpeed's practical 400 MB/s is doubled to match, so
/// the utilisation bar shows the link's real share rather than twice it.
inline constexpr std::uint64_t kLinkCapacityBytesPerSec = 800'000'000ULL;
inline constexpr const char* kUsbDescription = "USB 3.0 SuperSpeed";

inline constexpr SampleFormat kFormat = SampleFormat::Cf32;

/// Rates for a radio that has not been opened yet, so its firmware generation
/// is unknown. The widest either library lists for its firmware.
inline constexpr double kListedMinRate = 8e6;
inline constexpr double kStandardListedMaxRate = 80e6;
inline constexpr double kAgileListedMaxRate = 64e6;

inline constexpr std::string_view kClockInternal = "internal";
inline constexpr std::string_view kClockExternal = "external";

[[nodiscard]] std::string labelFor(Firmware firmware, std::string_view product) {
    const std::string_view base = product.empty() ? std::string_view("Fobos SDR") : product;
    return firmware == Firmware::Agile ? std::format("{} (agile)", base) : std::string(base);
}

class FobosDevice final : public plugin::Radio {
public:
    FobosDevice(std::unique_ptr<FobosHandle> handle, SdrDeviceInfo info)
        : m_handle(std::move(handle)), m_info(std::move(info)) {
        buildParameters();
    }

    ~FobosDevice() override { FobosDevice::stop(); }

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
        if (key == "lna_gain") {
            return SdrValue{std::int64_t{m_lnaGain.load()}};
        }
        if (key == "vga_gain") {
            return SdrValue{std::int64_t{m_vgaGain.load()}};
        }
        if (key == "clock_source") {
            return SdrValue{std::string(m_externalClock.load() ? kClockExternal : kClockInternal)};
        }
        if (key == "bandwidth" && m_handle->hasBandwidth()) {
            return SdrValue{m_bandwidthHz.load()};
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
            const Result<double> actual = m_handle->setSampleRate(asDouble(*coerced));
            if (!actual) {
                return std::unexpected(actual.error());
            }
            // What the radio is running at rather than what was asked: on a
            // fixed ladder the two differ, and every derived figure -- RBW,
            // span, the frequency axis -- follows this one.
            m_sampleRate.store(*actual);
            return ok();
        }
        if (key == "lna_gain") {
            const auto step = static_cast<unsigned>(asInt(*coerced));
            if (Status applied = m_handle->setLnaGain(step); !applied) {
                return applied;
            }
            m_lnaGain.store(static_cast<int>(step));
            return ok();
        }
        if (key == "vga_gain") {
            const auto step = static_cast<unsigned>(asInt(*coerced));
            if (Status applied = m_handle->setVgaGain(step); !applied) {
                return applied;
            }
            m_vgaGain.store(static_cast<int>(step));
            return ok();
        }
        if (key == "clock_source") {
            const bool external = asString(*coerced) == kClockExternal;
            if (Status applied = m_handle->setExternalClock(external); !applied) {
                return applied;
            }
            m_externalClock.store(external);
            return ok();
        }
        if (key == "bandwidth") {
            const double width = asDouble(*coerced);
            if (Status applied = m_handle->setBandwidth(width); !applied) {
                return applied;
            }
            m_bandwidthHz.store(width);
            return ok();
        }

        return fail(ErrorCode::NotFound, "no parameter '{}'", key);
    }

    [[nodiscard]] SampleFormat nativeFormat() const noexcept override { return kFormat; }

    [[nodiscard]] std::vector<double> supportedSampleRates() const override {
        // Empty is "anything in range", which is exactly true of a radio whose
        // list is only a recommendation.
        return m_handle->arbitrarySampleRates() ? std::vector<double>{} : m_handle->sampleRates();
    }

    [[nodiscard]] Status start(plugin::Stream& stream, const StreamConfig& config) override {
        if (m_streaming.load()) {
            return fail(ErrorCode::AlreadyExists, "already streaming");
        }

        // Both allocated whatever the block size: staging is where a read
        // goes when it does not fit one block exactly, and the discard buffer
        // has to be there at the moment the pool is not.
        const std::size_t samples = static_cast<std::size_t>(kReadFrames) * 2;
        m_staging.assign(samples, 0.0F);
        m_discard.assign(samples, 0.0F);
        m_stagedFrames = 0;
        m_stagedTaken = 0;
        m_readFailures = 0;

        if (Status started = m_handle->startSync(kReadFrames); !started) {
            return started;
        }

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
        // Joined before the library is told to stop: stopping frees the buffer
        // a read in progress is filling. The read the thread is in finishes
        // within one transfer -- both libraries wait on it without a timeout,
        // but at 8 MS/s and up one arrives every few milliseconds.
        m_thread.request_stop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        m_handle->stopSync();
        m_stream = nullptr;
    }

    [[nodiscard]] bool streaming() const noexcept override { return m_streaming.load(); }

    [[nodiscard]] Status retune(double centerHz) override {
        if (Status tuned = m_handle->setFrequency(centerHz); !tuned) {
            return tuned;
        }
        m_centerHz.store(centerHz);
        return ok();
    }

    [[nodiscard]] double deliveryGranularitySeconds(double sampleRate) const noexcept override {
        // One read is one USB transfer with one tuning attached, and nothing
        // inside it can be told apart in time.
        return sampleRate > 0.0 ? static_cast<double>(kReadFrames) / sampleRate : 0.0;
    }

    [[nodiscard]] double retuneSettleSeconds() const noexcept override {
        return kRetuneSettleSeconds;
    }

private:
    static constexpr std::size_t kBytesPerFrame = 2 * sizeof(float);

    /// A block, waiting briefly for one rather than giving up at once.
    ///
    /// The pool empties in bursts, when a retune's control transfer holds up
    /// the bus and the backlog behind it arrives in a rush. Waiting those out
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

    /// Reads one transfer and throws it away, to keep the radio draining when
    /// there is nowhere to put the data. Returns the frames lost.
    ///
    /// A synchronous interface drains only when it is called, so not reading
    /// is not a way of dropping: it stalls the USB stream, and the device
    /// overruns instead -- uncounted, where this is counted.
    [[nodiscard]] std::size_t drainAndDiscard(const std::stop_token& stop) {
        const Result<std::uint32_t> read = m_handle->readSync(m_discard.data());
        if (!read) {
            reportReadFailure(read.error(), stop);
            return 0;
        }
        // A gap the host caused: the samples after it are not continuous with
        // the ones before, and the sequence number says so.
        ++m_sequence;
        return *read;
    }

    /// Captures what the read about to be issued will be describing.
    ///
    /// Before the call rather than after, because the samples arrive during
    /// it: the tuning in force as it begins is the one they belong to. A
    /// retune landing mid-read is what the sweep engine's settle discard is
    /// for.
    void beginRead() noexcept {
        m_stagedStartNs = monotonicNs();
        m_stagedCenterHz = m_centerHz.load();
        m_stagedSampleRate = m_sampleRate.load();
        m_stagedNsPerFrame = m_stagedSampleRate > 0.0 ? 1e9 / m_stagedSampleRate : 0.0;
    }

    /// Reads one transfer into staging. False when nothing arrived.
    [[nodiscard]] bool fillStaging(const std::stop_token& stop) {
        beginRead();
        const Result<std::uint32_t> read = m_handle->readSync(m_staging.data());
        if (!read) {
            reportReadFailure(read.error(), stop);
            return false;
        }
        m_stagedFrames = *read;
        m_stagedTaken = 0;
        return m_stagedFrames > 0;
    }

    /// One read failure, logged well enough to act on, and then counted.
    ///
    /// Both libraries wait on a transfer with no timeout, so a failure comes
    /// back at once -- an unplugged radio fails every read immediately. The
    /// pause keeps that from spinning a core and flooding the log while the
    /// operator works out why the spectrum froze.
    void reportReadFailure(const Error& error, const std::stop_token& stop) {
        if (stop.stop_requested()) {
            return;
        }
        const std::uint64_t count = ++m_readFailures;
        if (count == 1 || count % 50 == 0) {
            log().warn("read failed: {} -- {} failure(s) so far", error.message(), count);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
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

            if (want == kReadFrames && m_stagedTaken >= m_stagedFrames) {
                // Zero-copy: one read is exactly one block, so the library
                // converts straight into the pool.
                beginRead();
                const Result<std::uint32_t> read = m_handle->readSync(block.as<float>());
                if (!read) {
                    reportReadFailure(read.error(), stop);
                    continue;
                }
                filled = *read;
                blockStartNs = m_stagedStartNs;
                blockCenterHz = m_stagedCenterHz;
                blockSampleRate = m_stagedSampleRate;
            } else {
                // Filled COMPLETELY, across as many reads as it takes. The
                // read size is fixed and the block size is one FFT; neither
                // divides the other in general, and a block short of one
                // transform is a block the pipeline can only drop.
                while (filled < want && !stop.stop_requested()) {
                    if (m_stagedTaken >= m_stagedFrames && !fillStaging(stop)) {
                        break;
                    }

                    if (filled == 0) {
                        // Stamped from the read its first sample came out of.
                        blockStartNs = m_stagedStartNs +
                                       static_cast<std::uint64_t>(
                                           static_cast<double>(m_stagedTaken) * m_stagedNsPerFrame);
                        blockCenterHz = m_stagedCenterHz;
                        blockSampleRate = m_stagedSampleRate;
                    }

                    const std::size_t take =
                        std::min(want - filled, m_stagedFrames - m_stagedTaken);
                    std::memcpy(block.as<float>() + (filled * 2),
                                m_staging.data() + (m_stagedTaken * 2), take * kBytesPerFrame);
                    filled += take;
                    m_stagedTaken += take;
                }
            }

            if (filled < want) {
                // A short read, a failed one, or a stop part-way through. The
                // block goes back unpublished, and the sequence moves so the
                // gap is visible downstream.
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
        }
    }

    void buildParameters() {
        const std::vector<double>& rates = m_handle->sampleRates();
        const double defaultRate = std::clamp(20e6, rates.front(), rates.back());

        m_parameters = {
            SdrParameter{.key = "center_hz",
                         .label = "Center frequency",
                         .group = "Tuning",
                         .type = SdrParameterType::Double,
                         .unit = "Hz",
                         .min = kMinFrequencyHz,
                         .max = kMaxFrequencyHz,
                         .step = 1.0,
                         .enumValues = {},
                         .readOnly = false,
                         .gridAffecting = true,
                         .calibrationAffecting = false,
                         .requiresStop = false,
                         .description = "50 MHz to 6.9 GHz.",
                         .defaultValue = 100e6},

            SdrParameter{.key = "sample_rate",
                         .label = "Sample rate",
                         .group = "Tuning",
                         .type = SdrParameterType::Double,
                         .unit = "S/s",
                         .min = rates.front(),
                         .max = rates.back(),
                         .step = 0.0,
                         .enumValues = sampleRateChoices(rates),
                         .readOnly = false,
                         .gridAffecting = true,
                         .calibrationAffecting = false,
                         .requiresStop = false,
                         .description =
                             m_handle->arbitrarySampleRates()
                                 ? "Any rate in range; the presets are the ones the radio's "
                                   "maker recommends."
                                 : "The radio has a fixed set of rates, and anything else is "
                                   "rounded to the nearest of them.",
                         .defaultValue = defaultRate},
        };

        if (m_handle->hasBandwidth()) {
            m_parameters.push_back(SdrParameter{
                .key = "bandwidth",
                .label = "Filter bandwidth",
                .group = "Filters",
                .type = SdrParameterType::Double,
                .unit = "Hz",
                .min = 0.0,
                .max = rates.back(),
                .step = 0.0,
                .enumValues = {},
                .readOnly = false,
                .gridAffecting = false,
                .calibrationAffecting = false,
                .requiresStop = false,
                .description = "Baseband filter width. 0 keeps it at 80% of the sample rate and "
                               "following it, which is almost always what you want -- a filter "
                               "narrower than the span silently loses its edges. The radio "
                               "rounds to a width its filter has.",
                .defaultValue = 0.0});
        }

        m_parameters.push_back(SdrParameter{
            .key = "lna_gain",
            .label = "LNA gain",
            .group = "Gain",
            .type = SdrParameterType::Int,
            .unit = {},
            .min = 0.0,
            .max = 3.0,
            .step = 1.0,
            .enumValues = {},
            .readOnly = false,
            .gridAffecting = false,
            .calibrationAffecting = true,
            .requiresStop = false,
            .description = "Front-end amplifier step: 0 and 1 are 0 dB, 2 is +16 dB, 3 is "
                           "+33 dB. Sets the noise figure -- raise this before the VGA, and "
                           "back it off near a strong transmitter.",
            .defaultValue = std::int64_t{2}});

        m_parameters.push_back(SdrParameter{
            .key = "vga_gain",
            .label = "VGA gain",
            .group = "Gain",
            .type = SdrParameterType::Int,
            .unit = {},
            .min = 0.0,
            .max = 31.0,
            .step = 1.0,
            .enumValues = {},
            .readOnly = false,
            .gridAffecting = false,
            .calibrationAffecting = true,
            .requiresStop = false,
            .description = "Baseband amplifier step, 2 dB each: 31 is +62 dB. Amplifies "
                           "signal and noise equally, so it sets the display level rather than "
                           "the sensitivity.",
            .defaultValue = std::int64_t{10}});

        m_parameters.push_back(SdrParameter{
            .key = "clock_source",
            .label = "Clock source",
            .group = "Clock",
            .type = SdrParameterType::Enum,
            .unit = {},
            .min = 0.0,
            .max = 0.0,
            .step = 0.0,
            .enumValues = {{.value = std::string(kClockInternal),
                            .label = "Internal",
                            .description = "The radio's own oscillator."},
                           {.value = std::string(kClockExternal),
                            .label = "External",
                            .description = "The reference on the clock input."}},
            .readOnly = false,
            .gridAffecting = false,
            .calibrationAffecting = false,
            .requiresStop = false,
            .description = "Leave on internal unless a reference is connected to the clock "
                           "input.",
            .defaultValue = std::string(kClockInternal)});
    }

    /// Declared first so it is destroyed last, after the receive thread that
    /// reads through it has been joined.
    std::unique_ptr<FobosHandle> m_handle;
    SdrDeviceInfo m_info;
    std::vector<SdrParameter> m_parameters;

    std::atomic<double> m_centerHz{100e6};
    std::atomic<double> m_sampleRate{20e6};
    std::atomic<double> m_bandwidthHz{0.0};
    std::atomic<int> m_lnaGain{2};
    std::atomic<int> m_vgaGain{10};
    std::atomic<bool> m_externalClock{false};

    std::atomic<bool> m_streaming{false};
    std::jthread m_thread;
    plugin::Stream* m_stream = nullptr;
    std::size_t m_framesPerBlock = kReadFrames;
    std::uint64_t m_sequence = 0;

    /// One read, for blocks that are not exactly one read long. Touched only
    /// by the receive thread.
    std::vector<float> m_staging;
    std::vector<float> m_discard;
    std::size_t m_stagedFrames = 0;
    std::size_t m_stagedTaken = 0;
    std::uint64_t m_stagedStartNs = 0;
    double m_stagedCenterHz = 0.0;
    double m_stagedSampleRate = 0.0;
    double m_stagedNsPerFrame = 0.0;
    std::uint64_t m_readFailures = 0;
};

class FobosFactory final : public plugin::Driver {
public:
    /// Asks both libraries. Counting opens nothing, and listing -- which does
    /// open each radio for a moment to read its serial -- only happens when
    /// the count found one.
    [[nodiscard]] std::vector<SdrDeviceInfo> enumerate() const override {
        std::vector<SdrDeviceInfo> devices;
        for (const ListedRadio& radio : listedRadios(listStandard(), listAgile())) {
            devices.push_back(SdrDeviceInfo{.driver = "fobos",
                                            .id = radio.id,
                                            .label = labelFor(radio.firmware, {}),
                                            .serial = radio.serial,
                                            .hardwareRevision = {},
                                            .firmware = {},
                                            .fpga = {},
                                            .minFrequencyHz = kMinFrequencyHz,
                                            .maxFrequencyHz = kMaxFrequencyHz,
                                            .minSampleRate = kListedMinRate,
                                            .maxSampleRate = radio.firmware == Firmware::Agile
                                                                 ? kAgileListedMaxRate
                                                                 : kStandardListedMaxRate,
                                            .linkCapacityBytesPerSec = kLinkCapacityBytesPerSec,
                                            .linkDescription = kUsbDescription});
        }
        return devices;
    }

    [[nodiscard]] Result<std::unique_ptr<plugin::Radio>> open(std::string_view id) override {
        const std::vector<ListedRadio> radios = listedRadios(listStandard(), listAgile());
        if (radios.empty()) {
            return fail<std::unique_ptr<plugin::Radio>>(ErrorCode::NotFound,
                                                        "no Fobos SDR is attached");
        }

        const auto match = id.empty()
                               ? radios.begin()
                               : std::ranges::find_if(radios, [id](const ListedRadio& radio) {
                                     return radio.id == id;
                                 });
        if (match == radios.end()) {
            return fail<std::unique_ptr<plugin::Radio>>(
                ErrorCode::NotFound, "no Fobos SDR '{}' among the {} attached", id, radios.size());
        }

        Result<std::unique_ptr<FobosHandle>> handle = match->firmware == Firmware::Agile
                                                          ? openAgile(match->index)
                                                          : openStandard(match->index);
        if (!handle) {
            // Neither library says why an open failed -- both collapse every
            // cause into "no device" -- so the likely ones are named here.
            return fail<std::unique_ptr<plugin::Radio>>(
                ErrorCode::DeviceError,
                "could not open Fobos SDR '{}': {} (on Linux this is usually a missing udev "
                "rule; otherwise another program already has the radio open)",
                match->id, handle.error().message());
        }

        const BoardInfo& board = (*handle)->board();

        // Opened by position, so a radio plugged or unplugged since the
        // listing could put a different one at that index.
        if (!match->serial.empty() && board.serial != match->serial) {
            return fail<std::unique_ptr<plugin::Radio>>(
                ErrorCode::Unavailable,
                "the attached radios changed while opening Fobos SDR '{}'; try again", match->id);
        }

        const std::vector<double>& rates = (*handle)->sampleRates();
        SdrDeviceInfo info{.driver = "fobos",
                           .id = id.empty() ? match->id : std::string(id),
                           .label = labelFor(match->firmware, board.product),
                           .serial = board.serial.empty() ? match->serial : board.serial,
                           .hardwareRevision = board.hardwareRevision,
                           .firmware = {.version = board.firmwareVersion,
                                        .knownLatest = {},
                                        .aheadOfDriver = false},
                           .fpga = {},
                           .minFrequencyHz = kMinFrequencyHz,
                           .maxFrequencyHz = kMaxFrequencyHz,
                           .minSampleRate = rates.front(),
                           .maxSampleRate = rates.back(),
                           .linkCapacityBytesPerSec = kLinkCapacityBytesPerSec,
                           .linkDescription = kUsbDescription};

        auto device = std::make_unique<FobosDevice>(*std::move(handle), std::move(info));

        // Applied so the radio's state matches what the panel reports the
        // moment it opens, rather than what the library opened it with.
        for (const SdrParameter& parameter : device->parameters()) {
            if (!parameter.readOnly) {
                (void)device->setParameter(parameter.key, parameter.defaultValue);
            }
        }

        const SdrDeviceInfo& opened = device->info();
        log().info("opened {} (serial {}, hardware {}, firmware {})", opened.label,
                   opened.serial.empty() ? "-" : opened.serial,
                   opened.hardwareRevision.empty() ? "-" : opened.hardwareRevision,
                   opened.firmware.version.empty() ? "-" : opened.firmware.version);

        return device;
    }
};

} // namespace

plugin::Logger& log() noexcept {
    static plugin::Logger logger;
    return logger;
}

plugin::Driver& driver() noexcept {
    static FobosFactory factory;
    return factory;
}

} // namespace sweeppp::fobos
