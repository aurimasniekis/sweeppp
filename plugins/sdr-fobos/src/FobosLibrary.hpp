// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// One open Fobos SDR, through whichever library speaks its firmware.
//
// The two libraries cannot meet in one translation unit: both headers define
// every FOBOS_ERR_* macro, and both define API_EXPORT. So each is wrapped in a
// source file of its own that includes its header and nothing of the other's
// -- FobosStandard.cpp and FobosAgile.cpp -- and the driver sees only this.

#include "FobosSerials.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <sweeppp/core/Result.hpp>
#include <vector>

namespace sweeppp::fobos {

struct BoardInfo {
    std::string hardwareRevision; ///< "3.0.0"
    std::string firmwareVersion;  ///< Version and build, as the library joins them
    std::string manufacturer;
    std::string product;
    std::string serial;
};

/// Closes the radio when destroyed.
///
/// Called from the thread that owns the radio, with one exception the driver
/// relies on: `setFrequency` arrives from the sweep engine while `readSync`
/// blocks on the receive thread. Both libraries use libusb's synchronous API
/// underneath, which allows a control transfer beside a bulk one.
class FobosHandle {
public:
    virtual ~FobosHandle() = default;

    FobosHandle(const FobosHandle&) = delete;
    FobosHandle& operator=(const FobosHandle&) = delete;

    [[nodiscard]] virtual Firmware firmware() const noexcept = 0;
    [[nodiscard]] virtual const BoardInfo& board() const noexcept = 0;

    /// Ascending. Never empty.
    [[nodiscard]] virtual const std::vector<double>& sampleRates() const noexcept = 0;

    /// The listed rates are recommendations, and anything between the first
    /// and the last is accepted exactly.
    [[nodiscard]] virtual bool arbitrarySampleRates() const noexcept = 0;

    [[nodiscard]] virtual bool hasBandwidth() const noexcept = 0;

    [[nodiscard]] virtual Status setFrequency(double hz) = 0;
    /// The rate actually in force afterwards.
    [[nodiscard]] virtual Result<double> setSampleRate(double rate) = 0;
    [[nodiscard]] virtual Status setLnaGain(unsigned step) = 0;
    [[nodiscard]] virtual Status setVgaGain(unsigned step) = 0;
    [[nodiscard]] virtual Status setExternalClock(bool external) = 0;
    /// Zero lets the filter follow the sample rate.
    [[nodiscard]] virtual Status setBandwidth(double hz) = 0;

    [[nodiscard]] virtual Status startSync(std::uint32_t frames) = 0;
    /// Interleaved I/Q floats into `samples`, which has room for the frames
    /// `startSync` was given. Returns the frames written.
    [[nodiscard]] virtual Result<std::uint32_t> readSync(float* samples) = 0;
    virtual void stopSync() = 0;

protected:
    FobosHandle() = default;
};

/// Serials of the radios on this firmware, in the order its open counts them.
[[nodiscard]] std::vector<std::string> listStandard();
[[nodiscard]] std::vector<std::string> listAgile();

[[nodiscard]] Result<std::unique_ptr<FobosHandle>> openStandard(std::uint32_t index);
[[nodiscard]] Result<std::unique_ptr<FobosHandle>> openAgile(std::uint32_t index);

/// Room for a `*_list_devices()` answer. Both libraries strcat every serial
/// into the caller's buffer with no bound, each up to 255 characters and a
/// space, so the buffer is sized for far more radios than a host can carry
/// rather than for however many were counted a moment earlier.
[[nodiscard]] inline std::size_t serialListBytes(int counted) noexcept {
    constexpr std::size_t kPerRadio = 256;
    constexpr std::size_t kFloor = 128;
    return (std::max<std::size_t>(static_cast<std::size_t>(std::max(counted, 0)) + 8, kFloor) *
            kPerRadio) +
           1;
}

} // namespace sweeppp::fobos
