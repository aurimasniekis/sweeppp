// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The standard firmware's library, libfobos, behind FobosHandle.
//
// Includes fobos.h and nothing of the agile library's; see FobosLibrary.hpp.
#include "FobosLibrary.hpp"

#include <array>
#include <fobos.h>
#include <format>
#include <libusb.h>

namespace sweeppp::fobos {
namespace {

/// Room for one board-info string. The library strcpy's into these with no
/// bound, and its longest -- version, a space and build -- is 129 bytes.
inline constexpr std::size_t kInfoBytes = 256;

[[nodiscard]] std::string describe(int code) {
    return std::format("{} ({})", fobos_rx_error_name(code), code);
}

class StandardHandle final : public FobosHandle {
public:
    explicit StandardHandle(fobos_dev_t* device) : m_device(device) {
        readBoard();
        readSampleRates();
    }

    ~StandardHandle() override { (void)fobos_rx_close(m_device); }

    [[nodiscard]] Firmware firmware() const noexcept override { return Firmware::Standard; }
    [[nodiscard]] const BoardInfo& board() const noexcept override { return m_board; }

    [[nodiscard]] const std::vector<double>& sampleRates() const noexcept override {
        return m_rates;
    }

    // The library snaps every request to its fixed table.
    [[nodiscard]] bool arbitrarySampleRates() const noexcept override { return false; }

    // It does set a filter, but only as a side effect of the sample rate: the
    // call that would set one directly is not in its header.
    [[nodiscard]] bool hasBandwidth() const noexcept override { return false; }

    [[nodiscard]] Status setFrequency(double hz) override {
        double actual = 0.0;
        const int result = fobos_rx_set_frequency(m_device, hz, &actual);
        if (result == FOBOS_ERR_UNSUPPORTED) {
            // Its band table has no row for this frequency.
            return fail(ErrorCode::InvalidArgument, "{:g} MHz is outside every band the radio has",
                        hz / 1e6);
        }
        if (result != FOBOS_ERR_OK) {
            return fail(ErrorCode::DeviceError, "set frequency {:g} MHz: {}", hz / 1e6,
                        describe(result));
        }
        return ok();
    }

    [[nodiscard]] Result<double> setSampleRate(double rate) override {
        double actual = 0.0;
        if (const int result = fobos_rx_set_samplerate(m_device, rate, &actual);
            result != FOBOS_ERR_OK) {
            return fail<double>(ErrorCode::DeviceError, "set sample rate: {}", describe(result));
        }
        return actual > 0.0 ? actual : rate;
    }

    [[nodiscard]] Status setLnaGain(unsigned step) override {
        if (const int result = fobos_rx_set_lna_gain(m_device, step); result != FOBOS_ERR_OK) {
            return fail(ErrorCode::DeviceError, "set LNA gain: {}", describe(result));
        }
        return ok();
    }

    [[nodiscard]] Status setVgaGain(unsigned step) override {
        if (const int result = fobos_rx_set_vga_gain(m_device, step); result != FOBOS_ERR_OK) {
            return fail(ErrorCode::DeviceError, "set VGA gain: {}", describe(result));
        }
        return ok();
    }

    [[nodiscard]] Status setExternalClock(bool external) override {
        // The result of a control transfer, passed through: a byte count on
        // success, and a libusb error on failure.
        if (const int result = fobos_rx_set_clk_source(m_device, external ? 1 : 0); result < 0) {
            return fail(ErrorCode::DeviceError, "set clock source: {}", libusb_error_name(result));
        }
        return ok();
    }

    [[nodiscard]] Status setBandwidth(double /*hz*/) override {
        return fail(ErrorCode::Unsupported,
                    "the standard firmware sets its filter from the sample rate");
    }

    [[nodiscard]] Status startSync(std::uint32_t frames) override {
        if (const int result = fobos_rx_start_sync(m_device, frames); result != FOBOS_ERR_OK) {
            return fail(ErrorCode::DeviceError, "fobos_rx_start_sync: {}", describe(result));
        }
        return ok();
    }

    [[nodiscard]] Result<std::uint32_t> readSync(float* samples) override {
        std::uint32_t frames = 0;
        // A libusb code on failure rather than one of the library's own: the
        // bulk transfer's result is returned unchanged.
        if (const int result = fobos_rx_read_sync(m_device, samples, &frames); result != 0) {
            return fail<std::uint32_t>(ErrorCode::DeviceError, "{}", libusb_error_name(result));
        }
        // The library converts in runs of eight frames and leaves a shorter
        // tail untouched, so a short read's last few frames are not samples.
        return frames - (frames % 8U);
    }

    void stopSync() override { (void)fobos_rx_stop_sync(m_device); }

private:
    void readBoard() {
        std::array<char, kInfoBytes> hardware{};
        std::array<char, kInfoBytes> version{};
        std::array<char, kInfoBytes> manufacturer{};
        std::array<char, kInfoBytes> product{};
        std::array<char, kInfoBytes> serial{};
        if (fobos_rx_get_board_info(m_device, hardware.data(), version.data(), manufacturer.data(),
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
        if (fobos_rx_get_samplerates(m_device, nullptr, &count) == FOBOS_ERR_OK && count > 0) {
            m_rates.resize(count);
            (void)fobos_rx_get_samplerates(m_device, m_rates.data(), &count);
            m_rates.resize(count);
        }
        if (m_rates.empty()) {
            // The rate the library itself opens the radio at, so the panel
            // still offers the one that is certainly in force.
            m_rates = {25e6};
        }
        std::ranges::sort(m_rates);
    }

    fobos_dev_t* m_device = nullptr;
    BoardInfo m_board;
    std::vector<double> m_rates;
};

} // namespace

std::vector<std::string> listStandard() {
    // Counted first because counting opens nothing, whereas listing opens every
    // radio it finds to read its serial -- and no radio is the common case.
    const int counted = fobos_rx_get_device_count();
    if (counted <= 0) {
        return {};
    }
    std::string buffer(serialListBytes(counted), '\0');
    const int listed = fobos_rx_list_devices(buffer.data());
    // Up to the terminator the library wrote, not the whole zeroed buffer.
    return parseSerials(std::string_view(buffer.data()),
                        static_cast<std::size_t>(std::max(listed, 0)));
}

Result<std::unique_ptr<FobosHandle>> openStandard(std::uint32_t index) {
    fobos_dev_t* device = nullptr;
    const int result = fobos_rx_open(&device, index);
    if (result != FOBOS_ERR_OK || device == nullptr) {
        return fail<std::unique_ptr<FobosHandle>>(ErrorCode::DeviceError, "{}", describe(result));
    }
    return std::make_unique<StandardHandle>(device);
}

} // namespace sweeppp::fobos
