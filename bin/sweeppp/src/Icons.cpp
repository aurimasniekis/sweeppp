// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Icons.hpp"

namespace sweeppp::ui::icon {
namespace {

bool g_available = false;

} // namespace

bool available() noexcept {
    return g_available;
}

void setAvailable(bool value) noexcept {
    g_available = value;
}

std::string glyphOr(const char* glyph, const char* fallback) {
    return g_available ? std::string(glyph) : std::string(fallback);
}

} // namespace sweeppp::ui::icon
