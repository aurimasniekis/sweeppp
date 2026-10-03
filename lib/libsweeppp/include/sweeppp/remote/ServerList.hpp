// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/remote/RemoteInstrument.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sweeppp::remote {

struct SavedServer {
    std::string name; ///< What the list calls it: "Roof Pi"
    RemoteEndpoint endpoint;
};

/// The servers this desktop knows, with their tokens.
///
/// A file of its own rather than part of a profile: a profile names a server
/// by address and is meant to be passed around, and a token in it would go
/// wherever the profile went.
class ServerList {
public:
    /// Missing is empty; malformed is logged and empty.
    [[nodiscard]] static ServerList load(const std::filesystem::path& path);

    /// Written readable by its owner only, since it holds the tokens.
    [[nodiscard]] Status save(const std::filesystem::path& path) const;

    [[nodiscard]] std::span<const SavedServer> entries() const noexcept { return m_entries; }

    /// By `RemoteEndpoint::address()`.
    [[nodiscard]] const SavedServer* find(std::string_view address) const;

    /// Adds `server`, or replaces the one at the same address.
    void put(SavedServer server);
    void remove(std::string_view address);

private:
    std::vector<SavedServer> m_entries;
};

} // namespace sweeppp::remote
