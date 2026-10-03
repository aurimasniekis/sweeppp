// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <sweeppp/ui/ColorMap.hpp>
#include <vector>

namespace sweeppp::ui {

/// GPU waterfall.
///
/// Two decisions carry the performance:
///
///  * The history lives in a **ring texture**. A new line overwrites the
///    oldest row and the scroll is a UV offset -- never a memmove. Shifting a
///    2048x2048 texture every line would cost 16 MB of copies per line and cap
///    the line rate in the low hundreds.
///  * Levels are stored as a **single channel** and coloured by a 256-entry
///    LUT sampled in the fragment shader. Changing colormap or gradient
///    limits is then a one-texel-row upload plus two uniforms, with no
///    re-upload of history -- which is what makes dragging the gradient
///    handles feel instant over an hour of waterfall.
///
/// The single-channel format is `GL_R8`, byte-identical to the session
/// container's quantised tiles, so a history tile uploads with no conversion.
class WaterfallRenderer {
public:
    WaterfallRenderer() = default;
    ~WaterfallRenderer();

    WaterfallRenderer(const WaterfallRenderer&) = delete;
    WaterfallRenderer& operator=(const WaterfallRenderer&) = delete;

    [[nodiscard]] std::expected<void, std::string> create(std::uint32_t bins, std::uint32_t lines);
    void destroy() noexcept;

    /// Resizes the history, keeping what is stored.
    ///
    /// Neither dimension is a change of frequency mapping, so neither discards:
    /// depth is how much scrollback there is, and bin count is the resolution
    /// each row was written at. Rows are carried across and resampled onto the
    /// new width. What *does* invalidate them is the span moving, and setSpan
    /// handles that on its own.
    ///
    /// This matters beyond window resizing: changing the sample rate mid-sweep
    /// re-plans the grid and so changes the bin count, while the swept span --
    /// which comes from the range, not the rate -- stays exactly where it was.
    /// Discarding there would throw away minutes of picture that is still
    /// perfectly valid, just recorded at a different resolution.
    [[nodiscard]] std::expected<void, std::string> resize(std::uint32_t bins, std::uint32_t lines);

private:
    /// Reallocates the ring, carrying the newest rows across and resampling
    /// them onto the new width.
    [[nodiscard]] std::expected<void, std::string> rebuild(std::uint32_t bins, std::uint32_t lines);

public:
    /// Declares what frequency range the texture's full width represents.
    ///
    /// Lines are stored against the *acquisition's* span, never against the
    /// visible window. That is what lets zooming re-map the entire history at
    /// once instead of only affecting rows written after the zoom: the view
    /// becomes a UV transform in the shader rather than a resampling decision
    /// taken at write time and baked in forever.
    ///
    /// Moving the span clears the history. Existing rows were written under the
    /// old mapping and there is no honest way to reinterpret them -- the same
    /// reasoning as a session segment boundary.
    ///
    /// "Moving" means by at least one bin. Re-planning the same swept range at
    /// a different sample rate divides it into a different number of bins, so
    /// the far edge shifts by a fraction of one -- which is finer than the
    /// stored rows can express, and not a retune.
    void setSpan(double startHz, double stopHz);

    [[nodiscard]] double spanStartHz() const noexcept { return m_spanStartHz; }
    [[nodiscard]] double spanStopHz() const noexcept { return m_spanStopHz; }

    /// Appends one line of dB values, mapped through the current gradient
    /// range. The most common entry point, from the live pipeline.
    ///
    /// `count` should be the frame's full bin count: the line is stored at the
    /// texture's resolution, not the display's.
    /// `monotonicNs` labels the line for the time axis. Zero means unknown,
    /// which shows as a blank label rather than a wrong one.
    void pushLine(const float* dbValues, std::uint32_t count, std::uint64_t monotonicNs = 0);

    /// When the line `linesBack` rows above the newest was written, or zero if
    /// that row has never been written.
    [[nodiscard]] std::uint64_t timeAtLinesBack(std::uint32_t linesBack) const noexcept;

    /// Appends one line already quantised to the container's format. Used when
    /// replaying or scrolling history: the bytes go straight to the GPU.
    void pushQuantisedLine(const std::uint8_t* values, std::uint32_t count, float originDb);

    /// Uploads a block of pre-quantised lines at once, for filling the view
    /// from a history tile.
    void pushQuantisedBlock(const std::uint8_t* values, std::uint32_t lines, std::uint32_t bins,
                            float originDb);

    void clear();

    /// Re-bakes the palette. Cheap: one 256-texel upload, no history touched.
    void setColorMap(const ColorMap& map);

    /// Gradient limits in dB. Also cheap -- two uniforms.
    void setGradientRange(float minDb, float maxDb) noexcept;

    /// How several bins sharing one pixel are reduced.
    ///
    /// Peak finds narrow carriers; average estimates the noise floor. The
    /// difference only appears once there is more than one bin per pixel,
    /// which on a wide span is most of the time.
    void setPeakDetect(bool peak) noexcept { m_peakDetect = peak; }

    /// How many lines back from the newest the top of the pane shows.
    void setScrollLines(std::uint32_t lines) noexcept { m_scrollLines = lines; }
    [[nodiscard]] std::uint32_t scrollLines() const noexcept { return m_scrollLines; }

    /// Oldest line that can be scrolled to while keeping the pane full.
    [[nodiscard]] std::uint32_t maxScrollLines(std::uint32_t visibleLines) const noexcept {
        const auto stored =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(m_linesPushed, m_lines - 1));
        return stored > visibleLines ? stored - visibleLines : 0;
    }
    [[nodiscard]] float gradientMinDb() const noexcept { return m_minDb; }
    [[nodiscard]] float gradientMaxDb() const noexcept { return m_maxDb; }

    /// Draws into the given rectangle, showing `visibleLines` of history.
    ///
    /// The rectangle is in **ImGui logical points**, not framebuffer pixels.
    /// `framebufferScale` converts between them -- on a Retina display they
    /// differ by 2x, and using points directly puts the waterfall at half
    /// position and half size, overlapping whatever is above it.
    ///
    /// `viewStartHz`/`viewStopHz` are the visible frequency window. They are
    /// resolved against the stored span into a UV transform, so the whole
    /// history zooms and pans together. Passing the full span shows
    /// everything.
    void draw(float x, float y, float width, float height, std::uint32_t visibleLines,
              double viewStartHz, double viewStopHz, float framebufferScaleX = 1.0F,
              float framebufferScaleY = 1.0F);

    [[nodiscard]] std::uint32_t bins() const noexcept { return m_bins; }
    [[nodiscard]] std::uint32_t lines() const noexcept { return m_lines; }

    /// Bytes the history texture currently occupies -- one byte per bin per
    /// line, the same layout as a session tile.
    [[nodiscard]] std::size_t historyBytes() const noexcept {
        return static_cast<std::size_t>(m_bins) * m_lines;
    }

    /// Largest texture edge this driver will accept, which is the hard ceiling
    /// on history depth: one line is one texture row. Requires a current GL
    /// context; zero before one exists.
    [[nodiscard]] static std::uint32_t maxTextureSize() noexcept;

    [[nodiscard]] std::uint64_t linesPushed() const noexcept { return m_linesPushed; }
    [[nodiscard]] bool valid() const noexcept { return m_program != 0; }

private:
    [[nodiscard]] std::expected<void, std::string> buildProgram();

    /// One level to a texture byte.
    ///
    /// Measured levels take 1..255 across the gradient range; byte 0 is
    /// reserved for a bin nothing measured, which the shader paints as
    /// background and every reduction skips. It is the same reservation the
    /// session container makes, which is what keeps a tile uploadable as a
    /// memcpy.
    [[nodiscard]] std::uint8_t quantiseLevel(float db) const noexcept;

    void quantiseInto(const float* dbValues, std::uint32_t count);

    std::uint32_t m_historyTexture = 0;
    std::uint32_t m_paletteTexture = 0;
    std::uint32_t m_program = 0;
    std::uint32_t m_vbo = 0;

    int m_uniformHistory = -1;
    int m_uniformPalette = -1;
    int m_uniformWriteRow = -1;
    int m_uniformLineCount = -1;
    int m_uniformVisibleLines = -1;
    int m_uniformValidLines = -1;
    int m_uniformScrollLines = -1;
    int m_uniformPeakDetect = -1;
    int m_uniformHorizontal = -1;
    int m_uniformTaps = -1;
    int m_uniformTexelStep = -1;

    /// Frequency range the texture's full width covers.
    double m_spanStartHz = 0.0;
    double m_spanStopHz = 0.0;

    std::uint32_t m_bins = 0;
    std::uint32_t m_lines = 0;
    /// Row the next line will be written to; the shader offsets by it.
    std::uint32_t m_writeRow = 0;
    std::uint64_t m_linesPushed = 0;

    bool m_peakDetect = false;
    std::uint32_t m_scrollLines = 0;

    /// Capture time per stored row, in the same ring order as the texture.
    std::vector<std::uint64_t> m_lineTimes;
    /// Mirrors ViewSettings' own defaults, which is what the window pushes in
    /// on the first frame; the history view starts from the same pair.
    float m_minDb = -75.0F;
    float m_maxDb = -15.0F;

    std::vector<std::uint8_t> m_scratch;
};

} // namespace sweeppp::ui
