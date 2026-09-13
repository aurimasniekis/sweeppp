// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/sdr/SdrParameter.hpp"

#include "sweeppp/core/Toml.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace sweeppp {
namespace {

/// Snaps to the nearest legal step within [min, max].
///
/// Rounds to nearest rather than truncating: a user typing 25 dB into a field
/// whose hardware steps are 8 dB should land on 24, not 16. Truncation would
/// make every typed value read low, which looks like a calibration error.
double snapToStep(double value, double minimum, double maximum, double step) {
    double clamped = std::clamp(value, minimum, maximum);
    if (step > 0.0) {
        const double steps = std::round((clamped - minimum) / step);
        clamped = minimum + steps * step;
        clamped = std::clamp(clamped, minimum, maximum);
    }
    return clamped;
}

} // namespace

std::string_view toString(SdrParameterType type) noexcept {
    switch (type) {
    case SdrParameterType::Bool:
        return "bool";
    case SdrParameterType::Int:
        return "int";
    case SdrParameterType::Double:
        return "double";
    case SdrParameterType::Enum:
        return "enum";
    case SdrParameterType::String:
        return "string";
    }
    return "double";
}

std::string toString(const SdrValue& value) {
    return std::visit(
        [](const auto& held) -> std::string {
            using T = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<T, bool>) {
                return held ? "true" : "false";
            } else if constexpr (std::is_same_v<T, std::string>) {
                return held;
            } else if constexpr (std::is_same_v<T, double>) {
                // Enough digits to round-trip a double exactly, so a profile
                // reload cannot shift a centre frequency by a fraction of a Hz.
                return std::format("{:.17g}", held);
            } else {
                return std::format("{}", held);
            }
        },
        value);
}

bool asBool(const SdrValue& value, bool fallback) noexcept {
    return std::visit(
        [fallback](const auto& held) -> bool {
            using T = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<T, bool>) {
                return held;
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return held != 0;
            } else if constexpr (std::is_same_v<T, double>) {
                return held != 0.0;
            } else {
                return held == "true" || held == "1" || held == "on" ? true : fallback;
            }
        },
        value);
}

std::int64_t asInt(const SdrValue& value, std::int64_t fallback) noexcept {
    return std::visit(
        [fallback](const auto& held) -> std::int64_t {
            using T = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<T, bool>) {
                return held ? 1 : 0;
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return held;
            } else if constexpr (std::is_same_v<T, double>) {
                return static_cast<std::int64_t>(std::llround(held));
            } else {
                return fallback;
            }
        },
        value);
}

double asDouble(const SdrValue& value, double fallback) noexcept {
    return std::visit(
        [fallback](const auto& held) -> double {
            using T = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<T, bool>) {
                return held ? 1.0 : 0.0;
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return static_cast<double>(held);
            } else if constexpr (std::is_same_v<T, double>) {
                return held;
            } else {
                // A string that looks like a frequency is still a frequency;
                // this is what lets a profile say center = "2.4 GHz".
                const auto parsed = toml_util::parseFrequency(held);
                return parsed ? *parsed : fallback;
            }
        },
        value);
}

std::string asString(const SdrValue& value, std::string_view fallback) {
    if (const auto* held = std::get_if<std::string>(&value)) {
        return *held;
    }
    if (std::holds_alternative<bool>(value) || std::holds_alternative<std::int64_t>(value) ||
        std::holds_alternative<double>(value)) {
        return toString(value);
    }
    return std::string(fallback);
}

bool equals(const SdrValue& a, const SdrValue& b) noexcept {
    if (a.index() != b.index()) {
        return false;
    }
    if (const auto* left = std::get_if<double>(&a)) {
        const double right = std::get<double>(b);
        // Exact comparison would make a re-applied gain look like a change on
        // every sweep step. A relative epsilon keeps no-op writes out of the
        // event stream without hiding real changes.
        const double scale = std::max({std::abs(*left), std::abs(right), 1.0});
        return std::abs(*left - right) <= 1e-12 * scale;
    }
    return a == b;
}

Result<SdrValue> parseSdrValue(std::string_view text, SdrParameterType type) {
    switch (type) {
    case SdrParameterType::Bool:
        if (text == "true" || text == "1" || text == "on" || text == "yes") {
            return SdrValue{true};
        }
        if (text == "false" || text == "0" || text == "off" || text == "no") {
            return SdrValue{false};
        }
        return fail<SdrValue>(ErrorCode::ParseError, "'{}' is not a boolean", text);

    case SdrParameterType::Int: {
        const auto parsed = toml_util::parseFrequency(text);
        if (!parsed) {
            return fail<SdrValue>(ErrorCode::ParseError, "'{}' is not an integer", text);
        }
        return SdrValue{static_cast<std::int64_t>(std::llround(*parsed))};
    }

    case SdrParameterType::Double: {
        // Reuses the frequency parser so "2.4G" works for any numeric
        // parameter, not only ones the UI happens to know are frequencies.
        const auto parsed = toml_util::parseFrequency(text);
        if (!parsed) {
            return fail<SdrValue>(ErrorCode::ParseError, "'{}' is not a number", text);
        }
        return SdrValue{*parsed};
    }

    case SdrParameterType::Enum:
    case SdrParameterType::String:
        return SdrValue{std::string(text)};
    }

    return fail<SdrValue>(ErrorCode::InvalidArgument, "unhandled parameter type");
}

Result<SdrValue> SdrParameter::coerce(const SdrValue& value) const {
    switch (type) {
    case SdrParameterType::Bool:
        return SdrValue{asBool(value)};

    case SdrParameterType::Int: {
        const auto raw = static_cast<double>(asInt(value));
        const double snapped = snapToStep(raw, min, max, step);
        return SdrValue{static_cast<std::int64_t>(std::llround(snapped))};
    }

    case SdrParameterType::Double:
        return SdrValue{snapToStep(asDouble(value), min, max, step)};

    case SdrParameterType::Enum: {
        const std::string requested = asString(value);
        const auto match =
            std::ranges::find_if(enumValues, [&requested](const SdrEnumValue& candidate) {
                return candidate.value == requested;
            });
        if (match == enumValues.end()) {
            // Listing what *is* accepted turns an error into a fix.
            std::string accepted;
            for (const SdrEnumValue& candidate : enumValues) {
                accepted += (accepted.empty() ? "" : ", ");
                accepted += candidate.value;
            }
            return fail<SdrValue>(ErrorCode::InvalidArgument,
                                  "'{}' is not a valid {} (accepted: {})", requested, key,
                                  accepted);
        }
        return SdrValue{match->value};
    }

    case SdrParameterType::String:
        return SdrValue{asString(value)};
    }

    return fail<SdrValue>(ErrorCode::InvalidArgument, "unhandled parameter type for '{}'", key);
}

std::string formatSampleRate(double rate) {
    if (rate >= 1e6) {
        return std::format("{:g} MS/s", rate / 1e6);
    }
    if (rate >= 1e3) {
        return std::format("{:g} kS/s", rate / 1e3);
    }
    return std::format("{:g} S/s", rate);
}

std::vector<SdrEnumValue> sampleRateChoices(std::span<const double> rates) {
    std::vector<SdrEnumValue> choices;
    choices.reserve(rates.size());

    for (const double rate : rates) {
        // The stored form round-trips the double exactly, so a rate picked
        // from this list and the same rate typed into `--sample-rate` are the
        // same bits and compare equal.
        choices.push_back(
            SdrEnumValue{.value = toString(SdrValue{rate}), .label = formatSampleRate(rate)});
    }
    return choices;
}

std::vector<double> steppedSampleRates(double minimumHz, double maximumHz, double stepHz) {
    if (!(maximumHz > minimumHz) || minimumHz <= 0.0 || stepHz <= 0.0) {
        return {};
    }

    std::vector<double> rates;
    rates.push_back(minimumHz);

    // Strictly between the endpoints, so a range whose minimum or maximum
    // happens to land on the step does not list it twice.
    for (double rate = stepHz; rate < maximumHz; rate += stepHz) {
        if (rate > minimumHz) {
            rates.push_back(rate);
        }
    }

    rates.push_back(maximumHz);
    return rates;
}

std::string SdrParameter::format(const SdrValue& value) const {
    if (type == SdrParameterType::Enum) {
        const std::string held = asString(value);
        const auto match = std::ranges::find_if(
            enumValues, [&held](const SdrEnumValue& candidate) { return candidate.value == held; });
        return match != enumValues.end() ? match->label : held;
    }

    // A numeric parameter carrying presets reads back as the one it matches:
    // "20 MS/s" rather than "20000000 S/s". An off-list value falls through to
    // the ordinary rendering below, because it is still a legal value.
    if (!enumValues.empty() && type == SdrParameterType::Double) {
        const double held = asDouble(value);
        const auto match = std::ranges::find_if(enumValues, [held](const SdrEnumValue& candidate) {
            const auto parsed = toml_util::parseFrequency(candidate.value);
            return parsed && *parsed == held;
        });
        if (match != enumValues.end()) {
            return match->label;
        }
    }

    if (type == SdrParameterType::Bool) {
        return asBool(value) ? "on" : "off";
    }

    if (unit == "Hz") {
        return toml_util::formatFrequencyShort(asDouble(value));
    }

    // Scaled the same way, and for the same reason: nothing in "17300000" says
    // which unit it is in. It also keeps a rate that matched no preset reading
    // like the ones that did, so the dropdown does not switch notation when the
    // operator types an exact figure.
    if (unit == "S/s" && type == SdrParameterType::Double) {
        return formatSampleRate(asDouble(value));
    }

    std::string rendered;
    if (type == SdrParameterType::Int) {
        rendered = std::format("{}", asInt(value));
    } else if (type == SdrParameterType::Double) {
        // Match the precision to the step: a 0.5 dB step deserves one decimal,
        // an integer step none.
        const int decimals = step >= 1.0 || step == 0.0 ? 0 : 2;
        rendered = std::format("{:.{}f}", asDouble(value), decimals);
    } else {
        rendered = asString(value);
    }

    return unit.empty() ? rendered : std::format("{} {}", rendered, unit);
}

} // namespace sweeppp
