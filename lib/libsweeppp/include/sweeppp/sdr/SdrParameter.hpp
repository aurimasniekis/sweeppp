// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sweeppp {

/// A device parameter's value.
///
/// These four alternatives are exactly TOML's scalar types, which is why
/// profiles can store any device's full configuration with no encoding tricks
/// and stay hand-editable. Adding a fifth alternative means adding a TOML
/// representation for it too.
using SdrValue = std::variant<bool, std::int64_t, double, std::string>;

enum class SdrParameterType : std::uint8_t {
    Bool,
    Int,
    Double,
    Enum, ///< Int or string constrained to `enumValues`.
    String,
};

[[nodiscard]] std::string_view toString(SdrParameterType type) noexcept;

/// One selectable value of an Enum parameter.
struct SdrEnumValue {
    std::string value; ///< Stored form, e.g. "rx1".
    std::string label; ///< Shown form, e.g. "RX1 (SMA)".
    std::string description;
};

/// Self-describing device parameter.
///
/// The SDR panel in the UI is generated *purely* from a device's
/// `parameters()` -- there is no per-device UI code anywhere in the project.
/// That is only possible if the description here is complete enough to build a
/// correct widget from: type, range, step, unit, grouping and help text.
struct SdrParameter {
    std::string key;   ///< Stable identifier used in profiles, e.g. "lna_gain".
    std::string label; ///< "LNA gain"
    std::string group; ///< Panel section, e.g. "Gain", "Tuning", "Filters".
    SdrParameterType type = SdrParameterType::Double;

    std::string unit; ///< "Hz", "dB", "S/s"; empty when dimensionless.
    double min = 0.0;
    double max = 0.0;
    /// Quantisation the hardware actually enforces. HackRF's LNA gain moves in
    /// 8 dB steps; showing a continuous slider would let the operator set a
    /// value the radio silently rounds, and then disbelieve the readout.
    double step = 0.0;

    /// For an Enum, the values it may take. For an Int or a Double, the
    /// PRESETS the panel offers -- it draws a dropdown instead of a slider or
    /// a text field, which is the only sane widget for a sample rate spanning
    /// 520 kS/s to 61.44 MS/s.
    ///
    /// Presets, not constraints: `coerce` does not snap a numeric value to
    /// this list, so a rate set from the CLI or restored from a profile is
    /// honoured whether or not it appears here, and the panel shows it as the
    /// current selection either way. That is the honest reading for every
    /// radio in this tree -- each of them accepts a continuous range and
    /// reports back what it actually tuned to.
    std::vector<SdrEnumValue> enumValues;

    bool readOnly = false;
    /// Changing this redefines the frequency grid, so it closes the current
    /// session segment and opens a new one. Sample rate, FFT-relevant
    /// bandwidth and centre frequency are the usual cases.
    bool gridAffecting = false;
    /// Changing this shifts the noise floor without changing the grid. Gain
    /// stages and reference level. Recorded in segment metadata so later
    /// analysis knows the calibration changed.
    bool calibrationAffecting = false;
    /// Cannot be changed while streaming; the UI disables it rather than
    /// letting the device reject it.
    bool requiresStop = false;

    std::string description;
    SdrValue defaultValue = std::int64_t{0};

    /// Another parameter this one depends on, and the values of it that make
    /// this one apply. Empty `appliesWhenKey` means always.
    ///
    /// Declared rather than left to the panel, which knows nothing about any
    /// particular radio and must not start: a manual gain means nothing while
    /// the gain mode is automatic, and only the driver can say so.
    std::string appliesWhenKey;
    std::vector<std::string> appliesWhenValues;

    /// Clamps and snaps `value` to this parameter's constraints, or explains
    /// why it cannot be represented at all. Devices call this before applying,
    /// so out-of-range input is corrected in one place rather than in each
    /// driver.
    [[nodiscard]] Result<SdrValue> coerce(const SdrValue& value) const;

    /// Human-readable rendering with the unit attached, for the panel and for
    /// the event log.
    [[nodiscard]] std::string format(const SdrValue& value) const;
};

/// Presets for a sample-rate parameter, labelled the way an operator reads one:
/// "20 MS/s", "520.834 kS/s".
///
/// Shared rather than written per driver because three of them build the same
/// list, and the same rate spelled two ways in two panels reads as two
/// different rates. The stored value round-trips a double exactly, so a
/// selection and a hand-typed figure end up bit-identical.
[[nodiscard]] std::vector<SdrEnumValue> sampleRateChoices(std::span<const double> rates);

/// The device's minimum, every `stepHz` above it, and its maximum.
///
/// What a radio with a continuous range wants for the list above: a ladder has
/// to come from somewhere, and the endpoints are the two rates an operator is
/// most likely to want. Returns empty when the range is not usable.
[[nodiscard]] std::vector<double> steppedSampleRates(double minimumHz, double maximumHz,
                                                     double stepHz);

/// Conversions used at the TOML and remote-protocol boundaries.
[[nodiscard]] std::string toString(const SdrValue& value);
[[nodiscard]] Result<SdrValue> parseSdrValue(std::string_view text, SdrParameterType type);

[[nodiscard]] bool asBool(const SdrValue& value, bool fallback = false) noexcept;
[[nodiscard]] std::int64_t asInt(const SdrValue& value, std::int64_t fallback = 0) noexcept;
[[nodiscard]] double asDouble(const SdrValue& value, double fallback = 0.0) noexcept;
[[nodiscard]] std::string asString(const SdrValue& value, std::string_view fallback = {});

/// True when both hold the same alternative with the same value. Devices use
/// it to skip no-op writes, which matters during a sweep where the same gain
/// is re-applied on every step.
[[nodiscard]] bool equals(const SdrValue& a, const SdrValue& b) noexcept;

} // namespace sweeppp
