// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/remote/ServerList.hpp"

#include "sweeppp/core/Log.hpp"
#include "sweeppp/core/Toml.hpp"

#include <algorithm>

namespace sweeppp::remote {

ServerList ServerList::load(const std::filesystem::path& path) {
    ServerList list;
    auto table = toml_util::load(path);
    if (!table) {
        if (table.error().code() != ErrorCode::NotFound) {
            logWarn("remote", "{}", table.error().describe());
        }
        return list;
    }

    const auto* rows = toml_util::at(*table, "server").as_array();
    if (rows == nullptr) {
        return list;
    }
    for (const ::toml::node& row : *rows) {
        const ::toml::table* entry = row.as_table();
        if (entry == nullptr) {
            continue;
        }
        const std::int64_t port = toml_util::getInt(*entry, "port", kDefaultPort);
        SavedServer server{
            .name = toml_util::getString(*entry, "name", ""),
            .endpoint = RemoteEndpoint{
                .host = toml_util::getString(*entry, "host", ""),
                .port = static_cast<std::uint16_t>(port > 0 && port <= 65535 ? port : kDefaultPort),
                .token = toml_util::getString(*entry, "token", "")}};
        const std::int64_t maxBins = toml_util::getInt(*entry, "max_bins", 0);
        server.maxBins =
            maxBins > 0 && maxBins <= kMaxGridBins ? static_cast<std::uint32_t>(maxBins) : 0;
        if (server.endpoint.host.empty()) {
            continue;
        }
        if (server.name.empty()) {
            server.name = server.endpoint.host;
        }
        list.put(std::move(server));
    }
    return list;
}

Status ServerList::save(const std::filesystem::path& path) const {
    ::toml::array rows;
    for (const SavedServer& server : m_entries) {
        ::toml::table row;
        row.insert_or_assign("name", server.name);
        row.insert_or_assign("host", server.endpoint.host);
        row.insert_or_assign("port", static_cast<std::int64_t>(server.endpoint.port));
        if (!server.endpoint.token.empty()) {
            row.insert_or_assign("token", server.endpoint.token);
        }
        if (server.maxBins > 0) {
            row.insert_or_assign("max_bins", static_cast<std::int64_t>(server.maxBins));
        }
        rows.push_back(std::move(row));
    }
    ::toml::table root;
    root.insert_or_assign("server", std::move(rows));

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (auto saved = toml_util::save(path, root, "Sweep++ servers -- holds tokens, keep private");
        !saved) {
        return saved;
    }
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
    if (ec) {
        logWarn("remote", "could not make {} private: {}", path.string(), ec.message());
    }
    return ok();
}

const SavedServer* ServerList::find(std::string_view address) const {
    const auto match = std::ranges::find_if(m_entries, [address](const SavedServer& server) {
        return server.endpoint.address() == address;
    });
    return match != m_entries.end() ? &*match : nullptr;
}

SavedServer* ServerList::find(std::string_view address) {
    const auto match = std::ranges::find_if(m_entries, [address](const SavedServer& server) {
        return server.endpoint.address() == address;
    });
    return match != m_entries.end() ? &*match : nullptr;
}

void ServerList::put(SavedServer server) {
    const std::string address = server.endpoint.address();
    const auto match = std::ranges::find_if(m_entries, [&address](const SavedServer& existing) {
        return existing.endpoint.address() == address;
    });
    if (match != m_entries.end()) {
        *match = std::move(server);
    } else {
        m_entries.push_back(std::move(server));
    }
}

void ServerList::remove(std::string_view address) {
    std::erase_if(m_entries, [address](const SavedServer& server) {
        return server.endpoint.address() == address;
    });
}

} // namespace sweeppp::remote
