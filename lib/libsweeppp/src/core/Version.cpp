// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/core/Version.hpp"

#include <sweeppp/core/BuildInfo.hpp>

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
