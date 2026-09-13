// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/// Writing a Sweep++ SDR driver as an ordinary C++ class.
///
/// `Radio` mirrors `ISdrDevice` member for member and `Driver` mirrors
/// `ISdrDeviceFactory`, so a driver that was built into the application becomes
/// a plugin by changing its base class and its two delivery calls. Everything
/// else -- every `SdrParameter` literal, `coerce`, the settle times, the health
/// readings -- is untouched.
///
/// Unlike `Plugin.hpp` this header DOES include libsweeppp's SDR value headers,
/// which the one rule permits: `SampleFormat`, `SdrDeviceInfo` and
/// `SdrParameter` are pure computation over their arguments, with no
/// per-image state behind them. It still touches no singleton.
///
/// The shape of a driver:
///
///     class MyRadio : public sweeppp::plugin::Radio { ... };
///     class MyDriver : public sweeppp::plugin::Driver { ... };
///
///     MyDriver& driver() { static MyDriver instance; return instance; }
///     const auto kVtable = sweeppp::plugin::makeSdrDriverVtable<MyDriver>();
///     // facet(SWEEPPP_FACET_SDR_DEVICE, "myradio", ..., &kVtable)
///     // host.registerFacet(facet, &driver());

#include "sweeppp/core/Result.hpp"
#include "sweeppp/plugin/Plugin.hpp"
#include "sweeppp/plugin/PluginStatus.hpp"
#include "sweeppp/sdr/SampleFormat.hpp"
#include "sweeppp/sdr/SdrDeviceInfo.hpp"
#include "sweeppp/sdr/SdrParameter.hpp"
#include "sweeppp/sdr/SdrRxPort.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace sweeppp::plugin {

// ----------------------------------------------------------- conversions

/// Switches rather than casts, although the numbering is deliberately
/// identical. An enumerator added to one enum and not the other is then a
/// compile error here, rather than a buffer read at the wrong stride later.
[[nodiscard]] constexpr sweeppp_sdr_format_t toAbiFormat(SampleFormat format) noexcept {
    switch (format) {
    case SampleFormat::Cu8:
        return SWEEPPP_SDR_FORMAT_CU8;
    case SampleFormat::Cs8:
        return SWEEPPP_SDR_FORMAT_CS8;
    case SampleFormat::Cu12:
        return SWEEPPP_SDR_FORMAT_CU12;
    case SampleFormat::Cs12:
        return SWEEPPP_SDR_FORMAT_CS12;
    case SampleFormat::Cu16:
        return SWEEPPP_SDR_FORMAT_CU16;
    case SampleFormat::Cs16:
        return SWEEPPP_SDR_FORMAT_CS16;
    case SampleFormat::Cu32:
        return SWEEPPP_SDR_FORMAT_CU32;
    case SampleFormat::Cs32:
        return SWEEPPP_SDR_FORMAT_CS32;
    case SampleFormat::Cf16:
        return SWEEPPP_SDR_FORMAT_CF16;
    case SampleFormat::Cf32:
        return SWEEPPP_SDR_FORMAT_CF32;
    case SampleFormat::Cf64:
        return SWEEPPP_SDR_FORMAT_CF64;
    }
    return SWEEPPP_SDR_FORMAT_CS8;
}

[[nodiscard]] constexpr SampleFormat fromAbiFormat(sweeppp_sdr_format_t format) noexcept {
    switch (format) {
    case SWEEPPP_SDR_FORMAT_CU8:
        return SampleFormat::Cu8;
    case SWEEPPP_SDR_FORMAT_CS8:
        return SampleFormat::Cs8;
    case SWEEPPP_SDR_FORMAT_CU12:
        return SampleFormat::Cu12;
    case SWEEPPP_SDR_FORMAT_CS12:
        return SampleFormat::Cs12;
    case SWEEPPP_SDR_FORMAT_CU16:
        return SampleFormat::Cu16;
    case SWEEPPP_SDR_FORMAT_CS16:
        return SampleFormat::Cs16;
    case SWEEPPP_SDR_FORMAT_CU32:
        return SampleFormat::Cu32;
    case SWEEPPP_SDR_FORMAT_CS32:
        return SampleFormat::Cs32;
    case SWEEPPP_SDR_FORMAT_CF16:
        return SampleFormat::Cf16;
    case SWEEPPP_SDR_FORMAT_CF32:
        return SampleFormat::Cf32;
    case SWEEPPP_SDR_FORMAT_CF64:
        return SampleFormat::Cf64;
    case SWEEPPP_SDR_FORMAT_FORCE_INT32:
        break;
    }
    return SampleFormat::Cs8;
}

[[nodiscard]] constexpr sweeppp_sdr_parameter_type_t
toAbiParameterType(SdrParameterType type) noexcept {
    switch (type) {
    case SdrParameterType::Bool:
        return SWEEPPP_SDR_PARAM_BOOL;
    case SdrParameterType::Int:
        return SWEEPPP_SDR_PARAM_INT;
    case SdrParameterType::Double:
        return SWEEPPP_SDR_PARAM_DOUBLE;
    case SdrParameterType::Enum:
        return SWEEPPP_SDR_PARAM_ENUM;
    case SdrParameterType::String:
        return SWEEPPP_SDR_PARAM_STRING;
    }
    return SWEEPPP_SDR_PARAM_DOUBLE;
}

// ------------------------------------------------------------ interfaces

/// A radio, as a plugin implements it. Mirrors `ISdrDevice` member for member.
///
/// The one difference: `start` is handed a `Stream` rather than a pool and a
/// callback, because the pool lives on the other side of the ABI. Fill a block
/// borrowed from it and publish; nothing converts and nothing is copied twice.
///
/// There is no `readSamples`. `ISdrDevice` already synthesises pull mode from
/// the push stream on the host's side, so a driver gets it by implementing
/// none of it.
class Radio {
public:
    virtual ~Radio() = default;

    Radio(const Radio&) = delete;
    Radio& operator=(const Radio&) = delete;

    [[nodiscard]] virtual const SdrDeviceInfo& info() const noexcept = 0;

    /// Every parameter this device exposes. The UI panel is generated from
    /// this alone -- no per-device UI code exists anywhere -- so it has to be
    /// complete enough to build a correct widget from.
    ///
    /// The returned span, and every string reachable from it, must stay valid
    /// until the next call to this or until the radio is destroyed. That is
    /// this ABI's one exception to static storage, and it exists because a
    /// real radio needs it: a bladeRF's gain modes are read from the hardware
    /// at open, so no static table could express them.
    [[nodiscard]] virtual std::span<const SdrParameter> parameters() const noexcept = 0;

    [[nodiscard]] virtual Result<SdrValue> getParameter(std::string_view key) const = 0;

    /// The host has already coerced `value` against this parameter's own
    /// descriptor, so it arrives clamped and snapped to what `parameters()`
    /// declared.
    [[nodiscard]] virtual Status setParameter(std::string_view key, const SdrValue& value) = 0;

    [[nodiscard]] virtual SampleFormat nativeFormat() const noexcept = 0;

    /// Rates the hardware actually supports. Empty means continuous within the
    /// range `info()` reports.
    [[nodiscard]] virtual std::vector<double> supportedSampleRates() const { return {}; }

    [[nodiscard]] virtual Status start(Stream& stream, const StreamConfig& config) = 0;

    /// Must not return until nothing of this driver's is still touching the
    /// stream: the host tears it down as soon as this returns.
    virtual void stop() = 0;

    [[nodiscard]] virtual bool streaming() const noexcept = 0;

    /// Retunes while streaming. Separate from `setParameter("center_hz")`
    /// because the sweep engine calls it thousands of times a second and needs
    /// the fastest path there is.
    [[nodiscard]] virtual Status retune(double centerHz) = 0;

    [[nodiscard]] virtual double retuneSettleSeconds() const noexcept { return 0.0; }

    /// How much signal time arrives in one delivery -- the hard floor on how
    /// fast a sweep can retune. Zero means this device does not constrain it.
    [[nodiscard]] virtual double deliveryGranularitySeconds(double sampleRate) const noexcept {
        (void)sampleRate;
        return 0.0;
    }

    /// Called at a few Hz, never per frame: on a USB radio each of these is a
    /// control transfer competing with the sample stream for the same bus.
    [[nodiscard]] virtual std::vector<SdrHealthReading> healthReadings() const { return {}; }

    /// The RF inputs this radio has. Empty -- the default -- means one implicit
    /// input, which is what a single-connector radio wants: implement none of
    /// these three and the host shows no port chooser at all.
    ///
    /// Same lifetime rule as `parameters()`: the span and everything reachable
    /// from it stay valid until the next call to this one.
    [[nodiscard]] virtual std::span<const SdrRxPort> rxPorts() const noexcept { return {}; }

    [[nodiscard]] virtual std::string_view selectedRxPort() const noexcept { return {}; }

    /// A port declaring `requiresStop` has had its stream stopped by the host
    /// before this is called.
    [[nodiscard]] virtual Status selectRxPort(std::string_view id) {
        return fail(ErrorCode::Unavailable, "this radio has one receive port, not '{}'", id);
    }

protected:
    Radio() = default;
};

/// One driver's worth of radios. Mirrors `ISdrDeviceFactory`.
class Driver {
public:
    virtual ~Driver() = default;

    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;

    /// Radios attached now. Called on a timer so hot-plug shows up without the
    /// operator hitting refresh, which means it has to be cheap.
    [[nodiscard]] virtual std::vector<SdrDeviceInfo> enumerate() const = 0;

    /// Opens one; an empty `id` means the first available. The error's message
    /// reaches the operator, so it is worth writing the sentence they can act
    /// on rather than the status code they cannot.
    [[nodiscard]] virtual Result<std::unique_ptr<Radio>> open(std::string_view id) = 0;

protected:
    Driver() = default;
};

// ---------------------------------------------------------------- thunks

namespace detail {

/// What `void* device` actually points at.
///
/// The radio, plus somewhere to park the ABI structs its answers point into.
/// Per device rather than thread-local because that is exactly the lifetime
/// the header promises: valid until the next such call on this device, or
/// until it is destroyed.
struct RadioHolder {
    std::unique_ptr<Radio> radio;
    Stream stream;

    /// Rebuilt by each `parameters` call. The strings are NOT copied -- they
    /// point into the radio's own `SdrParameter`s, which it keeps alive -- but
    /// the ABI arrays have no counterpart there and are built here.
    std::vector<sweeppp_sdr_enum_value_t> enumValues;
    std::vector<sweeppp_str_t> appliesWhenValues;

    /// Returned by value from the radio, so these have to be kept.
    std::vector<SdrHealthReading> health;
    std::vector<double> sampleRates;
    std::string parameterText;
};

[[nodiscard]] inline RadioHolder* holderOf(void* device) noexcept {
    return static_cast<RadioHolder*>(device);
}

/// Scratch for the factory's answers, whose documented lifetime is "until the
/// next call into this factory from this thread".
struct FactoryScratch {
    std::vector<SdrDeviceInfo> devices;
    std::string error;
};

[[nodiscard]] inline FactoryScratch& factoryScratch() noexcept {
    thread_local FactoryScratch scratch;
    return scratch;
}

inline void fillVersion(const VersionReport& from, sweeppp_sdr_version_t& out) noexcept {
    out.struct_size = sizeof(sweeppp_sdr_version_t);
    out.version = str(from.version);
    out.known_latest = str(from.knownLatest);
    out.ahead_of_driver = from.aheadOfDriver ? 1 : 0;
}

/// Every string points into `from`, which the caller keeps alive.
inline void fillInfo(const SdrDeviceInfo& from, sweeppp_sdr_device_info_t& out) noexcept {
    out.struct_size = sizeof(sweeppp_sdr_device_info_t);
    out.id = str(from.id);
    out.label = str(from.label);
    out.serial = str(from.serial);
    out.hardware_revision = str(from.hardwareRevision);
    out.min_frequency_hz = from.minFrequencyHz;
    out.max_frequency_hz = from.maxFrequencyHz;
    out.min_sample_rate = from.minSampleRate;
    out.max_sample_rate = from.maxSampleRate;
    fillVersion(from.firmware, out.firmware);
    fillVersion(from.fpga, out.fpga);
    out.link_capacity_bytes_per_sec = from.linkCapacityBytesPerSec;
    out.link_description = str(from.linkDescription);
}

inline void fillValue(const SdrValue& from, sweeppp_value_t& out) noexcept {
    out = sweeppp_value_t{};
    out.struct_size = sizeof(sweeppp_value_t);
    if (const auto* held = std::get_if<bool>(&from)) {
        out.type = SWEEPPP_VALUE_BOOL;
        out.boolean = *held ? 1 : 0;
    } else if (const auto* number = std::get_if<std::int64_t>(&from)) {
        out.type = SWEEPPP_VALUE_INT;
        out.integer = *number;
    } else if (const auto* real = std::get_if<double>(&from)) {
        out.type = SWEEPPP_VALUE_FLOAT;
        out.number = *real;
    } else if (const auto* text = std::get_if<std::string>(&from)) {
        out.type = SWEEPPP_VALUE_STRING;
        out.text = str(*text);
    }
}

[[nodiscard]] inline SdrValue valueFrom(const sweeppp_value_t& from) {
    switch (from.type) {
    case SWEEPPP_VALUE_BOOL:
        return SdrValue{from.boolean != 0};
    case SWEEPPP_VALUE_INT:
        return SdrValue{from.integer};
    case SWEEPPP_VALUE_FLOAT:
        return SdrValue{from.number};
    case SWEEPPP_VALUE_STRING:
        return SdrValue{std::string(view(from.text))};
    case SWEEPPP_VALUE_ABSENT:
    case SWEEPPP_VALUE_TYPE_FORCE_INT32:
        break;
    }
    return SdrValue{std::int64_t{0}};
}

} // namespace detail

/// The one device vtable, shared by every driver written against `Radio`.
///
/// One table rather than one per driver because every entry dispatches through
/// the base class anyway; the `void*` is a `RadioHolder`, not the radio itself.
[[nodiscard]] const sweeppp_sdr_device_vtable_t& sdrDeviceVtable() noexcept;

namespace detail {

inline void destroyThunk(void* device) {
    guard([device] { delete holderOf(device); });
}

inline sweeppp_plugin_status_t infoThunk(void* device, sweeppp_sdr_device_info_t* out) {
    auto status = SWEEPPP_PLUGIN_ERR_UNKNOWN;
    guard([device, out, &status] {
        if (out == nullptr) {
            status = SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
            return;
        }
        fillInfo(holderOf(device)->radio->info(), *out);
        status = SWEEPPP_PLUGIN_OK;
    });
    return status;
}

inline std::uint32_t parametersThunk(void* device, sweeppp_sdr_parameter_t* out,
                                     std::uint32_t capacity) {
    std::uint32_t total = 0;
    guard([device, out, capacity, &total] {
        RadioHolder* holder = holderOf(device);
        const std::span<const SdrParameter> parameters = holder->radio->parameters();
        total = static_cast<std::uint32_t>(parameters.size());

        if (out == nullptr || capacity < total) {
            return; // the host sizes from the return value and asks again
        }

        // Two passes over the side arrays. The ABI structs hold pointers into
        // these vectors, so nothing may point at them until they have stopped
        // growing -- indices first, addresses second.
        holder->enumValues.clear();
        holder->appliesWhenValues.clear();
        for (const SdrParameter& parameter : parameters) {
            for (const SdrEnumValue& value : parameter.enumValues) {
                holder->enumValues.push_back(
                    sweeppp_sdr_enum_value_t{.struct_size = sizeof(sweeppp_sdr_enum_value_t),
                                             .value = str(value.value),
                                             .label = str(value.label),
                                             .description = str(value.description)});
            }
            for (const std::string& value : parameter.appliesWhenValues) {
                holder->appliesWhenValues.push_back(str(value));
            }
        }

        std::size_t enumAt = 0;
        std::size_t appliesAt = 0;
        for (std::uint32_t i = 0; i < total; ++i) {
            const SdrParameter& parameter = parameters[i];

            out[i] = sweeppp_sdr_parameter_t{};
            out[i].struct_size = sizeof(sweeppp_sdr_parameter_t);
            out[i].key = str(parameter.key);
            out[i].label = str(parameter.label);
            out[i].group = str(parameter.group);
            out[i].type = toAbiParameterType(parameter.type);
            out[i].unit = str(parameter.unit);
            out[i].minimum = parameter.min;
            out[i].maximum = parameter.max;
            out[i].step = parameter.step;
            out[i].read_only = parameter.readOnly ? 1 : 0;
            out[i].grid_affecting = parameter.gridAffecting ? 1 : 0;
            out[i].calibration_affecting = parameter.calibrationAffecting ? 1 : 0;
            out[i].requires_stop = parameter.requiresStop ? 1 : 0;
            out[i].description = str(parameter.description);
            fillValue(parameter.defaultValue, out[i].default_value);

            if (!parameter.enumValues.empty()) {
                out[i].enum_values = holder->enumValues.data() + enumAt;
                out[i].enum_value_count = static_cast<std::uint32_t>(parameter.enumValues.size());
                enumAt += parameter.enumValues.size();
            }

            out[i].applies_when_key = str(parameter.appliesWhenKey);
            if (!parameter.appliesWhenValues.empty()) {
                out[i].applies_when_values = holder->appliesWhenValues.data() + appliesAt;
                out[i].applies_when_value_count =
                    static_cast<std::uint32_t>(parameter.appliesWhenValues.size());
                appliesAt += parameter.appliesWhenValues.size();
            }
        }
    });
    return total;
}

inline sweeppp_plugin_status_t getParameterThunk(void* device, sweeppp_str_t key,
                                                 sweeppp_value_t* out) {
    auto status = SWEEPPP_PLUGIN_ERR_UNKNOWN;
    guard([device, key, out, &status] {
        if (out == nullptr) {
            status = SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
            return;
        }
        RadioHolder* holder = holderOf(device);
        const Result<SdrValue> value = holder->radio->getParameter(view(key));
        if (!value) {
            status = toAbiStatus(value.error().code());
            return;
        }
        // A string alternative would die with the Result, so it is parked --
        // valid until the next call into this device, which is what the ABI
        // promises.
        if (const auto* text = std::get_if<std::string>(&*value)) {
            holder->parameterText = *text;
            *out = sweeppp_value_t{};
            out->struct_size = sizeof(sweeppp_value_t);
            out->type = SWEEPPP_VALUE_STRING;
            out->text = str(holder->parameterText);
        } else {
            fillValue(*value, *out);
        }
        status = SWEEPPP_PLUGIN_OK;
    });
    return status;
}

inline sweeppp_plugin_status_t setParameterThunk(void* device, sweeppp_str_t key,
                                                 const sweeppp_value_t* value) {
    auto status = SWEEPPP_PLUGIN_ERR_UNKNOWN;
    guard([device, key, value, &status] {
        if (value == nullptr) {
            status = SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
            return;
        }
        const Status applied = holderOf(device)->radio->setParameter(view(key), valueFrom(*value));
        status = applied ? SWEEPPP_PLUGIN_OK : toAbiStatus(applied.error().code());
    });
    return status;
}

inline sweeppp_sdr_format_t nativeFormatThunk(void* device) {
    auto format = SWEEPPP_SDR_FORMAT_CS8;
    guard([device, &format] { format = toAbiFormat(holderOf(device)->radio->nativeFormat()); });
    return format;
}

inline std::uint32_t sampleRatesThunk(void* device, double* out, std::uint32_t capacity) {
    std::uint32_t total = 0;
    guard([device, out, capacity, &total] {
        RadioHolder* holder = holderOf(device);
        holder->sampleRates = holder->radio->supportedSampleRates();
        total = static_cast<std::uint32_t>(holder->sampleRates.size());
        if (out != nullptr) {
            const std::uint32_t taken = std::min(total, capacity);
            std::copy_n(holder->sampleRates.begin(), taken, out);
        }
    });
    return total;
}

inline sweeppp_plugin_status_t startThunk(void* device, const sweeppp_sdr_stream_config_t* config,
                                          const sweeppp_sdr_stream_t* stream) {
    auto status = SWEEPPP_PLUGIN_ERR_UNKNOWN;
    guard([device, config, stream, &status] {
        if (config == nullptr || stream == nullptr) {
            status = SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
            return;
        }
        RadioHolder* holder = holderOf(device);
        holder->stream = Stream(stream);

        const StreamConfig wanted{.framesPerBlock = config->frames_per_block,
                                  .blockCount = config->block_count,
                                  .format = fromAbiFormat(config->format)};
        const Status started = holder->radio->start(holder->stream, wanted);
        status = started ? SWEEPPP_PLUGIN_OK : toAbiStatus(started.error().code());
    });
    return status;
}

inline void stopThunk(void* device) {
    guard([device] { holderOf(device)->radio->stop(); });
}

inline std::int32_t streamingThunk(void* device) {
    std::int32_t running = 0;
    guard([device, &running] { running = holderOf(device)->radio->streaming() ? 1 : 0; });
    return running;
}

inline sweeppp_plugin_status_t retuneThunk(void* device, double centerHz) {
    auto status = SWEEPPP_PLUGIN_ERR_UNKNOWN;
    guard([device, centerHz, &status] {
        const Status tuned = holderOf(device)->radio->retune(centerHz);
        status = tuned ? SWEEPPP_PLUGIN_OK : toAbiStatus(tuned.error().code());
    });
    return status;
}

inline double settleThunk(void* device) {
    double seconds = 0.0;
    guard([device, &seconds] { seconds = holderOf(device)->radio->retuneSettleSeconds(); });
    return seconds;
}

inline double granularityThunk(void* device, double sampleRate) {
    double seconds = 0.0;
    guard([device, sampleRate, &seconds] {
        seconds = holderOf(device)->radio->deliveryGranularitySeconds(sampleRate);
    });
    return seconds;
}

inline std::uint32_t healthThunk(void* device, sweeppp_sdr_health_t* out, std::uint32_t capacity) {
    std::uint32_t total = 0;
    guard([device, out, capacity, &total] {
        RadioHolder* holder = holderOf(device);
        // Kept, because the radio hands these back by value and the strings
        // would otherwise die with the temporary.
        holder->health = holder->radio->healthReadings();
        total = static_cast<std::uint32_t>(holder->health.size());

        if (out == nullptr || capacity < total) {
            return;
        }
        for (std::uint32_t i = 0; i < total; ++i) {
            out[i] = sweeppp_sdr_health_t{.struct_size = sizeof(sweeppp_sdr_health_t),
                                          .label = str(holder->health[i].label),
                                          .value = str(holder->health[i].value),
                                          .numeric = holder->health[i].numeric,
                                          .minimum = holder->health[i].minimum,
                                          .maximum = holder->health[i].maximum,
                                          .alarm = holder->health[i].alarm ? 1 : 0};
        }
    });
    return total;
}

inline std::uint32_t rxPortsThunk(void* device, sweeppp_sdr_rx_port_t* out,
                                  std::uint32_t capacity) {
    std::uint32_t total = 0;
    guard([device, out, capacity, &total] {
        // Not kept in the holder: the radio owns these for at least as long as
        // it promises the span, which is exactly the lifetime the ABI hands the
        // host. Copying them would be a second answer to keep in step.
        const std::span<const SdrRxPort> ports = holderOf(device)->radio->rxPorts();
        total = static_cast<std::uint32_t>(ports.size());

        if (out == nullptr || capacity < total) {
            return;
        }
        for (std::uint32_t i = 0; i < total; ++i) {
            out[i] = sweeppp_sdr_rx_port_t{.struct_size = sizeof(sweeppp_sdr_rx_port_t),
                                           .id = str(ports[i].id),
                                           .label = str(ports[i].label),
                                           .connector = str(ports[i].connector),
                                           .min_hz = ports[i].minHz,
                                           .max_hz = ports[i].maxHz,
                                           .bias_tee = ports[i].biasTee ? 1 : 0,
                                           .requires_stop = ports[i].requiresStop ? 1 : 0,
                                           .switch_seconds = ports[i].switchSeconds};
        }
    });
    return total;
}

inline sweeppp_str_t selectedRxPortThunk(void* device) {
    sweeppp_str_t id{};
    guard([device, &id] { id = str(holderOf(device)->radio->selectedRxPort()); });
    return id;
}

inline sweeppp_plugin_status_t selectRxPortThunk(void* device, sweeppp_str_t id) {
    auto status = SWEEPPP_PLUGIN_ERR_UNKNOWN;
    guard([device, id, &status] {
        const Status selected = holderOf(device)->radio->selectRxPort(view(id));
        status = selected ? SWEEPPP_PLUGIN_OK : toAbiStatus(selected.error().code());
    });
    return status;
}

} // namespace detail

inline const sweeppp_sdr_device_vtable_t& sdrDeviceVtable() noexcept {
    static const sweeppp_sdr_device_vtable_t kVtable{
        .struct_size = sizeof(sweeppp_sdr_device_vtable_t),
        .destroy = &detail::destroyThunk,
        .info = &detail::infoThunk,
        .parameters = &detail::parametersThunk,
        .get_parameter = &detail::getParameterThunk,
        .set_parameter = &detail::setParameterThunk,
        .native_format = &detail::nativeFormatThunk,
        .supported_sample_rates = &detail::sampleRatesThunk,
        .start = &detail::startThunk,
        .stop = &detail::stopThunk,
        .streaming = &detail::streamingThunk,
        .retune = &detail::retuneThunk,
        .retune_settle_seconds = &detail::settleThunk,
        .delivery_granularity_seconds = &detail::granularityThunk,
        .health_readings = &detail::healthThunk,
        .rx_ports = &detail::rxPortsThunk,
        .selected_rx_port = &detail::selectedRxPortThunk,
        .select_rx_port = &detail::selectRxPortThunk,
    };
    return kVtable;
}

/// Builds a factory vtable forwarding to `T`, which must derive from `Driver`.
///
/// Store the result in something with static storage duration: the host keeps
/// the pointer for the life of the process.
template <typename T>
[[nodiscard]] sweeppp_sdr_factory_vtable_t makeSdrDriverVtable() {
    static_assert(std::is_base_of_v<Driver, T>, "an SDR facet's instance must be a plugin::Driver");

    sweeppp_sdr_factory_vtable_t vtable{};
    vtable.struct_size = sizeof(sweeppp_sdr_factory_vtable_t);

    vtable.enumerate = [](void* instance, sweeppp_sdr_device_info_t* out,
                          std::uint32_t capacity) -> std::uint32_t {
        std::uint32_t total = 0;
        detail::guard([instance, out, capacity, &total] {
            detail::FactoryScratch& scratch = detail::factoryScratch();
            scratch.devices = static_cast<const T*>(instance)->enumerate();
            total = static_cast<std::uint32_t>(scratch.devices.size());

            const std::uint32_t taken = std::min(total, capacity);
            for (std::uint32_t i = 0; i < taken && out != nullptr; ++i) {
                detail::fillInfo(scratch.devices[i], out[i]);
            }
        });
        return total;
    };

    vtable.open = [](void* instance, sweeppp_str_t id, void** device,
                     const sweeppp_sdr_device_vtable_t** deviceVtable,
                     sweeppp_str_t* error) -> sweeppp_plugin_status_t {
        auto status = SWEEPPP_PLUGIN_ERR_UNKNOWN;
        detail::guard([instance, id, device, deviceVtable, error, &status] {
            if (device == nullptr || deviceVtable == nullptr) {
                status = SWEEPPP_PLUGIN_ERR_INVALID_ARGUMENT;
                return;
            }

            Result<std::unique_ptr<Radio>> radio = static_cast<T*>(instance)->open(view(id));
            if (!radio) {
                // The driver's own sentence, parked where it stays valid until
                // this factory's next call on this thread. A status code alone
                // is not something an operator can act on.
                //
                // The message rather than `describe()`: the code travels
                // separately as the return value and the host puts it back on
                // the front, so describing it here would say "DeviceError"
                // twice in one line.
                detail::FactoryScratch& scratch = detail::factoryScratch();
                scratch.error = radio.error().message();
                if (error != nullptr) {
                    *error = str(scratch.error);
                }
                status = toAbiStatus(radio.error().code());
                return;
            }

            auto holder = std::make_unique<detail::RadioHolder>();
            holder->radio = *std::move(radio);
            *device = holder.release();
            *deviceVtable = &sdrDeviceVtable();
            status = SWEEPPP_PLUGIN_OK;
        });
        return status;
    };

    return vtable;
}

} // namespace sweeppp::plugin
