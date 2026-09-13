// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "WaterfallRenderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <sweeppp/history/SessionFormat.hpp>
#include <sweeppp/pipeline/SpectrumFrame.hpp>
#include <sweeppp_gl.h>

namespace sweeppp::ui {
namespace {

/// Full-screen-quad vertex shader over the target rectangle.
constexpr const char* kVertexShader = R"(#version 410 core
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main()
{
    vTexCoord = aTexCoord;
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";

/// Samples the ring by offsetting the row index, then colours through the LUT.
///
/// The modulo on the row index is the entire scroll mechanism: no pixel of
/// history is ever moved, and the newest line is always the bottom row of the
/// view regardless of where it physically sits in the texture.
constexpr const char* kFragmentShader = R"(#version 410 core
in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D uHistory;
uniform sampler1D uPalette;
uniform float uWriteRow;
uniform float uLineCount;
uniform float uVisibleLines;
uniform float uValidLines;   // rows actually written; older ones are empty
uniform float uScrollLines;  // rows back from the newest that the top shows
uniform vec2  uHorizontal;   // x: offset, y: scale, in texture UV
uniform int   uTaps;         // texels to reduce per output pixel
uniform int   uPeakDetect;   // 1: keep the loudest of them, 0: average them
uniform float uTexelStep;    // 1 / texture width

void main()
{
    // Horizontal: the visible frequency window mapped onto the stored span.
    // Zoom and pan are entirely this transform, which is why the whole history
    // re-renders rather than only the rows written since the last zoom.
    float u = uHorizontal.x + vTexCoord.x * uHorizontal.y;
    if (u < 0.0 || u > 1.0) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Vertical: row 0 of the view is the newest line. Walk backwards from the
    // write cursor, wrapping around the ring.
    float linesBack = uScrollLines + vTexCoord.y * uVisibleLines;

    // Never show a row that has not been written.
    //
    // Untouched rows hold zero, which is the *bottom* of the colour map -- so
    // history that does not exist yet would render as a solid band
    // indistinguishable from spectrum measured and found quiet. Empty has to
    // look empty, the same rule the spectrum follows for unmeasured bins.
    if (linesBack >= uValidLines) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float row = mod(uWriteRow - 1.0 - linesBack + uLineCount, uLineCount);
    float v = (row + 0.5) / uLineCount;

    // Zoomed out, one pixel covers several texels, and which of them to show
    // is a detector choice -- the same one a spectrum analyser makes.
    //
    // Peak never misses a narrow carrier hiding between sample points, but
    // over noise it reports the loudest of N samples: the apparent floor lifts
    // by several dB and, worse, varies by several dB from line to line. On a
    // wide span with many bins per pixel that turns the whole waterfall into
    // shimmering static, because it is re-rolling the same dice every line.
    //
    // Averaging is the estimator for what the noise floor actually is. It
    // costs the isolated single-bin carrier, which the spectrum's own
    // min/max envelope still shows.
    //
    // Texel 0 takes part in neither. It is the unmeasured sentinel, not a
    // level, and how many texels a pixel covers is a pure function of zoom and
    // pane width -- so a kernel that reduced it alongside measurements would
    // paint the bins between two swept spans at some zooms and not others,
    // which is exactly the horizontal line that used to appear across a gap.
    float peak = 0.0;
    float total = 0.0;
    int seen = 0;
    for (int i = 0; i < uTaps; ++i) {
        float du = (float(i) - 0.5 * float(uTaps - 1)) * uTexelStep;
        float level = texture(uHistory, vec2(clamp(u + du, 0.0, 1.0), v)).r;
        if (level > 0.0) {
            peak = max(peak, level);
            total += level;
            ++seen;
        }
    }

    if (seen == 0) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    fragColor = texture(uPalette, uPeakDetect == 1 ? peak : total / float(seen));
}
)";

std::expected<std::uint32_t, std::string> compileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
        glGetShaderInfoLog(shader, length, nullptr, log.data());
        glDeleteShader(shader);
        return std::unexpected(
            std::format("{} shader: {}", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log));
    }
    return shader;
}

} // namespace

WaterfallRenderer::~WaterfallRenderer() {
    destroy();
}

std::expected<void, std::string> WaterfallRenderer::buildProgram() {
    auto vertex = compileShader(GL_VERTEX_SHADER, kVertexShader);
    if (!vertex) {
        return std::unexpected(vertex.error());
    }
    auto fragment = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!fragment) {
        glDeleteShader(*vertex);
        return std::unexpected(fragment.error());
    }

    m_program = glCreateProgram();
    glAttachShader(m_program, *vertex);
    glAttachShader(m_program, *fragment);
    glLinkProgram(m_program);

    GLint status = GL_FALSE;
    glGetProgramiv(m_program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        GLint length = 0;
        glGetProgramiv(m_program, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
        glGetProgramInfoLog(m_program, length, nullptr, log.data());
        glDeleteProgram(m_program);
        m_program = 0;
        return std::unexpected(std::format("link: {}", log));
    }

    glDetachShader(m_program, *vertex);
    glDetachShader(m_program, *fragment);
    glDeleteShader(*vertex);
    glDeleteShader(*fragment);

    m_uniformHistory = glGetUniformLocation(m_program, "uHistory");
    m_uniformPalette = glGetUniformLocation(m_program, "uPalette");
    m_uniformWriteRow = glGetUniformLocation(m_program, "uWriteRow");
    m_uniformLineCount = glGetUniformLocation(m_program, "uLineCount");
    m_uniformVisibleLines = glGetUniformLocation(m_program, "uVisibleLines");
    m_uniformValidLines = glGetUniformLocation(m_program, "uValidLines");
    m_uniformScrollLines = glGetUniformLocation(m_program, "uScrollLines");
    m_uniformHorizontal = glGetUniformLocation(m_program, "uHorizontal");
    m_uniformTaps = glGetUniformLocation(m_program, "uTaps");
    m_uniformPeakDetect = glGetUniformLocation(m_program, "uPeakDetect");
    m_uniformTexelStep = glGetUniformLocation(m_program, "uTexelStep");

    return {};
}

std::expected<void, std::string> WaterfallRenderer::create(std::uint32_t bins,
                                                           std::uint32_t lines) {
    destroy();

    if (bins == 0 || lines == 0) {
        return std::unexpected("waterfall needs a non-zero size");
    }

    // Refuse rather than silently truncate: a texture larger than the driver
    // allows would fail at upload time with no useful message.
    GLint maxTextureSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    if (static_cast<GLint>(bins) > maxTextureSize || static_cast<GLint>(lines) > maxTextureSize) {
        return std::unexpected(std::format("{}x{} exceeds this driver's maximum texture size of {}",
                                           bins, lines, maxTextureSize));
    }

    if (auto built = buildProgram(); !built) {
        return built;
    }

    m_bins = bins;
    m_lines = lines;
    m_writeRow = 0;
    m_linesPushed = 0;

    // Single channel, byte per bin. Same format as a session tile, so history
    // uploads with no conversion at all.
    glGenTextures(1, &m_historyTexture);
    glBindTexture(GL_TEXTURE_2D, m_historyTexture);
    // Nearest, not linear. Byte 0 is the unmeasured sentinel, and hardware
    // interpolation between it and a measured neighbour produces a dim level
    // that was never recorded -- a ramp out of every gap edge. It also stops
    // the magnified view inventing detail between two stored bins, the same
    // rule the ring's own resampler follows.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // Clamped horizontally so a zoomed-out view does not wrap the spectrum
    // around; the shader handles the vertical wrap itself.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    const std::vector<std::uint8_t> blank(static_cast<std::size_t>(bins) * lines, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, static_cast<GLsizei>(bins), static_cast<GLsizei>(lines),
                 0, GL_RED, GL_UNSIGNED_BYTE, blank.data());

    glGenTextures(1, &m_paletteTexture);
    glBindTexture(GL_TEXTURE_1D, m_paletteTexture);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    setColorMap(builtinColorMap("spectral"));

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);

    m_scratch.resize(bins);
    return {};
}

std::uint32_t WaterfallRenderer::maxTextureSize() noexcept {
    GLint value = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &value);
    return value > 0 ? static_cast<std::uint32_t>(value) : 0;
}

std::expected<void, std::string> WaterfallRenderer::resize(std::uint32_t bins,
                                                           std::uint32_t lines) {
    if (bins == m_bins && lines == m_lines) {
        return {};
    }

    // The stored rows survive either kind of change, because neither is a
    // change of *mapping*: the frequencies a row covers are set by setSpan,
    // which clears on its own when they really do move. Depth is capacity, and
    // bin count is resolution -- a rate change during a sweep alters the
    // second and leaves the first alone, so the history is still true and only
    // needs resampling.
    if (m_historyTexture != 0 && bins > 0 && lines > 0) {
        return rebuild(bins, lines);
    }

    return create(bins, lines);
}

namespace {

/// Resamples one stored row onto a different bin count.
///
/// The row covers the same frequencies either way -- that is what makes this
/// honest, and it is guaranteed by `setSpan`, which clears on its own when the
/// mapping really does move. Only the resolution is changing.
///
/// One loop covers both directions. Shrinking takes the MAX over the source
/// bins a destination bin covers, because a narrow carrier that survives
/// decimation is the property the whole display exists to show; a mean would
/// average it into the floor and quietly delete signals from the history.
/// Growing lands on a single source bin and replicates it -- nearest
/// neighbour, no interpolation, because detail that was never stored cannot be
/// recovered and inventing a smooth ramp between two bins would look like
/// measurement.
///
/// The unmeasured sentinel needs no special case here, but only because it is
/// byte 0: it loses every max against a real reading, so a destination bin
/// keeps it exactly when nothing it covers was measured.
void resampleRow(const std::uint8_t* source, std::uint32_t sourceBins, std::uint8_t* destination,
                 std::uint32_t destinationBins) {
    for (std::uint32_t i = 0; i < destinationBins; ++i) {
        const auto from = static_cast<std::uint32_t>(static_cast<std::uint64_t>(i) * sourceBins /
                                                     destinationBins);
        auto to = static_cast<std::uint32_t>(static_cast<std::uint64_t>(i + 1) * sourceBins /
                                             destinationBins);
        to = std::clamp(to, from + 1, sourceBins);

        std::uint8_t peak = 0;
        for (std::uint32_t k = from; k < to; ++k) {
            peak = std::max(peak, source[k]);
        }
        destination[i] = peak;
    }
}

} // namespace

std::expected<void, std::string> WaterfallRenderer::rebuild(std::uint32_t bins,
                                                            std::uint32_t lines) {
    GLint maxTextureSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    if (static_cast<GLint>(lines) > maxTextureSize || static_cast<GLint>(bins) > maxTextureSize) {
        return std::unexpected(std::format("{}x{} exceeds this driver's maximum texture "
                                           "size of {}",
                                           bins, lines, maxTextureSize));
    }

    const auto sourceRowBytes = static_cast<std::size_t>(m_bins);
    const auto rowBytes = static_cast<std::size_t>(bins);

    std::vector<std::uint8_t> existing(sourceRowBytes * m_lines);
    glBindTexture(GL_TEXTURE_2D, m_historyTexture);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, existing.data());

    // The newest rows are the ones worth keeping when the ring shrinks.
    const auto keep = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(m_linesPushed, std::min(m_lines, lines)));

    // Rewritten oldest-first from the start of the new ring rather than copied
    // at their old offsets: the write cursor moves, so the wrap point moves
    // with it, and a straight copy would leave the ring reading out of order.
    std::vector<std::uint8_t> rebuilt(rowBytes * lines, 0);
    std::vector<std::uint64_t> times(lines, 0);
    const bool haveTimes = m_lineTimes.size() == m_lines;
    const bool sameWidth = bins == m_bins;

    for (std::uint32_t i = 0; i < keep; ++i) {
        const std::uint32_t source = (m_writeRow + m_lines - keep + i) % m_lines;
        const std::uint8_t* in =
            existing.data() + static_cast<std::size_t>(source) * sourceRowBytes;
        std::uint8_t* out = rebuilt.data() + static_cast<std::size_t>(i) * rowBytes;

        if (sameWidth) {
            std::copy_n(in, rowBytes, out);
        } else {
            resampleRow(in, m_bins, out, bins);
        }
        if (haveTimes) {
            times[i] = m_lineTimes[source];
        }
    }

    glDeleteTextures(1, &m_historyTexture);
    glGenTextures(1, &m_historyTexture);
    glBindTexture(GL_TEXTURE_2D, m_historyTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, static_cast<GLsizei>(bins), static_cast<GLsizei>(lines),
                 0, GL_RED, GL_UNSIGNED_BYTE, rebuilt.data());

    m_bins = bins;
    m_lines = lines;
    m_lineTimes = std::move(times);
    m_linesPushed = keep;
    m_writeRow = keep % lines;
    m_scrollLines = std::min(m_scrollLines, keep);
    m_scratch.resize(bins);

    return {};
}

void WaterfallRenderer::destroy() noexcept {
    if (m_historyTexture != 0) {
        glDeleteTextures(1, &m_historyTexture);
        m_historyTexture = 0;
    }
    if (m_paletteTexture != 0) {
        glDeleteTextures(1, &m_paletteTexture);
        m_paletteTexture = 0;
    }
    if (m_vbo != 0) {
        glDeleteBuffers(1, &m_vbo);
        m_vbo = 0;
    }
    if (m_vao != 0) {
        glDeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }
    if (m_program != 0) {
        glDeleteProgram(m_program);
        m_program = 0;
    }
    m_bins = 0;
    m_lines = 0;
}

void WaterfallRenderer::setColorMap(const ColorMap& map) {
    if (m_paletteTexture == 0) {
        return;
    }

    // The baked LUT is already RGBA8 in the layout GL wants, so switching
    // colormap is one 1 KiB upload and nothing else.
    glBindTexture(GL_TEXTURE_1D, m_paletteTexture);
    glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA8, static_cast<GLsizei>(ColorMap::kLutSize), 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, map.lut().data());
}

void WaterfallRenderer::setSpan(double startHz, double stopHz) {
    if (stopHz <= startHz) {
        return;
    }

    // A tolerance rather than exact equality, and it is one BIN wide.
    //
    // Two different things arrive here looking the same. One is float jitter in
    // the last bits, because the span is recomputed from frame metadata every
    // frame. The other is a re-planned grid: the sweep's range is set by the
    // operator, but the bins that cover it are chosen from the sample rate, so
    // changing the rate re-divides the same range into a different number of
    // bins and the far edge lands in a slightly different place. Measured over
    // 70 MHz - 6 GHz, going from 20 MS/s to 122.88 MS/s moves it by 46 kHz --
    // which sounds like a lot until it is written as 0.694 of one bin.
    //
    // Neither is a retune, and a difference finer than one bin is finer than
    // the stored rows can express: there is no arrangement of them that would
    // look any different. A fixed 1e-3 Hz treated the second case as a retune
    // and wiped minutes of picture for a shift no pixel could show.
    const double storedSpan = m_spanStopHz - m_spanStartHz;
    const double tolerance =
        (m_bins > 0 && storedSpan > 0.0) ? storedSpan / static_cast<double>(m_bins) : 1e-3;

    if (std::abs(startHz - m_spanStartHz) < tolerance &&
        std::abs(stopHz - m_spanStopHz) < tolerance) {
        // Adopt the new numbers so drawing stays exact, and keep the rows.
        m_spanStartHz = startHz;
        m_spanStopHz = stopHz;
        return;
    }

    // A real move. Rows already stored were written against the previous
    // mapping, and there is no honest way to reinterpret them under a new one.
    if (m_spanStopHz > m_spanStartHz) {
        clear();
    }

    m_spanStartHz = startHz;
    m_spanStopHz = stopHz;
}

void WaterfallRenderer::setGradientRange(float minDb, float maxDb) noexcept {
    // An inverted or zero-width range would divide by zero in the quantiser.
    if (maxDb <= minDb) {
        maxDb = minDb + 1.0F;
    }
    m_minDb = minDb;
    m_maxDb = maxDb;
}

std::uint8_t WaterfallRenderer::quantiseLevel(float db) const noexcept {
    if (!measured(db)) {
        return session::kUnmeasuredByte;
    }

    // 254 steps, not 255: the scale starts at 1 because 0 is the sentinel.
    const float span = m_maxDb - m_minDb;
    const float scale = span > 0.0F ? 254.0F / span : 0.0F;
    return static_cast<std::uint8_t>(1.0F + std::clamp((db - m_minDb) * scale, 0.0F, 254.0F));
}

void WaterfallRenderer::quantiseInto(const float* dbValues, std::uint32_t count) {
    m_scratch.resize(m_bins);

    const std::uint32_t copied = std::min(count, m_bins);
    for (std::uint32_t i = 0; i < copied; ++i) {
        m_scratch[i] = quantiseLevel(dbValues[i]);
    }
    // A short line leaves the remainder unmeasured rather than showing
    // whatever the previous line had there.
    std::fill(m_scratch.begin() + copied, m_scratch.end(), session::kUnmeasuredByte);
}

std::uint64_t WaterfallRenderer::timeAtLinesBack(std::uint32_t linesBack) const noexcept {
    if (m_lines == 0 || m_lineTimes.size() < m_lines || linesBack >= m_linesPushed ||
        linesBack >= m_lines) {
        return 0;
    }
    const std::size_t row = (m_writeRow + m_lines - 1 - linesBack) % m_lines;
    return m_lineTimes[row];
}

void WaterfallRenderer::pushLine(const float* dbValues, std::uint32_t count,
                                 std::uint64_t monotonicNs) {
    if (m_historyTexture == 0 || dbValues == nullptr) {
        return;
    }

    quantiseInto(dbValues, count);

    // One row uploaded, nothing moved. The scroll is the write cursor.
    glBindTexture(GL_TEXTURE_2D, m_historyTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLint>(m_writeRow),
                    static_cast<GLsizei>(m_bins), 1, GL_RED, GL_UNSIGNED_BYTE, m_scratch.data());

    if (m_lineTimes.size() != m_lines) {
        m_lineTimes.assign(m_lines, 0);
    }
    m_lineTimes[m_writeRow] = monotonicNs;

    m_writeRow = (m_writeRow + 1) % m_lines;
    ++m_linesPushed;
}

void WaterfallRenderer::pushQuantisedLine(const std::uint8_t* values, std::uint32_t count,
                                          float originDb) {
    if (m_historyTexture == 0 || values == nullptr) {
        return;
    }

    // Stored tiles use a per-tile origin and 0.5 dB steps; the display uses
    // its own gradient range. Requantising is a cheap linear remap, and it
    // keeps the gradient handles meaningful for replayed data too.
    //
    // The sentinel survives it: byte 0 dequantises to the unmeasured level,
    // which quantises back to byte 0 whatever the gradient is set to.
    m_scratch.resize(m_bins);

    const std::uint32_t copied = std::min(count, m_bins);
    for (std::uint32_t i = 0; i < copied; ++i) {
        m_scratch[i] = quantiseLevel(
            static_cast<float>(session::dequantiseDb(values[i], static_cast<double>(originDb))));
    }
    std::fill(m_scratch.begin() + copied, m_scratch.end(), session::kUnmeasuredByte);

    glBindTexture(GL_TEXTURE_2D, m_historyTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLint>(m_writeRow),
                    static_cast<GLsizei>(m_bins), 1, GL_RED, GL_UNSIGNED_BYTE, m_scratch.data());

    if (m_lineTimes.size() != m_lines) {
        m_lineTimes.assign(m_lines, 0);
    }
    // Replayed lines carry their own time in the session file rather than
    // here; zero reads as unknown and labels blank instead of wrong.
    m_lineTimes[m_writeRow] = 0;

    m_writeRow = (m_writeRow + 1) % m_lines;
    ++m_linesPushed;
}

void WaterfallRenderer::pushQuantisedBlock(const std::uint8_t* values, std::uint32_t lines,
                                           std::uint32_t bins, float originDb) {
    for (std::uint32_t line = 0; line < lines; ++line) {
        pushQuantisedLine(values + static_cast<std::size_t>(line) * bins, bins, originDb);
    }
}

void WaterfallRenderer::clear() {
    if (m_historyTexture == 0) {
        return;
    }

    const std::vector<std::uint8_t> blank(static_cast<std::size_t>(m_bins) * m_lines, 0);
    glBindTexture(GL_TEXTURE_2D, m_historyTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(m_bins),
                    static_cast<GLsizei>(m_lines), GL_RED, GL_UNSIGNED_BYTE, blank.data());

    m_writeRow = 0;
    m_linesPushed = 0;
}

void WaterfallRenderer::draw(float x, float y, float width, float height,
                             std::uint32_t visibleLines, double viewStartHz, double viewStopHz,
                             float framebufferScaleX, float framebufferScaleY) {
    if (m_program == 0 || width <= 0.0F || height <= 0.0F) {
        return;
    }

    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(0x0BA2 /* GL_VIEWPORT */, viewport);
    const auto viewportWidth = static_cast<float>(viewport[2]);
    const auto viewportHeight = static_cast<float>(viewport[3]);
    if (viewportWidth <= 0.0F || viewportHeight <= 0.0F) {
        return;
    }

    // ImGui works in logical points; the GL viewport is in framebuffer pixels.
    // On a Retina display those differ by 2x, and skipping this conversion
    // draws the waterfall at half position and half size -- over the top of
    // the spectrum rather than in its own pane.
    const float pixelX = x * framebufferScaleX;
    const float pixelY = y * framebufferScaleY;
    const float pixelWidth = width * framebufferScaleX;
    const float pixelHeight = height * framebufferScaleY;

    // Framebuffer pixels to clip space. ImGui's origin is top-left and GL's is
    // bottom-left, hence the Y flip.
    const auto toClipX = [viewportWidth](float px) { return (px / viewportWidth) * 2.0F - 1.0F; };
    const auto toClipY = [viewportHeight](float px) { return 1.0F - (px / viewportHeight) * 2.0F; };

    const float x0 = toClipX(pixelX);
    const float x1 = toClipX(pixelX + pixelWidth);
    const float y0 = toClipY(pixelY);
    const float y1 = toClipY(pixelY + pixelHeight);

    // v=0 is the newest line, drawn at the top of the rectangle.
    const std::array<float, 24> vertices{
        x0, y0, 0.0F, 0.0F, x1, y0, 1.0F, 0.0F, x1, y1, 1.0F, 1.0F,
        x0, y0, 0.0F, 0.0F, x1, y1, 1.0F, 1.0F, x0, y1, 0.0F, 1.0F,
    };

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(vertices)), vertices.data(),
                 GL_STREAM_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<const void*>(2 * sizeof(float)));

    glUseProgram(m_program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_historyTexture);
    glUniform1i(m_uniformHistory, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_1D, m_paletteTexture);
    glUniform1i(m_uniformPalette, 1);

    glUniform1f(m_uniformWriteRow, static_cast<float>(m_writeRow));
    glUniform1f(m_uniformLineCount, static_cast<float>(m_lines));
    // One short of the ring, never the whole of it.
    //
    // Walking back exactly m_lines rows wraps to the row it started from, so
    // the bottom line of the pane drew a copy of the newest one -- a ghost of
    // the live trace sitting under an otherwise empty waterfall.
    glUniform1f(m_uniformVisibleLines,
                static_cast<float>(std::clamp(visibleLines, 1U, m_lines - 1)));
    glUniform1f(m_uniformValidLines,
                static_cast<float>(std::min<std::uint64_t>(m_linesPushed, m_lines - 1)));
    glUniform1f(m_uniformScrollLines, static_cast<float>(m_scrollLines));
    // The visible window resolved against the stored span. Everything about
    // zoom and pan is these two numbers -- no stored pixel moves, and the full
    // history re-maps at once.
    float horizontalOffset = 0.0F;
    float horizontalScale = 1.0F;

    const double storedSpan = m_spanStopHz - m_spanStartHz;
    if (storedSpan > 0.0 && viewStopHz > viewStartHz) {
        horizontalOffset = static_cast<float>((viewStartHz - m_spanStartHz) / storedSpan);
        horizontalScale = static_cast<float>((viewStopHz - viewStartHz) / storedSpan);
    }
    glUniform2f(m_uniformHorizontal, horizontalOffset, horizontalScale);

    // Texels covered by one output pixel, which is how many to max-reduce.
    // Capped because the cost is per fragment, and beyond a handful of taps the
    // extra ones add nothing an operator can see.
    const float texelsPerPixel =
        pixelWidth > 0.0F ? horizontalScale * static_cast<float>(m_bins) / pixelWidth : 1.0F;
    const int taps = std::clamp(static_cast<int>(std::ceil(texelsPerPixel)), 1, 16);

    glUniform1i(m_uniformTaps, taps);
    glUniform1i(m_uniformPeakDetect, m_peakDetect ? 1 : 0);
    glUniform1f(m_uniformTexelStep, 1.0F / static_cast<float>(m_bins));

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    // Clip to our own rectangle.
    //
    // ImGui's backend leaves the scissor test enabled and only updates the box
    // when it draws elements -- never for a user callback like this one. So
    // without setting it here, the waterfall is clipped to whatever rectangle
    // the *previous* command happened to use, and the pane appears, vanishes
    // or gets sliced depending on what else the UI drew just before it. That
    // made the waterfall disappear whenever the pane divider was hovered,
    // because hovering added one more draw command ahead of this callback.
    GLint previousScissor[4] = {0, 0, 0, 0};
    glGetIntegerv(0x0C10 /* GL_SCISSOR_BOX */, previousScissor);

    // GL counts from the bottom, ImGui from the top.
    glScissor(static_cast<GLint>(pixelX),
              static_cast<GLint>(viewportHeight - (pixelY + pixelHeight)),
              static_cast<GLsizei>(pixelWidth), static_cast<GLsizei>(pixelHeight));

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glScissor(previousScissor[0], previousScissor[1], previousScissor[2], previousScissor[3]);

    // ImGui assumes it owns this state, so anything changed here is put back.
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
}

} // namespace sweeppp::ui
