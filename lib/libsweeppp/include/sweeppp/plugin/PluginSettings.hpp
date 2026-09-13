// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"

#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <vector>

namespace sweeppp {

/// One plugin's own settings file, `<configDir>/plugins/<plugin.id>.toml`.
///
/// A plain value over `toml_util`, with no singleton behind it, which is what
/// makes it safe for a plugin to use directly: the file it reads is the file
/// the host named through `sweeppp_host_api_t::path`, so the two never
/// disagree about where the settings are even though they have separate copies
/// of everything else.
///
/// One file per plugin rather than a section in `settings.toml`, and that is
/// deliberate. `settings.toml` and every named profile are the same `Profile`
/// struct: a plugin's state living there would mean loading a profile silently
/// replacing a plugin's configuration, which is not a thing the operator asked
/// for and not a thing they would connect to what they did.
class PluginSettings {
public:
    PluginSettings() = default;

    /// Reads `path`, or starts empty when it does not exist or will not parse.
    ///
    /// Never fails: a plugin whose settings file has been mangled should come
    /// up with its defaults rather than refuse to load. A file that exists and
    /// would not parse appends its reason to `problem`, which the caller has
    /// somewhere to send -- an out-parameter rather than a log call, because
    /// the caller is usually a plugin and a plugin's copy of `Log` is not the
    /// host's.
    [[nodiscard]] static PluginSettings load(std::filesystem::path path,
                                             std::string* problem = nullptr);

    [[nodiscard]] bool getBool(std::string_view key, bool fallback) const;
    [[nodiscard]] std::int64_t getInt(std::string_view key, std::int64_t fallback) const;
    [[nodiscard]] double getDouble(std::string_view key, double fallback) const;
    [[nodiscard]] float getFloat(std::string_view key, float fallback) const;
    [[nodiscard]] std::string getString(std::string_view key, std::string_view fallback) const;

    /// Empty for a key that is missing or is not an array of strings.
    ///
    /// The one plural accessor, and it earns its place: a plugin holding a set
    /// -- which groups are off, which ids are pinned -- has nowhere to put it
    /// otherwise, and flattening one into a delimited scalar puts a delimiter
    /// inside values the operator hand-edits.
    [[nodiscard]] std::vector<std::string> getStringArray(std::string_view key) const;

    void set(std::string_view key, bool value);
    void set(std::string_view key, std::int64_t value);
    void set(std::string_view key, double value);
    void set(std::string_view key, std::string_view value);
    void set(std::string_view key, std::span<const std::string> values);

    /// Writes atomically, creating the directory if it is missing.
    [[nodiscard]] Status save() const;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }
    [[nodiscard]] const ::toml::table& table() const noexcept { return m_table; }

private:
    std::filesystem::path m_path;
    ::toml::table m_table;
};

/// What the operator has decided about plugins, in `<configDir>/plugins.toml`.
///
/// Which ones are off, which contributors answer first, and which of them draw
/// on the plots. All three are the operator's own arrangement of the
/// application rather than a measurement setup, which is why none of them
/// lives in a profile -- loading a profile must not silently reorder who names
/// the band under the marker any more than it should swap the plugin set.
///
/// A disabled-id array rather than an enabled one, following
/// `SweepPresetStore`'s hidden-builtins: an id naming a plugin that is no
/// longer on disk is simply inert, and a plugin that appears later is on by
/// default rather than invisible until someone finds the file. The contributor
/// order keeps unresolved ids for the same reason -- removing and restoring a
/// plugin must not lose its position.
class PluginEnablement {
public:
    [[nodiscard]] static PluginEnablement load(std::filesystem::path path);

    [[nodiscard]] bool isEnabled(std::string_view id) const;
    void setEnabled(std::string_view id, bool enabled);

    /// Contributor priority, highest first. An id absent from this list ranks
    /// after every id in it.
    [[nodiscard]] const std::vector<std::string>& contributorOrder() const noexcept {
        return m_contributorOrder;
    }

    void setContributorOrder(std::vector<std::string> ids);

    [[nodiscard]] bool contributorShown(std::string_view id) const;
    void setContributorShown(std::string_view id, bool shown);

    [[nodiscard]] Status save() const;

    [[nodiscard]] const std::set<std::string, std::less<>>& disabled() const noexcept {
        return m_disabled;
    }

private:
    std::filesystem::path m_path;
    std::set<std::string, std::less<>> m_disabled;
    std::vector<std::string> m_contributorOrder;
    std::set<std::string, std::less<>> m_contributorsHidden;
};

} // namespace sweeppp
