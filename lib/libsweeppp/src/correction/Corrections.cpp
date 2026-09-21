// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/correction/Corrections.hpp"

#include "sweeppp/core/Paths.hpp"
#include "sweeppp/core/Toml.hpp"
#include "sweeppp/pipeline/SpectrumFrame.hpp"
#include "sweeppp/rf/AntennaAssignments.hpp"
#include "sweeppp/sdr/ISdrDevice.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace sweeppp {
namespace {

constexpr std::uint8_t kDcRemovalBit = 1U << 0U;
constexpr std::uint8_t kFlattenBit = 1U << 1U;
constexpr std::uint8_t kSpurMaskBit = 1U << 2U;
constexpr std::uint8_t kAutoSpursBit = 1U << 3U;

/// The shape's level at a fraction of its span, linearly interpolated between
/// its samples and held flat past either end.
[[nodiscard]] float sampleShape(const FloorShape& shape, double fraction) noexcept {
    const std::size_t count = shape.levelDb.size();
    if (count == 1) {
        return shape.levelDb.front();
    }

    // Sample k sits at the centre of its slice, (k + 0.5) / count.
    const double position = (fraction * static_cast<double>(count)) - 0.5;
    if (position <= 0.0) {
        return shape.levelDb.front();
    }
    if (position >= static_cast<double>(count - 1)) {
        return shape.levelDb.back();
    }

    const auto low = static_cast<std::size_t>(position);
    const auto t = static_cast<float>(position - static_cast<double>(low));
    return shape.levelDb[low] + ((shape.levelDb[low + 1] - shape.levelDb[low]) * t);
}

/// Sets every bin whose centre lies within `halfWidthHz` of `hz`, plus one
/// bin of margin either side, where `hz` is measured from the first bin's
/// lower edge. Returns whether anything was set.
bool markRun(std::span<std::uint8_t> mask, double hz, double halfWidthHz,
             double binWidthHz) noexcept {
    const double lowHz = hz - halfWidthHz;
    const double highHz = hz + halfWidthHz;
    const auto count = static_cast<double>(mask.size());

    // Bin i has its centre at (i + 0.5) * binWidth, so the bins whose centres
    // fall inside the run are ceil(low - 0.5) .. floor(high - 0.5) in units
    // of a bin. The run is first widened by one bin either side, which covers
    // the skirt the window puts on a narrow spur.
    const double firstCentre = std::ceil((lowHz / binWidthHz) - 1.5);
    const double lastCentre = std::floor((highHz / binWidthHz) + 0.5);
    if (lastCentre < 0.0 || firstCentre >= count || lastCentre < firstCentre) {
        return false;
    }

    const auto first = static_cast<std::size_t>(std::max(firstCentre, 0.0));
    const auto last = static_cast<std::size_t>(std::min(lastCentre, count - 1.0));
    std::fill(mask.begin() + static_cast<std::ptrdiff_t>(first),
              mask.begin() + static_cast<std::ptrdiff_t>(last) + 1, std::uint8_t{1});
    return true;
}

/// Rebuilds the per-grid tables for a new bin count or sample rate.
void prepareScratch(CorrectionScratch& scratch, std::size_t binCount, double sampleRate,
                    const CorrectionSet& set) {
    scratch.binCount = binCount;
    scratch.sampleRate = sampleRate;

    scratch.floorDelta.assign(binCount, 0.0F);
    if (!set.floor.empty()) {
        const float median = set.floor.medianDb();
        for (std::size_t i = 0; i < binCount; ++i) {
            const double fraction = (static_cast<double>(i) + 0.5) / static_cast<double>(binCount);
            scratch.floorDelta[i] = sampleShape(set.floor, fraction) - median;
        }
    }

    scratch.loMask.assign(binCount, 0);
    scratch.loMasked = false;
    const double binWidthHz = sampleRate / static_cast<double>(binCount);
    for (const SpurEntry& spur : set.spurs) {
        if (spur.kind == SpurKind::LoOffset) {
            scratch.loMasked |= markRun(scratch.loMask, spur.hz + (sampleRate * 0.5),
                                        spur.widthHz * 0.5, binWidthHz);
        }
    }

    scratch.mask.assign(binCount, 0);
}

/// The nearest bin from `from` in direction `step` that is neither masked
/// nor unmeasured, or `count` when there is none. Both kinds are passed over
/// alike: a bin that carries no reading cannot anchor a line.
std::size_t nearestAnchor(std::span<const float> bins, std::span<const std::uint8_t> mask,
                          std::size_t from, std::ptrdiff_t step) noexcept {
    const std::size_t count = bins.size();
    std::size_t i = from;
    while (i < count) {
        if (mask[i] == 0 && measured(bins[i])) {
            return i;
        }
        if (i == 0 && step < 0) {
            break;
        }
        i = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(i) + step);
    }
    return count;
}

/// Writes a line from the left anchor's level to the right one's across bins
/// `first`..`last`; with one anchor, its level; with none, nothing.
void bridgeRun(std::span<float> bins, std::size_t first, std::size_t last, std::size_t left,
               std::size_t right) noexcept {
    const std::size_t count = bins.size();
    const bool hasLeft = left < count;
    const bool hasRight = right < count;
    if (!hasLeft && !hasRight) {
        return;
    }

    const float leftDb = hasLeft ? bins[left] : bins[right];
    const float rightDb = hasRight ? bins[right] : bins[left];
    const double span =
        hasLeft && hasRight ? static_cast<double>(right) - static_cast<double>(left) : 1.0;

    for (std::size_t k = first; k <= last; ++k) {
        if (!measured(bins[k])) {
            continue;
        }
        const double t =
            hasLeft && hasRight ? (static_cast<double>(k) - static_cast<double>(left)) / span : 0.0;
        bins[k] = leftDb + (static_cast<float>(t) * (rightDb - leftDb));
    }
}

/// Replaces every masked run with a line between its nearest measured,
/// unmasked neighbours.
void interpolateMasked(std::span<float> bins, std::span<const std::uint8_t> mask) noexcept {
    const std::size_t count = bins.size();
    std::size_t i = 0;

    while (i < count) {
        if (mask[i] == 0) {
            ++i;
            continue;
        }

        const std::size_t first = i;
        while (i < count && mask[i] != 0) {
            ++i;
        }
        const std::size_t last = i - 1;

        const std::size_t left = first == 0 ? count : nearestAnchor(bins, mask, first - 1, -1);
        const std::size_t right = nearestAnchor(bins, mask, last + 1, 1);
        bridgeRun(bins, first, last, left, right);
    }
}

std::string sanitisedFileStem(std::string_view key) {
    std::string stem(key);
    for (char& c : stem) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) == 0 && c != '-' && c != '_' && c != '.') {
            c = '_';
        }
    }
    return stem;
}

} // namespace

std::uint8_t CorrectionSettings::toBits() const noexcept {
    return static_cast<std::uint8_t>((dcRemoval ? kDcRemovalBit : 0U) |
                                     (flatten ? kFlattenBit : 0U) | (spurMask ? kSpurMaskBit : 0U) |
                                     (autoSpurs ? kAutoSpursBit : 0U));
}

CorrectionSettings CorrectionSettings::fromBits(std::uint8_t bits) noexcept {
    return {.dcRemoval = (bits & kDcRemovalBit) != 0,
            .flatten = (bits & kFlattenBit) != 0,
            .spurMask = (bits & kSpurMaskBit) != 0,
            .autoSpurs = (bits & kAutoSpursBit) != 0};
}

std::string_view toString(SpurKind kind) noexcept {
    return kind == SpurKind::Absolute ? "absolute" : "lo_offset";
}

float FloorShape::medianDb() const {
    if (levelDb.empty()) {
        return 0.0F;
    }
    std::vector<float> sorted = levelDb;
    const auto middle = sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2);
    std::nth_element(sorted.begin(), middle, sorted.end());
    return *middle;
}

std::string CalibrationContext::firstDifference(const CalibrationContext& other) const {
    if (driver != other.driver) {
        return "driver";
    }
    if (deviceId != other.deviceId) {
        return "device";
    }
    if (dcRemoval != other.dcRemoval) {
        return "dc_removal";
    }

    // Both lists are sorted by key, so one merge walk finds the first key
    // that is missing on either side or differs in value.
    std::size_t a = 0;
    std::size_t b = 0;
    while (a < parameters.size() || b < other.parameters.size()) {
        if (a == parameters.size()) {
            return other.parameters[b].first;
        }
        if (b == other.parameters.size()) {
            return parameters[a].first;
        }
        const auto& [keyA, valueA] = parameters[a];
        const auto& [keyB, valueB] = other.parameters[b];
        if (keyA != keyB) {
            return keyA < keyB ? keyA : keyB;
        }
        if (valueA != valueB) {
            return keyA;
        }
        ++a;
        ++b;
    }
    return {};
}

CalibrationContext calibrationContextFor(const ISdrDevice& device, bool dcRemoval) {
    CalibrationContext context;
    context.driver = device.info().driver;
    context.deviceId = device.info().id;
    context.dcRemoval = dcRemoval;

    // Read back rather than remembered, for the reason a profile is: what the
    // radio coerced is what the floor was measured at.
    for (const SdrParameter& parameter : device.parameters()) {
        if (parameter.key == "center_hz" ||
            !(parameter.gridAffecting || parameter.calibrationAffecting)) {
            continue;
        }
        if (auto value = device.getParameter(parameter.key)) {
            context.parameters.emplace_back(parameter.key, toString(*value));
        }
    }
    std::ranges::sort(context.parameters);
    return context;
}

Result<CorrectionSet> CorrectionSet::load(const std::filesystem::path& path) {
    auto table = toml_util::load(path);
    if (!table) {
        return std::unexpected(table.error());
    }

    CorrectionSet set;
    set.learnedAt = toml_util::getString(*table, "calibration.learned_at", "");
    set.context.driver = toml_util::getString(*table, "calibration.context.driver", "");
    set.context.deviceId = toml_util::getString(*table, "calibration.context.device", "");
    set.context.dcRemoval = toml_util::getBool(*table, "calibration.context.dc_removal", true);

    if (const auto* parameters =
            toml_util::at(*table, "calibration.context.parameters").as_table()) {
        for (const auto& [key, node] : *parameters) {
            set.context.parameters.emplace_back(std::string(key.str()),
                                                node.value<std::string>().value_or(""));
        }
        std::ranges::sort(set.context.parameters);
    }

    set.floor.sampleRate = toml_util::getDouble(*table, "calibration.floor.sample_rate", 0.0);
    if (const auto* floor = toml_util::at(*table, "calibration.floor").as_table()) {
        for (const double level : toml_util::getDoubleArray(*floor, "level_db")) {
            set.floor.levelDb.push_back(static_cast<float>(level));
        }
    }
    if (set.floor.sampleRate <= 0.0) {
        set.floor.levelDb.clear();
    }

    if (const ::toml::array* spurs = toml_util::at(*table, "calibration.spurs").as_array()) {
        for (const ::toml::node& node : *spurs) {
            const ::toml::table* entry = node.as_table();
            if (entry == nullptr) {
                continue;
            }
            const std::string kind = toml_util::getString(*entry, "kind", "lo_offset");
            set.spurs.push_back(SpurEntry{
                .kind = kind == "absolute" ? SpurKind::Absolute : SpurKind::LoOffset,
                .hz = toml_util::getDouble(*entry, "hz", 0.0),
                .widthHz = toml_util::getDouble(*entry, "width_hz", 0.0),
                .automatic = false,
            });
        }
    }

    return set;
}

Status CorrectionSet::save(const std::filesystem::path& path) const {
    ::toml::table root;
    ::toml::table& calibration = toml_util::ensureTable(root, "calibration");
    calibration.insert_or_assign("learned_at", learnedAt);

    ::toml::table& contextTable = toml_util::ensureTable(root, "calibration.context");
    contextTable.insert_or_assign("driver", context.driver);
    contextTable.insert_or_assign("device", context.deviceId);
    contextTable.insert_or_assign("dc_removal", context.dcRemoval);

    ::toml::table& parameters = toml_util::ensureTable(root, "calibration.context.parameters");
    for (const auto& [key, value] : context.parameters) {
        parameters.insert_or_assign(key, value);
    }

    if (!floor.empty()) {
        ::toml::table& floorTable = toml_util::ensureTable(root, "calibration.floor");
        floorTable.insert_or_assign("sample_rate", floor.sampleRate);
        ::toml::array levels;
        for (const float level : floor.levelDb) {
            levels.push_back(static_cast<double>(level));
        }
        floorTable.insert_or_assign("level_db", std::move(levels));
    }

    ::toml::array spurArray;
    for (const SpurEntry& spur : spurs) {
        if (spur.automatic) {
            continue;
        }
        ::toml::table entry;
        entry.insert_or_assign("kind", std::string(toString(spur.kind)));
        entry.insert_or_assign("hz", spur.hz);
        entry.insert_or_assign("width_hz", spur.widthHz);
        spurArray.push_back(std::move(entry));
    }
    calibration.insert_or_assign("spurs", std::move(spurArray));

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    return toml_util::save(path, root, "Sweep++ receiver corrections");
}

std::filesystem::path CorrectionSet::pathFor(const SdrDeviceInfo& info) {
    return Paths::instance().calibrationDir() /
           (sanitisedFileStem(AntennaAssignments::deviceKey(info)) + ".toml");
}

void applyCorrections(std::span<float> bins, double centerHz, double sampleRate,
                      const CorrectionSet& set, CorrectionSettings settings,
                      CorrectionScratch& scratch) {
    const std::size_t count = bins.size();
    if (count == 0 || sampleRate <= 0.0 || (!settings.flatten && !settings.spurMask)) {
        return;
    }

    if (scratch.binCount != count || scratch.sampleRate != sampleRate) {
        prepareScratch(scratch, count, sampleRate, set);
    }

    if (settings.flatten && !set.floor.empty()) {
        for (std::size_t i = 0; i < count; ++i) {
            if (measured(bins[i])) {
                bins[i] -= scratch.floorDelta[i];
            }
        }
    }

    if (!settings.spurMask || set.spurs.empty()) {
        return;
    }

    // The LO-offset mask is fixed for this grid; the absolute spurs that fall
    // inside this particular frame are added on top of a copy of it.
    std::ranges::copy(scratch.loMask, scratch.mask.begin());
    const double binWidthHz = sampleRate / static_cast<double>(count);
    const double startHz = centerHz - (sampleRate * 0.5);
    bool anyMasked = scratch.loMasked;
    for (const SpurEntry& spur : set.spurs) {
        if (spur.kind == SpurKind::Absolute) {
            anyMasked |= markRun(scratch.mask, spur.hz - startHz, spur.widthHz * 0.5, binWidthHz);
        }
    }

    if (anyMasked) {
        interpolateMasked(bins, scratch.mask);
    }
}

} // namespace sweeppp
