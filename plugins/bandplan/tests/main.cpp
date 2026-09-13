// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "BandPlan.hpp"

#include <doctest/doctest.h>

TEST_CASE("the loader is reachable") {
    // A plugin that cannot be tested without the host is a plugin nobody
    // tests. Its loader is a value type over toml_util with no ABI and no
    // singleton in it, which is exactly what makes this suite possible -- and
    // is the same property the host relies on when it says a plugin may link
    // libsweeppp for values.
    CHECK_FALSE(bandplan::BandPlan::load("/nonexistent/plan.toml").has_value());
}
