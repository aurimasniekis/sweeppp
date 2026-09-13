// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "HistoryView.hpp"

#include <algorithm>
#include <cmath>
#include <sweeppp_gl.h>

namespace sweeppp::ui {
namespace {

/// Smallest window the view may zoom into, so a scroll cannot collapse the
/// range to nothing and leave the pane blank with no way back.
constexpr std::uint64_t kMinSpanNs = 1'000'000;
constexpr double kMinSpanHz = 100.0;

/// Roughly how many stored lines a freshly opened session shows.
///
/// Chosen to sit near one line per pixel row on a typical pane. Opening on the
/// whole recording instead decimated an hour of sweeps down to whatever the
/// pane was tall, which both threw away the detail the file was captured with
/// and left the view with nowhere to scroll to.
constexpr std::uint64_t kDefaultVisibleLines = 1200;

/// Reduces the stored samples one destination pixel covers to a single level.
///
/// The reduction is the whole reason the history used to look nothing like the
/// live waterfall. Live averages the bins sharing a pixel and shows one stored
/// line per screen row; taking the max over both instead -- on tiles that are
/// themselves max-hold above LOD 1 -- is a max of a max of a max, which lifts
/// the apparent noise floor several dB and walks it out of the dark end of the
/// gradient. Same colormap, same limits, a washed-out picture.
///
/// Raw bytes rather than dequantised values: within one tile they share an
/// origin, so the byte scale is the dB scale shifted, and both reductions
/// commute with that shift. One dequantise at the end instead of per sample.
///
/// Byte 0 is coverage rather than a level and takes part in neither reduction,
/// so a pixel that covers a gap and a measurement shows the measurement, and
/// one covering only gap comes back unmeasured for the caller to leave as
/// background. Averaging it in would drag the level down towards a floor
/// nothing recorded, and how many samples a pixel covers depends on the zoom.
[[nodiscard]] std::uint8_t reduceSamples(const session::HistoryTile& tile, std::int64_t lineFrom,
                                         std::int64_t lineTo, std::int64_t binFrom,
                                         std::int64_t binTo, bool peak) {
    if (peak) {
        std::uint8_t loudest = session::kUnmeasuredByte;
        for (std::int64_t line = lineFrom; line < lineTo; ++line) {
            for (std::int64_t bin = binFrom; bin < binTo; ++bin) {
                loudest = std::max(loudest, tile.at(static_cast<std::uint32_t>(line),
                                                    static_cast<std::uint32_t>(bin)));
            }
        }
        return loudest;
    }

    std::uint64_t total = 0;
    std::uint64_t count = 0;
    for (std::int64_t line = lineFrom; line < lineTo; ++line) {
        for (std::int64_t bin = binFrom; bin < binTo; ++bin) {
            const std::uint8_t value =
                tile.at(static_cast<std::uint32_t>(line), static_cast<std::uint32_t>(bin));
            if (session::isMeasured(value)) {
                total += value;
                ++count;
            }
        }
    }
    return count > 0 ? static_cast<std::uint8_t>((total + count / 2) / count)
                     : session::kUnmeasuredByte;
}

} // namespace

HistoryView::~HistoryView() {
    // No GL call here: the destructor may run after the context is gone, and
    // deleting a texture without a current context is undefined. close() is
    // the ordered path and the window calls it.
}

void HistoryView::adopt(std::unique_ptr<session::SessionReader> reader,
                        std::filesystem::path path) {
    m_reader = std::move(reader);
    m_path = std::move(path);
    m_lastError.clear();
    resetView();
}

void HistoryView::close() {
    if (m_texture != 0) {
        glDeleteTextures(1, &m_texture);
        m_texture = 0;
    }
    if (m_overviewTexture != 0) {
        glDeleteTextures(1, &m_overviewTexture);
        m_overviewTexture = 0;
    }
    m_reader.reset();
    m_path.clear();
    m_pixels.clear();
    m_width = 0;
    m_height = 0;
    m_lastLod = 0;
    m_lastLines = 0;
}

void HistoryView::resetView() {
    if (!m_reader) {
        return;
    }

    const session::SessionSummary& summary = m_reader->summary();
    m_fromHz = summary.lowestHz;
    m_toHz = std::max(summary.highestHz, summary.lowestHz + kMinSpanHz);

    // A window into the recording rather than the whole of it, so the waterfall
    // opens at the detail the file was captured with and the session becomes
    // something to travel through. Anchored at the start, which is where the
    // transport's own beginning is.
    m_fromNs = summary.firstLineNs;
    m_toNs = m_fromNs + defaultTimeSpanNs();
    invalidate();
}

std::uint64_t HistoryView::defaultTimeSpanNs() const {
    if (!m_reader) {
        return kMinSpanNs;
    }

    const session::SessionSummary& summary = m_reader->summary();
    const std::uint64_t sessionSpan =
        summary.lastLineNs > summary.firstLineNs ? summary.lastLineNs - summary.firstLineNs : 0;

    if (sessionSpan == 0 || summary.totalLines <= kDefaultVisibleLines) {
        return std::max(sessionSpan, kMinSpanNs);
    }

    const auto span = static_cast<std::uint64_t>(static_cast<double>(sessionSpan) *
                                                 static_cast<double>(kDefaultVisibleLines) /
                                                 static_cast<double>(summary.totalLines));
    // Widened then capped, rather than clamped between the two: a recording
    // shorter than the minimum span would hand std::clamp a low bound above its
    // high one.
    return std::min(std::max(span, kMinSpanNs), sessionSpan);
}

void HistoryView::resetTimeZoom() {
    if (!m_reader) {
        return;
    }

    // The zoom returns to its default, the position does not. Sending the view
    // back to the start as well would mean losing the place you had navigated
    // to, which is rarely what "undo my zooming" is asking for.
    const std::uint64_t span = defaultTimeSpanNs();
    const auto centre =
        static_cast<double>(m_fromNs) + static_cast<double>(m_toNs - m_fromNs) * 0.5;

    const session::SessionSummary& summary = m_reader->summary();
    const auto sessionFrom = static_cast<double>(summary.firstLineNs);
    const auto sessionTo = static_cast<double>(summary.lastLineNs);

    double from = centre - static_cast<double>(span) * 0.5;
    from =
        std::clamp(from, sessionFrom, std::max(sessionFrom, sessionTo - static_cast<double>(span)));

    setTimeRange(static_cast<std::uint64_t>(from), static_cast<std::uint64_t>(from) + span);
}

void HistoryView::setTimeRange(std::uint64_t fromNs, std::uint64_t toNs) {
    if (toNs <= fromNs + kMinSpanNs) {
        toNs = fromNs + kMinSpanNs;
    }

    // Never begins before the recording does.
    //
    // Seeking to an instant centres the window on it, so seeking to the very
    // first line put the top of the view half a window earlier than anything
    // that exists. There is nothing to draw up there, and the time axis
    // subtracts the session start from each label in unsigned arithmetic, so
    // the labels wrapped into timestamps centuries long.
    if (m_reader) {
        const std::uint64_t first = m_reader->summary().firstLineNs;
        if (fromNs < first) {
            const std::uint64_t span = toNs - fromNs;
            fromNs = first;
            toNs = first + span;
        }
    }

    // Compared before assigning, because the window re-asserts its range every
    // frame. Invalidating unconditionally would recomposite the whole pane on
    // every one of them, which is exactly the per-frame cost the LOD pyramid
    // exists to avoid.
    if (fromNs == m_fromNs && toNs == m_toNs) {
        return;
    }
    m_fromNs = fromNs;
    m_toNs = toNs;
    invalidate();
}

void HistoryView::setFrequencyRange(double fromHz, double toHz) {
    if (toHz <= fromHz + kMinSpanHz) {
        toHz = fromHz + kMinSpanHz;
    }
    if (fromHz == m_fromHz && toHz == m_toHz) {
        return;
    }
    m_fromHz = fromHz;
    m_toHz = toHz;
    invalidate();
}

void HistoryView::zoomTime(double factor, double anchor) {
    if (!m_reader) {
        return;
    }

    const auto span = static_cast<double>(m_toNs - m_fromNs);
    const double pivot = static_cast<double>(m_fromNs) + span * std::clamp(anchor, 0.0, 1.0);
    const double scaled = std::max(span * factor, static_cast<double>(kMinSpanNs));

    // Held under the pointer: the pivot keeps the same fractional position, so
    // whatever was being examined stays there instead of sliding away.
    const double from = pivot - (pivot - static_cast<double>(m_fromNs)) * (scaled / span);

    // Clamped to the session rather than allowed to drift past its ends, which
    // would leave the operator staring at emptiness with no cue which way back.
    const session::SessionSummary& summary = m_reader->summary();
    const auto sessionFrom = static_cast<double>(summary.firstLineNs);
    const auto sessionTo = static_cast<double>(summary.lastLineNs);

    double clampedFrom = std::max(from, sessionFrom);
    if (clampedFrom + scaled > sessionTo && sessionTo - sessionFrom > scaled) {
        clampedFrom = sessionTo - scaled;
    }

    setTimeRange(static_cast<std::uint64_t>(clampedFrom),
                 static_cast<std::uint64_t>(clampedFrom + scaled));
}

void HistoryView::panTime(double fraction) {
    if (!m_reader) {
        return;
    }

    const auto span = static_cast<double>(m_toNs - m_fromNs);
    const double shift = span * fraction;

    const session::SessionSummary& summary = m_reader->summary();
    const auto sessionFrom = static_cast<double>(summary.firstLineNs);
    const auto sessionTo = static_cast<double>(summary.lastLineNs);

    double from = static_cast<double>(m_fromNs) + shift;
    from = std::clamp(from, sessionFrom, std::max(sessionFrom, sessionTo - span));

    setTimeRange(static_cast<std::uint64_t>(from), static_cast<std::uint64_t>(from + span));
}

void HistoryView::zoomFrequency(double factor, double anchor) {
    if (!m_reader) {
        return;
    }

    const double span = m_toHz - m_fromHz;
    const double pivot = m_fromHz + span * std::clamp(anchor, 0.0, 1.0);
    const double scaled = std::max(span * factor, kMinSpanHz);
    const double from = pivot - (pivot - m_fromHz) * (scaled / span);

    const session::SessionSummary& summary = m_reader->summary();
    double clampedFrom = std::max(from, summary.lowestHz);
    if (clampedFrom + scaled > summary.highestHz && summary.highestHz - summary.lowestHz > scaled) {
        clampedFrom = summary.highestHz - scaled;
    }

    setFrequencyRange(clampedFrom, clampedFrom + scaled);
}

void HistoryView::panFrequency(double fraction) {
    if (!m_reader) {
        return;
    }

    const double span = m_toHz - m_fromHz;
    const session::SessionSummary& summary = m_reader->summary();

    double from = m_fromHz + span * fraction;
    from = std::clamp(from, summary.lowestHz, std::max(summary.lowestHz, summary.highestHz - span));

    setFrequencyRange(from, from + span);
}

void HistoryView::setColorMap(const ColorMap& map) {
    // Compared by baked table rather than by name: the gradient editor mutates
    // stops in place under an unchanged name, so a name check would leave both
    // images showing the colours the operator just edited away from.
    //
    // The revision is what the overview watches. It has no dirty flag of its
    // own -- it is rebuilt from a description of what it last drew -- so a
    // counter is how a recolour reaches it, and without one the strip kept the
    // old palette while the waterfall above it changed.
    if (m_colorMap.lut() == map.lut()) {
        return;
    }
    m_colorMap = map;
    ++m_colorMapRevision;
    invalidate();
}

void HistoryView::setGradientRange(float minDb, float maxDb) {
    if (maxDb <= minDb) {
        maxDb = minDb + 1.0F;
    }
    if (minDb != m_minDb || maxDb != m_maxDb) {
        m_minDb = minDb;
        m_maxDb = maxDb;
        invalidate();
    }
}

void HistoryView::setPeakDetect(bool peak) {
    if (peak != m_peakDetect) {
        m_peakDetect = peak;
        invalidate();
    }
}

std::uint64_t HistoryView::timeAt(double fraction) const noexcept {
    const auto span = static_cast<double>(m_toNs - m_fromNs);
    return m_fromNs + static_cast<std::uint64_t>(span * std::clamp(fraction, 0.0, 1.0));
}

double HistoryView::frequencyAt(double fraction) const noexcept {
    return m_fromHz + (m_toHz - m_fromHz) * std::clamp(fraction, 0.0, 1.0);
}

std::optional<std::uint32_t> HistoryView::segmentAt(std::uint64_t monotonicNs) const {
    if (!m_reader) {
        return std::nullopt;
    }

    for (const session::SegmentInfo& segment : m_reader->segments()) {
        const bool openEnded = segment.endMonotonicNs == 0;
        if (monotonicNs >= segment.startMonotonicNs &&
            (openEnded || monotonicNs <= segment.endMonotonicNs)) {
            return segment.id;
        }
    }
    return std::nullopt;
}

std::uint32_t HistoryView::texture(std::uint32_t pixelWidth, std::uint32_t pixelHeight) {
    if (!m_reader || pixelWidth == 0 || pixelHeight == 0) {
        return 0;
    }

    if (m_dirty || pixelWidth != m_width || pixelHeight != m_height) {
        composite(pixelWidth, pixelHeight);
        upload();
        m_dirty = false;
    }

    return m_texture;
}

void HistoryView::composite(std::uint32_t width, std::uint32_t height) {
    m_width = width;
    m_height = height;
    m_lastLod = 0;
    m_lastLines = 0;

    // Accumulated as gradient indices, not as colours.
    //
    // Several bins routinely share a column, and the reduction has to be max
    // *level*. Taking the max of two packed RGBA words instead compares raw
    // integers, whose ordering follows the colormap's byte layout rather than
    // signal strength -- so on a palette that darkens at the top the louder of
    // two bins would lose. Colour is applied once, at the end.
    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
    m_indices.assign(pixelCount, 0);
    m_written.assign(pixelCount, 0);

    session::HistoryQuery request;
    request.fromNs = m_fromNs;
    request.toNs = m_toNs;
    request.fromHz = m_fromHz;
    request.toHz = m_toHz;
    // Asking for exactly the pane's size is the whole point of the LOD
    // pyramid: the reader picks a level whose spacing fits, so the cost of
    // drawing is set by the window rather than by how long the session ran.
    request.maxLines = height;
    request.maxBins = width;

    auto tiles = m_reader->query(request);
    if (!tiles) {
        m_lastError = tiles.error().describe();
        return;
    }

    const auto timeSpan = static_cast<double>(m_toNs - m_fromNs);
    const double freqSpan = m_toHz - m_fromHz;
    if (timeSpan <= 0.0 || freqSpan <= 0.0) {
        return;
    }

    const float dbSpan = m_maxDb - m_minDb;
    const float scale = dbSpan > 0.0F ? static_cast<float>(ColorMap::kLutSize - 1) / dbSpan : 0.0F;

    for (const session::HistoryTile& tile : *tiles) {
        m_lastLod = std::max(m_lastLod, tile.lod);

        if (tile.lines == 0 || tile.bins == 0) {
            continue;
        }

        // Tiles carry their own extents because the reader may have served a
        // coarser level than asked for, and because a query spanning a
        // parameter change returns tiles from segments on different grids.
        // Each is placed by its own geometry rather than by an assumed one.
        //
        // Walked destination-first, sampling the source, rather than scattering
        // each stored line at whatever row it lands on.
        //
        // Scattering only works while there are more source lines than pixel
        // rows. Zoomed in -- or on a short session -- there are fewer, so most
        // rows are never written and the waterfall comes out as thin streaks
        // separated by background. Asking each destination pixel what covers it
        // fills every one, and still reduces by max where several samples share
        // it, which is the behaviour that matters when zoomed out.
        const auto tileFirst = static_cast<double>(tile.firstLineNs);
        const auto tileLast = static_cast<double>(tile.lastLineNs);
        const double nsPerLine = tile.lines > 1
                                     ? (tileLast - tileFirst) / static_cast<double>(tile.lines - 1)
                                     : std::max(tileLast - tileFirst, 1.0);

        // Destination rows this tile can contribute to, so the scan is bounded
        // by the tile rather than by the pane.
        const double rowsPerNs = static_cast<double>(height) / timeSpan;
        const auto firstRow = static_cast<std::int64_t>(
            std::floor((tileFirst - nsPerLine * 0.5 - static_cast<double>(m_fromNs)) * rowsPerNs));
        const auto lastRow = static_cast<std::int64_t>(
            std::ceil((tileLast + nsPerLine * 0.5 - static_cast<double>(m_fromNs)) * rowsPerNs));

        const auto rowBegin = static_cast<std::uint32_t>(std::max<std::int64_t>(firstRow, 0));
        const auto rowEnd = static_cast<std::uint32_t>(std::min<std::int64_t>(lastRow + 1, height));

        for (std::uint32_t row = rowBegin; row < rowEnd; ++row) {
            // The time interval this row stands for, converted to the lines
            // covering it. When the view is zoomed in past the line spacing the
            // interval falls between two lines, and the nearest one is used --
            // which is what makes a magnified waterfall continuous instead of
            // striped.
            const double rowTop =
                static_cast<double>(m_fromNs) +
                timeSpan * (static_cast<double>(row) / static_cast<double>(height));
            const double rowBottom =
                static_cast<double>(m_fromNs) +
                timeSpan * (static_cast<double>(row + 1) / static_cast<double>(height));

            auto lineFrom = static_cast<std::int64_t>(std::floor((rowTop - tileFirst) / nsPerLine));
            auto lineTo = static_cast<std::int64_t>(std::ceil((rowBottom - tileFirst) / nsPerLine));
            if (lineTo <= lineFrom) {
                lineTo = lineFrom + 1;
            }
            lineFrom = std::clamp<std::int64_t>(lineFrom, 0, tile.lines - 1);
            lineTo = std::clamp<std::int64_t>(lineTo, lineFrom + 1, tile.lines);

            const std::size_t rowOffset = static_cast<std::size_t>(row) * width;

            const double columnsPerHz = static_cast<double>(width) / freqSpan;
            const auto colBegin = static_cast<std::uint32_t>(std::max<std::int64_t>(
                static_cast<std::int64_t>(std::floor((tile.startHz - m_fromHz) * columnsPerHz)),
                0));
            const auto colEnd = static_cast<std::uint32_t>(std::min<std::int64_t>(
                static_cast<std::int64_t>(std::ceil((tile.stopHz() - m_fromHz) * columnsPerHz)) + 1,
                width));

            for (std::uint32_t column = colBegin; column < colEnd; ++column) {
                const double colLeft = m_fromHz + freqSpan * (static_cast<double>(column) /
                                                              static_cast<double>(width));
                const double colRight = m_fromHz + freqSpan * (static_cast<double>(column + 1) /
                                                               static_cast<double>(width));

                auto binFrom = static_cast<std::int64_t>(
                    std::floor((colLeft - tile.startHz) / tile.binWidthHz));
                auto binTo = static_cast<std::int64_t>(
                    std::ceil((colRight - tile.startHz) / tile.binWidthHz));
                if (binTo <= binFrom) {
                    binTo = binFrom + 1;
                }
                if (binTo <= 0 || binFrom >= static_cast<std::int64_t>(tile.bins)) {
                    continue;
                }
                binFrom = std::clamp<std::int64_t>(binFrom, 0, tile.bins - 1);
                binTo = std::clamp<std::int64_t>(binTo, binFrom + 1, tile.bins);

                const std::uint8_t level =
                    reduceSamples(tile, lineFrom, lineTo, binFrom, binTo, m_peakDetect);
                // Nothing measured here, so nothing is drawn here: the pixel
                // stays background rather than taking the bottom of the
                // gradient, which is what a gap between two spans has to look
                // like.
                if (!session::isMeasured(level)) {
                    continue;
                }

                // Requantised against the display's gradient, not the tile's.
                // The stored origin is per tile and chosen from that tile's own
                // range, so drawing the raw bytes would make neighbouring tiles
                // disagree about what a given shade means.
                const auto db = static_cast<float>(
                    session::dequantiseDb(level, static_cast<double>(tile.originDb)));
                const auto index = static_cast<std::uint8_t>(std::clamp(
                    (db - m_minDb) * scale, 0.0F, static_cast<float>(ColorMap::kLutSize - 1)));

                const std::size_t offset = rowOffset + column;
                if (m_written[offset] == 0 || index > m_indices[offset]) {
                    m_indices[offset] = index;
                }
                m_written[offset] = 1;
            }
        }

        m_lastLines = std::max(m_lastLines, tile.lines);
    }

    // Colour last, once every column holds the loudest level that landed in it.
    // Unwritten pixels stay at the background rather than taking the bottom of
    // the gradient, so a gap in coverage does not read as a quiet band.
    const auto& lut = m_colorMap.lut();
    m_pixels.resize(pixelCount);
    for (std::size_t i = 0; i < pixelCount; ++i) {
        m_pixels[i] = m_written[i] != 0 ? lut[m_indices[i]] : 0xFF000000U;
    }
}

std::uint32_t HistoryView::overviewTexture(std::uint32_t pixelWidth, std::uint32_t pixelHeight,
                                           double centerHz, double bandwidthHz) {
    if (!m_reader || pixelWidth == 0 || pixelHeight == 0 || bandwidthHz <= 0.0) {
        return 0;
    }

    const session::SessionSummary& summary = m_reader->summary();

    // Slid back inside the recording rather than centred blindly.
    //
    // A band as wide as the session centred on a marker near either edge runs
    // half off the end, so most of the strip would be empty and moving the
    // marker would appear to do nothing but shift that emptiness around.
    const double width = std::min(bandwidthHz, summary.highestHz - summary.lowestHz);
    double bandFrom = centerHz - width * 0.5;
    bandFrom = std::clamp(bandFrom, summary.lowestHz, summary.highestHz - width);

    // Resolved first, because the cache turns on the band that comes out of
    // this rather than on the centre that went in. A whole-span band clamps to
    // the same edge whatever it is centred on, and the caller re-centres it on
    // the visible window -- so comparing the request would rebuild the entire
    // strip on every frame of a frequency pan that cannot move it.
    const bool unchanged = m_overviewTexture != 0 && pixelWidth == m_overviewWidth &&
                           pixelHeight == m_overviewHeight && bandFrom == m_overviewBandFromHz &&
                           bandFrom + width == m_overviewBandToHz &&
                           summary.firstLineNs == m_overviewFromNs &&
                           summary.lastLineNs == m_overviewToNs && m_minDb == m_overviewMinDb &&
                           m_maxDb == m_overviewMaxDb && m_peakDetect == m_overviewPeakDetect &&
                           m_colorMapRevision == m_overviewColorMapRevision;
    if (unchanged) {
        return m_overviewTexture;
    }

    m_overviewWidth = pixelWidth;
    m_overviewHeight = pixelHeight;
    m_overviewMinDb = m_minDb;
    m_overviewMaxDb = m_maxDb;
    m_overviewPeakDetect = m_peakDetect;
    m_overviewColorMapRevision = m_colorMapRevision;
    m_overviewFromNs = summary.firstLineNs;
    m_overviewToNs = summary.lastLineNs;
    m_overviewBandFromHz = bandFrom;
    m_overviewBandToHz = bandFrom + width;

    session::HistoryQuery request;
    request.fromNs = m_overviewFromNs;
    request.toNs = m_overviewToNs;
    request.fromHz = bandFrom;
    request.toHz = bandFrom + width;
    // Time runs across here, so it is the *width* that bounds how many lines
    // are worth reading.
    request.maxLines = pixelWidth;
    request.maxBins = pixelHeight;

    auto tiles = m_reader->query(request);
    if (!tiles) {
        m_lastError = tiles.error().describe();
        return 0;
    }

    const auto timeSpan = static_cast<double>(m_overviewToNs - m_overviewFromNs);
    if (timeSpan <= 0.0) {
        return 0;
    }

    const std::size_t pixelCount = static_cast<std::size_t>(pixelWidth) * pixelHeight;
    std::vector<std::uint8_t> indices(pixelCount, 0);
    std::vector<std::uint8_t> written(pixelCount, 0);

    const float dbSpan = m_maxDb - m_minDb;
    const float scale = dbSpan > 0.0F ? static_cast<float>(ColorMap::kLutSize - 1) / dbSpan : 0.0F;

    for (const session::HistoryTile& tile : *tiles) {
        if (tile.lines == 0 || tile.bins == 0) {
            continue;
        }

        const auto tileFirst = static_cast<double>(tile.firstLineNs);
        const auto tileLast = static_cast<double>(tile.lastLineNs);
        const double nsPerLine = tile.lines > 1
                                     ? (tileLast - tileFirst) / static_cast<double>(tile.lines - 1)
                                     : std::max(tileLast - tileFirst, 1.0);

        for (std::uint32_t column = 0; column < pixelWidth; ++column) {
            const double columnFrom =
                static_cast<double>(m_overviewFromNs) +
                timeSpan * (static_cast<double>(column) / static_cast<double>(pixelWidth));
            const double columnTo =
                static_cast<double>(m_overviewFromNs) +
                timeSpan * (static_cast<double>(column + 1) / static_cast<double>(pixelWidth));

            auto lineFrom =
                static_cast<std::int64_t>(std::floor((columnFrom - tileFirst) / nsPerLine));
            auto lineTo = static_cast<std::int64_t>(std::ceil((columnTo - tileFirst) / nsPerLine));
            if (lineTo <= 0 || lineFrom >= static_cast<std::int64_t>(tile.lines)) {
                continue;
            }
            lineFrom = std::clamp<std::int64_t>(lineFrom, 0, tile.lines - 1);
            lineTo = std::clamp<std::int64_t>(lineTo, lineFrom + 1, tile.lines);

            for (std::uint32_t row = 0; row < pixelHeight; ++row) {
                const double rowFrom = request.fromHz + width * (static_cast<double>(row) /
                                                                 static_cast<double>(pixelHeight));
                const double rowTo = request.fromHz + width * (static_cast<double>(row + 1) /
                                                               static_cast<double>(pixelHeight));

                auto binFrom = static_cast<std::int64_t>(
                    std::floor((rowFrom - tile.startHz) / tile.binWidthHz));
                auto binTo =
                    static_cast<std::int64_t>(std::ceil((rowTo - tile.startHz) / tile.binWidthHz));
                if (binTo <= 0 || binFrom >= static_cast<std::int64_t>(tile.bins)) {
                    continue;
                }
                binFrom = std::clamp<std::int64_t>(binFrom, 0, tile.bins - 1);
                binTo = std::clamp<std::int64_t>(binTo, binFrom + 1, tile.bins);

                const std::uint8_t level =
                    reduceSamples(tile, lineFrom, lineTo, binFrom, binTo, m_peakDetect);
                if (!session::isMeasured(level)) {
                    continue;
                }

                const auto db = static_cast<float>(
                    session::dequantiseDb(level, static_cast<double>(tile.originDb)));
                const auto index = static_cast<std::uint8_t>(std::clamp(
                    (db - m_minDb) * scale, 0.0F, static_cast<float>(ColorMap::kLutSize - 1)));

                const std::size_t offset = static_cast<std::size_t>(row) * pixelWidth + column;
                if (written[offset] == 0 || index > indices[offset]) {
                    indices[offset] = index;
                }
                written[offset] = 1;
            }
        }
    }

    const auto& lut = m_colorMap.lut();
    m_overviewPixels.resize(pixelCount);
    for (std::size_t i = 0; i < pixelCount; ++i) {
        m_overviewPixels[i] = written[i] != 0 ? lut[indices[i]] : 0xFF000000U;
    }

    if (m_overviewTexture == 0) {
        glGenTextures(1, &m_overviewTexture);
        glBindTexture(GL_TEXTURE_2D, m_overviewTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, m_overviewTexture);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(pixelWidth),
                 static_cast<GLsizei>(pixelHeight), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 m_overviewPixels.data());

    return m_overviewTexture;
}

void HistoryView::upload() {
    if (m_pixels.empty()) {
        return;
    }

    if (m_texture == 0) {
        glGenTextures(1, &m_texture);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, m_texture);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(m_width),
                 static_cast<GLsizei>(m_height), 0, GL_RGBA, GL_UNSIGNED_BYTE, m_pixels.data());
}

} // namespace sweeppp::ui
