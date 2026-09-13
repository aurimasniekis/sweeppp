// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/plugin/PluginSettings.hpp"

#include "sweeppp/core/Toml.hpp"

namespace sweeppp {
namespace {

/// Splits a dotted key into its table path and its final name, so `set()` can
/// create the intermediate tables `toml_util::at()` only reads.
std::pair<std::string_view, std::string_view> splitKey(std::string_view key) {
    const std::size_t dot = key.rfind('.');
    if (dot == std::string_view::npos) {
        return {std::string_view{}, key};
    }
    return {key.substr(0, dot), key.substr(dot + 1)};
}

} // namespace

PluginSettings PluginSettings::load(std::filesystem::path path, std::string* problem) {
    PluginSettings settings;
    settings.m_path = std::move(path);

    auto table = toml_util::load(settings.m_path);
    if (!table) {
        // Missing is the normal case on first run, and unparseable is a file
        // the operator hand-edited into a state that should not stop the
        // plugin coming up. Both mean defaults; only the second is worth
        // reporting.
        if (std::error_code ec;
            problem != nullptr && std::filesystem::exists(settings.m_path, ec)) {
            *problem = table.error().describe();
        }
        return settings;
    }

    settings.m_table = std::move(*table);
    return settings;
}

bool PluginSettings::getBool(std::string_view key, bool fallback) const {
    return toml_util::getBool(toml_util::at(m_table, key), fallback);
}

std::int64_t PluginSettings::getInt(std::string_view key, std::int64_t fallback) const {
    return toml_util::getInt(toml_util::at(m_table, key), fallback);
}

double PluginSettings::getDouble(std::string_view key, double fallback) const {
    return toml_util::getDouble(toml_util::at(m_table, key), fallback);
}

float PluginSettings::getFloat(std::string_view key, float fallback) const {
    return static_cast<float>(getDouble(key, static_cast<double>(fallback)));
}

std::string PluginSettings::getString(std::string_view key, std::string_view fallback) const {
    return toml_util::getString(toml_util::at(m_table, key), fallback);
}

std::vector<std::string> PluginSettings::getStringArray(std::string_view key) const {
    return toml_util::getStringArray(m_table, key);
}

void PluginSettings::set(std::string_view key, bool value) {
    const auto [path, name] = splitKey(key);
    ::toml::table& parent = path.empty() ? m_table : toml_util::ensureTable(m_table, path);
    parent.insert_or_assign(name, value);
}

void PluginSettings::set(std::string_view key, std::int64_t value) {
    const auto [path, name] = splitKey(key);
    ::toml::table& parent = path.empty() ? m_table : toml_util::ensureTable(m_table, path);
    parent.insert_or_assign(name, value);
}

void PluginSettings::set(std::string_view key, double value) {
    const auto [path, name] = splitKey(key);
    ::toml::table& parent = path.empty() ? m_table : toml_util::ensureTable(m_table, path);
    parent.insert_or_assign(name, value);
}

void PluginSettings::set(std::string_view key, std::string_view value) {
    const auto [path, name] = splitKey(key);
    ::toml::table& parent = path.empty() ? m_table : toml_util::ensureTable(m_table, path);
    parent.insert_or_assign(name, std::string(value));
}

void PluginSettings::set(std::string_view key, std::span<const std::string> values) {
    ::toml::array array;
    for (const std::string& value : values) {
        array.push_back(value);
    }

    const auto [path, name] = splitKey(key);
    ::toml::table& parent = path.empty() ? m_table : toml_util::ensureTable(m_table, path);
    parent.insert_or_assign(name, std::move(array));
}

Status PluginSettings::save() const {
    if (m_path.empty()) {
        return fail(ErrorCode::InvalidArgument, "these settings have no path");
    }

    std::error_code ec;
    std::filesystem::create_directories(m_path.parent_path(), ec);

    return toml_util::save(m_path, m_table, "Sweep++ plugin settings");
}

PluginEnablement PluginEnablement::load(std::filesystem::path path) {
    PluginEnablement enablement;
    enablement.m_path = std::move(path);

    auto table = toml_util::load(enablement.m_path);
    if (!table) {
        // No file means nothing has been turned off, which is the right answer
        // on a fresh install and after the file is deleted to start over.
        return enablement;
    }

    for (std::string& id : toml_util::getStringArray(*table, "plugins.disabled")) {
        enablement.m_disabled.insert(std::move(id));
    }
    enablement.m_contributorOrder = toml_util::getStringArray(*table, "plugins.contributors");
    for (std::string& id : toml_util::getStringArray(*table, "plugins.contributors_hidden")) {
        enablement.m_contributorsHidden.insert(std::move(id));
    }
    return enablement;
}

bool PluginEnablement::isEnabled(std::string_view id) const {
    return !m_disabled.contains(id);
}

void PluginEnablement::setEnabled(std::string_view id, bool enabled) {
    if (enabled) {
        if (const auto found = m_disabled.find(id); found != m_disabled.end()) {
            m_disabled.erase(found);
        }
    } else {
        m_disabled.emplace(id);
    }
}

void PluginEnablement::setContributorOrder(std::vector<std::string> ids) {
    m_contributorOrder = std::move(ids);
}

bool PluginEnablement::contributorShown(std::string_view id) const {
    return !m_contributorsHidden.contains(id);
}

void PluginEnablement::setContributorShown(std::string_view id, bool shown) {
    if (shown) {
        if (const auto found = m_contributorsHidden.find(id); found != m_contributorsHidden.end()) {
            m_contributorsHidden.erase(found);
        }
    } else {
        m_contributorsHidden.emplace(id);
    }
}

Status PluginEnablement::save() const {
    if (m_path.empty()) {
        return fail(ErrorCode::InvalidArgument, "this enablement store has no path");
    }

    ::toml::table root;
    ::toml::array disabled;
    for (const std::string& id : m_disabled) {
        disabled.push_back(id);
    }

    // Written in the operator's order, unresolved ids and all. An id here that
    // names nothing installed is the position a plugin gets back when it
    // returns, which is the whole reason it is not filtered on the way out.
    ::toml::array contributors;
    for (const std::string& id : m_contributorOrder) {
        contributors.push_back(id);
    }

    ::toml::array hidden;
    for (const std::string& id : m_contributorsHidden) {
        hidden.push_back(id);
    }

    ::toml::table& plugins = toml_util::ensureTable(root, "plugins");
    plugins.insert_or_assign("disabled", std::move(disabled));
    plugins.insert_or_assign("contributors", std::move(contributors));
    plugins.insert_or_assign("contributors_hidden", std::move(hidden));

    std::error_code ec;
    std::filesystem::create_directories(m_path.parent_path(), ec);

    return toml_util::save(m_path, root, "Sweep++ plugin enablement");
}

} // namespace sweeppp
