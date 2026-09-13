// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <string_view>
#include <sweeps/Config.hpp>

TEST_CASE("the library reports a version") {
    CHECK_FALSE(std::string_view(sweeps::libraryVersion()).empty());
}
