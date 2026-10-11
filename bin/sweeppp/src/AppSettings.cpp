// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AppSettings.hpp"

#include <algorithm>
#include <sweeppp/core/Log.hpp>
#include <sweeppp/core/Toml.hpp>

namespace sweeppp::ui {

AppSettings AppSettings::load(const std::filesystem::path& path) {
    AppSettings settings;

    auto table = toml_util::load(path);
    if (!table) {
        // Nothing saved yet is the normal first-run state; a file that will
        // not parse is worth a line in the log and then the defaults.
        if (table.error().code() != ErrorCode::NotFound) {
            logWarn("app", "{}", table.error().describe());
        }
        return settings;
    }

    const float scale = toml_util::getFloat(*table, "appearance.ui_scale", settings.uiScale);
    // Zero passes through untouched: it is the "follow the display" value, not
    // a scale, and clamping it to the minimum would silently turn automatic
    // into a fixed 0.75.
    settings.uiScale = scale <= 0.0F ? 0.0F : std::clamp(scale, kMinUiScale, kMaxUiScale);
    settings.fontSize =
        std::clamp(toml_util::getFloat(*table, "appearance.font_size", settings.fontSize),
                   kMinFontSize, kMaxFontSize);
    settings.fontWeight =
        std::clamp(toml_util::getFloat(*table, "appearance.font_weight", settings.fontWeight),
                   kMinFontWeight, kMaxFontWeight);
    settings.checkForUpdates =
        toml_util::getBool(*table, "updates.check_on_start", settings.checkForUpdates);
    settings.whatsNewSeen = toml_util::getString(*table, "whats_new.seen", settings.whatsNewSeen);

    if (const auto* devices = toml_util::at(*table, "network_device").as_array()) {
        for (const auto& node : *devices) {
            const auto* declared = node.as_table();
            if (declared == nullptr) {
                continue;
            }
            NetworkDevice device{.driver = toml_util::getString(*declared, "driver", ""),
                                 .id = toml_util::getString(*declared, "id", ""),
                                 .name = toml_util::getString(*declared, "name", "")};
            if (!device.driver.empty() && !device.id.empty()) {
                settings.networkDevices.push_back(std::move(device));
            }
        }
    }

    return settings;
}

Status AppSettings::save(const std::filesystem::path& path) const {
    ::toml::table root;

    ::toml::table& appearance = toml_util::ensureTable(root, "appearance");
    appearance.insert_or_assign("ui_scale", static_cast<double>(uiScale));
    appearance.insert_or_assign("font_size", static_cast<double>(fontSize));
    appearance.insert_or_assign("font_weight", static_cast<double>(fontWeight));

    ::toml::table& updates = toml_util::ensureTable(root, "updates");
    updates.insert_or_assign("check_on_start", checkForUpdates);

    toml_util::ensureTable(root, "whats_new").insert_or_assign("seen", whatsNewSeen);

    ::toml::array devices;
    for (const NetworkDevice& device : networkDevices) {
        ::toml::table row;
        row.insert_or_assign("driver", device.driver);
        row.insert_or_assign("id", device.id);
        row.insert_or_assign("name", device.name);
        devices.push_back(std::move(row));
    }
    root.insert_or_assign("network_device", std::move(devices));

    return toml_util::save(path, root,
                           "Sweep++ application preferences.\n"
                           "Machine-level, and deliberately not part of a profile: loading a "
                           "sweep configuration must not resize the interface.\n"
                           "ui_scale = 0 follows the display.");
}

} // namespace sweeppp::ui
