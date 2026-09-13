// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/PluginFacets.hpp"

#include "sweeppp/core/BlockPool.hpp"
#include "sweeppp/core/Clock.hpp"

#include <algorithm>
#include <array>

namespace sweeppp::plugin_facets {
namespace {

using plugin_abi::fromAbiFormat;
using plugin_abi::fromAbiParameterType;
using plugin_abi::fromAbiValue;
using plugin_abi::kLogCategory;
using plugin_abi::owned;
using plugin_abi::statusName;
using plugin_abi::toAbiFormat;
using plugin_abi::toAbiValue;
using plugin_abi::toErrorCode;

/// The most devices one `enumerate()` will report.
///
/// Bounded because the count comes from the plugin and is used to size a
/// buffer: a driver returning a nonsense figure should cost a truncated list,
/// not the address space. Two calls -- ask, then fill -- would have the same
/// problem and cost an extra crossing.
constexpr std::uint32_t kMaxEnumeratedDevices = 256;

} // namespace

// --------------------------------------------------------------- frames

FrameProcessor::FrameProcessor(std::string name, const sweeppp_frame_processor_vtable_t* vtable,
                               void* instance)
    : AsyncFrameConsumer(std::move(name)), m_vtable(vtable), m_instance(instance) {
    startWorker();
}

FrameProcessor::~FrameProcessor() {
    AsyncFrameConsumer::shutdown();
}

void FrameProcessor::processFrame(const SpectrumFramePtr& frame) {
    if (m_vtable == nullptr || m_vtable->on_frame == nullptr || !frame) {
        return;
    }

    // A view, not a copy. The frame is a `shared_ptr<const>` this thread holds
    // for the duration of the call, so a processor that wants to alter the
    // data republishes rather than writing through `bins_dbfs` -- every other
    // consumer is looking at the same buffer.
    const sweeppp_frame_t abi{
        .struct_size = sizeof(sweeppp_frame_t),
        .sequence = frame->sequence,
        .host_time_ns = frame->hostTimeNs,
        .wall_time_ns = frame->wallTimeNs,
        .sweep_pass = frame->sweepPass,
        .sweep_step = frame->sweepStep,
        .pass_complete = frame->passComplete ? 1 : 0,
        .start_hz = frame->startHz,
        .bin_width_hz = frame->binWidthHz,
        .bins_dbfs = frame->binsDbfs.data(),
        .bin_count = frame->binsDbfs.size(),
        .average_count = frame->averageCount,
        .clipped_fraction = frame->clippedFraction,
    };

    m_vtable->on_frame(m_instance, &abi);
}

// ------------------------------------------------------------------ fft

namespace {

/// One prepared transform, forwarded.
class PluginFftPlan final : public IFftPlan {
public:
    PluginFftPlan(const sweeppp_fft_plan_vtable_t* vtable, void* plan, std::size_t size,
                  bool inverse)
        : m_vtable(vtable), m_plan(plan), m_size(size), m_inverse(inverse) {}

    ~PluginFftPlan() override {
        if (m_vtable != nullptr && m_vtable->destroy != nullptr) {
            m_vtable->destroy(m_plan);
        }
    }

    PluginFftPlan(const PluginFftPlan&) = delete;
    PluginFftPlan& operator=(const PluginFftPlan&) = delete;

    void execute(const std::complex<float>* input, std::complex<float>* output) noexcept override {
        if (m_vtable->execute == nullptr) {
            return;
        }
        // complex<float> is specified to be layout-compatible with float[2],
        // which is what lets the ABI carry interleaved floats without a copy.
        m_vtable->execute(m_plan, reinterpret_cast<const float*>(input),
                          reinterpret_cast<float*>(output));
    }

    void executeBatch(const std::complex<float>* input, std::complex<float>* output,
                      std::size_t count) noexcept override {
        if (m_vtable->execute_batch != nullptr) {
            m_vtable->execute_batch(m_plan, reinterpret_cast<const float*>(input),
                                    reinterpret_cast<float*>(output), count);
            return;
        }
        for (std::size_t i = 0; i < count; ++i) {
            execute(input + (i * m_size), output + (i * m_size));
        }
    }

    [[nodiscard]] std::size_t size() const noexcept override { return m_size; }
    [[nodiscard]] bool inverse() const noexcept override { return m_inverse; }

private:
    const sweeppp_fft_plan_vtable_t* m_vtable;
    void* m_plan;
    std::size_t m_size;
    bool m_inverse;
};

} // namespace

Result<FftCapabilities> readCapabilities(const sweeppp_fft_backend_vtable_t* vtable,
                                         void* instance) {
    if (vtable == nullptr || !SWEEPPP_ABI_HAS(vtable, capabilities) ||
        vtable->capabilities == nullptr) {
        return fail<FftCapabilities>(ErrorCode::ProtocolError,
                                     "the FFT facet declares no capabilities entry point");
    }

    sweeppp_fft_capabilities_t abi{};
    abi.struct_size = sizeof(abi);
    if (const sweeppp_plugin_status_t status = vtable->capabilities(instance, &abi);
        status != SWEEPPP_PLUGIN_OK) {
        return fail<FftCapabilities>(toErrorCode(status), "capabilities: {}", statusName(status));
    }

    FftCapabilities capabilities;
    capabilities.type = abi.is_gpu != 0 ? FftBackendType::Gpu : FftBackendType::Cpu;
    capabilities.minSize = abi.min_size != 0 ? abi.min_size : 2;
    capabilities.maxSize = abi.max_size != 0 ? abi.max_size : (1U << 24U);
    capabilities.sizeConstraint =
        abi.power_of_two_only != 0 ? FftSizeConstraint::PowerOfTwo : FftSizeConstraint::Any;
    capabilities.supportsBatch = abi.supports_batch != 0;
    capabilities.supportsInPlace = abi.supports_in_place != 0;
    capabilities.threadSafeExecute = abi.thread_safe_execute != 0;
    capabilities.threadSafePlanning = abi.thread_safe_planning != 0;
    return capabilities;
}

FftBackend::FftBackend(std::string name, std::string displayName,
                       const sweeppp_fft_backend_vtable_t* vtable, void* instance,
                       FftCapabilities capabilities)
    : m_name(std::move(name)), m_displayName(std::move(displayName)), m_vtable(vtable),
      m_instance(instance), m_capabilities(capabilities) {
}

Result<std::unique_ptr<IFftPlan>> FftBackend::createPlan(const FftPlanConfig& config) {
    if (m_vtable->create_plan == nullptr) {
        return fail<std::unique_ptr<IFftPlan>>(ErrorCode::Unsupported,
                                               "FFT backend '{}' cannot create plans", m_name);
    }

    const sweeppp_fft_plan_config_t abi{
        .struct_size = sizeof(sweeppp_fft_plan_config_t),
        .size = config.size,
        .batch_count = config.batchCount,
        .inverse = config.inverse ? 1 : 0,
        .in_place = config.inPlace ? 1 : 0,
        .quality = static_cast<std::int32_t>(config.quality),
    };

    void* plan = nullptr;
    const sweeppp_fft_plan_vtable_t* planVtable = nullptr;
    const sweeppp_plugin_status_t status =
        m_vtable->create_plan(m_instance, &abi, &plan, &planVtable);
    if (status != SWEEPPP_PLUGIN_OK) {
        return fail<std::unique_ptr<IFftPlan>>(toErrorCode(status),
                                               "FFT backend '{}' could not plan size {}: {}",
                                               m_name, config.size, statusName(status));
    }
    if (planVtable == nullptr || planVtable->execute == nullptr) {
        return fail<std::unique_ptr<IFftPlan>>(
            ErrorCode::ProtocolError, "FFT backend '{}' returned a plan with no execute entry",
            m_name);
    }

    return std::make_unique<PluginFftPlan>(planVtable, plan, config.size, config.inverse);
}

std::size_t FftBackend::snapSize(std::size_t desired) const noexcept {
    if (m_vtable->snap_size != nullptr) {
        return m_vtable->snap_size(m_instance, desired);
    }
    return IFftBackend::snapSize(desired);
}

void FftBackend::reset() {
    if (m_vtable->reset != nullptr) {
        m_vtable->reset(m_instance);
    }
}
// ------------------------------------------------------------------ sdr

namespace {

/// Reads an array-filling call that reports how many there really are.
///
/// The count comes from the plugin and would otherwise size a buffer, so it is
/// clamped and the truncation is said out loud -- a list silently cut short
/// reads as a radio with fewer controls than it has.
template <typename T, std::size_t Capacity, typename Fn>
std::uint32_t fillArray(std::array<T, Capacity>& buffer, std::string_view what,
                        std::string_view owner, Fn&& call) {
    for (T& entry : buffer) {
        entry = T{};
        entry.struct_size = sizeof(T);
    }

    constexpr auto kCapacity = static_cast<std::uint32_t>(Capacity);
    const std::uint32_t count = call(buffer.data(), kCapacity);
    if (count > kCapacity) {
        logWarn(kLogCategory, "'{}' reports {} {}; taking the first {}", owner, count, what,
                kCapacity);
        return kCapacity;
    }
    return count;
}

/// One ABI parameter descriptor, as the panel's own type.
SdrParameter parameterFrom(const sweeppp_sdr_parameter_t& abi) {
    SdrParameter parameter;
    parameter.key = owned(abi.key);
    parameter.label = owned(abi.label);
    parameter.group = owned(abi.group);
    parameter.type = fromAbiParameterType(abi.type);
    parameter.unit = owned(abi.unit);
    parameter.min = abi.minimum;
    parameter.max = abi.maximum;
    parameter.step = abi.step;
    parameter.readOnly = abi.read_only != 0;
    parameter.gridAffecting = abi.grid_affecting != 0;
    parameter.calibrationAffecting = abi.calibration_affecting != 0;
    parameter.requiresStop = abi.requires_stop != 0;
    parameter.description = owned(abi.description);

    if (auto value = fromAbiValue(abi.default_value)) {
        parameter.defaultValue = std::move(*value);
    }

    const std::uint32_t enumCount =
        std::min(abi.enum_value_count, static_cast<std::uint32_t>(SWEEPPP_SDR_MAX_ENUM_VALUES));
    if (enumCount < abi.enum_value_count) {
        logWarn(kLogCategory, "parameter '{}' lists {} values; taking the first {}", parameter.key,
                abi.enum_value_count, enumCount);
    }
    parameter.enumValues.reserve(enumCount);
    plugin_abi::forEachElement(
        abi.enum_values, enumCount, [&parameter](const sweeppp_sdr_enum_value_t& value) {
            parameter.enumValues.push_back(SdrEnumValue{.value = owned(value.value),
                                                        .label = owned(value.label),
                                                        .description = owned(value.description)});
        });

    parameter.appliesWhenKey = owned(abi.applies_when_key);
    if (abi.applies_when_values != nullptr) {
        parameter.appliesWhenValues.reserve(abi.applies_when_value_count);
        for (std::uint32_t i = 0; i < abi.applies_when_value_count; ++i) {
            parameter.appliesWhenValues.push_back(owned(abi.applies_when_values[i]));
        }
    }

    return parameter;
}

/// A version report, guarded because the fields sit past the first release's
/// end of `sweeppp_sdr_device_info_t`.
VersionReport versionFrom(const sweeppp_sdr_version_t& abi) {
    if (abi.struct_size == 0) {
        return {};
    }
    return VersionReport{.version = owned(abi.version),
                         .knownLatest = owned(abi.known_latest),
                         .aheadOfDriver = abi.ahead_of_driver != 0};
}

void infoFrom(const sweeppp_sdr_device_info_t& abi, SdrDeviceInfo& out) {
    out.id = owned(abi.id);
    out.label = owned(abi.label);
    out.serial = owned(abi.serial);
    out.hardwareRevision = owned(abi.hardware_revision);
    out.minFrequencyHz = abi.min_frequency_hz;
    out.maxFrequencyHz = abi.max_frequency_hz;
    out.minSampleRate = abi.min_sample_rate;
    out.maxSampleRate = abi.max_sample_rate;

    if (SWEEPPP_ABI_HAS(&abi, firmware)) {
        out.firmware = versionFrom(abi.firmware);
    }
    if (SWEEPPP_ABI_HAS(&abi, fpga)) {
        out.fpga = versionFrom(abi.fpga);
    }
    if (SWEEPPP_ABI_HAS(&abi, link_capacity_bytes_per_sec)) {
        out.linkCapacityBytesPerSec = abi.link_capacity_bytes_per_sec;
    }
    if (SWEEPPP_ABI_HAS(&abi, link_description)) {
        out.linkDescription = owned(abi.link_description);
    }
}

} // namespace

// ---- the stream ------------------------------------------------------

BlockRef* StreamContext::resolve(void* handle) noexcept {
    if (handle == nullptr || held.empty()) {
        return nullptr;
    }

    // Arithmetic on the addresses rather than comparing unrelated pointers,
    // and the modulus matters as much as the bound: a handle landing inside an
    // element rather than on one would otherwise be read as a BlockRef at an
    // offset, which is a wild pointer with a plausible-looking value.
    const auto base = reinterpret_cast<std::uintptr_t>(held.data());
    const auto given = reinterpret_cast<std::uintptr_t>(handle);
    if (given < base) {
        return nullptr;
    }

    const std::uintptr_t offset = given - base;
    if (offset % sizeof(BlockRef) != 0) {
        return nullptr;
    }

    const std::size_t index = offset / sizeof(BlockRef);
    if (index >= held.size()) {
        return nullptr;
    }
    return held.data() + index;
}

namespace {

sweeppp_sdr_block_t acquireBlock(void* stream) noexcept {
    sweeppp_sdr_block_t block{};
    block.struct_size = sizeof(block);

    auto* context = static_cast<StreamContext*>(stream);
    if (context == nullptr || context->pool == nullptr) {
        return block;
    }

    BlockRef ref = context->pool->acquire();
    if (!ref.valid()) {
        // Exhausted. A normal answer, not a failure -- the plugin drops and
        // reports rather than waiting, because waiting on a transfer thread
        // turns a host backlog into a device overrun.
        return block;
    }

    const std::uint32_t index = ref.index();
    if (index >= context->held.size()) {
        return block; // ref releases itself; cannot happen with a sane pool
    }

    context->held[index] = std::move(ref);
    const BlockRef& slot = context->held[index];

    block.handle = context->held.data() + index;
    block.data = reinterpret_cast<std::uint8_t*>(slot.data());
    block.capacity_bytes = slot.capacityBytes();
    return block;
}

void publishBlock(void* stream, const sweeppp_sdr_delivery_t* delivery) noexcept {
    auto* context = static_cast<StreamContext*>(stream);
    if (context == nullptr || delivery == nullptr || delivery->struct_size == 0) {
        return;
    }

    BlockRef* slot = context->resolve(delivery->handle);
    if (slot == nullptr || !slot->valid()) {
        logWarn(kLogCategory, "a driver published a block handle the pool never lent it");
        return;
    }

    IqBlock block;
    block.block = std::move(*slot);
    block.format =
        SWEEPPP_ABI_HAS(delivery, format) ? fromAbiFormat(delivery->format) : context->format;

    // Clamped to what the block can actually hold, so a frame count the driver
    // got wrong costs a short block rather than a read past the end of one.
    const std::size_t capacity = block.block.capacityBytes() / bytesPerFrame(block.format);
    block.frames = std::min(delivery->frames, capacity);
    if (block.frames < delivery->frames) {
        logWarn(kLogCategory, "a driver published {} frames into a block holding {}",
                delivery->frames, capacity);
    }

    block.sequence = delivery->sequence;
    block.hostTimeNs = delivery->host_time_ns != 0 ? delivery->host_time_ns : monotonicNs();
    block.deviceTimeNs = delivery->device_time_ns;
    block.centerHz = delivery->center_hz != 0.0 ? delivery->center_hz
                                                : context->centerHz.load(std::memory_order_relaxed);
    block.sampleRate = delivery->sample_rate != 0.0
                           ? delivery->sample_rate
                           : context->sampleRate.load(std::memory_order_relaxed);
    block.sweepStep = delivery->sweep_step;
    block.sweepPass = delivery->sweep_pass;
    block.settling = delivery->settling != 0;

    if (context->callback && block.frames > 0) {
        context->callback(std::move(block));
    }
}

void releaseBlock(void* stream, void* handle) noexcept {
    auto* context = static_cast<StreamContext*>(stream);
    if (context == nullptr) {
        return;
    }
    if (BlockRef* slot = context->resolve(handle); slot != nullptr) {
        slot->reset();
    }
}

void reportDeviceOverrun(void* stream, std::uint64_t samples) noexcept {
    auto* context = static_cast<StreamContext*>(stream);
    if (context == nullptr || context->telemetry == nullptr) {
        return;
    }
    if (StreamCounters* counters = context->telemetry->load(); counters != nullptr) {
        counters->deviceOverruns.fetch_add(1, std::memory_order_relaxed);
        counters->samplesLostAtSource.fetch_add(samples, std::memory_order_relaxed);
    }
}

void reportPoolExhausted(void* stream, std::uint64_t samples) noexcept {
    auto* context = static_cast<StreamContext*>(stream);
    if (context == nullptr || context->telemetry == nullptr) {
        return;
    }
    if (StreamCounters* counters = context->telemetry->load(); counters != nullptr) {
        counters->poolExhaustedEvents.fetch_add(1, std::memory_order_relaxed);
        counters->samplesLostAtSource.fetch_add(samples, std::memory_order_relaxed);
    }
}

} // namespace

// ---- the device ------------------------------------------------------

std::vector<SdrParameter> SdrDevice::readParameters(const sweeppp_sdr_device_vtable_t* vtable,
                                                    void* device) {
    if (!SWEEPPP_ABI_HAS(vtable, parameters) || vtable->parameters == nullptr) {
        return {};
    }

    std::array<sweeppp_sdr_parameter_t, SWEEPPP_SDR_MAX_PARAMETERS> abi{};
    const std::uint32_t count =
        fillArray(abi, "parameters", "a driver",
                  [vtable, device](sweeppp_sdr_parameter_t* out, std::uint32_t capacity) {
                      return vtable->parameters(device, out, capacity);
                  });

    std::vector<SdrParameter> parameters;
    parameters.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (abi[i].struct_size == 0) {
            continue; // never written by the plugin
        }
        parameters.push_back(parameterFrom(abi[i]));
    }
    return parameters;
}

std::vector<SdrRxPort> SdrDevice::readRxPorts(const sweeppp_sdr_device_vtable_t* vtable,
                                              void* device) {
    if (!SWEEPPP_ABI_HAS(vtable, rx_ports) || vtable->rx_ports == nullptr) {
        return {};
    }

    std::array<sweeppp_sdr_rx_port_t, SWEEPPP_SDR_MAX_RX_PORTS> abi{};
    const std::uint32_t count =
        fillArray(abi, "receive ports", "a driver",
                  [vtable, device](sweeppp_sdr_rx_port_t* out, std::uint32_t capacity) {
                      return vtable->rx_ports(device, out, capacity);
                  });

    std::vector<SdrRxPort> ports;
    ports.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (abi[i].struct_size == 0) {
            continue; // never written by the plugin
        }

        // A port with no id is one nothing can be assigned to and the planner
        // cannot name, so it is worth less than none at all.
        std::string id = owned(abi[i].id);
        if (id.empty()) {
            logWarn(kLogCategory, "a driver declares a receive port with no id; ignored");
            continue;
        }

        std::string label = owned(abi[i].label);
        if (label.empty()) {
            label = id;
        }

        ports.push_back(SdrRxPort{.id = std::move(id),
                                  .label = std::move(label),
                                  .connector = owned(abi[i].connector),
                                  .minHz = abi[i].min_hz,
                                  .maxHz = abi[i].max_hz,
                                  .biasTee = abi[i].bias_tee != 0,
                                  .requiresStop = abi[i].requires_stop != 0,
                                  .switchSeconds = abi[i].switch_seconds});
    }
    return ports;
}

void SdrDevice::readInfo(const sweeppp_sdr_device_vtable_t* vtable, void* device,
                         SdrDeviceInfo& out) {
    if (!SWEEPPP_ABI_HAS(vtable, info) || vtable->info == nullptr) {
        return;
    }

    sweeppp_sdr_device_info_t abi{};
    abi.struct_size = sizeof(abi);
    if (vtable->info(device, &abi) == SWEEPPP_PLUGIN_OK && abi.struct_size != 0) {
        infoFrom(abi, out);
    }
}

SdrDevice::SdrDevice(SdrDeviceInfo info, std::vector<SdrParameter> parameters,
                     std::vector<SdrRxPort> ports, const sweeppp_sdr_device_vtable_t* vtable,
                     void* device, std::shared_ptr<std::atomic<int>> liveCount)
    : m_info(std::move(info)), m_vtable(vtable), m_device(device),
      m_liveCount(std::move(liveCount)), m_parameters(std::move(parameters)),
      m_ports(std::move(ports)) {
    m_liveCount->fetch_add(1, std::memory_order_relaxed);

    // Which port the driver came up on, resolved to an index once. After this
    // the host tracks it -- the id is only ever asked for here, so the ABI's
    // "valid until the next call" string never has to outlive this line.
    if (!m_ports.empty() && SWEEPPP_ABI_HAS(m_vtable, selected_rx_port) &&
        m_vtable->selected_rx_port != nullptr) {
        const std::string_view selected = plugin_abi::view(m_vtable->selected_rx_port(m_device));
        const auto match = std::ranges::find_if(
            m_ports, [selected](const SdrRxPort& port) { return port.id == selected; });
        if (match != m_ports.end()) {
            m_selectedPort.store(static_cast<std::size_t>(match - m_ports.begin()),
                                 std::memory_order_relaxed);
        }
    }
}

std::string_view SdrDevice::selectedRxPort() const noexcept {
    const std::size_t index = m_selectedPort.load(std::memory_order_relaxed);
    return index < m_ports.size() ? std::string_view(m_ports[index].id) : std::string_view{};
}

Status SdrDevice::selectRxPort(std::string_view id) {
    const auto match =
        std::ranges::find_if(m_ports, [id](const SdrRxPort& port) { return port.id == id; });
    if (match == m_ports.end()) {
        return fail(ErrorCode::InvalidArgument, "{} has no receive port '{}'", m_info.label, id);
    }
    if (!SWEEPPP_ABI_HAS(m_vtable, select_rx_port) || m_vtable->select_rx_port == nullptr) {
        return fail(ErrorCode::Unavailable,
                    "driver '{}' declares receive ports but cannot select "
                    "between them",
                    m_info.driver);
    }

    const sweeppp_plugin_status_t status =
        m_vtable->select_rx_port(m_device, plugin_abi::borrow(id));
    if (status != SWEEPPP_PLUGIN_OK) {
        return fail(toErrorCode(status), "{} would not select '{}': {}", m_info.label, id,
                    statusName(status));
    }

    m_selectedPort.store(static_cast<std::size_t>(match - m_ports.begin()),
                         std::memory_order_relaxed);
    return ok();
}

SdrDevice::~SdrDevice() {
    SdrDevice::stop();
    if (m_vtable != nullptr && m_vtable->destroy != nullptr) {
        m_vtable->destroy(m_device);
    }
    m_liveCount->fetch_sub(1, std::memory_order_relaxed);
}

Result<SdrValue> SdrDevice::getParameter(std::string_view key) const {
    if (!SWEEPPP_ABI_HAS(m_vtable, get_parameter) || m_vtable->get_parameter == nullptr) {
        return fail<SdrValue>(ErrorCode::Unsupported, "'{}' reports no parameters", m_info.label);
    }

    sweeppp_value_t abi{};
    abi.struct_size = sizeof(abi);
    abi.type = SWEEPPP_VALUE_ABSENT;

    const sweeppp_plugin_status_t status =
        m_vtable->get_parameter(m_device, plugin_abi::borrow(key), &abi);
    if (status != SWEEPPP_PLUGIN_OK) {
        return fail<SdrValue>(toErrorCode(status), "'{}' has no parameter '{}': {}", m_info.label,
                              key, statusName(status));
    }

    auto value = fromAbiValue(abi);
    if (!value) {
        return fail<SdrValue>(ErrorCode::ProtocolError, "'{}' returned no value for '{}'",
                              m_info.label, key);
    }
    return *std::move(value);
}

Status SdrDevice::setParameter(std::string_view key, const SdrValue& value) {
    const auto parameter = std::ranges::find_if(
        m_parameters, [key](const SdrParameter& candidate) { return candidate.key == key; });
    if (parameter == m_parameters.end()) {
        return fail(ErrorCode::NotFound, "no parameter '{}' on '{}'", key, m_info.label);
    }
    if (parameter->readOnly) {
        return fail(ErrorCode::InvalidArgument, "'{}' is read-only", key);
    }
    if (!SWEEPPP_ABI_HAS(m_vtable, set_parameter) || m_vtable->set_parameter == nullptr) {
        return fail(ErrorCode::Unsupported, "'{}' accepts no parameters", m_info.label);
    }

    // Coerced here rather than in the plugin: clamping and snapping to a
    // descriptor the host already holds is one implementation for every driver
    // instead of one per driver, and it is the descriptor the panel drew from.
    const auto coerced = parameter->coerce(value);
    if (!coerced) {
        return std::unexpected(coerced.error());
    }

    sweeppp_value_t abi{};
    toAbiValue(*coerced, abi);

    const sweeppp_plugin_status_t status =
        m_vtable->set_parameter(m_device, plugin_abi::borrow(key), &abi);
    if (status != SWEEPPP_PLUGIN_OK) {
        return fail(toErrorCode(status), "'{}' rejected {} = {}: {}", m_info.label, key,
                    parameter->format(*coerced), statusName(status));
    }
    return ok();
}

SampleFormat SdrDevice::nativeFormat() const noexcept {
    if (!SWEEPPP_ABI_HAS(m_vtable, native_format) || m_vtable->native_format == nullptr) {
        return SampleFormat::Cf32;
    }
    return fromAbiFormat(m_vtable->native_format(m_device));
}

std::vector<double> SdrDevice::supportedSampleRates() const {
    if (!SWEEPPP_ABI_HAS(m_vtable, supported_sample_rates) ||
        m_vtable->supported_sample_rates == nullptr) {
        return ISdrDevice::supportedSampleRates();
    }

    std::array<double, SWEEPPP_SDR_MAX_SAMPLE_RATES> rates{};
    std::uint32_t count =
        m_vtable->supported_sample_rates(m_device, rates.data(), SWEEPPP_SDR_MAX_SAMPLE_RATES);
    if (count > SWEEPPP_SDR_MAX_SAMPLE_RATES) {
        logWarn(kLogCategory, "'{}' reports {} sample rates; taking the first {}", m_info.label,
                count, SWEEPPP_SDR_MAX_SAMPLE_RATES);
        count = SWEEPPP_SDR_MAX_SAMPLE_RATES;
    }
    return std::vector<double>(rates.begin(), rates.begin() + count);
}

Status SdrDevice::start(BlockPool& pool, const StreamConfig& config, IqCallback callback) {
    if (m_streaming.load()) {
        return fail(ErrorCode::AlreadyExists, "'{}' is already streaming", m_info.label);
    }
    if (!SWEEPPP_ABI_HAS(m_vtable, start) || m_vtable->start == nullptr) {
        return fail(ErrorCode::Unsupported, "'{}' cannot deliver samples", m_info.label);
    }

    const SampleFormat format = nativeFormat();

    // A block has to hold whole complex samples, and the pool's block size is
    // whatever the caller sized it for -- so the frame count comes from the
    // pool rather than from the request when the two disagree.
    const std::size_t framesPerBlock =
        std::min(config.framesPerBlock, pool.blockBytes() / bytesPerFrame(format));
    if (framesPerBlock == 0) {
        return fail(ErrorCode::InvalidArgument, "a block of {} bytes holds no {} samples",
                    pool.blockBytes(), toString(format));
    }

    auto context = std::make_unique<StreamContext>();
    context->pool = &pool;
    context->callback = std::move(callback);
    context->telemetry = &m_telemetry;
    context->format = format;
    // One slot per pool block, so a handle is an index into this and never an
    // allocation. Sized once here, never resized while the stream runs -- the
    // plugin holds pointers into it.
    context->held.resize(pool.blockCount());

    if (const auto centre = getParameter("center_hz")) {
        context->centerHz.store(asDouble(*centre), std::memory_order_relaxed);
    }
    if (const auto rate = getParameter("sample_rate")) {
        context->sampleRate.store(asDouble(*rate), std::memory_order_relaxed);
    }

    context->abi.struct_size = sizeof(sweeppp_sdr_stream_t);
    context->abi.stream = context.get();
    context->abi.acquire = &acquireBlock;
    context->abi.publish = &publishBlock;
    context->abi.release = &releaseBlock;
    context->abi.report_device_overrun = &reportDeviceOverrun;
    context->abi.report_pool_exhausted = &reportPoolExhausted;

    sweeppp_sdr_stream_config_t abiConfig{};
    abiConfig.struct_size = sizeof(abiConfig);
    abiConfig.frames_per_block = framesPerBlock;
    abiConfig.block_count = config.blockCount;
    abiConfig.format = toAbiFormat(format);

    // Published before start, so the plugin's first callback already has
    // somewhere to report to.
    m_stream = std::move(context);

    const sweeppp_plugin_status_t status = m_vtable->start(m_device, &abiConfig, &m_stream->abi);
    if (status != SWEEPPP_PLUGIN_OK) {
        m_stream.reset();
        return fail(toErrorCode(status), "'{}' would not start: {}", m_info.label,
                    statusName(status));
    }

    m_streaming.store(true);
    return ok();
}

void SdrDevice::stop() {
    if (m_vtable != nullptr && SWEEPPP_ABI_HAS(m_vtable, stop) && m_vtable->stop != nullptr) {
        // Returns only once the plugin's transfer thread has stopped touching
        // the stream table, which is what makes tearing it down below safe.
        m_vtable->stop(m_device);
    }
    m_streaming.store(false);

    if (m_stream) {
        // Anything the plugin borrowed and neither published nor released.
        // Without this sweep a stop caught mid-transfer would shrink the pool
        // for good, and show up much later as unexplained drops.
        std::size_t stranded = 0;
        for (BlockRef& slot : m_stream->held) {
            if (slot.valid()) {
                slot.reset();
                ++stranded;
            }
        }
        if (stranded > 0) {
            logDebug(kLogCategory, "'{}' left {} block(s) unreturned at stop", m_info.label,
                     stranded);
        }
        m_stream.reset();
    }
}

bool SdrDevice::streaming() const noexcept {
    if (SWEEPPP_ABI_HAS(m_vtable, streaming) && m_vtable->streaming != nullptr) {
        return m_vtable->streaming(m_device) != 0;
    }
    return m_streaming.load();
}

Status SdrDevice::retune(double centerHz) {
    if (!SWEEPPP_ABI_HAS(m_vtable, retune) || m_vtable->retune == nullptr) {
        return fail(ErrorCode::Unsupported, "'{}' cannot retune", m_info.label);
    }
    if (const sweeppp_plugin_status_t status = m_vtable->retune(m_device, centerHz);
        status != SWEEPPP_PLUGIN_OK) {
        return fail(toErrorCode(status), "'{}' refused {} Hz: {}", m_info.label, centerHz,
                    statusName(status));
    }
    if (m_stream) {
        m_stream->centerHz.store(centerHz, std::memory_order_relaxed);
    }
    return ok();
}

double SdrDevice::retuneSettleSeconds() const noexcept {
    if (SWEEPPP_ABI_HAS(m_vtable, retune_settle_seconds) &&
        m_vtable->retune_settle_seconds != nullptr) {
        return m_vtable->retune_settle_seconds(m_device);
    }
    return ISdrDevice::retuneSettleSeconds();
}

double SdrDevice::deliveryGranularitySeconds(double sampleRate) const noexcept {
    if (SWEEPPP_ABI_HAS(m_vtable, delivery_granularity_seconds) &&
        m_vtable->delivery_granularity_seconds != nullptr) {
        return m_vtable->delivery_granularity_seconds(m_device, sampleRate);
    }
    return ISdrDevice::deliveryGranularitySeconds(sampleRate);
}

std::vector<ISdrDevice::HealthReading> SdrDevice::healthReadings() const {
    if (!SWEEPPP_ABI_HAS(m_vtable, health_readings) || m_vtable->health_readings == nullptr) {
        return {};
    }

    std::array<sweeppp_sdr_health_t, SWEEPPP_SDR_MAX_HEALTH_READINGS> abi{};
    const std::uint32_t count =
        fillArray(abi, "health readings", m_info.label,
                  [this](sweeppp_sdr_health_t* out, std::uint32_t capacity) {
                      return m_vtable->health_readings(m_device, out, capacity);
                  });

    std::vector<HealthReading> readings;
    readings.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (abi[i].struct_size == 0) {
            continue;
        }
        readings.push_back(HealthReading{.label = owned(abi[i].label),
                                         .value = owned(abi[i].value),
                                         .numeric = abi[i].numeric,
                                         .minimum = abi[i].minimum,
                                         .maximum = abi[i].maximum,
                                         .alarm = abi[i].alarm != 0});
    }
    return readings;
}

void SdrDevice::attachTelemetry(StreamCounters* counters) noexcept {
    m_telemetry.store(counters);
    if (counters != nullptr) {
        // Published by the host from info(), not carried across the ABI: a
        // plugin driver gets the Performance panel's link-utilisation bar
        // without implementing anything for it.
        counters->linkCapacityBytesPerSec.store(m_info.linkCapacityBytesPerSec,
                                                std::memory_order_relaxed);
    }
}

// ---- the factory -----------------------------------------------------

std::string validateSdrFactory(const sweeppp_sdr_factory_vtable_t* vtable) {
    if (vtable == nullptr || vtable->struct_size == 0) {
        return "the SDR facet has no vtable";
    }
    if (!SWEEPPP_ABI_HAS(vtable, open) || vtable->open == nullptr) {
        return "the SDR facet cannot open a device";
    }
    // `enumerate` may be null: a driver whose devices are named rather than
    // discovered -- a file, a network radio -- has nothing to list, and the
    // built-in iqfile driver is exactly that shape.
    return {};
}

SdrFactory::SdrFactory(std::string driver, std::string displayName,
                       const sweeppp_sdr_factory_vtable_t* vtable, void* instance)
    : m_driver(std::move(driver)), m_displayName(std::move(displayName)), m_vtable(vtable),
      m_instance(instance) {
}

std::vector<SdrDeviceInfo> SdrFactory::enumerate() const {
    if (m_vtable->enumerate == nullptr) {
        return {};
    }

    std::array<sweeppp_sdr_device_info_t, kMaxEnumeratedDevices> abi{};
    const std::uint32_t count = fillArray(
        abi, "devices", m_driver, [this](sweeppp_sdr_device_info_t* out, std::uint32_t capacity) {
            return m_vtable->enumerate(m_instance, out, capacity);
        });

    std::vector<SdrDeviceInfo> devices;
    devices.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (abi[i].struct_size == 0) {
            continue;
        }
        SdrDeviceInfo info{.driver = m_driver};
        infoFrom(abi[i], info);
        devices.push_back(std::move(info));
    }
    return devices;
}

Result<std::unique_ptr<ISdrDevice>> SdrFactory::open(std::string_view id) {
    if (m_vtable->open == nullptr) {
        return fail<std::unique_ptr<ISdrDevice>>(ErrorCode::Unsupported,
                                                 "driver '{}' cannot open devices", m_driver);
    }

    void* device = nullptr;
    const sweeppp_sdr_device_vtable_t* deviceVtable = nullptr;
    sweeppp_str_t error{};

    const sweeppp_plugin_status_t status =
        m_vtable->open(m_instance, plugin_abi::borrow(id), &device, &deviceVtable, &error);
    if (status != SWEEPPP_PLUGIN_OK) {
        // The driver's own sentence where it wrote one. ERR_DEVICE alone is a
        // mystery; "LIBUSB_ERROR_ACCESS (usually a missing udev rule)" is
        // something an operator can act on.
        const std::string_view detail = plugin_abi::view(error);
        return fail<std::unique_ptr<ISdrDevice>>(toErrorCode(status), "{}:{} would not open: {}",
                                                 m_driver, id,
                                                 detail.empty() ? statusName(status) : detail);
    }
    if (deviceVtable == nullptr || deviceVtable->struct_size == 0) {
        return fail<std::unique_ptr<ISdrDevice>>(
            ErrorCode::ProtocolError, "driver '{}' opened a device with no vtable", m_driver);
    }

    // Identity from the open device rather than from a second enumeration.
    // Firmware, FPGA and link speed need a handle, so the rescan the previous
    // shape did -- a whole USB bus walk on every open -- could never have
    // filled them in anyway.
    SdrDeviceInfo info{.driver = m_driver, .id = std::string(id)};
    SdrDevice::readInfo(deviceVtable, device, info);
    if (info.label.empty()) {
        info.label = m_displayName;
    }
    if (info.id.empty()) {
        info.id = std::string(id);
    }

    return std::make_unique<SdrDevice>(
        std::move(info), SdrDevice::readParameters(deviceVtable, device),
        SdrDevice::readRxPorts(deviceVtable, device), deviceVtable, device, m_liveDevices);
}

std::string SdrFactory::withdrawalBlocker() const {
    const int live = m_liveDevices->load(std::memory_order_relaxed);
    if (live <= 0) {
        return {};
    }
    return std::format("{} device{} from this driver {} still open", live, live == 1 ? "" : "s",
                       live == 1 ? "is" : "are");
}

// ---- RF path ---------------------------------------------------------

namespace {

void rfPathInfoFrom(const sweeppp_rf_path_info_t& abi, RfPathInfo& out) {
    out.id = owned(abi.id);
    out.label = owned(abi.label);
    out.model = owned(abi.model);
    out.serial = owned(abi.serial);
    out.inputCount = abi.input_count;
    out.requiresStop = abi.requires_stop != 0;
    out.switchSeconds = abi.switch_seconds;
}

} // namespace

std::vector<RfPathInput> RfPath::readInputs(const sweeppp_rf_path_vtable_t* vtable, void* path) {
    if (!SWEEPPP_ABI_HAS(vtable, inputs) || vtable->inputs == nullptr) {
        return {};
    }

    std::array<sweeppp_rf_path_input_t, SWEEPPP_RF_PATH_MAX_INPUTS> abi{};
    const std::uint32_t count =
        fillArray(abi, "inputs", "a switcher",
                  [vtable, path](sweeppp_rf_path_input_t* out, std::uint32_t capacity) {
                      return vtable->inputs(path, out, capacity);
                  });

    std::vector<RfPathInput> inputs;
    inputs.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (abi[i].struct_size == 0) {
            continue;
        }

        std::string id = owned(abi[i].id);
        if (id.empty()) {
            // An input nothing can be assigned to is worth less than none at
            // all: the chain the planner walks is keyed on this string.
            logWarn(kLogCategory, "a switcher declares an input with no id; ignored");
            continue;
        }

        std::string label = owned(abi[i].label);
        if (label.empty()) {
            label = id;
        }

        inputs.push_back(RfPathInput{.id = std::move(id),
                                     .label = std::move(label),
                                     .minHz = abi[i].min_hz,
                                     .maxHz = abi[i].max_hz});
    }
    return inputs;
}

void RfPath::readInfo(const sweeppp_rf_path_vtable_t* vtable, void* path, RfPathInfo& out) {
    if (!SWEEPPP_ABI_HAS(vtable, info) || vtable->info == nullptr) {
        return;
    }

    sweeppp_rf_path_info_t abi{};
    abi.struct_size = sizeof(abi);
    if (vtable->info(path, &abi) == SWEEPPP_PLUGIN_OK && abi.struct_size != 0) {
        rfPathInfoFrom(abi, out);
    }
}

RfPath::RfPath(RfPathInfo info, std::vector<RfPathInput> inputs,
               const sweeppp_rf_path_vtable_t* vtable, void* path,
               std::shared_ptr<std::atomic<int>> liveCount)
    : m_info(std::move(info)), m_inputs(std::move(inputs)), m_vtable(vtable), m_path(path),
      m_liveCount(std::move(liveCount)) {
    m_liveCount->fetch_add(1, std::memory_order_relaxed);

    if (SWEEPPP_ABI_HAS(m_vtable, selected_input) && m_vtable->selected_input != nullptr) {
        m_selected.store(m_vtable->selected_input(m_path), std::memory_order_relaxed);
    }
}

RfPath::~RfPath() {
    if (m_vtable != nullptr && m_vtable->destroy != nullptr) {
        m_vtable->destroy(m_path);
    }
    m_liveCount->fetch_sub(1, std::memory_order_relaxed);
}

Status RfPath::selectInput(std::uint32_t index) {
    if (index >= m_inputs.size()) {
        return fail(ErrorCode::InvalidArgument, "{} has no input {}", m_info.label, index);
    }
    if (!SWEEPPP_ABI_HAS(m_vtable, select_input) || m_vtable->select_input == nullptr) {
        return fail(ErrorCode::Unavailable, "{} cannot select between its inputs", m_info.label);
    }

    const sweeppp_plugin_status_t status = m_vtable->select_input(m_path, index);
    if (status != SWEEPPP_PLUGIN_OK) {
        return fail(toErrorCode(status), "{} would not select input {}: {}", m_info.label, index,
                    statusName(status));
    }

    m_selected.store(index, std::memory_order_relaxed);
    return ok();
}

std::uint32_t RfPath::selectedInput() const noexcept {
    return m_selected.load(std::memory_order_relaxed);
}

std::vector<IRfPath::HealthReading> RfPath::healthReadings() const {
    if (!SWEEPPP_ABI_HAS(m_vtable, health_readings) || m_vtable->health_readings == nullptr) {
        return {};
    }

    std::array<sweeppp_sdr_health_t, SWEEPPP_SDR_MAX_HEALTH_READINGS> abi{};
    const std::uint32_t count =
        fillArray(abi, "health readings", m_info.label,
                  [this](sweeppp_sdr_health_t* out, std::uint32_t capacity) {
                      return m_vtable->health_readings(m_path, out, capacity);
                  });

    std::vector<HealthReading> readings;
    readings.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (abi[i].struct_size == 0) {
            continue;
        }
        readings.push_back(HealthReading{.label = owned(abi[i].label),
                                         .value = owned(abi[i].value),
                                         .numeric = abi[i].numeric,
                                         .minimum = abi[i].minimum,
                                         .maximum = abi[i].maximum,
                                         .alarm = abi[i].alarm != 0});
    }
    return readings;
}

std::string validateRfPathFactory(const sweeppp_rf_path_factory_vtable_t* vtable) {
    if (vtable == nullptr || vtable->struct_size == 0) {
        return "the RF path facet has no vtable";
    }
    if (!SWEEPPP_ABI_HAS(vtable, open) || vtable->open == nullptr) {
        return "the RF path facet cannot open a switcher";
    }
    return {};
}

RfPathFactory::RfPathFactory(std::string driver, std::string displayName,
                             const sweeppp_rf_path_factory_vtable_t* vtable, void* instance)
    : m_driver(std::move(driver)), m_displayName(std::move(displayName)), m_vtable(vtable),
      m_instance(instance) {
}

std::vector<RfPathInfo> RfPathFactory::enumerate() const {
    if (!SWEEPPP_ABI_HAS(m_vtable, enumerate) || m_vtable->enumerate == nullptr) {
        return {};
    }

    std::array<sweeppp_rf_path_info_t, kMaxEnumeratedDevices> abi{};
    const std::uint32_t count = fillArray(
        abi, "switchers", m_driver, [this](sweeppp_rf_path_info_t* out, std::uint32_t capacity) {
            return m_vtable->enumerate(m_instance, out, capacity);
        });

    std::vector<RfPathInfo> paths;
    paths.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (abi[i].struct_size == 0) {
            continue;
        }
        RfPathInfo info{.driver = m_driver};
        rfPathInfoFrom(abi[i], info);
        paths.push_back(std::move(info));
    }
    return paths;
}

Result<std::unique_ptr<IRfPath>> RfPathFactory::open(std::string_view id) {
    void* path = nullptr;
    const sweeppp_rf_path_vtable_t* pathVtable = nullptr;
    sweeppp_str_t error{};

    const sweeppp_plugin_status_t status =
        m_vtable->open(m_instance, plugin_abi::borrow(id), &path, &pathVtable, &error);
    if (status != SWEEPPP_PLUGIN_OK) {
        const std::string detail = owned(error);
        return fail<std::unique_ptr<IRfPath>>(toErrorCode(status), "{}:{} would not open: {}",
                                              m_driver, id,
                                              detail.empty() ? statusName(status) : detail);
    }
    if (pathVtable == nullptr || pathVtable->struct_size == 0) {
        return fail<std::unique_ptr<IRfPath>>(
            ErrorCode::ProtocolError, "driver '{}' opened a switcher with no vtable", m_driver);
    }

    RfPathInfo info{.driver = m_driver, .id = std::string(id)};
    RfPath::readInfo(pathVtable, path, info);
    if (info.label.empty()) {
        info.label = m_displayName;
    }
    if (info.id.empty()) {
        info.id = std::string(id);
    }

    std::vector<RfPathInput> inputs = RfPath::readInputs(pathVtable, path);
    if (info.inputCount == 0) {
        info.inputCount = static_cast<std::uint32_t>(inputs.size());
    }

    return std::make_unique<RfPath>(std::move(info), std::move(inputs), pathVtable, path,
                                    m_livePaths);
}

std::string RfPathFactory::withdrawalBlocker() const {
    const int live = m_livePaths->load(std::memory_order_relaxed);
    if (live <= 0) {
        return {};
    }
    return std::format("{} switcher{} from this driver {} still open", live, live == 1 ? "" : "s",
                       live == 1 ? "is" : "are");
}

} // namespace sweeppp::plugin_facets
