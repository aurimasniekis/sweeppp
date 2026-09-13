// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The agile firmware's library, libfobos_sdr, behind FobosHandle.
//
// Includes fobos_sdr.h and nothing of the standard library's; see
// FobosLibrary.hpp.
//
// This firmware does its tuning, rate and filter arithmetic on the radio, and
// the library only forwards the numbers, mostly without checking that the
// control transfer carrying them arrived -- so a setter reporting success
// here says less than it does elsewhere.
#include "FobosLibrary.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fobos_sdr.h>
#include <format>
#include <libusb.h>

namespace sweeppp::fobos {
namespace {

/// Room for one board-info string. The library strcpy's into these with no
/// bound, and its longest -- version, a space and build -- is 129 bytes.
inline constexpr std::size_t kInfoBytes = 256;

/// The filter width, relative to the sample rate, that "automatic" means: the
/// same 0.8 the standard library applies to itself on every rate change.
inline constexpr double kAutoBandwidthRatio = 0.8;

/// The first hardware generation on which the rates listed are
/// recommendations rather than the whole set.
inline constexpr int kArbitraryRateRevision = 4;

[[nodiscard]] std::string describe(int code) {
    return std::format("{} ({})", fobos_sdr_error_name(code), code);
}

/// "4.0.1" -> 4, and 0 for anything that does not start with a number.
[[nodiscard]] int leadingNumber(std::string_view text) noexcept {
    int value = 0;
    (void)std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

class AgileHandle final : public FobosHandle {
public:
    explicit AgileHandle(fobos_sdr_dev_t* device) : m_device(device) {
        readBoard();
        readSampleRates();
    }

    ~AgileHandle() override { (void)fobos_sdr_close(m_device); }

    [[nodiscard]] Firmware firmware() const noexcept override { return Firmware::Agile; }
    [[nodiscard]] const BoardInfo& board() const noexcept override { return m_board; }

    [[nodiscard]] const std::vector<double>& sampleRates() const noexcept override {
        return m_rates;
    }

    [[nodiscard]] bool arbitrarySampleRates() const noexcept override { return m_arbitraryRates; }

    [[nodiscard]] bool hasBandwidth() const noexcept override { return true; }

    [[nodiscard]] Status setFrequency(double hz) override {
        if (const int result = fobos_sdr_set_frequency(m_device, hz); result != FOBOS_ERR_OK) {
            return fail(ErrorCode::DeviceError, "set frequency {:g} MHz: {}", hz / 1e6,
                        describe(result));
        }
        return ok();
    }

    [[nodiscard]] Result<double> setSampleRate(double rate) override {
        // The library has no way to say what the radio settled on. Where the
        // listed rates are the whole set the firmware takes the nearest, so
        // that is snapped here too; anything else would label every sweep
        // with a rate the radio is not running at.
        const double wanted = m_arbitraryRates ? rate : nearestListed(rate);
        if (const int result = fobos_sdr_set_samplerate(m_device, wanted); result != FOBOS_ERR_OK) {
            return fail<double>(ErrorCode::DeviceError, "set sample rate: {}", describe(result));
        }
        return wanted;
    }

    [[nodiscard]] Status setLnaGain(unsigned step) override {
        if (const int result = fobos_sdr_set_lna_gain(m_device, step); result != FOBOS_ERR_OK) {
            return fail(ErrorCode::DeviceError, "set LNA gain: {}", describe(result));
        }
        return ok();
    }

    [[nodiscard]] Status setVgaGain(unsigned step) override {
        if (const int result = fobos_sdr_set_vga_gain(m_device, step); result != FOBOS_ERR_OK) {
            return fail(ErrorCode::DeviceError, "set VGA gain: {}", describe(result));
        }
        return ok();
    }

    [[nodiscard]] Status setExternalClock(bool external) override {
        if (const int result = fobos_sdr_set_clk_source(m_device, external ? 1 : 0);
            result != FOBOS_ERR_OK) {
            return fail(ErrorCode::DeviceError, "set clock source: {}", describe(result));
        }
        return ok();
    }

    [[nodiscard]] Status setBandwidth(double hz) override {
        // Two calls rather than one with a sentinel: each turns the other's
        // mode off, and the relative one keeps following later rate changes.
        const int result = hz > 0.0 ? fobos_sdr_set_bandwidth(m_device, hz)
                                    : fobos_sdr_set_auto_bandwidth(m_device, kAutoBandwidthRatio);
        if (result != FOBOS_ERR_OK) {
            return fail(ErrorCode::DeviceError, "set bandwidth: {}", describe(result));
        }
        return ok();
    }

    [[nodiscard]] Status startSync(std::uint32_t frames) override {
        if (const int result = fobos_sdr_start_sync(m_device, frames); result != FOBOS_ERR_OK) {
            return fail(ErrorCode::DeviceError, "fobos_sdr_start_sync: {}", describe(result));
        }
        return ok();
    }

    [[nodiscard]] Result<std::uint32_t> readSync(float* samples) override {
        std::uint32_t frames = 0;
        // A libusb code on failure rather than one of the library's own: the
        // bulk transfer's result is returned unchanged.
        if (const int result = fobos_sdr_read_sync(m_device, samples, &frames); result != 0) {
            return fail<std::uint32_t>(ErrorCode::DeviceError, "{}", libusb_error_name(result));
        }
        // Converted in runs of 8192 frames when the firmware interleaves its
        // two halves, and a shorter tail is left untouched -- so a short
        // read's last partial run is not samples.
        return frames - (frames % 8192U);
    }

    void stopSync() override { (void)fobos_sdr_stop_sync(m_device); }

private:
    void readBoard() {
        std::array<char, kInfoBytes> hardware{};
        std::array<char, kInfoBytes> version{};
        std::array<char, kInfoBytes> manufacturer{};
        std::array<char, kInfoBytes> product{};
        std::array<char, kInfoBytes> serial{};
        if (fobos_sdr_get_board_info(m_device, hardware.data(), version.data(), manufacturer.data(),
                                     product.data(), serial.data()) == FOBOS_ERR_OK) {
            m_board = {.hardwareRevision = hardware.data(),
                       .firmwareVersion = version.data(),
                       .manufacturer = manufacturer.data(),
                       .product = product.data(),
                       .serial = serial.data()};
        }
    }

    void readSampleRates() {
        unsigned int count = 0;
        if (fobos_sdr_get_samplerates(m_device, nullptr, &count) == FOBOS_ERR_OK && count > 0) {
            m_rates.resize(count);
            (void)fobos_sdr_get_samplerates(m_device, m_rates.data(), &count);
            m_rates.resize(count);
        }
        if (m_rates.empty()) {
            // A firmware generation the library does not recognise lists
            // nothing. The rate it opens the radio at is the one certainly in
            // force, so that is what the panel offers.
            m_rates = {25e6};
        }
        std::ranges::sort(m_rates);

        // The header documents the change by hardware revision, and the
        // source tests the firmware's major version. Either one reaching 4 is
        // taken as the answer.
        m_arbitraryRates =
            std::max(leadingNumber(m_board.hardwareRevision),
                     leadingNumber(m_board.firmwareVersion)) >= kArbitraryRateRevision;
    }

    [[nodiscard]] double nearestListed(double rate) const {
        return *std::ranges::min_element(m_rates, [rate](double a, double b) {
            return std::abs(a - rate) < std::abs(b - rate);
        });
    }

    fobos_sdr_dev_t* m_device = nullptr;
    BoardInfo m_board;
    std::vector<double> m_rates;
    bool m_arbitraryRates = false;
};

} // namespace

std::vector<std::string> listAgile() {
    // Counted first because counting opens nothing, whereas listing opens every
    // radio it finds to read its serial -- and no radio is the common case.
    const int counted = fobos_sdr_get_device_count();
    if (counted <= 0) {
        return {};
    }
    std::string buffer(serialListBytes(counted), '\0');
    const int listed = fobos_sdr_list_devices(buffer.data());
    // Up to the terminator the library wrote, not the whole zeroed buffer.
    return parseSerials(std::string_view(buffer.data()),
                        static_cast<std::size_t>(std::max(listed, 0)));
}

Result<std::unique_ptr<FobosHandle>> openAgile(std::uint32_t index) {
    fobos_sdr_dev_t* device = nullptr;
    const int result = fobos_sdr_open(&device, index);
    if (result != FOBOS_ERR_OK || device == nullptr) {
        return fail<std::unique_ptr<FobosHandle>>(ErrorCode::DeviceError, "{}", describe(result));
    }
    return std::make_unique<AgileHandle>(device);
}

} // namespace sweeppp::fobos
