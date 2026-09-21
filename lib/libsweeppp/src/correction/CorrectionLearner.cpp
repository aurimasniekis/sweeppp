// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/correction/CorrectionLearner.hpp"

#include <algorithm>
#include <cmath>

namespace sweeppp {
namespace {

/// Running median over ±`halfWindow` bins, skipping bins with no reading.
///
/// A bin with nothing measured takes the median of whatever neighbours have
/// one, and the global median when none of them have. O(window) per bin,
/// which is a few hundred milliseconds at the largest transform and runs once
/// per learn.
std::vector<float> runningMedian(std::span<const float> values, std::span<const std::uint8_t> valid,
                                 std::size_t halfWindow) {
    const std::size_t count = values.size();
    std::vector<float> result(count, 0.0F);

    std::vector<float> window;
    window.reserve((2 * halfWindow) + 1);

    std::vector<float> all;
    all.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (valid[i] != 0) {
            all.push_back(values[i]);
        }
    }
    float globalMedian = 0.0F;
    if (!all.empty()) {
        const auto middle = all.begin() + static_cast<std::ptrdiff_t>(all.size() / 2);
        std::nth_element(all.begin(), middle, all.end());
        globalMedian = *middle;
    }

    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t from = i >= halfWindow ? i - halfWindow : 0;
        const std::size_t to = std::min(count - 1, i + halfWindow);

        window.clear();
        for (std::size_t k = from; k <= to; ++k) {
            if (valid[k] != 0) {
                window.push_back(values[k]);
            }
        }
        if (window.empty()) {
            result[i] = globalMedian;
            continue;
        }
        const auto middle = window.begin() + static_cast<std::ptrdiff_t>(window.size() / 2);
        std::nth_element(window.begin(), middle, window.end());
        result[i] = *middle;
    }

    return result;
}

/// Runs of bins standing more than the threshold above the running median,
/// no wider than the limit, as (first bin, bin count).
///
/// The threshold is the fixed minimum or a multiple of the scatter still in
/// the data, whichever is larger. The scatter is read off the data itself,
/// as the median absolute deviation from the running median -- a robust
/// estimate the spurs being looked for cannot inflate.
std::vector<std::pair<std::size_t, std::size_t>> findSpurRuns(std::span<const float> values,
                                                              std::span<const std::uint8_t> valid,
                                                              const LearnParameters& params) {
    const std::vector<float> floor = runningMedian(values, valid, params.medianHalfWindow);
    const std::size_t count = values.size();

    std::vector<float> deviations;
    deviations.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (valid[i] != 0) {
            deviations.push_back(std::abs(values[i] - floor[i]));
        }
    }
    float threshold = params.spurThresholdDb;
    if (!deviations.empty()) {
        const auto middle = deviations.begin() + static_cast<std::ptrdiff_t>(deviations.size() / 2);
        std::nth_element(deviations.begin(), middle, deviations.end());
        // 1.4826 turns a median absolute deviation into a standard deviation
        // for a normal scatter.
        const float sigma = 1.4826F * *middle;
        threshold = std::max(threshold, params.sigmaMultiplier * sigma);
    }

    std::vector<std::pair<std::size_t, std::size_t>> runs;
    std::size_t i = 0;
    while (i < count) {
        if (valid[i] == 0 || values[i] - floor[i] <= threshold) {
            ++i;
            continue;
        }
        const std::size_t first = i;
        while (i < count && valid[i] != 0 && values[i] - floor[i] > threshold) {
            ++i;
        }
        const std::size_t width = i - first;
        if (width <= params.maxSpurBins) {
            runs.emplace_back(first, width);
        }
    }
    return runs;
}

std::vector<SpurEntry> entriesFromRuns(const std::vector<std::pair<std::size_t, std::size_t>>& runs,
                                       double firstBinEdgeHz, double binWidthHz, SpurKind kind) {
    std::vector<SpurEntry> entries;
    entries.reserve(runs.size());
    for (const auto& [first, width] : runs) {
        // The centre of the run: half way across its bins, measured from the
        // first bin's lower edge.
        const double centreHz =
            firstBinEdgeHz +
            ((static_cast<double>(first) + (static_cast<double>(width) * 0.5)) * binWidthHz);
        entries.push_back(SpurEntry{.kind = kind,
                                    .hz = centreHz,
                                    .widthHz = static_cast<double>(width) * binWidthHz,
                                    .automatic = false});
    }
    return entries;
}

} // namespace

void CorrectionLearner::begin(std::size_t binCount, double sampleRate, bool sweeping) {
    m_mean.assign(binCount, 0.0F);
    m_count.assign(binCount, 0);
    m_frames = 0;
    m_sampleRate = sampleRate;
    m_centerHz = 0.0;
    m_sweeping = sweeping;
}

void CorrectionLearner::addFrame(std::span<const float> bins, double centerHz) noexcept {
    if (bins.size() != m_mean.size() || bins.empty()) {
        return;
    }

    for (std::size_t i = 0; i < bins.size(); ++i) {
        const float level = bins[i];
        if (!measured(level)) {
            continue;
        }
        const std::uint32_t seen = m_count[i];
        if (seen == 0) {
            m_mean[i] = level;
        } else {
            m_mean[i] += (level - m_mean[i]) / static_cast<float>(seen + 1);
        }
        m_count[i] = seen + 1;
    }

    m_centerHz = centerHz;
    ++m_frames;
}

FloorShape CorrectionLearner::floorShape(std::size_t points, const LearnParameters& params) const {
    const std::size_t count = m_mean.size();
    if (m_frames == 0 || count == 0 || points == 0) {
        return {};
    }

    std::vector<std::uint8_t> valid(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        valid[i] = m_count[i] > 0 ? 1 : 0;
    }
    const std::vector<float> smoothed = runningMedian(m_mean, valid, params.medianHalfWindow);

    // Resampled by span fraction: bin i's centre sits at (i + 0.5) / count of
    // the way across, and so does sample k at (k + 0.5) / points.
    FloorShape shape;
    shape.sampleRate = m_sampleRate;
    shape.levelDb.resize(points);
    for (std::size_t k = 0; k < points; ++k) {
        const double fraction = (static_cast<double>(k) + 0.5) / static_cast<double>(points);
        const double position = (fraction * static_cast<double>(count)) - 0.5;
        if (position <= 0.0) {
            shape.levelDb[k] = smoothed.front();
        } else if (position >= static_cast<double>(count - 1)) {
            shape.levelDb[k] = smoothed.back();
        } else {
            const auto low = static_cast<std::size_t>(position);
            const auto t = static_cast<float>(position - static_cast<double>(low));
            shape.levelDb[k] = smoothed[low] + ((smoothed[low + 1] - smoothed[low]) * t);
        }
    }
    return shape;
}

std::vector<SpurEntry> CorrectionLearner::loSpurs(const LearnParameters& params) const {
    const std::size_t count = m_mean.size();
    if (m_frames == 0 || count == 0 || m_sampleRate <= 0.0) {
        return {};
    }

    std::vector<std::uint8_t> valid(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        valid[i] = m_count[i] > 0 ? 1 : 0;
    }

    const double binWidthHz = m_sampleRate / static_cast<double>(count);
    const auto runs = findSpurRuns(m_mean, valid, params);

    // Offsets are measured from the lower band edge, -fs/2 from the LO. At a
    // fixed tune the LO never moved, so the same offsets are absolute
    // frequencies against the one centre that was seen.
    if (m_sweeping) {
        return entriesFromRuns(runs, -m_sampleRate * 0.5, binWidthHz, SpurKind::LoOffset);
    }
    return entriesFromRuns(runs, m_centerHz - (m_sampleRate * 0.5), binWidthHz, SpurKind::Absolute);
}

} // namespace sweeppp
