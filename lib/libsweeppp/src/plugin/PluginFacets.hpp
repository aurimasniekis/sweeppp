// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plugin/PluginAbiSupport.hpp"
#include "sweeppp/core/BlockPool.hpp"
#include "sweeppp/core/Telemetry.hpp"
#include "sweeppp/fft/FftBackendManager.hpp"
#include "sweeppp/pipeline/AsyncFrameConsumer.hpp"
#include "sweeppp/rf/IRfPath.hpp"
#include "sweeppp/sdr/ISdrDevice.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace sweeppp::plugin_facets {

/// A plugin's frame processor, behind the host's own queue and thread.
///
/// Deliberately `AsyncFrameConsumer` and not a bare `IFrameConsumer`: a
/// consumer called inline runs on a pipeline worker, so a plugin that spends a
/// millisecond in `on_frame` does not merely slow itself down -- it backs up
/// the block queue and turns into dropped *samples*. Behind the queue, the
/// worst a slow plugin can do is drop its own frames, which is exactly the
/// degradation an operator can understand from the Performance panel.
class FrameProcessor final : public AsyncFrameConsumer {
public:
    FrameProcessor(std::string name, const sweeppp_frame_processor_vtable_t* vtable,
                   void* instance);
    ~FrameProcessor() override;

protected:
    void processFrame(const SpectrumFramePtr& frame) override;

private:
    const sweeppp_frame_processor_vtable_t* m_vtable;
    void* m_instance;
};

/// A plugin's FFT backend, as the pipeline's own interface.
///
/// Registered through `FftBackendManager::registerBackend`'s existing
/// `info + std::function` shape, so a plugin backend gets lazy instantiation
/// and demote-on-first-failure for free rather than needing a second code path
/// in the manager.
class FftBackend final : public IFftBackend {
public:
    FftBackend(std::string name, std::string displayName,
               const sweeppp_fft_backend_vtable_t* vtable, void* instance,
               FftCapabilities capabilities);

    [[nodiscard]] std::string_view name() const noexcept override { return m_name; }
    [[nodiscard]] std::string displayName() const override { return m_displayName; }
    [[nodiscard]] const FftCapabilities& capabilities() const noexcept override {
        return m_capabilities;
    }

    [[nodiscard]] Result<std::unique_ptr<IFftPlan>>
    createPlan(const FftPlanConfig& config) override;

    [[nodiscard]] std::size_t snapSize(std::size_t desired) const noexcept override;

    void reset() override;

private:
    std::string m_name;
    std::string m_displayName;
    const sweeppp_fft_backend_vtable_t* m_vtable;
    void* m_instance;
    FftCapabilities m_capabilities;
};

/// Reads a plugin backend's capabilities across the ABI. Separate from the
/// backend itself because the registry wants them before anything is
/// instantiated -- that is what "list it without initialising every library"
/// means.
[[nodiscard]] Result<FftCapabilities> readCapabilities(const sweeppp_fft_backend_vtable_t* vtable,
                                                       void* instance);

/// The host's side of one running stream, as the plugin sees it.
///
/// A member of the device rather than a local, because the plugin keeps the
/// pointer for as long as the stream runs.
///
/// Nothing here blocks, allocates or takes a contended lock: a HackRF driver
/// calls `publish` from inside a libusb callback, and anything that could wait
/// there turns a host-side backlog into a device-side overrun.
struct StreamContext {
    sweeppp_sdr_stream_t abi{};

    BlockPool* pool = nullptr;
    IqCallback callback;
    std::atomic<StreamCounters*>* telemetry = nullptr;

    SampleFormat format = SampleFormat::Cs8;

    /// One slot per pool block, indexed by `BlockRef::index()`. A handle is
    /// `&held[index]`: no allocation on the acquisition path, O(1) to resolve,
    /// and unique while the block is checked out -- which it is, because that
    /// is what a pool index means.
    std::vector<BlockRef> held;

    /// The slot a handle names, or null if it names nothing.
    ///
    /// Bounds-checked, and that is not defensive habit: this pointer came from
    /// a plugin, and a stray one must cost a dropped block and a log line
    /// rather than a wild write into the middle of the pool.
    [[nodiscard]] BlockRef* resolve(void* handle) noexcept;

    /// What the device's centre and rate were when the block was lent out,
    /// for a delivery that does not carry its own.
    std::atomic<double> centerHz{0.0};
    std::atomic<double> sampleRate{0.0};
};

/// A plugin's radio, as the pipeline's own interface.
///
/// The ABI is push-mode and mirrors `ISdrDevice` member for member, so this is
/// mostly forwarding: every entry the plugin left null falls through to what
/// `ISdrDevice` does by default. Everything downstream -- the sweep engine,
/// telemetry, the session writer -- sees an ordinary `ISdrDevice`.
///
/// `readSamples` is deliberately NOT overridden. The base implementation
/// synthesises pull mode from the push stream, so a plugin driver gets it
/// without the ABI carrying a second delivery path.
class SdrDevice final : public ISdrDevice {
public:
    SdrDevice(SdrDeviceInfo info, std::vector<SdrParameter> parameters,
              std::vector<SdrRxPort> ports, const sweeppp_sdr_device_vtable_t* vtable, void* device,
              std::shared_ptr<std::atomic<int>> liveCount);
    ~SdrDevice() override;

    [[nodiscard]] const SdrDeviceInfo& info() const noexcept override { return m_info; }
    [[nodiscard]] std::span<const SdrParameter> parameters() const noexcept override {
        return m_parameters;
    }

    [[nodiscard]] Result<SdrValue> getParameter(std::string_view key) const override;
    [[nodiscard]] Status setParameter(std::string_view key, const SdrValue& value) override;

    [[nodiscard]] SampleFormat nativeFormat() const noexcept override;
    [[nodiscard]] std::vector<double> supportedSampleRates() const override;

    [[nodiscard]] Status start(BlockPool& pool, const StreamConfig& config,
                               IqCallback callback) override;
    void stop() override;
    [[nodiscard]] bool streaming() const noexcept override;

    [[nodiscard]] Status retune(double centerHz) override;
    [[nodiscard]] double retuneSettleSeconds() const noexcept override;
    [[nodiscard]] double deliveryGranularitySeconds(double sampleRate) const noexcept override;

    [[nodiscard]] std::vector<HealthReading> healthReadings() const override;

    [[nodiscard]] std::span<const SdrRxPort> rxPorts() const noexcept override { return m_ports; }
    [[nodiscard]] std::string_view selectedRxPort() const noexcept override;
    [[nodiscard]] Status selectRxPort(std::string_view id) override;

    void attachTelemetry(StreamCounters* counters) noexcept override;

    /// Reads a device's parameter table across the ABI. Static because the
    /// factory calls it once at open, before the device object exists.
    [[nodiscard]] static std::vector<SdrParameter>
    readParameters(const sweeppp_sdr_device_vtable_t* vtable, void* device);

    /// Reads a device's receive ports across the ABI, once at open.
    ///
    /// Once, and not on demand like `healthReadings`: the ABI promises the
    /// strings only until the next such call, and the sweep planner holds port
    /// ids across a whole pass. Copying them here is what makes that safe --
    /// and a driver's port list does not change while it is open.
    [[nodiscard]] static std::vector<SdrRxPort>
    readRxPorts(const sweeppp_sdr_device_vtable_t* vtable, void* device);

    /// Reads a device's own identity, which `enumerate` cannot know: firmware,
    /// FPGA and link speed all need an open handle.
    static void readInfo(const sweeppp_sdr_device_vtable_t* vtable, void* device,
                         SdrDeviceInfo& out);

private:
    SdrDeviceInfo m_info;
    const sweeppp_sdr_device_vtable_t* m_vtable;
    void* m_device;
    std::shared_ptr<std::atomic<int>> m_liveCount;

    std::vector<SdrParameter> m_parameters;

    /// Never mutated after construction, so `selectedRxPort` can hand out a
    /// view into it while the sweep thread is switching ports.
    std::vector<SdrRxPort> m_ports;
    std::atomic<std::size_t> m_selectedPort{0};

    std::atomic<StreamCounters*> m_telemetry{nullptr};
    std::atomic<bool> m_streaming{false};

    /// Held for the life of the stream, because the plugin has its address.
    std::unique_ptr<StreamContext> m_stream;
};

/// Why an SDR facet's vtable cannot be registered, or empty when it can.
///
/// Checked at registration rather than at open. A driver that got as far as
/// the registry would appear in `info`, in the device chip and in a saved
/// profile, and fail only when an operator picked it -- which is a fault
/// reported in the wrong place, at the wrong time, to the wrong person.
[[nodiscard]] std::string validateSdrFactory(const sweeppp_sdr_factory_vtable_t* vtable);

/// A plugin's driver, as the registry's own interface.
class SdrFactory final : public ISdrDeviceFactory {
public:
    SdrFactory(std::string driver, std::string displayName,
               const sweeppp_sdr_factory_vtable_t* vtable, void* instance);

    [[nodiscard]] std::string_view driver() const noexcept override { return m_driver; }
    [[nodiscard]] std::string displayName() const override { return m_displayName; }

    [[nodiscard]] std::vector<SdrDeviceInfo> enumerate() const override;
    [[nodiscard]] Result<std::unique_ptr<ISdrDevice>> open(std::string_view id) override;

    /// Devices this factory has handed out and not yet seen destroyed. The
    /// registry refuses to withdraw the driver while any is alive, because
    /// their vtables live in the plugin's image.
    [[nodiscard]] std::string withdrawalBlocker() const override;

private:
    std::string m_driver;
    std::string m_displayName;
    const sweeppp_sdr_factory_vtable_t* m_vtable;
    void* m_instance;

    /// Shared with every device this factory opens, decremented by their
    /// destructors. A `shared_ptr` rather than a member, so a device outliving
    /// its factory -- which the refusal exists to prevent, but which a bug
    /// could still produce -- decrements a live counter rather than a dangling
    /// one.
    std::shared_ptr<std::atomic<int>> m_liveDevices = std::make_shared<std::atomic<int>>(0);
};

// ---- RF path ---------------------------------------------------------

/// A plugin's antenna switcher, as the registry's own interface.
class RfPath final : public IRfPath {
public:
    RfPath(RfPathInfo info, std::vector<RfPathInput> inputs, const sweeppp_rf_path_vtable_t* vtable,
           void* path, std::shared_ptr<std::atomic<int>> liveCount);
    ~RfPath() override;

    [[nodiscard]] const RfPathInfo& info() const noexcept override { return m_info; }
    [[nodiscard]] std::span<const RfPathInput> inputs() const noexcept override { return m_inputs; }

    [[nodiscard]] Status selectInput(std::uint32_t index) override;
    [[nodiscard]] std::uint32_t selectedInput() const noexcept override;

    [[nodiscard]] std::vector<HealthReading> healthReadings() const override;

    /// Read once at open, for the same reason the SDR facet's ports are: the
    /// ABI promises the strings only until the next such call, and an
    /// assignment holds an input id for the life of the installation.
    [[nodiscard]] static std::vector<RfPathInput> readInputs(const sweeppp_rf_path_vtable_t* vtable,
                                                             void* path);

    static void readInfo(const sweeppp_rf_path_vtable_t* vtable, void* path, RfPathInfo& out);

private:
    RfPathInfo m_info;
    std::vector<RfPathInput> m_inputs;
    const sweeppp_rf_path_vtable_t* m_vtable;
    void* m_path;
    std::shared_ptr<std::atomic<int>> m_liveCount;

    std::atomic<std::uint32_t> m_selected{0};
};

/// Why an RF path facet's vtable cannot be registered, or empty when it can.
[[nodiscard]] std::string validateRfPathFactory(const sweeppp_rf_path_factory_vtable_t* vtable);

class RfPathFactory final : public IRfPathFactory {
public:
    RfPathFactory(std::string driver, std::string displayName,
                  const sweeppp_rf_path_factory_vtable_t* vtable, void* instance);

    [[nodiscard]] std::string_view driver() const noexcept override { return m_driver; }
    [[nodiscard]] std::string displayName() const override { return m_displayName; }

    [[nodiscard]] std::vector<RfPathInfo> enumerate() const override;
    [[nodiscard]] Result<std::unique_ptr<IRfPath>> open(std::string_view id) override;
    [[nodiscard]] std::string withdrawalBlocker() const override;

private:
    std::string m_driver;
    std::string m_displayName;
    const sweeppp_rf_path_factory_vtable_t* m_vtable;
    void* m_instance;

    std::shared_ptr<std::atomic<int>> m_livePaths = std::make_shared<std::atomic<int>>(0);
};

} // namespace sweeppp::plugin_facets
