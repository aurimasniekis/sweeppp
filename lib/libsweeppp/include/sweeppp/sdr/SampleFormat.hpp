// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/BlockPool.hpp"
#include "sweeppp/core/Result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sweeppp {

/// Native sample formats, carried end to end.
///
/// The whole point of naming these is to avoid converting at acquisition. At
/// 100 MS/s, complex<float> is 800 MB/s -- more than USB 3.0 delivers, so a
/// float conversion on the transfer thread is not merely wasteful, it makes
/// the rate unreachable. HackRF delivers CS8 (200 MB/s at 100 MS/s) and
/// BladeRF CS16; both stay in that form until the FFT worker, where the
/// conversion is fused with windowing in a single pass.
///
/// The list is wider than the two formats the radios here deliver, because the
/// moment a driver comes from outside this tree it brings its own: a bare
/// unsigned byte stream is the commonest thing a cheap receiver produces, and a
/// format that cannot be named has to be converted at the source, which is the
/// one thing this type exists to prevent.
///
/// Ordinals are not persisted anywhere -- a `.sweeps` container stores computed
/// spectra, never raw IQ -- so this list may be reordered. The *names* are
/// persisted: `toString` feeds the synthetic device's `format` parameter into
/// profiles and `sampleFormatFromString` reads an IQ file's extension, so a
/// spelling that changes is a profile that stops loading.
enum class SampleFormat : std::uint8_t {
    Cu8,  ///< 2 bytes/frame. RTL-SDR, Airspy Mini.
    Cs8,  ///< 2 bytes/frame. HackRF.
    Cu12, ///< 3 bytes/frame, packed.
    Cs12, ///< 3 bytes/frame, packed. LimeSDR, some UHD.
    Cu16, ///< 4 bytes/frame.
    Cs16, ///< 4 bytes/frame. BladeRF, most 12/16-bit radios.
    Cu32, ///< 8 bytes/frame.
    Cs32, ///< 8 bytes/frame. UHD sc32.
    Cf16, ///< 4 bytes/frame. IEEE binary16.
    Cf32, ///< 8 bytes/frame. Synthetic device, IQ files.
    Cf64, ///< 16 bytes/frame. IEEE binary64.
};

[[nodiscard]] std::string_view toString(SampleFormat format) noexcept;
[[nodiscard]] std::string_view displayName(SampleFormat format) noexcept;
[[nodiscard]] Result<SampleFormat> sampleFormatFromString(std::string_view name);

/// Every enumerator, in declaration order. Lets the panel, the tests and the
/// synthetic generator iterate without restating the list and drifting from it.
[[nodiscard]] std::span<const SampleFormat> allSampleFormats() noexcept;

/// Bytes for one complex sample (I and Q together).
///
/// The 12-bit formats are the reason this is bytes-per-*frame* rather than
/// bytes-per-component: they pack two components into three bytes, so a
/// component has no whole-byte size of its own.
[[nodiscard]] constexpr std::size_t bytesPerFrame(SampleFormat format) noexcept {
    switch (format) {
    case SampleFormat::Cu8:
    case SampleFormat::Cs8:
        return 2;
    case SampleFormat::Cu12:
    case SampleFormat::Cs12:
        return 3;
    case SampleFormat::Cu16:
    case SampleFormat::Cs16:
    case SampleFormat::Cf16:
        return 4;
    case SampleFormat::Cu32:
    case SampleFormat::Cs32:
    case SampleFormat::Cf32:
        return 8;
    case SampleFormat::Cf64:
        return 16;
    }
    return 2;
}

/// Full-scale magnitude of one component, for normalising to +/-1.0.
[[nodiscard]] constexpr float fullScale(SampleFormat format) noexcept {
    switch (format) {
    case SampleFormat::Cu8:
    case SampleFormat::Cs8:
        return 128.0F;
    case SampleFormat::Cu12:
    case SampleFormat::Cs12:
        return 2048.0F;
    case SampleFormat::Cu16:
    case SampleFormat::Cs16:
        return 32768.0F;
    case SampleFormat::Cu32:
    case SampleFormat::Cs32:
        return 2147483648.0F;
    case SampleFormat::Cf16:
    case SampleFormat::Cf32:
    case SampleFormat::Cf64:
        return 1.0F;
    }
    return 1.0F;
}

/// What an unsigned format calls zero, to be subtracted before scaling.
///
/// Conversion is `(raw - zeroOffset) * (1 / fullScale)` for every format, so no
/// call site has to special-case sign. Getting this wrong on an unsigned format
/// does not crash: it puts the whole stream's DC offset at full scale, which
/// shows up as a spike at bin 0 and nothing else.
[[nodiscard]] constexpr float zeroOffset(SampleFormat format) noexcept {
    switch (format) {
    case SampleFormat::Cu8:
        return 128.0F;
    case SampleFormat::Cu12:
        return 2048.0F;
    case SampleFormat::Cu16:
        return 32768.0F;
    case SampleFormat::Cu32:
        return 2147483648.0F;
    case SampleFormat::Cs8:
    case SampleFormat::Cs12:
    case SampleFormat::Cs16:
    case SampleFormat::Cs32:
    case SampleFormat::Cf16:
    case SampleFormat::Cf32:
    case SampleFormat::Cf64:
        return 0.0F;
    }
    return 0.0F;
}

/// A batch of samples in the device's native format, borrowed from the pool.
///
/// `block` keeps the underlying storage alive: the same block may be handed to
/// several frame consumers, and reference counting is what lets a slow
/// consumer hold on to one without stalling acquisition.
struct IqBlock {
    BlockRef block;
    std::size_t frames = 0; ///< Complex samples, NOT bytes.
    SampleFormat format = SampleFormat::Cs8;

    /// Monotonically increasing per stream. A gap means the acquisition side
    /// dropped a block, which is how overruns are detected on devices (like
    /// HackRF) that report no overrun status of their own.
    std::uint64_t sequence = 0;

    std::uint64_t hostTimeNs = 0;

    /// Device timestamp where available (BladeRF metadata), 0 otherwise.
    std::uint64_t deviceTimeNs = 0;

    /// Centre frequency in effect when these samples were captured. Carried
    /// per block because during a sweep it changes every few milliseconds, and
    /// the worker that eventually processes this block must not have to guess
    /// which step it belonged to.
    double centerHz = 0.0;
    double sampleRate = 0.0;

    /// Sweep step this block belongs to, and the pass containing it.
    std::uint32_t sweepStep = 0;
    std::uint64_t sweepPass = 0;

    /// True for blocks captured during the post-retune settle window. They are
    /// carried rather than discarded at the source so the discard decision
    /// stays in one place (the worker) and shows up in the telemetry.
    bool settling = false;

    [[nodiscard]] const std::byte* data() const noexcept { return block.data(); }
    [[nodiscard]] std::size_t bytes() const noexcept { return frames * bytesPerFrame(format); }
    [[nodiscard]] bool valid() const noexcept { return block.valid() && frames > 0; }
};

} // namespace sweeppp
