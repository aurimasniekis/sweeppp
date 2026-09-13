// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/sdr/SampleFormat.hpp"
#include "sweeppp/sdr/SdrDeviceInfo.hpp"
#include "sweeppp/sdr/SdrParameter.hpp"
#include "sweeppp/sdr/SdrRxPort.hpp"

#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sweeppp {

class BlockPool;
struct StreamCounters;

/// Delivered from the device's own transfer thread. MUST NOT block: on
/// libhackrf and libbladeRF this runs on the USB transfer thread, and blocking
/// it causes device-side overruns -- the very failure the telemetry exists to
/// report. Implementations enqueue and return.
using IqCallback = std::function<void(IqBlock&&)>;

/// A radio.
///
/// Two ways to get samples out, and the split is deliberate:
///
///  * `start(callback)` is the primary path -- the device pushes native-format
///    blocks from the pool as they arrive. This is the only path that can
///    sustain 100 MS/s.
///  * `readSamples()` is a pull-mode convenience adapter that converts to
///    complex<float>. Useful for tests and simple tools; it cannot carry a
///    high-rate stream and does not pretend to.
class ISdrDevice {
public:
    virtual ~ISdrDevice() = default;

    ISdrDevice(const ISdrDevice&) = delete;
    ISdrDevice& operator=(const ISdrDevice&) = delete;

    [[nodiscard]] virtual const SdrDeviceInfo& info() const noexcept = 0;

    /// Every parameter this device exposes. The UI panel is generated from
    /// this alone -- no per-device UI code exists anywhere.
    [[nodiscard]] virtual std::span<const SdrParameter> parameters() const noexcept = 0;

    [[nodiscard]] virtual Result<SdrValue> getParameter(std::string_view key) const = 0;
    [[nodiscard]] virtual Status setParameter(std::string_view key, const SdrValue& value) = 0;

    /// Native format of the delivered blocks. May differ from what was
    /// requested in StreamConfig if the device cannot produce it.
    [[nodiscard]] virtual SampleFormat nativeFormat() const noexcept = 0;

    /// Sample rates the hardware actually supports. Empty means continuous
    /// within the info() range.
    [[nodiscard]] virtual std::vector<double> supportedSampleRates() const { return {}; }

    /// Begins streaming. `pool` supplies the blocks; the device never
    /// allocates while running.
    [[nodiscard]] virtual Status start(BlockPool& pool, const StreamConfig& config,
                                       IqCallback callback) = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual bool streaming() const noexcept = 0;

    /// Retunes while streaming. Separate from setParameter("center_hz")
    /// because the sweep engine calls it thousands of times a second and needs
    /// the fastest path the driver has.
    [[nodiscard]] virtual Status retune(double centerHz) = 0;

    /// Time the hardware needs after a retune before its output is
    /// trustworthy. The sweep engine discards this much rather than guessing,
    /// and it is per-device because a HackRF and a BladeRF differ by an order
    /// of magnitude.
    [[nodiscard]] virtual double retuneSettleSeconds() const noexcept { return 0.0; }

    /// The RF inputs this radio has, in the order a panel should list them.
    ///
    /// Empty means one implicit input, which is every single-connector radio
    /// and the default: the panel then shows a single antenna assignment with
    /// no port chooser, and routing has nothing to choose between. A driver
    /// with one connector implements none of these three.
    ///
    /// The span stays valid until the next call to this function or until the
    /// device is destroyed. A driver may discover its port count at open --
    /// bladeRF asks the hardware how many RX channels it has -- so this is not
    /// required to be a compile-time list.
    [[nodiscard]] virtual std::span<const SdrRxPort> rxPorts() const noexcept { return {}; }

    /// Which port is selected, by `SdrRxPort::id`. Empty when the device has
    /// no port concept.
    [[nodiscard]] virtual std::string_view selectedRxPort() const noexcept { return {}; }

    /// Selects one, by id.
    ///
    /// Called by the sweep engine mid-pass on a routed sweep. A port whose
    /// `requiresStop` is set gets its stream cycled by the caller first, so an
    /// implementation may assume it is not streaming in that case.
    [[nodiscard]] virtual Status selectRxPort(std::string_view id) {
        return fail(ErrorCode::Unavailable, "this radio has one receive port, not '{}'", id);
    }

    /// How much signal time arrives in one delivery from the driver.
    ///
    /// The hard floor on how fast a sweep can retune, and the reason a sweep
    /// can look like it is hopping at random when it is not. A USB radio hands
    /// over a whole transfer at once -- on a HackRF, 262144 bytes, which at
    /// 20 MS/s is 6.5 ms of signal. Everything in that transfer arrives with
    /// one tuning attached, because that is all the driver can know. Retune
    /// faster than this and each delivery spans several steps, so most steps
    /// are never attributed anything while a few collect it all: an evenly
    /// spaced pattern of measured and unmeasured bands that looks nothing like
    /// a sweep advancing in order.
    ///
    /// Zero means the device delivers finely enough not to constrain the
    /// sweep. The engine holds each step for at least this long.
    [[nodiscard]] virtual double deliveryGranularitySeconds(double sampleRate) const noexcept {
        (void)sampleRate;
        return 0.0;
    }

    /// Lives at namespace scope as `SdrHealthReading` so a plugin driver can
    /// name it without naming this class; spelled here too because every call
    /// site reads it as a property of a device.
    using HealthReading = SdrHealthReading;

    /// Whatever the device can say about its own condition.
    ///
    /// Called at a few Hz, never per frame: on a USB radio each of these is a
    /// control transfer, and issuing a handful of them every rendered frame
    /// competes with the sample stream for the same bus.
    ///
    /// Empty by default: most radios report nothing, and a driver that does
    /// gets it displayed with no UI change, exactly as `parameters()` does for
    /// controls.
    [[nodiscard]] virtual std::vector<HealthReading> healthReadings() const { return {}; }

    /// Lets the device publish overrun counts and link capacity straight into
    /// the shared telemetry. Optional; devices with no such reporting (HackRF)
    /// leave the overrun counter alone and host-side sequence-gap detection
    /// covers it instead.
    virtual void attachTelemetry(StreamCounters* counters) noexcept { (void)counters; }

    /// Pull-mode convenience adapter. Converts to complex<float> and is
    /// therefore unsuitable above a few MS/s; the callback path is the real
    /// interface. Returns the number of frames written.
    [[nodiscard]] virtual Result<std::size_t> readSamples(std::complex<float>* out,
                                                          std::size_t frames);

protected:
    ISdrDevice() = default;
};

/// Creates devices of one driver kind. Plugins register their own.
class ISdrDeviceFactory {
public:
    virtual ~ISdrDeviceFactory() = default;

    ISdrDeviceFactory(const ISdrDeviceFactory&) = delete;
    ISdrDeviceFactory& operator=(const ISdrDeviceFactory&) = delete;

    [[nodiscard]] virtual std::string_view driver() const noexcept = 0;
    [[nodiscard]] virtual std::string displayName() const = 0;

    /// Radios currently attached. Must be cheap enough to call on a timer, so
    /// hot-plug shows up without the operator hitting refresh.
    [[nodiscard]] virtual std::vector<SdrDeviceInfo> enumerate() const = 0;

    [[nodiscard]] virtual Result<std::unique_ptr<ISdrDevice>> open(std::string_view id) = 0;

    /// Why this factory cannot be withdrawn from the registry right now, or
    /// empty when it can be.
    ///
    /// Only a plugin's factory ever answers anything but empty, and only it
    /// can: `open()` hands back a `unique_ptr<ISdrDevice>` whose vtable lives
    /// in the plugin's image, so the factory is the one thing that knows
    /// whether any of those are still alive. The built-in drivers are never
    /// withdrawn, so the default is what they want.
    [[nodiscard]] virtual std::string withdrawalBlocker() const { return {}; }

protected:
    ISdrDeviceFactory() = default;
};

/// Registry of device drivers, mirroring FftBackendManager.
class SdrDeviceManager {
public:
    [[nodiscard]] static SdrDeviceManager& instance();

    void registerFactory(std::unique_ptr<ISdrDeviceFactory> factory);

    /// Whether a driver name is already claimed.
    ///
    /// `registerFactory` replaces by driver name, so two plugins both claiming
    /// "hackrf" would silently be last-wins. The plugin host asks this first
    /// and refuses the second, listing it with the reason -- a driver quietly
    /// becoming a different driver is not a thing an operator can debug.
    [[nodiscard]] bool hasDriver(std::string_view driver) const;

    /// Withdraws a driver, for a plugin being disabled.
    ///
    /// Refuses when the factory reports a blocker -- an open radio, typically.
    /// `open()` returns a `unique_ptr` whose vtable is in the plugin, so
    /// withdrawing the factory while one is alive would leave the operator
    /// holding a device that cannot even be destroyed.
    [[nodiscard]] Status unregisterFactory(std::string_view driver);

    /// Every device across every driver. Failures in one driver are logged and
    /// skipped rather than aborting the scan -- one misbehaving USB device
    /// must not hide the others.
    [[nodiscard]] std::vector<SdrDeviceInfo> enumerateAll() const;

    [[nodiscard]] Result<std::unique_ptr<ISdrDevice>> open(std::string_view driver,
                                                           std::string_view id);

    /// Accepts "driver" or "driver:id"; with no id, opens the first match.
    [[nodiscard]] Result<std::unique_ptr<ISdrDevice>> openSpecifier(std::string_view specifier);

    [[nodiscard]] std::vector<std::string> drivers() const;

private:
    SdrDeviceManager() = default;

    mutable std::mutex m_mutex;

    /// Shared rather than owned outright, because `open()` and
    /// `enumerateAll()` must call into a factory with the lock released -- USB
    /// enumeration and a firmware handshake are far too slow to hold a mutex
    /// the UI takes on a timer. Holding a raw pointer across that gap was safe
    /// only while nothing was ever withdrawn; a plugin being disabled makes it
    /// a use-after-free. Copying the `shared_ptr` under the lock keeps the
    /// factory alive for exactly as long as the call that is using it.
    ///
    /// `registerFactory` still takes a `unique_ptr`, so no caller changes.
    std::vector<std::shared_ptr<ISdrDeviceFactory>> m_factories;
};

/// Registers the drivers that need nothing outside this library: the synthetic
/// generator and IQ-file replay.
///
/// "Built in" now means exactly that -- no external dependency -- and not
/// "shipped". Every radio arrives through a plugin, so a build of libsweeppp
/// links no device library at all, and an application that wants a HackRF
/// discovers plugins as well as calling this.
///
/// Explicit rather than a static registrar: libsweeppp is a static library, and
/// a translation unit whose symbols are never referenced can be dropped by the
/// linker, taking its registrar with it. A call from a referenced function
/// cannot be elided.
void registerBuiltinSdrDevices();

} // namespace sweeppp
