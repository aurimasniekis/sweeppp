// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/sdr/SdrDeviceInfo.hpp"

#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sweeppp {

/// One input of a switcher.
struct RfPathInput {
    std::string id;    ///< Stable; what an assignment stores. "in1"
    std::string label; ///< "J1"

    /// Where this input is usable, when the box itself narrows it -- a
    /// filtered or amplified port. Zero means only the antenna's own range
    /// applies.
    double minHz = 0.0;
    double maxHz = 0.0;
};

/// Identity of a switcher, enough to list it and reopen it.
struct RfPathInfo {
    std::string driver; ///< The facet id the registry keys on
    std::string id;     ///< Unique within the driver; usually the serial
    std::string label;  ///< "Mini-Circuits USB-1SP8T-63H"
    std::string model;
    std::string serial;

    std::uint32_t inputCount = 0;

    /// Selecting an input needs the receiver's stream stopped.
    bool requiresStop = false;

    /// Settle after selecting. A mechanical relay is tens of milliseconds, and
    /// the predicted pass time should say so.
    double switchSeconds = 0.0;
};

/// An antenna switcher: a box between one receive port and several antennas.
///
/// Not an `ISdrDevice` and deliberately so. It has no samples, no tuning and
/// no stream, and listing one among the radios would be a lie the device
/// chooser could not recover from.
class IRfPath {
public:
    virtual ~IRfPath() = default;

    IRfPath(const IRfPath&) = delete;
    IRfPath& operator=(const IRfPath&) = delete;

    [[nodiscard]] virtual const RfPathInfo& info() const noexcept = 0;

    /// The inputs, in the order a panel should list them.
    [[nodiscard]] virtual std::span<const RfPathInput> inputs() const noexcept = 0;

    /// Selects one, by position in `inputs()`.
    ///
    /// An index rather than an id because this is on the sweep path: the
    /// engine may drive it several times a pass and must not be comparing
    /// strings to do it. The id is what an assignment persists.
    [[nodiscard]] virtual Status selectInput(std::uint32_t index) = 0;
    [[nodiscard]] virtual std::uint32_t selectedInput() const noexcept = 0;

    using HealthReading = SdrHealthReading;

    /// Whatever the box can say about itself. Called at a few Hz, never per
    /// frame.
    [[nodiscard]] virtual std::vector<HealthReading> healthReadings() const { return {}; }

protected:
    IRfPath() = default;
};

/// Creates switchers of one driver kind. Plugins register their own.
class IRfPathFactory {
public:
    virtual ~IRfPathFactory() = default;

    IRfPathFactory(const IRfPathFactory&) = delete;
    IRfPathFactory& operator=(const IRfPathFactory&) = delete;

    [[nodiscard]] virtual std::string_view driver() const noexcept = 0;
    [[nodiscard]] virtual std::string displayName() const = 0;

    [[nodiscard]] virtual std::vector<RfPathInfo> enumerate() const = 0;
    [[nodiscard]] virtual Result<std::unique_ptr<IRfPath>> open(std::string_view id) = 0;

    /// Why this factory cannot be withdrawn right now, or empty when it can.
    /// Same rule as `ISdrDeviceFactory`: `open()` hands back an object whose
    /// vtable lives in the plugin's image.
    [[nodiscard]] virtual std::string withdrawalBlocker() const { return {}; }

protected:
    IRfPathFactory() = default;
};

/// A switcher the application has open, under the key assignments file it by.
///
/// A borrowed pointer: the application owns the switchers for as long as it is
/// running, and the sweep engine only drives them.
struct OpenRfPath {
    std::string key; ///< "driver:id", matching `AntennaAssignment::switcher`
    IRfPath* path = nullptr;
};

/// The key a switcher is filed under: its driver and its id.
[[nodiscard]] std::string rfPathKey(const RfPathInfo& info);

/// Registry of switcher drivers, mirroring `SdrDeviceManager`.
class RfPathManager {
public:
    [[nodiscard]] static RfPathManager& instance();

    void registerFactory(std::unique_ptr<IRfPathFactory> factory);

    [[nodiscard]] bool hasDriver(std::string_view driver) const;

    /// Withdraws a driver, for a plugin being disabled. Refuses while one of
    /// its switchers is still open.
    [[nodiscard]] Status unregisterFactory(std::string_view driver);

    /// Every switcher across every driver. A failure in one driver is logged
    /// and skipped rather than aborting the scan.
    [[nodiscard]] std::vector<RfPathInfo> enumerateAll() const;

    [[nodiscard]] Result<std::unique_ptr<IRfPath>> open(std::string_view driver,
                                                        std::string_view id);

    /// Accepts "driver" or "driver:id"; with no id, opens the first match.
    [[nodiscard]] Result<std::unique_ptr<IRfPath>> openSpecifier(std::string_view specifier);

    [[nodiscard]] std::vector<std::string> drivers() const;

private:
    RfPathManager() = default;

    mutable std::mutex m_mutex;

    /// Shared rather than owned outright, for the reason `SdrDeviceManager`
    /// gives: `open()` must be called with the lock released, and holding a
    /// raw pointer across that gap becomes a use-after-free the moment a
    /// plugin can be withdrawn.
    std::vector<std::shared_ptr<IRfPathFactory>> m_factories;
};

} // namespace sweeppp
