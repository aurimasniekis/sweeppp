// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <sweeppp/core/Version.hpp>

TEST_CASE("version string is populated") {
    CHECK_FALSE(sweeppp::versionString().empty());
    CHECK(sweeppp::versionString() != "0.0.0-unknown");
}
