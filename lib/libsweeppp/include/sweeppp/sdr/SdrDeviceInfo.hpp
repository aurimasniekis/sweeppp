// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/sdr/SampleFormat.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace sweeppp {

/// The value types a radio is described by, with no interface attached.
///
/// Split out of ISdrDevice.hpp so a plugin can name them without dragging in
/// `ISdrDevice` itself: a driver living in a shared object implements a
/// different base class, and the host converts. These are pure data either
/// way, which is what the plugin ABI's one rule permits crossing an image
/// boundary.

/// A version a device reports, and how current the driver believes it is.
struct VersionReport {
    std::string version;
    std::string knownLatest;
    bool aheadOfDriver = false;
};

/// Identity of a discovered radio, enough to reopen it later.
struct SdrDeviceInfo {
    std::string driver; ///< "hackrf", "bladerf", "synthetic", "iqfile"
    std::string id;     ///< Unique within the driver; usually the serial.

    std::string label; ///< Exact model: "HackRF One r9", "bladeRF 2.0 micro xA9".
    std::string serial;
    std::string hardwareRevision; ///< The variant alone: "r9", "xA9".
    VersionReport firmware;
    VersionReport fpga;

    double minFrequencyHz = 0.0;
    double maxFrequencyHz = 0.0;
    double minSampleRate = 0.0;
    double maxSampleRate = 0.0;

    /// USB (or link) capacity in bytes/s, from libusb_get_device_speed where
    /// available. Zero when unknown. Feeds the Performance panel's link
    /// utilisation bar -- an operator seeing 95% of a USB 2.0 link knows
    /// immediately that the cable or port is the problem, not the settings.
    std::uint64_t linkCapacityBytesPerSec = 0;
    std::string linkDescription; ///< "USB 3.0 SuperSpeed"
};

struct StreamConfig {
    /// Complex samples per delivered block. Larger blocks mean fewer wakeups
    /// and less per-block overhead, at the cost of latency.
    std::size_t framesPerBlock = 262'144;
    std::size_t blockCount = 64;
    /// Preferred format; devices report what they actually produce through
    /// nativeFormat(). Requesting a format the device cannot produce is an
    /// error rather than a silent conversion.
    SampleFormat format = SampleFormat::Cs8;
};

/// A named reading a device can report about itself -- temperature, supply,
/// signal strength, oscillator trim.
///
/// Formatted by the driver rather than returned as a number, because the unit
/// and sensible precision are things only the driver knows, and the panel that
/// shows them must not need per-device knowledge to do it.
struct SdrHealthReading {
    std::string label;
    std::string value;

    /// The same reading as a number, and the range it is expected to move
    /// within, so it can be plotted alongside every other rate in the
    /// Performance panel. A maximum at or below the minimum means the reading
    /// has no meaningful scale -- a power source, a serial -- and is shown as
    /// text only.
    float numeric = 0.0F;
    float minimum = 0.0F;
    float maximum = 0.0F;

    /// Set when the reading is outside what the hardware is happy with, so the
    /// display can say so without knowing what any of them mean.
    bool alarm = false;
};

} // namespace sweeppp
