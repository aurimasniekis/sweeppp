// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// libbladeRF's compatibility tables, and what can be said about a version.
//
// Split out of the driver so it can be tested. None of it needs a radio -- it
// is string formatting, an ordering and a sanity check over a table -- and
// `plausible()` in particular should not first be exercised on hardware: it
// exists to decide whether a table read out of another library's private data
// looks like a table at all, and the case it guards against is a layout change
// upstream that would otherwise produce a confident wrong answer.

#include <cstdint>
#include <format>
#include <libbladeRF.h>
#include <optional>
#include <string>
#include <string_view>
#include <sweeppp/sdr/SdrDeviceInfo.hpp>
#include <tuple>

// What this copy of libbladeRF is old enough to be missing. The enumerators do
// not exist at all where they are absent, so these are compile-time questions
// rather than runtime ones, and the version test is the one libbladeRF.h
// documents for exactly this purpose.
//
//   2.5.0  bladerf_enable_feature(), BLADERF_FEATURE_OVERSAMPLE and
//          BLADERF_FORMAT_SC8_Q7_META -- the 8-bit link above 61.44 MS/s --
//          and BLADERF_FPGA_A5.
//   2.6.0  BLADERF_FORMAT_SC16_Q11_PACKED -- the packed 12-bit link.
//
// Every distribution is past both. PothosSDR, the only prebuilt libbladeRF for
// Windows, is still on 2021.07.25 and has neither. Here rather than in
// BladeRfDevice.cpp because the FPGA-size table below needs them too.
//
// Overridable so the reduced builds can be compiled on a machine whose
// libbladeRF is current.
#if !defined(SWEEPPP_BLADERF_HAS_OVERSAMPLE)
#if defined(LIBBLADERF_API_VERSION) && LIBBLADERF_API_VERSION >= 0x02050000
#define SWEEPPP_BLADERF_HAS_OVERSAMPLE 1
#else
#define SWEEPPP_BLADERF_HAS_OVERSAMPLE 0
#endif
#endif

#if !defined(SWEEPPP_BLADERF_HAS_FPGA_A5)
#define SWEEPPP_BLADERF_HAS_FPGA_A5 SWEEPPP_BLADERF_HAS_OVERSAMPLE
#endif

#if !defined(SWEEPPP_BLADERF_HAS_PACKED)
#if defined(LIBBLADERF_API_VERSION) && LIBBLADERF_API_VERSION >= 0x02060000
#define SWEEPPP_BLADERF_HAS_PACKED 1
#else
#define SWEEPPP_BLADERF_HAS_PACKED 0
#endif
#endif

// The tables libbladeRF's own version.c walks to decide whether a device is too
// old to talk to. The newest entry in one is the newest firmware or FPGA that
// build of libbladeRF has heard of, and it is the only thing that can answer
// "is there an update": the public API offers only BLADERF_ERR_UPDATE_FPGA,
// which fires when an image is already too old to use rather than when it is
// merely behind.
//
// These are not in libbladeRF.h. The structs mirror helpers/version.h, and the
// tables are weak, so a libbladeRF that does not export them leaves the
// pointers null and the panel says nothing about updates rather than failing to
// link. A layout change upstream would be silent, which is why plausible()
// declines to believe a table that reads like uninitialised memory.
#if defined(__GNUC__) || defined(__clang__)
#define SWEEPPP_BLADERF_COMPAT_TABLES 1

namespace sweeppp::blade {

struct BladeCompatEntry {
    struct bladerf_version version;
    struct bladerf_version requiredCounterpart;
};

struct BladeCompatTable {
    const BladeCompatEntry* entries;
    unsigned int length;
};

} // namespace sweeppp::blade

// `visibility("default")` as well as `weak`, and it is load-bearing rather than
// decoration: the plugin is built with CXX_VISIBILITY_PRESET hidden, which
// applies to DECLARATIONS too, and a hidden undefined reference must be
// satisfied at static link time rather than by the loader. Without it these
// stop resolving and the failure is silent -- the "libbladeRF supports firmware
// up to vX" line simply stops appearing.
extern "C" {
extern const sweeppp::blade::BladeCompatTable bladerf1_fw_compat_table
    __attribute__((weak, visibility("default")));
extern const sweeppp::blade::BladeCompatTable bladerf1_fpga_compat_table
    __attribute__((weak, visibility("default")));
extern const sweeppp::blade::BladeCompatTable bladerf2_fw_compat_table
    __attribute__((weak, visibility("default")));
extern const sweeppp::blade::BladeCompatTable bladerf2_fpga_compat_table
    __attribute__((weak, visibility("default")));
}
#endif

namespace sweeppp::blade {

/// The variant suffix an owner would use for a board: x40, x115, xA4, xA5, xA9.
///
/// The FPGA size is the only thing libbladeRF exposes that separates them, and
/// it is enough because the sizes do not repeat across the two generations. It
/// lives in flash, so it reads back whether or not an FPGA image is loaded.
[[nodiscard]] inline const char* fpgaVariant(bladerf_fpga_size size) {
    switch (size) {
    case BLADERF_FPGA_40KLE:
        return "x40";
    case BLADERF_FPGA_115KLE:
        return "x115";
    case BLADERF_FPGA_A4:
        return "xA4";
#if SWEEPPP_BLADERF_HAS_FPGA_A5
    case BLADERF_FPGA_A5:
        return "xA5";
#endif
    case BLADERF_FPGA_A9:
        return "xA9";
    case BLADERF_FPGA_UNKNOWN:
    default:
        return nullptr;
    }
}

/// "bladeRF 2.0 micro xA9", from the board name and the FPGA size.
[[nodiscard]] inline std::string modelName(const char* boardName, bladerf_fpga_size fpgaSize) {
    const std::string_view board = boardName != nullptr ? boardName : "";
    std::string name = board == "bladerf2" ? "bladeRF 2.0 micro" : "bladeRF";

    if (const char* variant = fpgaVariant(fpgaSize); variant != nullptr) {
        name += ' ';
        name += variant;
    }
    return name;
}

[[nodiscard]] inline std::string versionText(const struct bladerf_version& version) {
    return std::format("{}.{}.{}", version.major, version.minor, version.patch);
}

[[nodiscard]] inline bool olderThan(const struct bladerf_version& a,
                                    const struct bladerf_version& b) {
    return std::tie(a.major, a.minor, a.patch) < std::tie(b.major, b.minor, b.patch);
}

/// What the driver knows about how current a version the radio reports is.
///
/// `newest` is the top of libbladeRF's table; a radio above it is not a problem
/// in itself, but libbladeRF said so in its own log and the reason is worth
/// repeating: from there on it is guessing about a device newer than itself.
[[nodiscard]] inline VersionReport
describeVersion(const struct bladerf_version& reported,
                const std::optional<struct bladerf_version>& newest) {
    VersionReport report{.version = versionText(reported)};
    if (!newest) {
        return report;
    }

    report.knownLatest = versionText(*newest);
    report.aheadOfDriver = olderThan(*newest, reported);
    return report;
}

#ifdef SWEEPPP_BLADERF_COMPAT_TABLES

/// Rejects a table that does not read like a table, so a layout change upstream
/// costs the update hint rather than producing a confident wrong one.
///
/// A pointer, and null is one of the answers: an unresolved weak symbol has no
/// address, so there is nothing to bind a reference to.
[[nodiscard]] inline bool plausible(const BladeCompatTable* table) {
    constexpr unsigned int kSaneLength = 256;
    return table != nullptr && table->entries != nullptr && table->length > 0 &&
           table->length < kSaneLength && table->entries[0].version.major < 100;
}

/// The newest entry in one of libbladeRF's compatibility tables.
///
/// Scanned rather than read from the front: they happen to be sorted
/// newest-first, and nothing in the library promises they will stay that way.
[[nodiscard]] inline std::optional<struct bladerf_version>
newestKnown(const BladeCompatTable* table) {
    if (!plausible(table)) {
        return std::nullopt;
    }

    const struct bladerf_version* newest = &table->entries[0].version;
    for (unsigned int i = 1; i < table->length; ++i) {
        if (olderThan(*newest, table->entries[i].version)) {
            newest = &table->entries[i].version;
        }
    }
    return *newest;
}

#endif

/// The newest firmware and FPGA this libbladeRF has heard of, for one board.
struct KnownLatest {
    std::optional<struct bladerf_version> firmware;
    std::optional<struct bladerf_version> fpga;
};

[[nodiscard]] inline KnownLatest knownLatest([[maybe_unused]] bool micro) {
#ifdef SWEEPPP_BLADERF_COMPAT_TABLES
    return {.firmware = newestKnown(micro ? &bladerf2_fw_compat_table : &bladerf1_fw_compat_table),
            .fpga = newestKnown(micro ? &bladerf2_fpga_compat_table : &bladerf1_fpga_compat_table)};
#else
    return {};
#endif
}

} // namespace sweeppp::blade
