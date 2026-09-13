// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <sweeppp/history/SessionReader.hpp>
#include <sweeppp/ui/ColorMap.hpp>
#include <vector>

namespace sweeppp::ui {

/// A session file rendered as a scrollable, zoomable image.
///
/// Deliberately not the live waterfall's ring. The ring is a fixed number of
/// rows on the GPU and its depth is capped by the largest texture the driver
/// accepts; a session is unbounded and lives on disk. What is drawn here is
/// whatever the current view asks for, resampled to the size of the pane by
/// the reader's own LOD pyramid -- so showing three hours reads a few thousand
/// lines, not three hours of them.
///
/// The composited image is built on the CPU and uploaded as one texture, and
/// only when the view actually changes. Panning and zooming are therefore
/// free; it is only the query that costs anything, and that is bounded by the
/// pane size rather than the session length.
class HistoryView {
public:
    HistoryView() = default;
    ~HistoryView();

    HistoryView(const HistoryView&) = delete;
    HistoryView& operator=(const HistoryView&) = delete;

    /// Takes over a reader opened elsewhere, replacing whatever was open.
    ///
    /// Opening is not free -- every session is walked record by record, and a
    /// file whose index was lost is rebuilt from that walk -- so the window
    /// does it on a worker thread and hands the finished reader here. Nothing
    /// in this class blocks, and nothing in it touches the file behind the
    /// caller's back.
    void adopt(std::unique_ptr<session::SessionReader> reader, std::filesystem::path path);

    void close();

    [[nodiscard]] bool isOpen() const noexcept { return m_reader != nullptr; }
    [[nodiscard]] const session::SessionReader* reader() const noexcept { return m_reader.get(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

    /// The window currently displayed, in monotonic nanoseconds and Hz.
    [[nodiscard]] std::uint64_t viewFromNs() const noexcept { return m_fromNs; }
    [[nodiscard]] std::uint64_t viewToNs() const noexcept { return m_toNs; }
    [[nodiscard]] double viewFromHz() const noexcept { return m_fromHz; }
    [[nodiscard]] double viewToHz() const noexcept { return m_toHz; }

    /// LOD the last composite was served from, and how many lines it covered.
    /// Shown so an operator can tell a decimated view from a native one.
    [[nodiscard]] std::uint32_t lastLod() const noexcept { return m_lastLod; }
    [[nodiscard]] std::uint32_t lastLines() const noexcept { return m_lastLines; }

    void resetView();

    /// Returns the time window to its opening width, keeping the position.
    void resetTimeZoom();

    void setTimeRange(std::uint64_t fromNs, std::uint64_t toNs);
    void setFrequencyRange(double fromHz, double toHz);

    /// Scrolls and zooms in time. `anchor` is 0..1 across the pane, so zooming
    /// keeps whatever is under the pointer where it is.
    void zoomTime(double factor, double anchor);
    void panTime(double fraction);
    void zoomFrequency(double factor, double anchor);
    void panFrequency(double fraction);

    void setColorMap(const ColorMap& map);
    void setGradientRange(float minDb, float maxDb);

    /// How the stored samples sharing one pixel are reduced, matching the live
    /// waterfall's own setting so the two images can be compared.
    void setPeakDetect(bool peak);

    /// Recomposites if anything invalidated the image, then returns the
    /// texture. Zero when there is nothing to draw.
    [[nodiscard]] std::uint32_t texture(std::uint32_t pixelWidth, std::uint32_t pixelHeight);

    [[nodiscard]] std::uint32_t textureWidth() const noexcept { return m_width; }
    [[nodiscard]] std::uint32_t textureHeight() const noexcept { return m_height; }

    /// The whole session as a horizontal strip: time across, frequency down,
    /// restricted to a band around `centerHz`.
    ///
    /// The navigator. A waterfall shows a window; this shows the entire
    /// recording at one frequency of interest, so a burst an hour ago is
    /// visible without hunting for it -- and clicking it is how you get there.
    /// Rebuilt only when the band or the size changes, never per frame.
    [[nodiscard]] std::uint32_t overviewTexture(std::uint32_t pixelWidth, std::uint32_t pixelHeight,
                                                double centerHz, double bandwidthHz);

    /// Session-relative extent of the overview, so a click maps back to a time.
    [[nodiscard]] std::uint64_t overviewFromNs() const noexcept { return m_overviewFromNs; }
    [[nodiscard]] std::uint64_t overviewToNs() const noexcept { return m_overviewToNs; }

    /// The band the strip actually drew, after the requested one was narrowed
    /// to the session and slid back inside it.
    ///
    /// Published rather than left to the caller to recompute. The view
    /// rectangle is drawn in these coordinates, and a second copy of the
    /// clamping arithmetic is exactly how the mark and the pixels beneath it
    /// drift apart.
    [[nodiscard]] double overviewFromHz() const noexcept { return m_overviewBandFromHz; }
    [[nodiscard]] double overviewToHz() const noexcept { return m_overviewBandToHz; }

    /// Monotonic time at a fraction down the pane.
    [[nodiscard]] std::uint64_t timeAt(double fraction) const noexcept;
    /// Frequency at a fraction across the pane.
    [[nodiscard]] double frequencyAt(double fraction) const noexcept;

    /// Segment covering an instant, which is what a spectrum query needs.
    [[nodiscard]] std::optional<std::uint32_t> segmentAt(std::uint64_t monotonicNs) const;

    [[nodiscard]] const std::string& lastError() const noexcept { return m_lastError; }

private:
    void invalidate() noexcept { m_dirty = true; }

    /// Time window a freshly opened session gets, and the width a zoom reset
    /// returns to. Derived from the session's own line rate, so it means the
    /// same amount of detail whatever the recording's length.
    [[nodiscard]] std::uint64_t defaultTimeSpanNs() const;

    void composite(std::uint32_t width, std::uint32_t height);
    void upload();

    std::unique_ptr<session::SessionReader> m_reader;
    std::filesystem::path m_path;
    std::string m_lastError;

    std::uint64_t m_fromNs = 0;
    std::uint64_t m_toNs = 0;
    double m_fromHz = 0.0;
    double m_toHz = 0.0;

    ColorMap m_colorMap;
    /// Bumped whenever the baked gradient actually changes, so the overview can
    /// tell a recolour from the window re-asserting the same map every frame.
    std::uint64_t m_colorMapRevision = 0;
    /// Mirrors ViewSettings' own defaults, which is what the window pushes in
    /// on the first frame; the live renderer starts from the same pair.
    float m_minDb = -75.0F;
    float m_maxDb = -15.0F;
    bool m_peakDetect = false;

    bool m_dirty = true;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint32_t m_texture = 0;
    std::vector<std::uint32_t> m_pixels;
    /// Max gradient index per pixel, and whether anything landed there at all.
    std::vector<std::uint8_t> m_indices;
    std::vector<std::uint8_t> m_written;

    std::uint32_t m_lastLod = 0;
    std::uint32_t m_lastLines = 0;

    std::uint32_t m_overviewTexture = 0;
    std::uint32_t m_overviewWidth = 0;
    std::uint32_t m_overviewHeight = 0;
    float m_overviewMinDb = 0.0F;
    float m_overviewMaxDb = 0.0F;
    bool m_overviewPeakDetect = false;
    std::uint64_t m_overviewColorMapRevision = 0;
    std::uint64_t m_overviewFromNs = 0;
    std::uint64_t m_overviewToNs = 0;
    double m_overviewBandFromHz = 0.0;
    double m_overviewBandToHz = 0.0;
    std::vector<std::uint32_t> m_overviewPixels;
};

} // namespace sweeppp::ui
