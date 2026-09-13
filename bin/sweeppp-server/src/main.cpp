// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <print>
#include <sweeppp/core/Version.hpp>

int main() {
    std::println("sweeppp-server {} (skeleton)", sweeppp::versionString());
    return 0;
}
