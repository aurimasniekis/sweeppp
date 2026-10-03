// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/ui/TraceStore.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sweeppp/core/Clock.hpp>

namespace sweeppp::ui {
namespace {

/// Bins per entry at each step up the pyramid.
constexpr std::size_t kPyramidFactor = 8;

/// How many entries a pixel column must hold before a level is used.
///
/// An entry lands in one column by its centre, so bins near a column's edge
/// can be counted one column over. At four entries a column that is at most a
/// quarter of a pixel, which no one can see -- and below it the bins are read
/// directly, so a zoomed-in trace is exact.
constexpr double kEntriesPerColumn = 4.0;

/// One level from the one below: the max of the maxes, and the min of the
/// mins that were measured.
void reduceLevel(const float* lows, const float* highs, std::size_t count,
                 std::vector<float>& outLows, std::vector<float>& outHighs) {
    const std::size_t entries = (count + kPyramidFactor - 1) / kPyramidFactor;
    outLows.resize(entries);
    outHighs.resize(entries);

    for (std::size_t e = 0; e < entries; ++e) {
        const std::size_t from = e * kPyramidFactor;
        const std::size_t to = std::min(from + kPyramidFactor, count);
        float low = std::numeric_limits<float>::infinity();
        float high = kUnmeasuredDbfs;
        for (std::size_t i = from; i < to; ++i) {
            high = std::max(high, highs[i]);
            low = highs[i] > kUnmeasuredDbfs ? std::min(low, lows[i]) : low;
        }
        outLows[e] = std::isinf(low) ? kUnmeasuredDbfs : low;
        outHighs[e] = high;
    }
}

} // namespace

std::string_view traceKindName(TraceKind kind) noexcept {
    switch (kind) {
    case TraceKind::Live:
        return "live";
    case TraceKind::MaxHold:
        return "max-hold";
    case TraceKind::MinHold:
        return "min-hold";
    case TraceKind::Average:
        return "average";
    }
    return "live";
}

std::string_view traceKindLabel(TraceKind kind) noexcept {
    switch (kind) {
    case TraceKind::Live:
        return "Live";
    case TraceKind::MaxHold:
        return "Max Hold";
    case TraceKind::MinHold:
        return "Min Hold";
    case TraceKind::Average:
        return "Average";
    }
    return "Live";
}

const Trace& TraceStore::trace(TraceKind kind) const {
    switch (kind) {
    case TraceKind::MaxHold:
        return m_maxHold;
    case TraceKind::MinHold:
        return m_minHold;
    case TraceKind::Average:
        return m_average;
    case TraceKind::Live:
        break;
    }
    return m_live;
}

Trace& TraceStore::trace(TraceKind kind) {
    return const_cast<Trace&>(std::as_const(*this).trace(kind));
}

void TraceStore::update(const SpectrumFrame& frame) {
    if (frame.binsDbfs.empty()) {
        return;
    }

    // A change of grid invalidates every accumulated trace: a max-hold from a
    // different span is not merely stale, its bins mean different frequencies.
    const bool gridChanged = frame.binsDbfs.size() != m_binCount ||
                             std::abs(frame.startHz - m_startHz) > 1e-3 ||
                             std::abs(frame.binWidthHz - m_binWidthHz) > 1e-9;

    if (gridChanged) {
        m_startHz = frame.startHz;
        m_binWidthHz = frame.binWidthHz;
        m_binCount = frame.binsDbfs.size();

        m_live.values.assign(m_binCount, kUnmeasuredDbfs);
        m_maxHold.reset();
        m_minHold.reset();
        m_average.reset();
    }

    if (m_smoothing > 0.0F && m_live.count > 0) {
        // Exponential smoothing of the live trace. Steadier picture, at the
        // cost of blunting exactly the transients a sweeper is looking for --
        // which is why it is off unless asked for.
        for (std::size_t i = 0; i < m_binCount; ++i) {
            m_live.values[i] += (frame.binsDbfs[i] - m_live.values[i]) * (1.0F - m_smoothing);
        }
    } else {
        m_live.values.assign(frame.binsDbfs.begin(), frame.binsDbfs.end());
    }
    ++m_live.count;

    if (m_maxHold.visible) {
        // Decay lets the hold forget.
        //
        // An undecayed max-hold only ever rises, so after a few minutes it is
        // a record of everything that has ever transmitted and says nothing
        // about now -- and there is no way to clear it short of toggling the
        // trace off and on. Bleeding it down at a fixed rate keeps a burst
        // visible long enough to be noticed while still letting the display
        // return to the truth once the burst has stopped.
        //
        // Applied per second rather than per frame so the behaviour does not
        // change with the frame rate, which on a sweep varies with the span.
        float decayDb = 0.0F;
        if (m_maxHoldDecayDbPerSecond > 0.0F && m_lastFrameNs != 0 &&
            frame.hostTimeNs > m_lastFrameNs) {
            decayDb = m_maxHoldDecayDbPerSecond *
                      static_cast<float>(nsToSeconds(frame.hostTimeNs - m_lastFrameNs));
        }

        if (m_maxHold.values.size() != m_binCount) {
            m_maxHold.values.assign(m_live.values.begin(), m_live.values.end());
        } else {
            for (std::size_t i = 0; i < m_binCount; ++i) {
                m_maxHold.values[i] = std::max(m_maxHold.values[i] - decayDb, m_live.values[i]);
            }
        }
        ++m_maxHold.count;
    }
    m_lastFrameNs = frame.hostTimeNs;

    if (m_minHold.visible) {
        if (m_minHold.values.size() != m_binCount) {
            m_minHold.values.assign(m_live.values.begin(), m_live.values.end());
        } else {
            for (std::size_t i = 0; i < m_binCount; ++i) {
                // Never-measured bins must not drag the min-hold to -200 dB.
                if (m_live.values[i] > kUnmeasuredDbfs) {
                    m_minHold.values[i] = std::min(m_minHold.values[i], m_live.values[i]);
                }
            }
        }
        ++m_minHold.count;
    }

    if (m_average.visible) {
        if (m_average.values.size() != m_binCount) {
            m_average.values.assign(m_live.values.begin(), m_live.values.end());
            m_average.count = 1;
        } else {
            // Running mean over a bounded window, so the average keeps
            // responding rather than freezing after a few thousand frames.
            const auto window = static_cast<float>(std::max(m_averageWindow, 1U));
            const float weight = 1.0F / std::min(static_cast<float>(m_average.count + 1), window);
            for (std::size_t i = 0; i < m_binCount; ++i) {
                m_average.values[i] += (m_live.values[i] - m_average.values[i]) * weight;
            }
            ++m_average.count;
        }
    }

    ++m_generation;
}

void TraceStore::resetHolds() {
    m_maxHold.reset();
    m_minHold.reset();
    m_average.reset();
    ++m_generation;
}

void TraceStore::clear() {
    m_live.reset();
    resetHolds();
    m_binCount = 0;
    m_startHz = 0.0;
    m_binWidthHz = 0.0;
    ++m_generation;
}

const TraceStore::Pyramid& TraceStore::pyramid(TraceKind kind) const {
    Pyramid& pyramid = m_pyramids[static_cast<std::size_t>(kind)];
    if (pyramid.built && pyramid.generation == m_generation) {
        return pyramid;
    }
    pyramid.built = true;
    pyramid.generation = m_generation;

    // Up to where a level would be a handful of entries; nothing is zoomed out
    // further than that.
    constexpr std::size_t kSmallestLevel = 64;
    const std::vector<float>& values = trace(kind).values;
    std::size_t levels = 0;
    for (std::size_t count = values.size(); count > kSmallestLevel; count /= kPyramidFactor) {
        ++levels;
    }
    pyramid.lows.resize(levels);
    pyramid.highs.resize(levels);

    const float* lows = values.data();
    const float* highs = values.data();
    std::size_t count = values.size();
    for (std::size_t level = 0; level < levels; ++level) {
        reduceLevel(lows, highs, count, pyramid.lows[level], pyramid.highs[level]);
        lows = pyramid.lows[level].data();
        highs = pyramid.highs[level].data();
        count = pyramid.highs[level].size();
    }
    return pyramid;
}

const Envelope& TraceStore::envelope(TraceKind kind, double fromHz, double toHz, std::size_t pixels,
                                     EnvelopeCache& cache) const {
    EnvelopeCache::Slot& slot = cache.slots[static_cast<std::size_t>(kind)];
    Envelope& out = slot.envelope;

    // Recompute only when something that affects the result changed. Panning
    // the view at 60 fps otherwise recomputes a million-bin envelope every
    // frame for no reason.
    const bool cacheValid = slot.filled && slot.generation == m_generation &&
                            slot.pixels == pixels && std::abs(slot.fromHz - fromHz) < 1e-6 &&
                            std::abs(slot.toHz - toHz) < 1e-6;
    if (cacheValid) {
        return out;
    }

    out.clear();
    slot.filled = true;
    slot.fromHz = fromHz;
    slot.toHz = toHz;
    slot.pixels = pixels;
    slot.generation = m_generation;

    const Trace& source = trace(kind);
    if (source.values.empty() || pixels == 0 || toHz <= fromHz || m_binWidthHz <= 0.0) {
        return out;
    }

    out.minimum.assign(pixels, std::numeric_limits<float>::infinity());
    out.maximum.assign(pixels, -std::numeric_limits<float>::infinity());
    out.frequency.resize(pixels);

    const double span = toHz - fromHz;
    const double perPixel = span / static_cast<double>(pixels);

    for (std::size_t i = 0; i < pixels; ++i) {
        out.frequency[i] = fromHz + perPixel * (static_cast<double>(i) + 0.5);
    }

    // The bins themselves when a column holds only a few, otherwise the
    // coarsest pyramid level that still puts several entries in each column.
    const float* lows = source.values.data();
    const float* highs = source.values.data();
    std::size_t entries = source.values.size();
    std::size_t stride = 1;
    if (const double binsPerColumn = perPixel / m_binWidthHz;
        binsPerColumn >= static_cast<double>(kPyramidFactor) * kEntriesPerColumn) {
        const Pyramid& levels = pyramid(kind);
        for (std::size_t level = 0; level < levels.highs.size(); ++level) {
            const std::size_t next = stride * kPyramidFactor;
            if (static_cast<double>(next) * kEntriesPerColumn > binsPerColumn) {
                break;
            }
            stride = next;
            lows = levels.lows[level].data();
            highs = levels.highs[level].data();
            entries = levels.highs[level].size();
        }
    }

    // Walk the entries once and drop each into its pixel column, so every bin
    // is counted in exactly one -- a narrow signal cannot be skipped however
    // far zoomed out the view is.
    const auto firstBin =
        static_cast<std::ptrdiff_t>(std::floor((fromHz - m_startHz) / m_binWidthHz));
    const auto lastBin = static_cast<std::ptrdiff_t>(std::ceil((toHz - m_startHz) / m_binWidthHz));

    const auto entryStride = static_cast<std::ptrdiff_t>(stride);
    const std::ptrdiff_t begin = std::max<std::ptrdiff_t>(firstBin, 0) / entryStride;
    const std::ptrdiff_t end = std::min<std::ptrdiff_t>((lastBin + entryStride - 1) / entryStride,
                                                        static_cast<std::ptrdiff_t>(entries));
    const double entryWidthHz = m_binWidthHz * static_cast<double>(stride);

    for (std::ptrdiff_t entry = begin; entry < end; ++entry) {
        const float high = highs[static_cast<std::size_t>(entry)];
        if (high <= kUnmeasuredDbfs) {
            continue;
        }

        const double hz = m_startHz + entryWidthHz * (static_cast<double>(entry) + 0.5);
        const auto column = static_cast<std::ptrdiff_t>((hz - fromHz) / perPixel);
        if (column < 0 || column >= static_cast<std::ptrdiff_t>(pixels)) {
            continue;
        }

        const auto index = static_cast<std::size_t>(column);
        out.minimum[index] = std::min(out.minimum[index], lows[static_cast<std::size_t>(entry)]);
        out.maximum[index] = std::max(out.maximum[index], high);
    }

    // Columns with no bin at all -- a view zoomed in past the bin spacing, or
    // a gap between discontinuous sweep segments. Interpolate across short
    // gaps so a zoomed-in trace is continuous, but leave wide ones empty so a
    // genuine gap in coverage stays visible.
    //
    // Only *between* measured columns, though. Carrying the last value forward
    // with no right-hand bound paints the trace flat from the final measured
    // bin all the way to the edge of the view, which reads as a strong signal
    // filling the band rather than as the absence of data it is. The left edge
    // never showed the same fault only because there is nothing to carry
    // before the first measured column.
    std::size_t lastMeasured = 0;
    for (std::size_t i = 0; i < pixels; ++i) {
        if (!std::isinf(out.maximum[i])) {
            lastMeasured = i;
        }
    }

    float lastMin = 0.0F;
    float lastMax = 0.0F;
    bool haveLast = false;

    for (std::size_t i = 0; i < pixels; ++i) {
        if (std::isinf(out.maximum[i])) {
            if (haveLast && perPixel < m_binWidthHz && i < lastMeasured) {
                out.minimum[i] = lastMin;
                out.maximum[i] = lastMax;
            } else {
                out.minimum[i] = kUnmeasuredDbfs;
                out.maximum[i] = kUnmeasuredDbfs;
            }
        } else {
            lastMin = out.minimum[i];
            lastMax = out.maximum[i];
            haveLast = true;
        }
    }

    return out;
}

float TraceStore::levelAt(double hz) const {
    if (m_live.values.empty() || m_binWidthHz <= 0.0) {
        return kUnmeasuredDbfs;
    }

    const auto bin = static_cast<std::ptrdiff_t>((hz - m_startHz) / m_binWidthHz);
    if (bin < 0 || bin >= static_cast<std::ptrdiff_t>(m_live.values.size())) {
        return kUnmeasuredDbfs;
    }
    return m_live.values[static_cast<std::size_t>(bin)];
}

bool TraceStore::peakIn(double fromHz, double toHz, double& outHz, float& outDb) const {
    if (m_live.values.empty() || m_binWidthHz <= 0.0) {
        return false;
    }

    const auto firstBin = std::max<std::ptrdiff_t>(
        static_cast<std::ptrdiff_t>((fromHz - m_startHz) / m_binWidthHz), 0);
    const auto lastBin =
        std::min<std::ptrdiff_t>(static_cast<std::ptrdiff_t>((toHz - m_startHz) / m_binWidthHz) + 1,
                                 static_cast<std::ptrdiff_t>(m_live.values.size()));

    if (firstBin >= lastBin) {
        return false;
    }

    float best = -std::numeric_limits<float>::infinity();
    std::ptrdiff_t bestBin = -1;

    for (std::ptrdiff_t bin = firstBin; bin < lastBin; ++bin) {
        const float value = m_live.values[static_cast<std::size_t>(bin)];
        if (value > kUnmeasuredDbfs && value > best) {
            best = value;
            bestBin = bin;
        }
    }

    if (bestBin < 0) {
        return false;
    }

    outHz = m_startHz + m_binWidthHz * (static_cast<double>(bestBin) + 0.5);
    outDb = best;
    return true;
}

} // namespace sweeppp::ui
