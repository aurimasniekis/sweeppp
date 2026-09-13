// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "Detector.hpp"

#include <doctest/doctest.h>

TEST_CASE("the model is reachable") {
    // A plugin that cannot be tested without the host is a plugin nobody
    // tests. `Detector` takes its time and its data by argument and reaches no
    // singleton at all, which is what makes this suite possible -- and is the
    // same property the host relies on when it says a plugin may link
    // libsweeppp for values.
    const detections::Detector detector;
    CHECK(detector.detections().empty());
    CHECK(detector.config().mode == detections::ThresholdMode::AboveNoise);
}
