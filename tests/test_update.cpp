// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <UpdateCheck.hpp>
#include <doctest/doctest.h>

using namespace sweeppp::ui;

TEST_CASE("a release is newer by its numbers, not by its spelling") {
    // The comparison a string compare gets wrong, and the reason this is not
    // one: "0.10.0" sorts before "0.9.0" as text.
    CHECK(UpdateCheck::isNewer("0.10.0", "0.9.0"));
    CHECK_FALSE(UpdateCheck::isNewer("0.9.0", "0.10.0"));

    CHECK(UpdateCheck::isNewer("0.2.0", "0.1.0"));
    CHECK(UpdateCheck::isNewer("1.0.0", "0.99.99"));
    CHECK_FALSE(UpdateCheck::isNewer("0.1.0", "0.1.0"));
    CHECK_FALSE(UpdateCheck::isNewer("0.0.9", "0.1.0"));

    // A tag carries a v and a build string carries a commit. Neither is part
    // of the version, and neither may decide the answer.
    CHECK(UpdateCheck::isNewer("v0.2.0", "0.1.0"));
    CHECK_FALSE(UpdateCheck::isNewer("v0.1.0", "0.1.0"));
    CHECK_FALSE(UpdateCheck::isNewer("v0.1.0", "0.1.0+abc12345"));
    CHECK(UpdateCheck::isNewer("v0.1.1", "0.1.0+abc12345.dirty"));

    // Missing components are zero, so 0.2 and 0.2.0 are the same release.
    CHECK_FALSE(UpdateCheck::isNewer("0.2", "0.2.0"));
    CHECK(UpdateCheck::isNewer("0.2.1", "0.2"));

    // Nothing numeric to compare is not an upgrade. A server that answered
    // with something unexpected must not produce a card.
    CHECK_FALSE(UpdateCheck::isNewer("", "0.1.0"));
    CHECK_FALSE(UpdateCheck::isNewer("nightly", "0.1.0"));
    CHECK_FALSE(UpdateCheck::isNewer("v", "0.1.0"));
}
