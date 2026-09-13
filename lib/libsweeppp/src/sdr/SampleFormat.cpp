// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/sdr/SampleFormat.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace sweeppp {

std::string_view toString(SampleFormat format) noexcept {
    switch (format) {
    case SampleFormat::Cu8:
        return "cu8";
    case SampleFormat::Cs8:
        return "cs8";
    case SampleFormat::Cu12:
        return "cu12";
    case SampleFormat::Cs12:
        return "cs12";
    case SampleFormat::Cu16:
        return "cu16";
    case SampleFormat::Cs16:
        return "cs16";
    case SampleFormat::Cu32:
        return "cu32";
    case SampleFormat::Cs32:
        return "cs32";
    case SampleFormat::Cf16:
        return "cf16";
    case SampleFormat::Cf32:
        return "cf32";
    case SampleFormat::Cf64:
        return "cf64";
    }
    return "cs8";
}

std::string_view displayName(SampleFormat format) noexcept {
    switch (format) {
    case SampleFormat::Cu8:
        return "Complex uint8";
    case SampleFormat::Cs8:
        return "Complex int8";
    case SampleFormat::Cu12:
        return "Complex uint12 (packed)";
    case SampleFormat::Cs12:
        return "Complex int12 (packed)";
    case SampleFormat::Cu16:
        return "Complex uint16";
    case SampleFormat::Cs16:
        return "Complex int16";
    case SampleFormat::Cu32:
        return "Complex uint32";
    case SampleFormat::Cs32:
        return "Complex int32";
    case SampleFormat::Cf16:
        return "Complex float16";
    case SampleFormat::Cf32:
        return "Complex float32";
    case SampleFormat::Cf64:
        return "Complex float64";
    }
    return "Complex int8";
}

std::span<const SampleFormat> allSampleFormats() noexcept {
    static constexpr std::array kFormats{
        SampleFormat::Cu8,  SampleFormat::Cs8,  SampleFormat::Cu12, SampleFormat::Cs12,
        SampleFormat::Cu16, SampleFormat::Cs16, SampleFormat::Cu32, SampleFormat::Cs32,
        SampleFormat::Cf16, SampleFormat::Cf32, SampleFormat::Cf64,
    };
    return kFormats;
}

Result<SampleFormat> sampleFormatFromString(std::string_view name) {
    std::string lowered;
    lowered.reserve(name.size());
    for (const char ch : name) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    // The sc/cs spellings both appear in the wild, IQ files on disk are
    // commonly named .cf32, and numpy names a pair of float32 "complex64". All
    // of them are accepted; only the canonical spelling above is ever written.
    struct Alias {
        std::string_view text;
        SampleFormat format;
    };
    static constexpr std::array kAliases{
        Alias{"cu8", SampleFormat::Cu8},        Alias{"u8", SampleFormat::Cu8},
        Alias{"uint8", SampleFormat::Cu8},

        Alias{"cs8", SampleFormat::Cs8},        Alias{"sc8", SampleFormat::Cs8},
        Alias{"s8", SampleFormat::Cs8},         Alias{"int8", SampleFormat::Cs8},

        Alias{"cu12", SampleFormat::Cu12},      Alias{"u12", SampleFormat::Cu12},

        Alias{"cs12", SampleFormat::Cs12},      Alias{"sc12", SampleFormat::Cs12},
        Alias{"s12", SampleFormat::Cs12},

        Alias{"cu16", SampleFormat::Cu16},      Alias{"u16", SampleFormat::Cu16},
        Alias{"uint16", SampleFormat::Cu16},

        Alias{"cs16", SampleFormat::Cs16},      Alias{"sc16", SampleFormat::Cs16},
        Alias{"s16", SampleFormat::Cs16},       Alias{"int16", SampleFormat::Cs16},

        Alias{"cu32", SampleFormat::Cu32},      Alias{"u32", SampleFormat::Cu32},
        Alias{"uint32", SampleFormat::Cu32},

        Alias{"cs32", SampleFormat::Cs32},      Alias{"sc32", SampleFormat::Cs32},
        Alias{"s32", SampleFormat::Cs32},       Alias{"int32", SampleFormat::Cs32},

        Alias{"cf16", SampleFormat::Cf16},      Alias{"fc16", SampleFormat::Cf16},
        Alias{"f16", SampleFormat::Cf16},       Alias{"float16", SampleFormat::Cf16},
        Alias{"half", SampleFormat::Cf16},

        Alias{"cf32", SampleFormat::Cf32},      Alias{"fc32", SampleFormat::Cf32},
        Alias{"f32", SampleFormat::Cf32},       Alias{"float32", SampleFormat::Cf32},
        Alias{"complex64", SampleFormat::Cf32},

        Alias{"cf64", SampleFormat::Cf64},      Alias{"fc64", SampleFormat::Cf64},
        Alias{"f64", SampleFormat::Cf64},       Alias{"float64", SampleFormat::Cf64},
        Alias{"double", SampleFormat::Cf64},    Alias{"complex128", SampleFormat::Cf64},
    };

    const auto found = std::ranges::find_if(
        kAliases, [&lowered](const Alias& alias) { return alias.text == lowered; });
    if (found != kAliases.end()) {
        return found->format;
    }

    return fail<SampleFormat>(ErrorCode::InvalidArgument, "unknown sample format '{}'", name);
}

} // namespace sweeppp
