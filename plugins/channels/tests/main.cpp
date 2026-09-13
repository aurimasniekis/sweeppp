// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "ChannelSet.hpp"

#include <doctest/doctest.h>

TEST_CASE("the model is reachable") {
    // A plugin that cannot be tested without the host is a plugin nobody
    // tests. `ChannelSet` is a value type over toml_util with no ABI and no
    // singleton in it, which is what makes this suite possible -- and is the
    // same property the host relies on when it says a plugin may link
    // libsweeppp for values.
    CHECK_FALSE(channels::ChannelSet::load("/nonexistent/channels.toml").has_value());
}
