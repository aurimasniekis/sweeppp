// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <sweeppp/web/WebServer.hpp>

namespace sweeppp::cli {

/// The browser UI as built into this binary; empty when it was built without.
[[nodiscard]] std::span<const web::EmbeddedFile> webAssets();

} // namespace sweeppp::cli
