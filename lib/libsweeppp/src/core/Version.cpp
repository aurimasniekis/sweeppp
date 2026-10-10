// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/core/Version.hpp"

#include <charconv>
#include <sweeppp/core/BuildInfo.hpp>
#include <system_error>

#ifndef SWEEPPP_VERSION_STRING
#define SWEEPPP_VERSION_STRING "0.0.0-unknown"
#endif

#ifndef SWEEPPP_CHANNEL
#define SWEEPPP_CHANNEL "release"
#endif

#ifndef SWEEPPP_APP_ID
#define SWEEPPP_APP_ID "sweeppp"
#endif

#ifndef SWEEPPP_PRODUCT_NAME
#define SWEEPPP_PRODUCT_NAME "Sweep++"
#endif

#ifndef SWEEPPP_BUILD_COMPILER
#define SWEEPPP_BUILD_COMPILER "unknown compiler"
#endif

#ifndef SWEEPPP_BUILD_PLATFORM
#define SWEEPPP_BUILD_PLATFORM "unknown platform"
#endif

namespace sweeppp {
namespace {

struct VersionComponent {
    long value = 0;
    bool numeric = false;
};

VersionComponent nextComponent(std::string_view& text) {
    if (text.empty()) {
        return {.value = 0, .numeric = true};
    }

    const std::size_t dot = text.find('.');
    const std::string_view piece = text.substr(0, dot);
    text = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);

    VersionComponent component;
    const auto* end = piece.data() + piece.size();
    const auto result = std::from_chars(piece.data(), end, component.value);
    component.numeric = result.ec == std::errc{} && result.ptr == end;
    return component;
}

} // namespace

int compareVersions(std::string_view left, std::string_view right) {
    while (!left.empty() || !right.empty()) {
        const VersionComponent a = nextComponent(left);
        const VersionComponent b = nextComponent(right);

        // A component that is not a number ends the comparison: "1.2.3-rc1"
        // and "1.2.3" differ in a way this cannot rank, and guessing would put
        // a pre-release either side of its own release depending on the
        // spelling. Everything compared so far decides it.
        if (!a.numeric || !b.numeric) {
            return 0;
        }
        if (a.value != b.value) {
            return a.value < b.value ? -1 : 1;
        }
    }
    return 0;
}

std::string_view versionString() noexcept {
    return SWEEPPP_VERSION_STRING;
}

std::string_view buildString() noexcept {
    // Concatenated by the preprocessor rather than assembled at runtime, so
    // this stays a view of a string literal and an empty suffix leaves no
    // trailing "+" to trim.
    return SWEEPPP_VERSION_STRING SWEEPPP_BUILD_SUFFIX;
}

std::string_view channel() noexcept {
    return SWEEPPP_CHANNEL;
}

std::string_view productName() noexcept {
    return SWEEPPP_PRODUCT_NAME;
}

std::string_view appId() noexcept {
    return SWEEPPP_APP_ID;
}

std::string_view buildDate() noexcept {
    // __DATE__ rather than a CMake-substituted timestamp: a generated one
    // changes on every build, which would rewrite the header and recompile
    // this file every time. This one is fixed when the file is compiled, which
    // is exactly the moment it describes.
    return __DATE__;
}

std::string_view buildCompiler() noexcept {
    return SWEEPPP_BUILD_COMPILER;
}

std::string_view buildPlatform() noexcept {
    return SWEEPPP_BUILD_PLATFORM;
}

} // namespace sweeppp
