// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { parseFrequency } from "../../../render/format";
import {
  formatFrequencyShort,
  type SdrParameter,
  SdrParameterType,
  type SdrValue,
} from "../../../protocol/wire";

/** printf's `%.<precision>g`, trailing zeros dropped. */
function general(v: number, precision: number): string {
  if (!Number.isFinite(v)) {
    return String(v);
  }
  if (v === 0) {
    return "0";
  }
  const exponent = Math.floor(Math.log10(Math.abs(v)));
  if (exponent < -4 || exponent >= precision) {
    const [mantissa = "", power = "0"] = v.toExponential(precision - 1).split("e");
    const trimmed = mantissa.includes(".") ? mantissa.replace(/\.?0+$/, "") : mantissa;
    const p = Number(power);
    return `${trimmed}e${p < 0 ? "-" : "+"}${String(Math.abs(p)).padStart(2, "0")}`;
  }
  const fixed = v.toFixed(Math.max(0, precision - 1 - exponent));
  return fixed.includes(".") ? fixed.replace(/\.?0+$/, "") : fixed;
}

/** As `toString(SdrValue)`: what `appliesWhenValues` and enum values are written in. */
export function asString(value: SdrValue): string {
  switch (typeof value) {
    case "boolean":
      return value ? "true" : "false";
    case "bigint":
      return value.toString();
    case "number":
      return general(value, 17);
    default:
      return value;
  }
}

export function asBool(value: SdrValue | undefined): boolean {
  switch (typeof value) {
    case "boolean":
      return value;
    case "bigint":
      return value !== 0n;
    case "number":
      return value !== 0;
    case "string":
      return value === "true" || value === "1" || value === "on";
    default:
      return false;
  }
}

export function asNumber(value: SdrValue): number {
  switch (typeof value) {
    case "boolean":
      return value ? 1 : 0;
    case "bigint":
      return Number(value);
    case "number":
      return value;
    default:
      return parseFrequency(value) ?? 0;
  }
}

function formatSampleRate(rate: number): string {
  if (rate >= 1e6) return `${general(rate / 1e6, 6)} MS/s`;
  if (rate >= 1e3) return `${general(rate / 1e3, 6)} kS/s`;
  return `${general(rate, 6)} S/s`;
}

/** The preset a numeric value is, by the value its label was made from. */
export function presetIndex(parameter: SdrParameter, value: SdrValue): number {
  const held = asNumber(value);
  return parameter.enumValues.findIndex((candidate) => parseFrequency(candidate.value) === held);
}

/** As `SdrParameter::format`. */
export function formatParameter(parameter: SdrParameter, value: SdrValue): string {
  if (parameter.type === SdrParameterType.Enum) {
    const held = asString(value);
    return parameter.enumValues.find((c) => c.value === held)?.label ?? held;
  }
  if (parameter.enumValues.length > 0 && parameter.type === SdrParameterType.Double) {
    const match = parameter.enumValues[presetIndex(parameter, value)];
    if (match) {
      return match.label;
    }
  }
  if (parameter.type === SdrParameterType.Bool) {
    return asBool(value) ? "on" : "off";
  }
  if (parameter.unit === "Hz") {
    return formatFrequencyShort(asNumber(value));
  }
  if (parameter.unit === "S/s" && parameter.type === SdrParameterType.Double) {
    return formatSampleRate(asNumber(value));
  }
  let rendered: string;
  if (parameter.type === SdrParameterType.Int) {
    rendered = String(Math.round(asNumber(value)));
  } else if (parameter.type === SdrParameterType.Double) {
    rendered = asNumber(value).toFixed(decimals(parameter));
  } else {
    rendered = asString(value);
  }
  return parameter.unit ? `${rendered} ${parameter.unit}` : rendered;
}

/** A 0.5 dB step deserves a decimal, an integer step none. */
function decimals(parameter: SdrParameter): number {
  return parameter.step >= 1 || parameter.step === 0 ? 0 : 2;
}

/** Whether the governing parameter holds one of the values that make this one
 * mean something. An unreadable governor counts as satisfied, so a row cannot
 * be hidden by a parameter that no longer exists. */
export function applies(parameter: SdrParameter, values: Map<string, SdrValue>): boolean {
  if (!parameter.appliesWhenKey) {
    return true;
  }
  const governing = values.get(parameter.appliesWhenKey);
  return governing === undefined || parameter.appliesWhenValues.includes(asString(governing));
}

/** While sweeping, a sample rate goes into the plan and is applied by a
 * re-plan rather than by the driver, so it needs no stop then. */
export function needsStop(parameter: SdrParameter, sweeping: boolean): boolean {
  return parameter.requiresStop && !(sweeping && parameter.key === "sample_rate");
}
