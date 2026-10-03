// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Telemetry.hpp"
#include "sweeppp/instrument/Instrument.hpp"

#include <span>
#include <sweeps/Metadata.hpp>
#include <utility>
#include <vector>

/// The instrument's value types as typed metadata, for the remote protocol.
///
/// Typed rather than text: a double crosses bit for bit, so a plan sent and
/// sent back compares equal. Decoding is total. A missing key, or one of the
/// wrong type, takes the field's default and an enum out of range takes its
/// default too, so a newer peer's additions and an older peer's omissions are
/// both readable.
namespace sweeppp::remote {

using sweeps::Metadata;

[[nodiscard]] sweeps::Value encodeValue(const SdrValue& value);
[[nodiscard]] SdrValue decodeValue(const sweeps::Value& value);

[[nodiscard]] Metadata encodeDevice(const DeviceDescriptor& device);
[[nodiscard]] DeviceDescriptor decodeDevice(const Metadata& in);

[[nodiscard]] Metadata encodePlan(const SweepPlan& plan);
[[nodiscard]] SweepPlan decodePlan(const Metadata& in);

[[nodiscard]] Metadata encodePipeline(const PipelineConfig& config);
[[nodiscard]] PipelineConfig decodePipeline(const Metadata& in);

[[nodiscard]] Metadata encodeCorrectionSettings(const CorrectionSettings& settings);
[[nodiscard]] CorrectionSettings decodeCorrectionSettings(const Metadata& in);

[[nodiscard]] Metadata encodeCorrectionSummary(const CorrectionSummary& summary);
[[nodiscard]] CorrectionSummary decodeCorrectionSummary(const Metadata& in);

[[nodiscard]] Metadata encodeSchedule(const ScheduleSummary& schedule);
[[nodiscard]] ScheduleSummary decodeSchedule(const Metadata& in);

[[nodiscard]] Metadata encodeEngineStats(const EngineStats& stats);
[[nodiscard]] EngineStats decodeEngineStats(const Metadata& in);

[[nodiscard]] Metadata encodeBackends(std::span<const FftBackendInfo> backends);
[[nodiscard]] std::vector<FftBackendInfo> decodeBackends(const Metadata& in);

/// The whole library, shipped entries included and marked as such.
[[nodiscard]] Metadata encodeAntennas(std::span<const Antenna> antennas);
[[nodiscard]] std::vector<Antenna> decodeAntennas(const Metadata& in);

[[nodiscard]] Metadata encodeAssignments(const AntennaAssignments& assignments);
[[nodiscard]] AntennaAssignments decodeAssignments(const Metadata& in);

[[nodiscard]] Metadata encodeSwitcherViews(std::span<const SwitcherView> views);
[[nodiscard]] std::vector<SwitcherView> decodeSwitcherViews(const Metadata& in);

[[nodiscard]] Metadata encodeSwitcherInfos(std::span<const RfPathInfo> infos);
[[nodiscard]] std::vector<RfPathInfo> decodeSwitcherInfos(const Metadata& in);

[[nodiscard]] Metadata encodeRfLegs(std::span<const RfLegView> legs);
[[nodiscard]] std::vector<RfLegView> decodeRfLegs(const Metadata& in);

[[nodiscard]] Metadata encodeRanges(std::span<const std::pair<double, double>> ranges);
[[nodiscard]] std::vector<std::pair<double, double>> decodeRanges(const Metadata& in);

[[nodiscard]] Metadata encodeHealth(std::span<const SdrHealthReading> readings);
[[nodiscard]] std::vector<SdrHealthReading> decodeHealth(const Metadata& in);

[[nodiscard]] Metadata encodeStreamStats(const StreamStats& stats);
[[nodiscard]] StreamStats decodeStreamStats(const Metadata& in);

[[nodiscard]] Metadata encodeProcessStats(const ProcessStats& stats);
[[nodiscard]] ProcessStats decodeProcessStats(const Metadata& in);

[[nodiscard]] Metadata encodeNotice(const InstrumentNotice& notice);
[[nodiscard]] InstrumentNotice decodeNotice(const Metadata& in);

/// The hash under `key`, or an empty one: what every nested decode starts
/// from, so a missing section decodes to defaults like a missing field.
[[nodiscard]] const Metadata& hashAt(const Metadata& in, std::string_view key);

} // namespace sweeppp::remote
