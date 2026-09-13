// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// Sidebar sections, popovers, the Performance panel and the History window.
#include "BarChrome.hpp"
#include "ContributionOverlay.hpp"
#include "FileDialog.hpp"
#include "Icons.hpp"
#include "MainWindow.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <future>
#include <imgui.h>
#include <implot.h>
#include <optional>
#include <span>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/core/Paths.hpp>
#include <sweeppp/core/SystemInfo.hpp>
#include <sweeppp/core/Toml.hpp>
#include <sweeppp/core/Version.hpp>
#include <sweeppp/fft/FftBenchmark.hpp>
#include <sweeppp/history/SweepsLog.hpp>
#include <sweeppp/plugin/PluginHost.hpp>
#include <utility>

namespace sweeppp::ui {
namespace {

ImVec4 toImVec4(const Color& color) {
    return {color.r, color.g, color.b, color.a};
}
ImU32 packed(const Color& color) {
    return color.packed();
}

/// The main window's divider, so both drag the same way.

/// A position within a recording, at a precision the axis can actually
/// distinguish.
///
/// formatDuration drops to whole seconds past a minute, which is right for a
/// duration but wrong for a scale: zoomed into two seconds an hour deep, every
/// label rounds to the same "58:14" and the axis stops saying anything. `step`
/// is the spacing between labels, so the precision follows the zoom.
std::string formatTimestamp(double seconds, double step) {
    if (step >= 1.0) {
        return formatDuration(seconds);
    }

    const int decimals = step >= 0.1 ? 1 : (step >= 0.01 ? 2 : 3);
    const auto minutes = static_cast<long long>(seconds / 60.0);
    if (minutes > 0) {
        const double remainder = seconds - static_cast<double>(minutes) * 60.0;
        return std::format("{}:{:0{}.{}f}", minutes, remainder, decimals + 3, decimals);
    }
    return std::format("{:.{}f} s", seconds, decimals);
}

/// Digit grouping, so a line count reads at a glance instead of being counted.
std::string groupedCount(std::uint64_t value) {
    std::string digits = std::format("{}", value);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    const std::size_t lead = digits.size() % 3 == 0 ? 3 : digits.size() % 3;

    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i == lead || (i > lead && (i - lead) % 3 == 0)) {
            out.push_back(',');
        }
        out.push_back(digits[i]);
    }
    return out;
}

/// Padding inside the viewer's centred cards, and the radius of their corners.
constexpr ImVec2 kCardPadding{24.0F, 20.0F};
constexpr float kCardRounding = 12.0F;

/// Opens a panel floating in the middle of the viewer's content area.
///
/// The root window has no padding and, with no file open, nothing else in it,
/// so a line of text written at the cursor lands in the top-left corner of a
/// black rectangle -- which reads as a drawing that failed rather than as an
/// invitation to open something. Height is the caller's because the card is
/// centred: an auto-sized child knows its height only after it has been drawn,
/// which is one frame too late to place it by.
void beginCentredCard(const char* id, const ImVec2& size, const ChromeTheme& chrome) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float top = viewport->WorkPos.y + bar::toolbarHeight();
    const float height = viewport->WorkSize.y - bar::toolbarHeight();

    ImGui::SetCursorScreenPos(ImVec2(viewport->WorkPos.x + (viewport->WorkSize.x - size.x) * 0.5F,
                                     top + std::max((height - size.y) * 0.5F, 0.0F)));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, toImVec4(chrome.panelBackground));
    ImGui::PushStyleColor(ImGuiCol_Border, toImVec4(chrome.border));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, kCardRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, kCardPadding);
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

void endCentredCard() {
    ImGui::EndChild();
}

/// One centred line of a card.
///
/// `within` is the width to centre inside, for a card that auto-resizes: its
/// content region is last frame's answer, so the first frame of a panel that
/// has just opened would place every line against a width it no longer has.
void centredText(const char* text, const Color& color, float within = 0.0F) {
    const float available = within > 0.0F ? within : ImGui::GetContentRegionAvail().x;
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max((available - width) * 0.5F, 0.0F));
    ImGui::TextColored(toImVec4(color), "%s", text);
}

/// An indeterminate progress ring, drawn at the cursor.
///
/// Indeterminate because the reader cannot say how far through it is: what it
/// is looking for is written at the end of the file, and until that is found
/// the only measure of progress is bytes walked, which it does not report. All
/// this has to say is that the window is alive and the file is being read.
void spinner(float radius, float thickness, const Color& track, const Color& arc) {
    constexpr float kPi = 3.14159265F;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 centre(origin.x + radius, origin.y + radius);

    draw->PathArcTo(centre, radius, 0.0F, kPi * 2.0F, 48);
    draw->PathStroke(packed(track), thickness);

    const auto start = static_cast<float>(ImGui::GetTime()) * 3.0F;
    draw->PathArcTo(centre, radius, start, start + kPi * 0.8F, 24);
    draw->PathStroke(packed(arc), thickness);

    ImGui::Dummy(ImVec2(radius * 2.0F, radius * 2.0F));
}

/// The playhead, in the shape of the session it is moving through.
///
/// formatDuration is right for a duration and wrong for a position: under a
/// minute it prints milliseconds, and it changes unit as the value passes one
/// second and one minute -- so every frame of playback was a different string,
/// and everything after it on the status bar moved with it. The shape is
/// chosen from the session's length rather than from the playhead, so it is
/// fixed for as long as the file is open.
std::string formatPlayhead(double seconds, double sessionSeconds) {
    if (sessionSeconds < 60.0) {
        return std::format("{:.1f} s", seconds);
    }

    const auto total = static_cast<long long>(seconds);
    const long long minutes = (total % 3600) / 60;
    const long long secs = total % 60;
    if (sessionSeconds >= 3600.0) {
        return std::format("{}:{:02}:{:02}", total / 3600, minutes, secs);
    }
    return std::format("{}:{:02}", minutes, secs);
}

/// The pyramid level as what it costs the picture rather than as its number.
///
/// The file carries the recorded lines plus copies decimated in time, and the
/// reader serves whichever fits the pane. "LOD 1" named which one; "1:8
/// detail" says what that means for what is on screen. Derived from the
/// format's own decimation so the label cannot drift from the levels.
std::string detailLabel(std::uint32_t lod) {
    const std::uint32_t decimation = session::lodDecimation(lod);
    return decimation <= 1 ? "full detail" : std::format("1:{} detail", decimation);
}

/// A square button carrying one icon glyph.
///
/// The zeroed padding is the whole point, and it is not cosmetic. ImGui centres
/// a label only while it fits between the frame padding -- RenderTextClipped
/// left-aligns anything wider. An icon's advance is a full font size, because
/// GlyphMinAdvanceX pads it to one so icons line up in a column; the panel pads
/// frames by seven either side; and these buttons are one frame tall. So a
/// 15px label was being fitted into a 9px gap, gave up, and parked itself
/// against the inner left edge -- three pixels right of centre, on every icon
/// equally. Making the square its own text box is also just what a button with
/// no words in it wants.
///
/// Centring the advance box, not the ink: an icon font draws every glyph
/// centred in its own em, so the em is the thing to centre, and ImGui does that
/// for free. The rect the atlas reports per glyph is the rasterised bitmap --
/// oversampling margins and all, wider than the ink by a different amount on
/// each glyph. It looks like the thing to measure and it is not.
bool iconButton(const char* id, const char* glyph, const char* fallback, float size) {
    const std::string label = std::format("{}{}", icon::glyphOr(glyph, fallback), id);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0F, 0.0F));
    const bool pressed = ImGui::Button(label.c_str(), ImVec2(size, size));
    ImGui::PopStyleVar();

    return pressed;
}

void helpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0F);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

/// Width of the label column shared by every two-column row in a panel.
///
/// In points, like every `labelWidth` passed around below: `field` and
/// `fieldCaption` scale it where they use it, so a column that fits its labels
/// at 100% still fits them at 150%.
constexpr float kFieldLabelWidth = 116.0F;

/// Label on the left, control filling the rest of the row.
///
/// ImGui's built-in label sits *after* the control, which in a narrow sidebar
/// with SetNextItemWidth(-1) leaves it truncated to a single character. Two
/// columns is also what an instrument panel wants: the eye scans a column of
/// names, not a ragged mix of names and values.
///
/// `tooltip` hangs off the label rather than off a "(?)" marker, following the
/// device panel: a marker costs a column of width on every row, and there is
/// none to spare once the control fills what is left.
///
/// `reserve` holds width back at the right-hand end for a trailing widget --
/// a button beside a combo, say -- which SetNextItemWidth(-1) would otherwise
/// push off the edge of the panel.
///
/// Call immediately before the widget, and give the widget a "##id" label.
void field(const char* label, const char* tooltip = nullptr, float reserve = 0.0F,
           float labelWidth = kFieldLabelWidth) {
    // Measured from where this row actually starts rather than from the
    // window's left edge. SameLine's offset ignores the current indent, so a
    // section drawn inside Indent() had its control placed one indent early --
    // over the tail of its own label, which is how "Usable bandwidth" came to
    // read "Usable bandwidtl".
    const float rowStart = ImGui::GetCursorPosX();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip != nullptr && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0F);
        ImGui::TextUnformatted(tooltip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    // A label wider than the column pushes the control right rather than
    // overlapping it: a long one like "Sample rate (MHz)" would otherwise have
    // the control drawn on top of its last few characters.
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float used = ImGui::CalcTextSize(label).x + spacing;
    ImGui::SameLine();
    ImGui::SetCursorPosX(rowStart + std::max(bar::scaled(labelWidth), used));
    ImGui::SetNextItemWidth(reserve > 0.0F ? -(reserve + spacing) : -1.0F);
}

/// The width a two-ended range widget must take to end inside the panel.
///
/// `DragFloatRange2` draws its two halves and then finishes with a SameLine and
/// its label -- and a "##id" label renders as nothing at all. So a row given
/// the whole width by `field` ends `ItemInnerSpacing.x` past the panel's right
/// edge, with nothing drawn out there to show for it.
///
/// An invisible overflow is still an overflow: the popover's content is then
/// wider than the popover, and a trackpad swipe slides the whole panel sideways
/// by those few pixels over a section where nothing was clipped and nothing
/// looked wrong. Handing the spacing back is what stops it.
///
/// Call between `field` and the widget, so the -1 it set has a cursor to
/// resolve against.
[[nodiscard]] float rangeItemWidth() {
    return std::max(ImGui::CalcItemWidth() - ImGui::GetStyle().ItemInnerSpacing.x, 1.0F);
}

/// A dim line under a row, aligned to the control column.
///
/// It explains that row's own value rather than the section, so it belongs
/// under the control -- which is what the leading spaces it used to be padded
/// with were trying, and in a proportional font failing, to do.
/// `labelWidth` of zero puts it at the margin, for a caption under a row that
/// has no control column of its own. Zero cannot be passed to Indent, which
/// reads it as "the default indent" rather than as none.
///
/// Wrapped at the panel's right edge. These panels live in popovers capped at
/// 440 points, so a caption longer than that is not merely inelegant -- it is
/// clipped, and the half an operator needs is as likely to be in the half that
/// vanished. Wrapping costs nothing for the short captions that are most of
/// them: the wrap point only takes effect once the text reaches it.
void fieldCaption(const std::string& text, float labelWidth = kFieldLabelWidth) {
    const float column = bar::scaled(labelWidth);
    const bool indented = column > 0.0F;
    if (indented) {
        ImGui::Indent(column);
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    // Zero means the window's own content edge -- and inside a table, the
    // column's, which is what makes this correct in a readout cell too.
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    if (indented) {
        ImGui::Unindent(column);
    }
}

/// "Wideband · discone · 25 MHz - 1.3 GHz · 2.1 dBi · bias-T"
///
/// Built by joining only the parts that were filled in: a library entry with
/// no category is common on a hand-typed one, and " ·  · discone" reads as a
/// rendering fault rather than as an omission.
std::string describeAntenna(const Antenna& antenna) {
    std::string text;
    const auto append = [&text](std::string_view part) {
        if (part.empty()) {
            return;
        }
        if (!text.empty()) {
            text += " · ";
        }
        text += part;
    };

    append(antenna.category);
    append(antenna.type);
    append(antenna.describeRange());
    append(std::format("{:+.1f} dBi", antenna.gainDbi));
    if (antenna.needsBiasT) {
        append("bias-T");
    }
    return text;
}

/// First letter upper-cased.
///
/// `toString` returns the canonical lowercase spelling -- "repository" -- which
/// is what belongs in a file and on a command line. A button is a label rather
/// than a value, and sits in a row with "Show in folder".
std::string capitalised(std::string_view text) {
    std::string result(text);
    if (!result.empty()) {
        result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
    }
    return result;
}

/// A button that shows `path` in the desktop's file manager.
///
/// Returns what to say about it, or nothing when there is nothing to say.
/// Falling back to the clipboard rather than doing nothing matters: a headless
/// session or a minimal container has no file manager at all, and a button
/// that silently does nothing is worse than one that hands over the path.
///
/// `create` makes the directory first, for the places an operator is being
/// invited to put something -- a plugins folder that does not exist yet is
/// exactly when someone wants to open it.
[[nodiscard]] std::string revealButton(const char* label, const std::filesystem::path& path,
                                       bool create = false) {
    const bool clicked = ImGui::SmallButton(label);

    // The path is what the button used to be, so it stays reachable by
    // hovering rather than only by clicking through to a file manager.
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", path.string().c_str());
    }
    if (!clicked) {
        return {};
    }

    if (create) {
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
    }

    if (revealInFileManager(path)) {
        return {};
    }

    ImGui::SetClipboardText(path.string().c_str());
    return std::format("no file manager here; copied {}", path.string());
}

struct Toggle {
    const char* label;
    bool* value;
};

/// Related toggles sharing one row, on an even pitch.
///
/// Three of them stacked vertically cost three rows to say one thing. Side by
/// side they read as the set they are, and the panel keeps the height for the
/// controls that need it.
void toggleRow(std::span<const Toggle> toggles) {
    if (toggles.empty()) {
        return;
    }

    const float start = ImGui::GetCursorPosX();
    const float pitch = ImGui::GetContentRegionAvail().x / static_cast<float>(toggles.size());

    for (std::size_t i = 0; i < toggles.size(); ++i) {
        if (i > 0) {
            ImGui::SameLine();
            ImGui::SetCursorPosX(start + pitch * static_cast<float>(i));
        }
        ImGui::Checkbox(toggles[i].label, toggles[i].value);
    }
}

/// One row of a read-only two-column readout: dim name, plain value.
///
/// Inside a table, because padded strings do not line up: in a proportional
/// font "steps       271" and "FFT size    123188" put their numbers in
/// different places, and a column of figures the eye cannot run down is not a
/// column.
void readoutRow(const char* label, const std::string& value, const char* tooltip = nullptr) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    if (tooltip != nullptr && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0F);
        ImGui::TextUnformatted(tooltip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::TableNextColumn();

    // Wrapped inside the value column. A readout carries short things most of
    // the time -- a bin count, an RBW -- but not always: a plugin's file path
    // or a device's link description is routinely wider than a 440-point
    // popover, and an unwrapped one is clipped at the column's clip rect with
    // no indication that anything was cut. Inside a table ImGui takes the
    // column's own right edge as the wrap point, so this stays within its
    // cell rather than running under the next one.
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopTextWrapPos();
}

/// A long hexadecimal serial, cut down to the part that identifies the radio.
///
/// 32 characters fill the sidebar edge to edge, and none of them are ever read:
/// a serial is compared, against a label on a case or another window, and the
/// ends are what a comparison uses. Callers keep the full string for the
/// tooltip.
[[nodiscard]] std::string shortSerial(const std::string& serial) {
    constexpr std::size_t kKeep = 8;

    // A HackRF pads its 64-bit part ID out to the full 128-bit field, so half
    // the string is zeros carrying nothing. Stripped only while enough remains
    // to still be a serial, so a genuinely short one is left alone.
    std::size_t begin = serial.find_first_not_of('0');
    if (begin == std::string::npos || serial.size() - begin < kKeep) {
        begin = 0;
    }

    const std::string_view trimmed = std::string_view(serial).substr(begin);
    if (trimmed.size() <= kKeep * 2 + 3) {
        return std::string(trimmed);
    }
    return std::format("{}...{}", trimmed.substr(0, kKeep), trimmed.substr(trimmed.size() - kKeep));
}

/// A firmware or FPGA version, with what the driver supports beside it.
///
/// A radio ahead of its driver gets the warning colour: nothing is broken, but
/// the driver is reading it through a compatibility table written before the
/// radio existed, so everything else it reports about the hardware comes from a
/// version older than the hardware.
void versionRow(const char* label, const VersionReport& report, const ChromeTheme& chrome) {
    if (report.version.empty()) {
        return;
    }

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();

    if (report.aheadOfDriver) {
        ImGui::TextColored(toImVec4(chrome.warning), "%s", report.version.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Newer than this driver supports (%s)", report.knownLatest.c_str());
        }
        return;
    }

    ImGui::TextUnformatted(report.version.c_str());
    if (report.knownLatest.empty()) {
        return;
    }

    // "supported", never "latest": this is the newest the driver on this
    // machine was built to handle, a floor under the vendor's newest release
    // rather than a substitute for it. Nothing here talks to the vendor.
    ImGui::SameLine();
    ImGui::TextColored(report.knownLatest == report.version ? toImVec4(chrome.textDim)
                                                            : toImVec4(chrome.accent),
                       "(supported: %s)", report.knownLatest.c_str());
}

/// The unit written after a parameter's control, empty when there is none to
/// write or the widget carries its own inside.
///
/// Hz and S/s are displayed scaled to mega, and nothing in "61.400001" says so
/// -- it is a plausible sample rate in three different units.
[[nodiscard]] std::string trailingUnit(const SdrParameter& parameter) {
    // A parameter with presets is drawn as a dropdown whose entries already
    // read "20 MS/s", so a unit written after it would say so twice.
    if (!parameter.enumValues.empty()) {
        return {};
    }

    const bool scaledToMega = parameter.type == SdrParameterType::Double &&
                              (parameter.unit == "Hz" || parameter.unit == "S/s");
    return scaledToMega ? std::format("M{}", parameter.unit) : std::string();
}

/// A dropdown over a parameter's declared values, answering with the stored
/// form of whichever was picked.
///
/// Shared by the enum widget and by the numeric parameters that carry presets,
/// since the two differ only in how the chosen string becomes a value again.
///
/// `preview` is what the closed box reads, and it comes from
/// `SdrParameter::format` so an off-list value shows as itself. A rate set from
/// `--sample-rate` or restored from a profile is perfectly legal and simply is
/// not one of the presets; showing it beats showing an empty box, and beats
/// snapping it to a neighbour behind the operator's back.
[[nodiscard]] std::optional<std::string>
drawChoices(const std::string& label, const SdrParameter& parameter, const std::string& preview) {
    std::optional<std::string> chosen;
    if (!ImGui::BeginCombo(label.c_str(), preview.c_str())) {
        return chosen;
    }

    for (const SdrEnumValue& option : parameter.enumValues) {
        // Compared on the label because that is what `format` produced for the
        // current value, whatever its underlying type.
        const bool selected = option.label == preview;
        if (ImGui::Selectable(option.label.c_str(), selected)) {
            chosen = option.value;
        }
        if (!option.description.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", option.description.c_str());
        }
    }

    ImGui::EndCombo();
    return chosen;
}

/// Opens the two-column table `readoutRow` writes into.
bool beginReadout(const char* id, float labelWidth = kFieldLabelWidth) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) {
        return false;
    }
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed,
                            labelWidth - ImGui::GetStyle().CellPadding.x * 2.0F);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

/// The rule a settings panel's buttons sit under, with air either side of it.
///
/// A bare Separator() between two rows spaced five pixels apart is a line
/// touching both of them, which reads as a border around the last control
/// rather than as the end of the settings.
void footerRule() {
    ImGui::Dummy(ImVec2(0.0F, 3.0F));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0F, 2.0F));
}

/// Buttons sharing the footer row, splitting the width between them.
///
/// Stacked full-width buttons give a destructive action the same weight as the
/// panel's heading, and a lone "Clear" in the middle of a column of checkboxes
/// reads as one of them.
float footerButtonWidth(int count) {
    const float spacing = ImGui::GetStyle().ItemSpacing.x * static_cast<float>(count - 1);
    return (ImGui::GetContentRegionAvail().x - spacing) / static_cast<float>(count);
}

/// Column layout shared by every table in the Performance panel.
///
/// Fixed widths rather than stretch-to-content: these values change several
/// times a second, and a column sized to what is in it re-measures on every
/// update, so the whole panel twitches a pixel or two sideways continuously.
/// Fixing them also lines the sections up with each other, which
/// stretch-sizing cannot do because each table measures only its own rows.
void setupStatColumns() {
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 150.0F);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, 170.0F);
    ImGui::TableSetupColumn("spark", ImGuiTableColumnFlags_WidthStretch);
}

/// One labelled statistic with an optional sparkline.
void statRow(const char* label, const std::string& value, const float* history = nullptr,
             int historyCount = 0, float minimum = 0.0F, float maximum = 1.0F,
             const Color* color = nullptr) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value.c_str());
    ImGui::TableNextColumn();

    if (history != nullptr && historyCount > 1 && color != nullptr) {
        ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));

        const std::string id = std::format("##spark{}", label);
        if (ImPlot::BeginPlot(id.c_str(), ImVec2(-1, 22),
                              ImPlotFlags_CanvasOnly | ImPlotFlags_NoInputs)) {
            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations,
                              ImPlotAxisFlags_NoDecorations);
            ImPlot::SetupAxesLimits(0, historyCount - 1, static_cast<double>(minimum),
                                    static_cast<double>(maximum), ImPlotCond_Always);
            ImPlotSpec spec;
            spec.LineColor = toImVec4(*color);
            spec.LineWeight = 1.2F;
            ImPlot::PlotLine("##v", history, historyCount, 1.0, 0.0, spec);
            ImPlot::EndPlot();
        }

        ImGui::PopStyleColor();
        ImPlot::PopStyleVar();
    }
}

/// Equivalent noise bandwidth of a window, in bins.
///
/// The factor that makes RBW mean what it says: a Hann window spreads a tone
/// over about 1.5 bins, so a transform of N points at rate fs resolves
/// fs * ENBW / N, not fs / N. Measured from the window itself rather than
/// tabulated, so a changed window definition cannot silently invalidate it.
double windowEnbw(WindowType type, double beta) {
    if (auto window = Window::create(type, 1024, beta)) {
        return window->properties().enbw;
    }
    return 1.5;
}

/// Transform sizes offered in the advanced panel.
///
/// Powers of two only. Every FFT implementation is fastest on them, anything
/// between two of them costs the same as the larger while resolving like the
/// smaller, and a fixed list makes the resolution ladder something an operator
/// can step through rather than a number to guess at. The small end matters on
/// a wide sweep, where a short transform per step is what keeps the sweep
/// rate up.
/// The top of the list is `snapFftSize`'s own ceiling, so every size the RBW
/// field can reach is also one this list can name. At 20 MS/s it spans 470 kHz
/// down to 29 Hz of resolution bandwidth, and at 100 MS/s 2.3 MHz down to
/// 143 Hz -- a 1 kHz RBW is somewhere in the middle of the ladder at any rate
/// a radio here supports.
constexpr std::array<std::uint32_t, 15> kFftSizes{64,    128,    256,    512,    1024,
                                                  2048,  4096,   8192,   16384,  32768,
                                                  65536, 131072, 262144, 524288, 1048576};

/// Clamps a requested transform size to something the backend will accept.
///
/// Even, and within range. Not rounded to a power of two: FFTW handles any
/// size, and forcing powers of two would mean an operator asking for a
/// specific RBW could not have it.
std::uint32_t snapFftSize(double requested) {
    const double clamped = std::clamp(requested, 16.0, 1048576.0);
    auto size = static_cast<std::uint32_t>(clamped + 0.5);
    return size % 2 == 0 ? size : size + 1;
}

/// Copies a rolling history into a flat array for plotting.
template <std::size_t N>
int flatten(const RollingHistory<N>& history, std::array<float, N>& out) {
    const int count = static_cast<int>(history.count());
    for (int i = 0; i < count; ++i) {
        out[static_cast<std::size_t>(i)] = history.at(static_cast<std::size_t>(i));
    }
    return count;
}

} // namespace

// ----------------------------------------------------------------- panels

bool MainWindow::frequencyRow(const char* label, double& valueHz, int id) {
    bool changed = false;
    ImGui::PushID(id);

    // Measured from the row rather than from the window's left edge, for the
    // reason field() is: SameLine's offset ignores the current indent, and this
    // section draws inside one.
    const float rowStart = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(rowStart + 72.0F);

    double megahertz = valueHz / 1e6;
    ImGui::SetNextItemWidth(104.0F);
    if (ImGui::InputDouble("##value", &megahertz, 0.0, 0.0, "%.3f",
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
        valueHz = megahertz * 1e6;
        changed = true;
    }

    // Symmetric steps either side of zero, coarse to fine outwards, so the
    // pair for a given size sit next to each other and the row reads as a
    // scale rather than a list of buttons.
    constexpr std::array<double, 6> kStepsMhz{-100.0, -10.0, -1.0, 1.0, 10.0, 100.0};
    for (std::size_t i = 0; i < kStepsMhz.size(); ++i) {
        ImGui::SameLine(0.0F, 3.0F);
        const std::string caption =
            std::format("{}{:g}##step{}", kStepsMhz[i] > 0.0 ? "+" : "", kStepsMhz[i], i);
        if (ImGui::Button(caption.c_str())) {
            // Never negative: a frequency below zero is not a thing, and
            // clamping here means the plan never has to be rejected for it.
            valueHz = std::max(0.0, valueHz + kStepsMhz[i] * 1e6);
            changed = true;
        }
    }

    ImGui::PopID();
    return changed;
}

bool MainWindow::drawPresetList(SweepPlan& plan) {
    bool changed = false;
    SweepPresetStore& store = m_state.presets();

    ImGui::SeparatorText("Presets");

    // Saving the current range. The name defaults to the range itself, so the
    // common case is one click rather than a naming decision.
    if (m_newPresetName.empty()) {
        m_newPresetName = std::format("{} - {}", toml_util::formatFrequencyShort(plan.lowestHz()),
                                      toml_util::formatFrequencyShort(plan.highestHz()));
    }

    // "Save", not "Add": the rows below now have an add of their own, which
    // puts a preset's ranges *into* the plan. One panel cannot have two Adds
    // pointing in opposite directions.
    const float saveWidth = ImGui::CalcTextSize("Save").x + ImGui::GetStyle().FramePadding.x * 4.0F;

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s", m_newPresetName.c_str());
    ImGui::SetNextItemWidth(-(saveWidth + ImGui::GetStyle().ItemSpacing.x));
    if (ImGui::InputText("##presetname", buffer, sizeof(buffer))) {
        m_newPresetName = buffer;
    }
    ImGui::SameLine();
    if (ImGui::Button("Save", ImVec2(saveWidth, 0)) && !m_newPresetName.empty()) {
        store.add(SweepPreset{.name = m_newPresetName, .segments = plan.segments});
        m_state.savePresets();
        m_newPresetName.clear();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Save the ranges above under this name");
    }

    const ISdrDevice* device = m_state.device();

    // Square buttons the height of the row, so the three of them and the row
    // between them share one baseline. SmallButton is shorter than a frame and
    // left the actions floating above the name they belong to.
    const float action = ImGui::GetFrameHeight();
    const float actionsWidth = (action + ImGui::GetStyle().ItemSpacing.x) * 2.0F;

    std::string pendingRemoval;
    for (const SweepPreset& preset : store.presets()) {
        ImGui::PushID(preset.name.c_str());

        // A preset the open radio cannot reach is shown but not offered. The
        // list is the same on every machine, so hiding it would make presets
        // silently disappear when a narrower device is plugged in -- greying
        // it out says "this exists, just not for this radio".
        const bool reachable =
            device == nullptr || (preset.lowestHz() >= device->info().minFrequencyHz &&
                                  preset.highestHz() <= device->info().maxFrequencyHz);

        // The star toggles favourite, which is what controls the ordering --
        // the list grows without bound otherwise and the daily handful sinks.
        if (iconButton("##favourite", preset.favourite ? icon::kStar : icon::kStarOff,
                       preset.favourite ? "*" : "-", action)) {
            store.setFavourite(preset.name, !preset.favourite);
            m_state.savePresets();
            ImGui::PopID();
            break;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(preset.favourite ? "Remove from favourites"
                                               : "Keep at the top of the list");
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(!reachable);

        // Name and range on one row, drawn rather than concatenated into the
        // label.
        //
        // A button centres its text and a trailing column has to fit whatever
        // is left, so putting the range after the button pushed it off the
        // edge of the popup -- the range being exactly the part that says
        // whether the preset is the one wanted. Drawing both inside the row
        // pins the name left and the range right, and neither can crowd the
        // other out.
        const float rowWidth = std::max(ImGui::GetContentRegionAvail().x - actionsWidth, 80.0F);

        const std::string id = std::format("##load{}", preset.name);
        const bool activated = ImGui::Selectable(id.c_str(), false, ImGuiSelectableFlags_None,
                                                 ImVec2(rowWidth, action));
        const bool rowHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();

        // Alt-click adds instead of replacing, which is the modifier the same
        // choice already carries on the spectrum: shift+ctrl+drag sweeps a
        // band, and adding alt makes it "also sweep this". The row's own +
        // button does the same thing without a modifier to remember.
        if (activated) {
            if (ImGui::GetIO().KeyAlt) {
                addPresetToPlan(plan, preset);
            } else {
                plan.segments = preset.segments;
                m_multipleRanges = preset.segments.size() > 1;
            }
            changed = true;
        }
        ImGui::EndDisabled();

        const std::string range = preset.describeRange();
        const float rangeWidth = ImGui::CalcTextSize(range.c_str()).x;
        const float textY = rowMin.y + (rowMax.y - rowMin.y - ImGui::GetTextLineHeight()) * 0.5F;

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ChromeTheme& chrome = m_state.theme().chrome();
        draw->AddText(ImVec2(rowMin.x + 6.0F, textY),
                      packed(reachable ? chrome.text : chrome.textDim), preset.name.c_str());
        draw->AddText(ImVec2(rowMax.x - rangeWidth - 6.0F, textY), packed(chrome.textDim),
                      range.c_str());

        if (rowHovered) {
            if (reachable) {
                ImGui::SetTooltip("%s\n%s\n\nClick: sweep this\nAlt-click or +: add to the plan",
                                  preset.name.c_str(), range.c_str());
            } else {
                ImGui::SetTooltip(
                    "%s\n%s\n\nOutside what %s can tune (%s - %s).", preset.name.c_str(),
                    range.c_str(), device->info().label.c_str(),
                    toml_util::formatFrequencyShort(device->info().minFrequencyHz).c_str(),
                    toml_util::formatFrequencyShort(device->info().maxFrequencyHz).c_str());
            }
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(!reachable);
        if (iconButton("##addtoplan", icon::kAdd, "+", action)) {
            addPresetToPlan(plan, preset);
            changed = true;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Add to the plan");
        }

        ImGui::SameLine();
        if (iconButton("##remove", icon::kClose, "x", action)) {
            pendingRemoval = preset.name;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(preset.builtin ? "Hide this built-in preset" : "Delete this preset");
        }
        ImGui::PopID();
    }

    // Deferred: removing inside the loop would invalidate the range being
    // iterated.
    if (!pendingRemoval.empty()) {
        store.remove(pendingRemoval);
        m_state.savePresets();
    }

    return changed;
}

/// Puts a preset's ranges into the plan without discarding what is there.
///
/// addSegment rather than append, so a preset abutting or overlapping a range
/// already planned becomes one range: overlapping segments are not a plan the
/// stitcher can resolve, and validate() rejects them outright. This is the same
/// call the shift+ctrl+alt drag on the spectrum makes, so the two ways of
/// saying "also look here" cannot end up meaning different things.
void MainWindow::addPresetToPlan(SweepPlan& plan, const SweepPreset& preset) {
    for (const SweepSegment& segment : preset.segments) {
        plan.addSegment(segment);
    }

    // The multi-range editor is what can show more than one row, so a plan that
    // has just grown one needs it -- otherwise the panel collapses straight
    // back to a single start/stop and the range just added is invisible.
    m_multipleRanges = plan.segments.size() > 1;
}

void MainWindow::drawSourceSection() {
    const ISdrDevice* device = m_state.device();

    // Nothing open means there is only one useful thing to show, whatever the
    // panel was last displaying.
    if (device == nullptr) {
        m_deviceChooserMode = true;
    }

    if (m_deviceChooserMode) {
        ImGui::SeparatorText(device == nullptr ? "Select a device" : "Switch device");

        const std::vector<SdrDeviceInfo>& available = m_state.availableDevices();
        if (available.empty()) {
            ImGui::TextDisabled("No radios detected.");
            ImGui::TextDisabled("Connect one and refresh.");
        }

        // Deferred for the same reason the preset removal above is, with worse
        // consequences: opening a device closes the current one, and `device`
        // is a plain pointer to it. Acting inside the loop left the next
        // iteration calling info() through a destroyed object's vtable.
        std::optional<SdrDeviceInfo> pendingOpen;

        for (const SdrDeviceInfo& info : available) {
            const bool isOpen = device != nullptr && device->info().id == info.id;

            // The whole row is the target, so the identifying detail can sit
            // under the name instead of being crammed into a button label.
            ImGui::BeginGroup();
            const std::string id = std::format("##dev{}", info.id);
            if (ImGui::Selectable(id.c_str(), isOpen, ImGuiSelectableFlags_None,
                                  ImVec2(0, ImGui::GetTextLineHeight() * 2.4F))) {
                // Copied, not referenced: opening may rebuild the very list
                // being iterated.
                pendingOpen = info;
            }

            const ImVec2 rowMin = ImGui::GetItemRectMin();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddText(ImVec2(rowMin.x + 6.0F, rowMin.y + 3.0F),
                          packed(m_state.theme().chrome().text), info.label.c_str());

            // Enough to tell two attached radios apart, and no more: what a
            // radio can say about itself needs it open, which this row is the
            // way to.
            const std::string detail = std::format(
                "{}  {}", info.driver, info.serial.empty() ? info.id : shortSerial(info.serial));
            draw->AddText(ImVec2(rowMin.x + 6.0F, rowMin.y + 3.0F + ImGui::GetTextLineHeight()),
                          packed(m_state.theme().chrome().textDim), detail.c_str());
            ImGui::EndGroup();
        }

        ImGui::Spacing();
        if (ImGui::Button("Refresh", ImVec2(-1, 0))) {
            m_state.beginRefreshDevices();
        }

        if (device != nullptr) {
            if (ImGui::Button("Back to settings", ImVec2(-1, 0))) {
                m_deviceChooserMode = false;
            }
        }

        // Last, once nothing above still holds a pointer into what this
        // invalidates. `device` is stale from here on, which is why the panel
        // simply ends and redraws next frame against the new radio.
        //
        // Started, not waited for: claiming a radio takes seconds, and doing it
        // here left the window frozen with the panel still on screen and the
        // row apparently unclicked. The progress panel says what is happening
        // and holds the controls until it is done; failures arrive in the
        // corner the same way the startup radio's do.
        if (pendingOpen) {
            m_state.beginOpenDevice(pendingOpen->driver, pendingOpen->id, pendingOpen->label);

            // Straight to its settings: choosing a radio is a step towards
            // using it, not the end of the task.
            m_deviceChooserMode = false;
        }
        return;
    }

    const SdrDeviceInfo& info = device->info();

    ImGui::SeparatorText(info.label.c_str());

    if (beginReadout("##identity")) {
        if (!info.serial.empty()) {
            readoutRow("Serial", shortSerial(info.serial));

            // The full string is what a --device argument or a bug report
            // wants, and a click is cheaper than transcribing 32 characters.
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\n\nClick to copy.", info.serial.c_str());
            }
            if (ImGui::IsItemClicked()) {
                ImGui::SetClipboardText(info.serial.c_str());
                toast(ToastSeverity::Info, "serial copied to the clipboard");
            }
        }
        versionRow("Firmware", info.firmware, m_state.theme().chrome());
        versionRow("FPGA", info.fpga, m_state.theme().chrome());
        if (!info.linkDescription.empty()) {
            readoutRow("Link", info.linkDescription);
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    drawDeviceParameters();

    ImGui::Spacing();
    drawDeviceAntennas();

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Change device", ImVec2(-1, 0))) {
        m_deviceChooserMode = true;
    }
    if (ImGui::Button("Close device", ImVec2(-1, 0))) {
        m_state.stop();
        m_state.closeDevice();
        m_deviceChooserMode = true;
    }
}

void MainWindow::drawAntennaRow(const char* label, const SdrRxPort* port,
                                std::string_view assignedId, bool biasTeeOn, bool deviceHasBiasTee,
                                const std::function<void(std::string_view)>& assign,
                                const std::function<void(std::string_view)>& assignSwitcher) {
    const AntennaLibrary& library = m_state.antennas();
    const ChromeTheme& chrome = m_state.theme().chrome();
    const Antenna* assigned = assignedId.empty() ? nullptr : library.find(assignedId);

    field(label, port != nullptr && !port->connector.empty() ? port->connector.c_str() : nullptr);
    if (ImGui::BeginCombo("##antenna", assigned != nullptr ? assigned->name.c_str() : "- none -")) {
        if (ImGui::Selectable("- none -", assigned == nullptr)) {
            assign({});
        }
        for (const Antenna& antenna : library.entries()) {
            const bool isSelected = assigned != nullptr && assigned->id == antenna.id;
            if (ImGui::Selectable(antenna.name.c_str(), isSelected)) {
                assign(antenna.id);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", describeAntenna(antenna).c_str());
            }
        }

        // A connector can carry a box instead of an antenna; an input of that
        // box cannot carry another one. Nothing in the model forbids nesting,
        // and nothing in the RF does either -- but a chain the panel cannot
        // draw is a chain an operator cannot check against the bench.
        if (assignSwitcher && !m_switcherList.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("Switchers");
            for (const RfPathInfo& info : m_switcherList) {
                const std::string key = rfPathKey(info);
                if (ImGui::Selectable(info.label.c_str())) {
                    assignSwitcher(key);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s -- %u inputs", key.c_str(), info.inputCount);
                }
            }
        }

        // The second way in to the editor, and the one that matters:
        // discovering the library is empty happens here, with a connector in
        // front of you, not in a settings menu.
        ImGui::Separator();
        if (ImGui::Selectable("New antenna...")) {
            beginEditingAntenna(nullptr);
        }
        ImGui::EndCombo();
    }

    if (assigned == nullptr) {
        if (!assignedId.empty()) {
            // The assignment is kept rather than dropped: restoring the
            // antenna restores it, and silently unassigning would leave a
            // routed sweep quietly measuring this band through whatever is
            // connected.
            ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(chrome.warning));
            fieldCaption(std::format("antenna '{}' is missing from the library", assignedId));
            ImGui::PopStyleColor();
        } else {
            fieldCaption("nothing assigned");
        }
        return;
    }

    fieldCaption(describeAntenna(*assigned));

    if (!assigned->needsBiasT) {
        return;
    }

    const char* reason = nullptr;
    if (port != nullptr && !port->biasTee) {
        reason = "this port cannot supply bias-T";
    } else if (!deviceHasBiasTee) {
        reason = "this radio cannot supply bias-T";
    } else if (!biasTeeOn) {
        reason = "bias-T is switched off";
    }
    if (reason != nullptr) {
        // An unpowered active antenna measures its own noise floor, and it
        // does so convincingly -- a flat trace at a plausible level, with
        // nothing anywhere saying why the band went quiet.
        ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(chrome.warning));
        fieldCaption(reason);
        ImGui::PopStyleColor();
    }
}

void MainWindow::drawDeviceAntennas() {
    ISdrDevice* device = m_state.device();
    if (device == nullptr) {
        return;
    }

    ImGui::SeparatorText("Antennas");

    // Rescanned on a timer rather than per frame: `enumerate` is a bus scan
    // per driver, and this panel is drawn sixty times a second.
    if (const std::uint64_t now = monotonicNs();
        now - m_switcherListNs > secondsToNs(2.0) || m_switcherListNs == 0) {
        m_switcherList = RfPathManager::instance().enumerateAll();
        m_switcherListNs = now;
    }

    const std::span<const SdrRxPort> ports = device->rxPorts();
    const std::string deviceKey = m_state.deviceAntennaKey();
    const AntennaLibrary& library = m_state.antennas();
    const SweepPlan& plan = m_state.sweepPlan();
    AntennaAssignments& assignments = m_state.antennaAssignments();

    // Whether the radio's bias tee is on right now, where it has one at all.
    // Per-device rather than per-port on every radio in the tree, which is why
    // the warning distinguishes "this port cannot supply it" from "it is
    // switched off".
    const std::span<const SdrParameter> parameters = device->parameters();
    const bool hasBiasTee =
        std::ranges::any_of(parameters, [](const SdrParameter& p) { return p.key == "bias_tee"; });
    bool biasTeeOn = false;
    if (hasBiasTee) {
        if (const auto value = device->getParameter("bias_tee")) {
            biasTeeOn = asBool(*value);
        }
    }

    // A radio with one connector still gets a row: the antenna in front of it
    // is just as much a fact about the measurement. What it does not get is a
    // port chooser, because there is nothing to choose.
    const std::string_view selected = device->selectedRxPort();
    const std::size_t rowCount = std::max<std::size_t>(ports.size(), 1);

    for (std::size_t i = 0; i < rowCount; ++i) {
        const SdrRxPort* port = i < ports.size() ? &ports[i] : nullptr;
        const std::string portId = port != nullptr ? port->id : std::string{};
        const std::string label = port != nullptr ? port->label : std::string("Antenna");

        ImGui::PushID(static_cast<int>(i));

        const std::string switcherKey{assignments.switcherFor(deviceKey, portId)};
        if (!switcherKey.empty()) {
            IRfPath* switcher = m_state.switcher(switcherKey);

            field(label.c_str(),
                  port != nullptr && !port->connector.empty() ? port->connector.c_str() : nullptr);
            if (ImGui::BeginCombo("##switcher", switcher != nullptr ? switcher->info().label.c_str()
                                                                    : switcherKey.c_str())) {
                if (ImGui::Selectable("- none -")) {
                    assignments.assignSwitcher(deviceKey, portId, {});
                    m_state.saveAntennaAssignments();
                }
                for (const Antenna& antenna : library.entries()) {
                    if (ImGui::Selectable(antenna.name.c_str())) {
                        assignments.assign(deviceKey, portId, antenna.id);
                        m_state.saveAntennaAssignments();
                    }
                }
                if (!m_switcherList.empty()) {
                    ImGui::Separator();
                    ImGui::TextDisabled("Switchers");
                }
                for (const RfPathInfo& info : m_switcherList) {
                    const std::string key = rfPathKey(info);
                    if (ImGui::Selectable(info.label.c_str(), key == switcherKey)) {
                        assignments.assignSwitcher(deviceKey, portId, key);
                        m_state.saveAntennaAssignments();
                    }
                }
                ImGui::EndCombo();
            }

            if (switcher == nullptr) {
                ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(m_state.theme().chrome().warning));
                fieldCaption("this switcher is not connected; its antennas are not available");
                ImGui::PopStyleColor();
                ImGui::PopID();
                continue;
            }

            // One row per input, indented under the box they are inside. The
            // indent is the whole visual statement that this is a chain and
            // not four more connectors on the radio.
            ImGui::Indent(12.0F);
            const std::span<const RfPathInput> inputs = switcher->inputs();
            for (std::size_t in = 0; in < inputs.size(); ++in) {
                ImGui::PushID(static_cast<int>(in));
                const std::string inputId = inputs[in].id;
                const std::string_view assignedId =
                    assignments.antennaOnInput(switcherKey, inputId);

                drawAntennaRow(inputs[in].label.c_str(), port, assignedId, biasTeeOn, hasBiasTee,
                               [&](std::string_view antennaId) {
                                   assignments.assignInput(switcherKey, inputId, antennaId);
                                   m_state.saveAntennaAssignments();
                               },
                               {});

                ImGui::PopID();
            }
            ImGui::Unindent(12.0F);

            if (port != nullptr && port->id == selected) {
                fieldCaption("listening on this one now");
            }
            ImGui::PopID();
            continue;
        }

        const std::string_view assignedId = assignments.antennaFor(deviceKey, portId);
        drawAntennaRow(
            label.c_str(), port, assignedId, biasTeeOn, hasBiasTee,
            [&](std::string_view antennaId) {
                assignments.assign(deviceKey, portId, antennaId);
                m_state.saveAntennaAssignments();
            },
            [&](std::string_view key) {
                assignments.assignSwitcher(deviceKey, portId, key);
                m_state.saveAntennaAssignments();
            });

        if (port != nullptr && port->id == selected) {
            fieldCaption("listening on this one now");
        }

        ImGui::PopID();
    }

    // Where the frequencies nothing covers get measured.
    //
    // Offered only where there is a choice: on a one-connector radio the sweep
    // has nowhere else to go, and a combo whose every setting does the same
    // thing is worse than no combo.
    if (ports.size() > 1) {
        const std::string_view fallbackId = assignments.fallbackPort(deviceKey);

        // Each entry names the connector and what is on it, because "RX1" and
        // "RX1 (Wideband discone)" are different answers to "what will hear
        // the band nothing claims".
        const auto describePort = [&](const SdrRxPort& port) {
            const Antenna* antenna = m_state.antennaOnPort(port.id);
            if (antenna != nullptr) {
                return std::format("{} ({})", port.label, antenna->name);
            }
            if (!assignments.switcherFor(deviceKey, port.id).empty()) {
                return std::format("{} (switcher)", port.label);
            }
            return port.label;
        };

        const auto current = std::ranges::find_if(
            ports, [fallbackId](const SdrRxPort& port) { return port.id == fallbackId; });

        field("Uncovered ranges", "Connector for frequencies no assigned antenna covers.");
        if (ImGui::BeginCombo("##fallbackport", current != ports.end()
                                                    ? describePort(*current).c_str()
                                                    : "- leave as is -")) {
            if (ImGui::Selectable("- leave as is -", current == ports.end())) {
                assignments.setFallbackPort(deviceKey, {});
                m_state.saveAntennaAssignments();
            }
            for (const SdrRxPort& port : ports) {
                if (ImGui::Selectable(describePort(port).c_str(), port.id == fallbackId)) {
                    assignments.setFallbackPort(deviceKey, port.id);
                    m_state.saveAntennaAssignments();
                }
            }
            ImGui::EndCombo();
        }
    }

    // What the assigned antennas add up to against the plan on screen. The
    // question an operator actually has is not "which antenna is on RX2" but
    // "is any of what I am about to sweep going through nothing at all".
    //
    // Through the same resolver the sweep plans against, so the two cannot
    // disagree about what this bench can hear.
    if (!plan.segments.empty()) {
        double covered = 0.0;
        for (const SweepSegment& segment : plan.segments) {
            for (const auto& [startHz, stopHz] : m_state.antennaCoverage()) {
                covered += std::max(0.0, std::min(segment.stopHz, stopHz) -
                                             std::max(segment.startHz, startHz));
            }
        }

        const double planned = plan.totalSpanHz();
        fieldCaption(std::format("covers {} of the planned {}",
                                 toml_util::formatFrequencyShort(std::min(covered, planned)),
                                 toml_util::formatFrequencyShort(planned)),
                     0.0F);
    }
}

void MainWindow::drawDeviceParameters() {
    ISdrDevice* device = m_state.device();
    if (device == nullptr) {
        return;
    }

    // Generated entirely from parameters(). There is no per-device UI code
    // anywhere in the project: a driver that describes its parameters properly
    // gets a correct panel for free, and one that does not cannot be fixed
    // here.
    const std::span<const SdrParameter> parameters = device->parameters();

    // Gathered by group instead of trusting the order they were declared in. A
    // driver may append a parameter after the ones it belongs with -- the
    // bladeRF's gain mode is discovered from the hardware and pushed last --
    // and a header printed on every change of group then wrote "Gain" twice,
    // with somebody else's row in between.
    std::vector<const SdrParameter*> ordered;
    ordered.reserve(parameters.size());
    for (const SdrParameter& parameter : parameters) {
        const bool placed = std::ranges::any_of(ordered, [&parameter](const SdrParameter* seen) {
            return seen->group == parameter.group;
        });
        if (placed) {
            continue;
        }
        for (const SdrParameter& sibling : parameters) {
            if (sibling.group == parameter.group) {
                ordered.push_back(&sibling);
            }
        }
    }

    // One column for the units, sized to the widest of them. Measured across
    // all of them and not only the rows drawn this frame, so the column does
    // not move when a parameter appears or goes away with the gain mode.
    float unitColumnWidth = 0.0F;
    for (const SdrParameter* entry : ordered) {
        const std::string unit = trailingUnit(*entry);
        if (!unit.empty()) {
            unitColumnWidth = std::max(unitColumnWidth, ImGui::CalcTextSize(unit.c_str()).x);
        }
    }

    std::string currentGroup;
    for (const SdrParameter* entry : ordered) {
        const SdrParameter& parameter = *entry;

        // A parameter that does not apply right now is not shown at all. A
        // control that cannot do anything is a question the operator has to
        // answer -- "is this broken, or am I?" -- and the driver has already
        // said which values of which other parameter make this one mean
        // something. An unreadable dependency is treated as satisfied: a row
        // hidden by a parameter that no longer exists is unreachable.
        if (!parameter.appliesWhenKey.empty()) {
            if (auto governing = device->getParameter(parameter.appliesWhenKey)) {
                const std::string held = asString(*governing);
                if (!std::ranges::contains(parameter.appliesWhenValues, held)) {
                    continue;
                }
            }
        }

        if (parameter.group != currentGroup) {
            currentGroup = parameter.group;
            ImGui::SeparatorText(currentGroup.c_str());
        }

        auto current = device->getParameter(parameter.key);
        if (!current) {
            continue;
        }

        const bool disabled =
            parameter.readOnly || (m_state.parameterNeedsStop(parameter) && m_state.running());
        if (disabled) {
            ImGui::BeginDisabled();
        }

        // Two columns, like every other control in a panel. The tooltip
        // carries the description rather than a (?) marker, which would cost a
        // column of width on every row.
        std::string tooltip = parameter.description;
        if (m_state.parameterNeedsStop(parameter) && m_state.running()) {
            if (!tooltip.empty()) {
                tooltip += "\n\n";
            }
            tooltip += "Stop the radio to change this.";
        }
        // The column width is shared by the rows that have a unit, so those
        // line up with each other instead of each ending wherever its own unit
        // happens to. A row with nothing to write there keeps the width.
        const std::string unit = trailingUnit(parameter);
        field(parameter.label.c_str(), tooltip.empty() ? nullptr : tooltip.c_str(),
              unit.empty() ? 0.0F : unitColumnWidth);

        const std::string label = std::format("##{}", parameter.key);
        const std::string intFormat =
            parameter.unit.empty() ? "%d" : std::format("%d {}", parameter.unit);
        const std::string floatFormat =
            parameter.unit.empty() ? "%.2f" : std::format("%.2f {}", parameter.unit);
        bool changed = false;
        SdrValue updated = *current;

        switch (parameter.type) {
        case SdrParameterType::Bool: {
            bool value = asBool(*current);
            if (ImGui::Checkbox(label.c_str(), &value)) {
                updated = SdrValue{value};
                changed = true;
            }
            break;
        }

        case SdrParameterType::Int: {
            int value = static_cast<int>(asInt(*current));
            // A slider when the range is bounded, a drag when it is not --
            // stepping is taken from the parameter so the widget cannot offer
            // a value the hardware would silently round.
            if (parameter.max > parameter.min) {
                if (ImGui::SliderInt(label.c_str(), &value, static_cast<int>(parameter.min),
                                     static_cast<int>(parameter.max), intFormat.c_str())) {
                    updated = SdrValue{static_cast<std::int64_t>(value)};
                    changed = true;
                }
            } else if (ImGui::DragInt(label.c_str(), &value,
                                      static_cast<float>(std::max(parameter.step, 1.0)), 0, 0,
                                      intFormat.c_str())) {
                updated = SdrValue{static_cast<std::int64_t>(value)};
                changed = true;
            }
            break;
        }

        case SdrParameterType::Double: {
            double value = asDouble(*current);

            if (!parameter.enumValues.empty()) {
                // Presets, so a dropdown. The only sane widget for a sample
                // rate spanning 520 kS/s to 61 MS/s: a slider cannot express a
                // usable step across that, and a text field makes the operator
                // recall figures the driver already knows.
                //
                // The value stays a double, so a rate that came from the CLI
                // or a profile and is not on the list still shows -- as itself,
                // rather than as an empty box or a silent snap to a neighbour.
                if (const auto chosen = drawChoices(label, parameter, parameter.format(*current));
                    chosen.has_value()) {
                    if (const auto parsed = toml_util::parseFrequency(*chosen)) {
                        updated = SdrValue{*parsed};
                        changed = true;
                    }
                }

            } else if (parameter.unit == "Hz" || parameter.unit == "S/s") {
                // Frequencies get a text field, because a slider across
                // 1 MHz - 6 GHz cannot express a usable step.
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "%.6f", value / 1e6);
                if (ImGui::InputText(label.c_str(), buffer, sizeof(buffer),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    if (auto parsed = toml_util::parseFrequency(buffer)) {
                        updated = SdrValue{*parsed < 1e6 ? *parsed * 1e6 : *parsed};
                        changed = true;
                    }
                }

            } else {
                auto floatValue = static_cast<float>(value);
                if (ImGui::SliderFloat(label.c_str(), &floatValue,
                                       static_cast<float>(parameter.min),
                                       static_cast<float>(parameter.max), floatFormat.c_str())) {
                    updated = SdrValue{static_cast<double>(floatValue)};
                    changed = true;
                }
            }
            break;
        }

        case SdrParameterType::Enum: {
            if (const auto chosen = drawChoices(label, parameter, parameter.format(*current));
                chosen.has_value()) {
                updated = SdrValue{*chosen};
                changed = true;
            }
            break;
        }

        case SdrParameterType::String: {
            std::string value = asString(*current);
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
            if (ImGui::InputText(label.c_str(), buffer, sizeof(buffer),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                updated = SdrValue{std::string(buffer)};
                changed = true;
            }
            break;
        }
        }

        if (!unit.empty()) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", unit.c_str());
        }

        if (disabled) {
            ImGui::EndDisabled();
        }

        if (changed) {
            if (auto applied = m_state.setDeviceParameter(parameter.key, updated); !applied) {
                toast(ToastSeverity::Error, applied.error().describe());
            } else {
                // Published so the session records it. A grid-affecting change
                // opens a new segment; a calibration-affecting one is noted
                // because the noise floor just moved.
                m_state.events().publish(
                    ParameterChangedEvent{.monotonicNs = monotonicNs(),
                                          .key = parameter.key,
                                          .value = toString(updated),
                                          .gridAffecting = parameter.gridAffecting,
                                          .calibrationAffecting = parameter.calibrationAffecting});
            }
        }
    }
}

void MainWindow::drawRangeSection() {
    ImGui::Indent(6.0F);

    SweepPlan plan = m_state.sweepPlan();
    bool changed = false;

    ImGui::SeparatorText("Ranges");

    // One range or several. Collapsed to a single editor by default, because
    // a discontinuous sweep is the rarer job and the extra rows are pure noise
    // for the operator who wants "look between here and here".
    bool multiple = plan.segments.size() > 1 || m_multipleRanges;
    const float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    if (ImGui::Checkbox("Multiple ranges", &multiple)) {
        m_multipleRanges = multiple;
        if (!multiple && plan.segments.size() > 1) {
            // Keep the extent rather than the first row: collapsing should
            // narrow what is swept as little as possible.
            plan.segments = {SweepSegment{.startHz = plan.lowestHz(), .stopHz = plan.highestHz()}};
            changed = true;
        }
    }
    helpMarker("Sweep several separate bands as one job.");

    // Start/stop or centre/span: the same two numbers, and which pair is
    // natural depends entirely on the task. Tuning onto something reads as a
    // centre and a width; surveying a band reads as two edges.
    //
    // Right-aligned on the row above the fields it re-labels, rather than
    // stacked as a third mode switch: it chooses how to write one range, which
    // is a smaller question than how many ranges there are.
    if (!m_multipleRanges && plan.segments.size() <= 1) {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float width =
            ImGui::CalcTextSize("Start / stop").x + ImGui::CalcTextSize("Centre / span").x +
            ImGui::GetFrameHeight() * 2.0F + style.ItemInnerSpacing.x * 2.0F + style.ItemSpacing.x;

        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rightEdge - width));
        ImGui::RadioButton("Start / stop", &m_rangeEntryMode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Centre / span", &m_rangeEntryMode, 1);
    }

    if (plan.segments.empty()) {
        plan.segments.push_back(SweepSegment{.startHz = 88e6, .stopHz = 108e6});
        changed = true;
    }

    if (!m_multipleRanges && plan.segments.size() == 1) {
        SweepSegment& segment = plan.segments.front();

        if (m_rangeEntryMode == 0) {
            changed |= frequencyRow("Start", segment.startHz, 0);
            changed |= frequencyRow("Stop", segment.stopHz, 1);
        } else {
            double centre = (segment.startHz + segment.stopHz) * 0.5;
            double span = segment.stopHz - segment.startHz;
            bool edited = false;
            edited |= frequencyRow("Centre", centre, 2);
            edited |= frequencyRow("Span", span, 3);
            if (edited) {
                segment.startHz = centre - span * 0.5;
                segment.stopHz = centre + span * 0.5;
                changed = true;
            }
        }

        // The pair the editor is not showing. Whichever way a range is being
        // written, the other reading of it is the one that says whether it is
        // the right range -- and it is two numbers, not a control.
        if (segment.stopHz > segment.startHz) {
            fieldCaption(
                m_rangeEntryMode == 0
                    ? std::format(
                          "centre {}, span {}",
                          toml_util::formatFrequencyShort((segment.startHz + segment.stopHz) * 0.5),
                          toml_util::formatFrequencyShort(segment.stopHz - segment.startHz))
                    : std::format("{} - {}", toml_util::formatFrequencyShort(segment.startHz),
                                  toml_util::formatFrequencyShort(segment.stopHz)),
                72.0F);
        } else {
            ImGui::TextColored(toImVec4(m_state.theme().chrome().warning),
                               "stop must be above start");
        }
    } else {
        // Numbered, because with four or five rows "the third one" is how an
        // operator refers to a range, and the numbers are also what the span
        // readout underneath is counting.
        const float indexWidth = ImGui::CalcTextSize("88").x + ImGui::GetStyle().ItemSpacing.x;

        for (std::size_t i = 0; i < plan.segments.size(); ++i) {
            SweepSegment& segment = plan.segments[i];
            ImGui::PushID(static_cast<int>(i));

            double start = segment.startHz / 1e6;
            double stop = segment.stopHz / 1e6;

            const float rowStart = ImGui::GetCursorPosX();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%zu", i + 1);
            ImGui::SameLine();
            ImGui::SetCursorPosX(rowStart + indexWidth);

            ImGui::SetNextItemWidth(96.0F);
            if (ImGui::InputDouble("##start", &start, 0.0, 0.0, "%.3f",
                                   ImGuiInputTextFlags_EnterReturnsTrue)) {
                segment.startHz = start * 1e6;
                changed = true;
            }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("-");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(96.0F);
            if (ImGui::InputDouble("##stop", &stop, 0.0, 0.0, "%.3f",
                                   ImGuiInputTextFlags_EnterReturnsTrue)) {
                segment.stopHz = stop * 1e6;
                changed = true;
            }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("MHz");

            // The width of the row, which is what says whether this is the
            // range meant -- two edges in megahertz do not read as a span.
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("(%s)", toml_util::formatFrequencyShort(
                                            std::max(segment.stopHz - segment.startHz, 0.0))
                                            .c_str());

            // The last range cannot be removed: a plan with nothing in it is
            // not a state the engine can be put into.
            if (plan.segments.size() > 1) {
                const float remove = ImGui::GetFrameHeight();
                ImGui::SameLine();
                const float rowRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
                ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rowRight - remove));
                if (iconButton("##dropRange", icon::kClose, "x", remove)) {
                    plan.segments.erase(plan.segments.begin() + static_cast<std::ptrdiff_t>(i));
                    changed = true;
                    ImGui::PopID();
                    break;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Drop this range from the sweep");
                }
            }
            ImGui::PopID();
        }

        if (ImGui::Button("Add range", ImVec2(-1, 0))) {
            const double lastStop = plan.highestHz();
            plan.segments.push_back(SweepSegment{.startHz = lastStop, .stopHz = lastStop + 100e6});
            changed = true;
        }

        // What the plan adds up to. With disjoint ranges the extent says
        // almost nothing -- 88 MHz to 5.9 GHz can be two narrow bands -- so the
        // number that matters is how much spectrum is actually covered.
        fieldCaption(std::format("{} ranges, {} covered", plan.segments.size(),
                                 toml_util::formatFrequencyShort(plan.totalSpanHz())),
                     indexWidth);
    }

    // The radio's own limits, and one click to take all of them.
    //
    // Worth its own control rather than leaving it to the presets: the full
    // range is the first thing to look at on an unfamiliar band, it is the
    // only range that is a property of the hardware rather than of the
    // operator's interest, and it changes when the device does.
    if (const ISdrDevice* device = m_state.device()) {
        const SdrDeviceInfo& info = device->info();

        ImGui::Spacing();
        const std::string caption = std::format(
            "Full device range  ({} - {})", toml_util::formatFrequencyShort(info.minFrequencyHz),
            toml_util::formatFrequencyShort(info.maxFrequencyHz));
        if (ImGui::Button(caption.c_str(), ImVec2(-1, 0))) {
            plan.segments = {
                SweepSegment{.startHz = info.minFrequencyHz, .stopHz = info.maxFrequencyHz}};
            m_multipleRanges = false;
            changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Sweep everything %s can tune", info.label.c_str());
        }

        // The same offer, bounded by what is actually plugged in.
        //
        // Worth its own button beside the full range rather than left to the
        // operator to type: a device range is what the radio *could* hear and
        // an antenna range is what it *can*, and the difference is usually
        // most of the span -- time spent sweeping bands no antenna on the
        // bench responds to, printed as spectrum.
        if (const std::vector<std::pair<double, double>> coverage = m_state.antennaCoverage();
            !coverage.empty()) {
            double total = 0.0;
            for (const auto& [startHz, stopHz] : coverage) {
                total += stopHz - startHz;
            }

            const std::string antennaCaption =
                coverage.size() == 1
                    ? std::format("Antenna range  ({} - {})",
                                  toml_util::formatFrequencyShort(coverage.front().first),
                                  toml_util::formatFrequencyShort(coverage.front().second))
                    : std::format("Antenna ranges  ({} ranges, {})", coverage.size(),
                                  toml_util::formatFrequencyShort(total));

            if (ImGui::Button(antennaCaption.c_str(), ImVec2(-1, 0))) {
                plan.segments.clear();
                for (const auto& [startHz, stopHz] : coverage) {
                    plan.addSegment(SweepSegment{.startHz = startHz, .stopHz = stopHz});
                }
                m_multipleRanges = plan.segments.size() > 1;
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Sweep only what the assigned antennas can hear");
            }
        }

        // Said here rather than only when the engine refuses it, so the
        // operator sees the problem while editing rather than on Start.
        if (plan.lowestHz() < info.minFrequencyHz || plan.highestHz() > info.maxFrequencyHz) {
            ImGui::TextColored(toImVec4(m_state.theme().chrome().warning),
                               "outside what this radio can tune");
        }
    }

    changed |= drawPresetList(plan);

    if (changed) {
        if (auto applied = m_state.applySweepPlan(plan); !applied) {
            toast(ToastSeverity::Error, applied.error().describe());
        }
    }

    ImGui::Unindent(6.0F);
}

void MainWindow::drawAnalysisSection() {
    ImGui::Indent(6.0F);

    SweepPlan plan = m_state.sweepPlan();
    PipelineConfig config = m_state.pipelineConfig();
    bool planChanged = false;
    bool configChanged = false;

    // Simple or advanced, in one panel rather than two.
    //
    // Resolution bandwidth and FFT size are the same quantity seen from two
    // ends -- RBW = sampleRate * ENBW / N -- so splitting them across a
    // "Sweep" panel and an "FFT" panel meant setting one and watching the
    // other quietly disagree. Most operators only ever want the first of
    // these; the rest are here for when the default is wrong.
    //
    // Right-aligned, on the panel's own title row: it chooses how much of this
    // panel to show rather than setting anything, and reads as a view switch
    // where a setting would not.
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float width = ImGui::CalcTextSize("Simple").x + ImGui::CalcTextSize("Advanced").x +
                            ImGui::GetFrameHeight() * 2.0F + style.ItemInnerSpacing.x * 2.0F +
                            style.ItemSpacing.x;
        const float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;

        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Analysis");
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rightEdge - width));
        ImGui::RadioButton("Simple", &m_advancedAnalysis, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Advanced", &m_advancedAnalysis, 1);
    }

    ImGui::SeparatorText("Sweep");

    bool sweeping = m_state.sweeping();
    if (ImGui::Checkbox("Sweep the planned range", &sweeping)) {
        if (auto applied = m_state.setSweeping(sweeping); !applied) {
            toast(ToastSeverity::Error, applied.error().describe());
        }
    }
    helpMarker("Off: stay at one centre frequency.\nOn: step across the planned range.");

    if (sweeping) {
        int mode = plan.mode == SweepMode::Fast ? 0 : 1;
        field("Priority", "Fast: one FFT per step.\nDetail: more averaging and overlap, slower.");
        if (ImGui::RadioButton("Fast", &mode, 0)) {
            plan.applyMode(SweepMode::Fast);
            planChanged = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Detail", &mode, 1)) {
            plan.applyMode(SweepMode::Detail);
            planChanged = true;
        }

        const ISdrDevice* device = m_state.device();
        const std::size_t portCount = device != nullptr ? device->rxPorts().size() : 0;

        if (ImGui::Checkbox("Route by antenna", &plan.antennaRouting)) {
            planChanged = true;
        }
        helpMarker("Switch connectors so each band goes through the antenna that covers it. "
                   "Assign antennas in the device panel.");

        // The strategy only exists where there is a choice to make. Shown
        // rather than hidden on a one-port radio would be a control whose
        // every setting does the same thing.
        if (plan.antennaRouting && portCount > 1) {
            int strategy = plan.portStrategy == SweepPlan::PortStrategy::TightestFit   ? 0
                           : plan.portStrategy == SweepPlan::PortStrategy::PortOrder   ? 1
                           : plan.portStrategy == SweepPlan::PortStrategy::HighestGain ? 2
                                                                                       : 3;
            field("When both cover it",
                  "Tightest fit: the narrowest-range antenna.\n"
                  "Port order: the first connector.\n"
                  "Most gain: the highest-gain antenna.\n"
                  "Fewest switches: stay on a connector while it covers the band.");
            if (ImGui::Combo("##portstrategy", &strategy,
                             "Tightest fit\0Port order\0Most gain\0Fewest switches\0")) {
                plan.portStrategy = strategy == 0   ? SweepPlan::PortStrategy::TightestFit
                                    : strategy == 1 ? SweepPlan::PortStrategy::PortOrder
                                    : strategy == 2 ? SweepPlan::PortStrategy::HighestGain
                                                    : SweepPlan::PortStrategy::FewestSwitches;
                planChanged = true;
            }
        }

        if (plan.antennaRouting) {
            const SweepSchedule& schedule = m_state.sweepEngine().schedule();
            if (!schedule.unroutedHz.empty()) {
                std::string ranges;
                for (const auto& [fromHz, toHz] : schedule.unroutedHz) {
                    if (!ranges.empty()) {
                        ranges += ", ";
                    }
                    ranges += std::format("{} - {}", toml_util::formatFrequencyShort(fromHz),
                                          toml_util::formatFrequencyShort(toHz));
                }
                // Named where the operator nominated one, because "swept on
                // RX1" and "swept on whatever is connected" are different
                // amounts of trouble.
                const ISdrDevice* routed = m_state.device();
                const std::string_view fallback =
                    routed != nullptr
                        ? m_state.antennaAssignments().fallbackPort(m_state.deviceAntennaKey())
                        : std::string_view{};

                ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(m_state.theme().chrome().warning));
                fieldCaption(fallback.empty()
                                 ? std::format("no assigned antenna covers {} -- swept through "
                                               "whatever is connected",
                                               ranges)
                                 : std::format("no assigned antenna covers {} -- swept on {}",
                                               ranges, fallback),
                             0.0F);
                ImGui::PopStyleColor();
            }

            if (schedule.portSwitches > 0) {
                fieldCaption(std::format("{} port change{} per pass", schedule.portSwitches,
                                         schedule.portSwitches == 1 ? "" : "s"),
                             0.0F);
            }
        }
    }

    ImGui::SeparatorText("Resolution");

    double sampleRate = plan.sampleRate;
    if (!sweeping) {
        // Fixed tune has no plan to read the rate from, so it comes from the
        // radio itself.
        const ISdrDevice* device = m_state.device();
        sampleRate = device != nullptr
                         ? asDouble(device->getParameter("sample_rate").value_or(SdrValue{20e6}))
                         : 20e6;
    }
    const double enbw = windowEnbw(config.window, config.windowBeta);

    // In sweep mode the size in force is the planner's, not the pipeline's
    // stored one -- the planner derives it from the RBW and the pipeline is
    // configured from the result.
    const auto activePoints = [&]() -> std::uint32_t {
        return sweeping ? m_state.sweepEngine().schedule().fftSize : config.fftSize;
    };

    if (m_advancedAnalysis == 0) {
        // Simple asks for the answer, not the machinery: resolution bandwidth
        // is the thing an operator actually wants, and the transform size that
        // achieves it is an implementation detail.
        auto rbwKhz = static_cast<float>(
            (sweeping ? plan.rbwHz : sampleRate * enbw / static_cast<double>(config.fftSize)) /
            1e3);
        field("RBW", "Narrower resolves more detail but sweeps slower.");
        if (ImGui::DragFloat("##rbw", &rbwKhz, 0.5F, 0.05F, 1000.0F, "%.2f kHz",
                             ImGuiSliderFlags_Logarithmic)) {
            const double rbwHz = std::max(static_cast<double>(rbwKhz) * 1e3, 1.0);
            if (sweeping) {
                plan.rbwHz = rbwHz;
                planChanged = true;
            } else {
                config.fftSize = snapFftSize(sampleRate * enbw / rbwHz);
                configChanged = true;
            }
        }

        fieldCaption(std::format("{} points per transform", groupedCount(activePoints())));
    } else {
        // Advanced asks for the transform size directly, from a fixed list.
        //
        // A free numeric field is the wrong control here: the useful sizes are
        // the powers of two, everything between them is a worse plan for the
        // same cost, and typing one digit wrong turns a 4096-point transform
        // into a 496-point one with no indication that anything is off.
        int index = 0;
        for (std::size_t i = 0; i < kFftSizes.size(); ++i) {
            if (kFftSizes[i] <= activePoints()) {
                index = static_cast<int>(i);
            }
        }

        field("FFT points", "RBW = sample rate x window ENBW / points");
        if (ImGui::BeginCombo("##fftsize",
                              groupedCount(kFftSizes[static_cast<std::size_t>(index)]).c_str())) {
            for (std::size_t i = 0; i < kFftSizes.size(); ++i) {
                const bool selected = static_cast<int>(i) == index;
                if (ImGui::Selectable(groupedCount(kFftSizes[i]).c_str(), selected)) {
                    if (sweeping) {
                        // The planner derives the size from the RBW, so asking
                        // for a size means asking for the RBW that produces it.
                        plan.rbwHz = sampleRate * enbw / static_cast<double>(kFftSizes[i]);
                        planChanged = true;
                    } else {
                        config.fftSize = kFftSizes[i];
                        configChanged = true;
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s resolution bandwidth",
                                      toml_util::formatFrequencyShort(
                                          sampleRate * enbw / static_cast<double>(kFftSizes[i]))
                                          .c_str());
                }
            }
            ImGui::EndCombo();
        }

        fieldCaption(std::format("{} resolution bandwidth",
                                 toml_util::formatFrequencyShort(
                                     sampleRate * enbw / static_cast<double>(activePoints()))));
    }

    if (m_advancedAnalysis == 1) {
        ImGui::SeparatorText("Window");

        field("Window");
        if (ImGui::BeginCombo("##window", std::string(displayName(config.window)).c_str())) {
            for (const WindowType type : allWindowTypes()) {
                if (ImGui::Selectable(std::string(displayName(type)).c_str(),
                                      type == config.window)) {
                    config.window = type;
                    plan.window = type;
                    configChanged = true;
                    planChanged = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", std::string(description(type)).c_str());
                }
            }
            ImGui::EndCombo();
        }

        if (config.window == WindowType::Kaiser) {
            auto beta = static_cast<float>(config.windowBeta);
            field("Kaiser beta");
            if (ImGui::SliderFloat("##kaiserbeta", &beta, 0.0F, 20.0F, "%.1f")) {
                config.windowBeta = static_cast<double>(beta);
                plan.windowBeta = config.windowBeta;
                configChanged = true;
                planChanged = true;
            }
        }

        auto overlap = static_cast<float>(sweeping ? plan.fftOverlap : config.overlap);
        field("Overlap", "Fraction of samples shared between transforms. Higher costs more CPU.");
        if (ImGui::SliderFloat("##overlap", &overlap, 0.0F, 0.9F, "%.2f")) {
            config.overlap = static_cast<double>(overlap);
            plan.fftOverlap = config.overlap;
            configChanged = true;
            planChanged = true;
        }

        ViewSettings& view = m_state.view();

        auto holdDecay = view.maxHoldDecayDbPerSec;
        field("Max hold decay");
        if (ImGui::SliderFloat("##holddecay", &holdDecay, 0.0F, 60.0F,
                               holdDecay <= 0.0F ? "hold forever" : "%.1f dB/s")) {
            view.maxHoldDecayDbPerSec = holdDecay;
        }

        auto frameRate = static_cast<float>(config.targetFrameRate);
        field("Frame rate", "Display refresh cap. Does not limit what is measured.");
        if (ImGui::SliderFloat("##framerate", &frameRate, 5.0F, 144.0F, "%.0f fps")) {
            config.targetFrameRate = static_cast<double>(frameRate);
            configChanged = true;
        }

        auto averageCount = static_cast<int>(sweeping ? plan.averageCount : config.averageCount);
        field("Averages", "Lowers the noise floor; blunts short bursts.");
        if (ImGui::SliderInt("##averages", &averageCount, 1, 64)) {
            config.averageCount = static_cast<std::uint32_t>(averageCount);
            plan.averageCount = config.averageCount;
            configChanged = true;
            planChanged = true;
        }

        if (sweeping) {
            ImGui::SeparatorText("Stitching");

            auto usable = static_cast<float>(plan.usableBandwidthFraction);
            field("Usable band", "Fraction of each step kept, cropping the filter roll-off.");
            if (ImGui::SliderFloat("##usablebw", &usable, 0.4F, 1.0F, "%.2f")) {
                plan.usableBandwidthFraction = static_cast<double>(usable);
                planChanged = true;
            }

            auto stepOverlap = static_cast<float>(plan.stepOverlap);
            field("Step overlap", "Extra overlap so tuning error leaves no gaps between steps.");
            if (ImGui::SliderFloat("##stepoverlap", &stepOverlap, 0.0F, 0.5F, "%.2f")) {
                plan.stepOverlap = static_cast<double>(stepOverlap);
                planChanged = true;
            }

            auto dcGuard = static_cast<float>(plan.dcGuardFraction);
            field("LO guard", "Discards the LO leak at each step's centre. Roughly halves the "
                              "sweep rate; zero leaves evenly spaced LO peaks.");
            if (ImGui::SliderFloat("##dcguard", &dcGuard, 0.0F, 0.2F, "%.3f")) {
                plan.dcGuardFraction = static_cast<double>(dcGuard);
                planChanged = true;
            }
        }

        ImGui::SeparatorText("Throughput");

        // Backend selector. Unavailable backends stay in the list with their
        // reason -- that is the entire point of showing them.
        const std::vector<FftBackendInfo> backends = FftBackendManager::instance().enumerate();

        // The backend in use, not `suggestedDefault()`. The latter is fixed at
        // whichever available backend registered first, so with two installed
        // the combo showed that one for ever and the selection appeared not to
        // take.
        const std::string_view currentBackend = m_state.fftBackendName();
        const auto active =
            std::ranges::find_if(backends, [currentBackend](const FftBackendInfo& backend) {
                return backend.name == currentBackend;
            });
        const std::string preview =
            active != backends.end() ? active->displayName : std::string(currentBackend);

        // Width held back for the benchmark button, so the combo stops short of
        // the panel edge instead of the button being pushed off it.
        const std::string benchLabel = icon::glyphOr(icon::kPerformance, "Bench");
        const float benchWidth =
            std::max(ImGui::GetFrameHeight(), ImGui::CalcTextSize(benchLabel.c_str()).x +
                                                  ImGui::GetStyle().FramePadding.x * 2.0F);

        field("Backend", "FFT implementation. Switching restarts acquisition.", benchWidth);
        if (ImGui::BeginCombo("##backend", preview.c_str())) {
            for (const FftBackendInfo& backend : backends) {
                if (!backend.available) {
                    ImGui::BeginDisabled();
                }
                const bool selected = backend.name == currentBackend;
                if (ImGui::Selectable(backend.displayName.c_str(), selected) && backend.available) {
                    // Selection is explicit; nothing is ever chosen silently.
                    if (auto switched = m_state.setFftBackend(backend.name); !switched) {
                        toast(ToastSeverity::Error,
                              std::format("could not switch to '{}': {}", backend.name,
                                          switched.error().message()));
                    } else {
                        toast(ToastSeverity::Info,
                              std::format("FFT backend '{}' selected", backend.name));
                    }
                }
                if (!backend.available) {
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::SetTooltip("Unavailable: %s", backend.unavailableReason.c_str());
                    }
                } else if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", backend.description.c_str());
                }
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        if (ImGui::Button(benchLabel.c_str(), ImVec2(benchWidth, 0.0F))) {
            m_showFftBenchmark = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Benchmark every available backend on this machine");
        }

        int throttle = static_cast<int>(config.throttleMode);
        field("Throttle", "Every Nth: process 1 in N buffers.\n"
                          "Auto: as much as the CPU allows.\n"
                          "All samples: never skip.");
        if (ImGui::Combo("##throttle", &throttle, "Every Nth\0Auto\0All samples\0")) {
            config.throttleMode = static_cast<ThrottleMode>(throttle);
            configChanged = true;
        }

        if (config.throttleMode == ThrottleMode::EveryNth) {
            auto everyNth = static_cast<int>(config.everyNth);
            field("N");
            if (ImGui::SliderInt("##everynth", &everyNth, 1, 64)) {
                config.everyNth = static_cast<std::uint32_t>(everyNth);
                configChanged = true;
            }
        }

        auto workers = static_cast<int>(config.workerCount);
        field("Workers", "Auto: cores minus two.");
        if (ImGui::SliderInt("##workers", &workers, 0, 16, workers == 0 ? "auto" : "%d")) {
            config.workerCount = static_cast<std::uint32_t>(workers);
            configChanged = true;
        }
    }

    if (planChanged) {
        if (auto applied = m_state.applySweepPlan(plan); !applied) {
            toast(ToastSeverity::Error, applied.error().describe());
        }
    }
    if (configChanged) {
        if (auto applied = m_state.applyPipelineConfig(config); !applied) {
            toast(ToastSeverity::Error, applied.error().describe());
        }
    }

    // What the plan will actually do, before it is started.
    if (sweeping && !m_state.sweepEngine().schedule().steps.empty()) {
        const SweepSchedule& schedule = m_state.sweepEngine().schedule();
        ImGui::SeparatorText("Predicted");

        if (beginReadout("##predicted")) {
            readoutRow("steps", groupedCount(schedule.steps.size()));
            readoutRow("FFT size", groupedCount(schedule.fftSize));
            readoutRow("actual RBW", toml_util::formatFrequencyShort(schedule.actualRbwHz));
            readoutRow("pass time", formatDuration(schedule.estimatedPassSeconds));
            readoutRow("sweep rate",
                       std::format("{:.1f} MHz/s", schedule.estimatedSweepRateHzPerSec / 1e6));
            readoutRow("retune cost",
                       std::format("{:.0f}% of the pass", schedule.retuneOverheadFraction * 100.0),
                       "Share of the pass spent retuning rather than measuring.");
            ImGui::EndTable();
        }
    }

    ImGui::Unindent(6.0F);
}

void MainWindow::drawDisplaySection() {
    ImGui::Indent(6.0F);
    ViewSettings& view = m_state.view();

    ImGui::Checkbox("Grid", &view.showGrid);

    // Unlike the contribution flags below, this one has no button anywhere
    // else: it is not something an operator flips minute to minute, it is a
    // check made when setting the bench up or when a band looks wrong.
    ImGui::Checkbox("Antenna ranges", &view.showAntennaRanges);
    helpMarker("Shades what the assigned antennas can hear.");

    // The allocations and the channel flags had a tick each here, as the place
    // B and C were discovered. Both are buttons on the toolbar now, drawn by
    // the plugins that contribute them and lit while their spans are on the
    // plot -- which says the same thing from where the eye already is, and
    // says it without a panel being opened to read it.
    //
    // Ticks left beside them would be a second control for one flag: the same
    // switch, one screen away, agreeing until an operator wonders why there
    // are two. The keys are on the buttons' own tooltips instead.

    ImGui::SeparatorText("Traces");
    {
        const std::array<Toggle, 3> traces{{{"Max hold", &view.showMaxHold},
                                            {"Min hold", &view.showMinHold},
                                            {"Average", &view.showAverage}}};
        toggleRow(traces);
    }
    if (view.showAverage) {
        field("Average window", "Frames in the running average.");
        ImGui::SliderInt("##avgwindow", &view.averageWindow, 2, 256);
    }
    if (ImGui::Button("Reset holds", ImVec2(-1, 0))) {
        m_state.traces().resetHolds();
    }

    ImGui::SeparatorText("Levels");
    field("Y range");
    ImGui::SetNextItemWidth(rangeItemWidth());
    ImGui::DragFloatRange2("##yrange", &view.yMinDb, &view.yMaxDb, 0.5F, kScaleFloorDbfs,
                           kScaleCeilingDbfs, "%.0f dB", "%.0f dB");
    field("Gradient");
    ImGui::SetNextItemWidth(rangeItemWidth());
    ImGui::DragFloatRange2("##gradrange", &view.gradientMinDb, &view.gradientMaxDb, 0.5F,
                           kScaleFloorDbfs, kScaleCeilingDbfs, "%.0f dB", "%.0f dB");

    // The markers themselves are their own panel on the bar. What stays here is
    // where their readout is drawn, which is a drawing preference like the
    // waterfall's own time-axis position.
    ImGui::SeparatorText("Markers");
    field("Readout", "Where the selected marker's readout card is drawn.");
    ImGui::Combo("##markerreadout", &view.markerReadout,
                 "Off\0Top left\0Top centre\0Top right\0Middle left\0Middle right\0"
                 "Bottom left\0Bottom centre\0Bottom right\0");

    ImGui::Unindent(6.0F);
}

void MainWindow::drawMarkersSection() {
    ImGui::Indent(6.0F);
    MarkerSet& markers = m_state.markers();

    if (markers.items.empty()) {
        ImGui::TextDisabled("Nothing placed yet.");
        m_editingMarker = 0;
    } else {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float square = ImGui::GetFrameHeight();

        // Ten rows before it scrolls, and exactly as tall as its contents below
        // that: a list of two markers should not be drawn in a box sized for
        // ten.
        constexpr std::size_t kMaxRows = 10;
        const bool scrolling = markers.items.size() > kMaxRows;
        const float rowHeight = square + style.CellPadding.y * 2.0F;

        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit |
                                      (scrolling ? ImGuiTableFlags_ScrollY : 0);
        const ImVec2 size(0.0F, scrolling ? rowHeight * static_cast<float>(kMaxRows) : 0.0F);

        if (ImGui::BeginTable("##markers", 6, flags, size)) {
            // No header row: "M1", "779.430 MHz" and "-82.9 dBFS" say what they
            // are, and a header would cost a row of the panel to repeat it.
            //
            // Widths reserved from the widest value each column can hold rather
            // than from what is in it. These change as a marker is walked along
            // a signal, and a column sized to its own text drags the whole row
            // sideways every time a digit appears.
            // A column is wider than what goes in it by its own padding, and a
            // fixed width that forgets that clips the last few pixels of every
            // cell -- which on the actions column means half the delete button.
            const auto column = [&style](float content) {
                return content + style.CellPadding.x * 2.0F;
            };

            ImGui::TableSetupColumn("##shown", ImGuiTableColumnFlags_WidthFixed, column(square));
            ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed,
                                    column(ImGui::CalcTextSize("M88").x));
            ImGui::TableSetupColumn("##frequency", ImGuiTableColumnFlags_WidthFixed,
                                    column(ImGui::CalcTextSize("8888.888 MHz").x));
            ImGui::TableSetupColumn("##level", ImGuiTableColumnFlags_WidthFixed,
                                    column(ImGui::CalcTextSize("-888.8 dBFS").x));
            ImGui::TableSetupColumn("##at", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed,
                                    column(square * 2.0F + style.ItemSpacing.x));

            // Recorded and applied after the table, following the channels
            // editor: erasing mid-iteration would invalidate the row the rest
            // of this pass is still drawing.
            int remove = 0;

            for (Marker& marker : markers.items) {
                ImGui::PushID(marker.id);
                ImGui::TableNextRow();

                // The tick is submitted before the row's selectable, so it wins
                // the hit test over its own few pixels: showing and hiding a
                // marker must not also re-aim the right button.
                ImGui::TableNextColumn();
                ImGui::Checkbox("##visible", &marker.visible);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Show on the plot");
                }

                ImGui::TableNextColumn();
                const std::string name = std::format("M{}", marker.id);
                const bool active = marker.id == markers.activeId;

                // The selectable is the hit target only; the band behind the
                // row is painted as the row's own background.
                //
                // A selectable's box is offset by the row's text baseline --
                // which is what aligns plain text with framed widgets beside
                // it -- so on this row it sat several pixels below the tick and
                // the buttons it is meant to be behind, and sizing it to match
                // them only grew the row by the same offset again. The row
                // background has no such offset: it is the row.
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
                const bool picked = ImGui::Selectable(name.c_str(), active,
                                                      ImGuiSelectableFlags_SpanAllColumns |
                                                          ImGuiSelectableFlags_AllowOverlap);
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopStyleColor(3);

                if (picked) {
                    markers.activeId = marker.id;
                }
                if (active || hovered) {
                    ImGui::TableSetBgColor(
                        ImGuiTableBgTarget_RowBg1,
                        ImGui::GetColorU32(active ? ImGuiCol_Header : ImGuiCol_HeaderHovered));
                }

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(toml_util::formatFrequency(marker.frequencyHz, 3).c_str());

                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", levelText(marker.levelDb).c_str());

                ImGui::TableNextColumn();
                const std::vector<Contribution> found =
                    PluginManager::instance().contributionsAt(marker.frequencyHz);
                if (found.empty()) {
                    ImGui::TextDisabled("-");
                } else {
                    // A gap held back from the buttons that follow, so a long
                    // name stops short of them rather than at them.
                    constexpr float kGapToActions = 8.0F;
                    ImGui::TextColored(
                        toImVec4(Color{.r = found.front().color[0],
                                       .g = found.front().color[1],
                                       .b = found.front().color[2],
                                       .a = found.front().color[3]}),
                        "%s",
                        bar::ellipsised(found.front().name,
                                        ImGui::GetContentRegionAvail().x - kGapToActions)
                            .c_str());
                }

                ImGui::TableNextColumn();
                if (iconButton("##edit", icon::kEdit, "e", square)) {
                    m_editingMarker = m_editingMarker == marker.id ? 0 : marker.id;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Type this marker onto an exact frequency");
                }

                ImGui::SameLine();
                if (iconButton("##delete", icon::kClose, "x", square)) {
                    remove = marker.id;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Delete this marker");
                }

                ImGui::PopID();
            }
            ImGui::EndTable();

            if (remove != 0) {
                markers.remove(remove);
                if (m_editingMarker == remove) {
                    m_editingMarker = 0;
                }
            }
        }
    }

    // One line and a marker, rather than the paragraph this used to be: the
    // gestures have to be discoverable somewhere, and this is the panel they
    // belong to -- but three lines of prose over a four-line list reads as
    // instructions with a table attached.
    ImGui::TextDisabled("Right-click the plot to place one");
    helpMarker("Right-click: move the selected marker (hold to drag)\n"
               "Ctrl/Cmd right-click: add a marker\n"
               "Click: select the nearest marker\n"
               "Backspace: delete the selected marker");

    // Typing a marker onto an exact frequency, which no click can do: a channel
    // edge is a number an operator has, not a pixel they can hit. Only for the
    // row whose pencil is pressed.
    Marker* editing = nullptr;
    if (m_editingMarker != 0) {
        const auto found = std::ranges::find(markers.items, m_editingMarker, &Marker::id);
        if (found != markers.items.end()) {
            editing = &*found;
        } else {
            m_editingMarker = 0;
        }
    }

    if (editing != nullptr) {
        if (frequencyRow(std::format("M{}", editing->id).c_str(), editing->frequencyHz,
                         editing->id)) {
            editing->peakLocked = false;
        }
        ImGui::Checkbox("Peak", &editing->peakLocked);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Follow the loudest bin nearby");
        }
    }

    // Collapsed until asked for. The list above is read constantly while a
    // sweep runs and the saved sets are touched at the start and end of a job,
    // so an unfolded preset list would push the markers themselves off the
    // panel for something looked at twice a day.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Presets")) {
        drawMarkerPresetList(markers);
    }

    footerRule();
    {
        const float width = footerButtonWidth(2);

        if (ImGui::Button("Add at centre", ImVec2(width, 0))) {
            double fromHz = 0.0;
            double toHz = 0.0;
            m_state.visibleRange(fromHz, toHz);
            markers.add((fromHz + toHz) * 0.5);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Place a marker in the middle of the visible span");
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(markers.items.empty());
        if (ImGui::Button("Clear all", ImVec2(width, 0))) {
            markers.clear();
        }
        ImGui::EndDisabled();
    }

    ImGui::Unindent(6.0F);
}

void MainWindow::drawMarkerPresetList(MarkerSet& markers) {
    MarkerPresetStore& store = m_state.markerPresets();

    // What the set is, offered as a placeholder and used when nothing is
    // typed: saving stays one click for the common case, without the field
    // having to hold text the operator did not write. Seeding the field itself
    // would mean it could never look empty -- clearing it on save would refill
    // it on the next frame with the name of the set just saved.
    const std::string suggestion =
        markers.items.empty() ? std::string() : presetFromMarkers(markers, "").describe();

    const float saveWidth = ImGui::CalcTextSize("Save").x + ImGui::GetStyle().FramePadding.x * 4.0F;

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s", m_newMarkerPresetName.c_str());
    ImGui::SetNextItemWidth(-(saveWidth + ImGui::GetStyle().ItemSpacing.x));
    if (ImGui::InputTextWithHint("##markerpresetname", suggestion.c_str(), buffer,
                                 sizeof(buffer))) {
        m_newMarkerPresetName = buffer;
    }

    ImGui::SameLine();
    const std::string name = m_newMarkerPresetName.empty() ? suggestion : m_newMarkerPresetName;
    ImGui::BeginDisabled(name.empty());
    if (ImGui::Button("Save", ImVec2(saveWidth, 0))) {
        store.add(presetFromMarkers(markers, name));
        m_state.saveMarkerPresets();
        m_newMarkerPresetName.clear();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(markers.items.empty() ? "No markers to save"
                                                : "Save the markers above under this name");
    }

    if (store.presets().empty()) {
        ImGui::TextDisabled("Nothing saved yet.");
        return;
    }

    // Square buttons the height of the row, as in the range presets, so the
    // actions and the name they belong to share one baseline.
    const float action = ImGui::GetFrameHeight();
    const float actionsWidth = (action + ImGui::GetStyle().ItemSpacing.x) * 2.0F;

    std::string pendingRemoval;
    for (const MarkerPreset& preset : store.presets()) {
        ImGui::PushID(preset.name.c_str());

        // Marking a favourite re-sorts the list this loop is walking, so the
        // pass ends here and the reordered list is drawn next frame.
        if (iconButton("##favourite", preset.favourite ? icon::kStar : icon::kStarOff,
                       preset.favourite ? "*" : "-", action)) {
            store.setFavourite(preset.name, !preset.favourite);
            m_state.saveMarkerPresets();
            ImGui::PopID();
            break;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(preset.favourite ? "Remove from favourites"
                                               : "Keep at the top of the list");
        }

        ImGui::SameLine();

        // Name left, what it holds right, both drawn into the row rather than
        // concatenated into the button's label: a button centres its text, and
        // the part that says whether this is the wanted set is the frequencies.
        const float rowWidth = std::max(ImGui::GetContentRegionAvail().x - actionsWidth, 80.0F);

        const std::string id = std::format("##load{}", preset.name);
        const bool activated = ImGui::Selectable(id.c_str(), false, ImGuiSelectableFlags_None,
                                                 ImVec2(rowWidth, action));
        const bool rowHovered = ImGui::IsItemHovered();
        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();

        // Alt-click places the set alongside what is up, plain click replaces
        // it -- the same modifier, and the same +, the range presets carry.
        if (activated) {
            if (ImGui::GetIO().KeyAlt) {
                addPresetToMarkers(preset, markers);
            } else {
                applyPreset(preset, markers);
            }
            m_editingMarker = 0;
        }

        const std::string summary = preset.describe();
        const float summaryWidth = ImGui::CalcTextSize(summary.c_str()).x;
        const float textY = rowMin.y + (rowMax.y - rowMin.y - ImGui::GetTextLineHeight()) * 0.5F;

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ChromeTheme& chrome = m_state.theme().chrome();
        draw->AddText(ImVec2(rowMin.x + 6.0F, textY), packed(chrome.text), preset.name.c_str());
        draw->AddText(ImVec2(rowMax.x - summaryWidth - 6.0F, textY), packed(chrome.textDim),
                      summary.c_str());

        if (rowHovered) {
            ImGui::SetTooltip("%s\n%s\n\nClick: replace the markers\nAlt-click or +: add to them",
                              preset.name.c_str(), summary.c_str());
        }

        ImGui::SameLine();
        if (iconButton("##addmarkers", icon::kAdd, "+", action)) {
            addPresetToMarkers(preset, markers);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Add to the current markers");
        }

        ImGui::SameLine();
        if (iconButton("##remove", icon::kClose, "x", action)) {
            pendingRemoval = preset.name;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Delete this preset");
        }

        ImGui::PopID();
    }

    // Deferred: erasing inside the loop would invalidate the range being
    // iterated.
    if (!pendingRemoval.empty()) {
        store.remove(pendingRemoval);
        m_state.saveMarkerPresets();
    }
}

void MainWindow::drawAppearanceSection() {
    AppSettings& settings = m_state.appSettings();

    // Set once a gesture has finished, never while one is running: see
    // m_pendingUiScalePercent. It is therefore also the moment the file is
    // written, which is the moment worth writing at anyway -- a preference
    // lost because the application crashed an hour later is a preference
    // nobody set, and one written on every frame of a drag is a file being
    // hammered for nothing.
    bool changed = false;

    ImGui::SeparatorText("Scale");

    const bool automatic = settings.uiScale <= 0.0F;

    // Shown as a percentage. "1.25x" is a developer's way of saying it; every
    // desktop the operator has ever set this in says 125%.
    float percent = m_pendingUiScalePercent.value_or(m_state.effectiveUiScale() * 100.0F);
    field("Interface");
    if (ImGui::SliderFloat("##uiscale", &percent, AppSettings::kMinUiScale * 100.0F,
                           AppSettings::kMaxUiScale * 100.0F, "%.0f%%")) {
        m_pendingUiScalePercent = percent;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        settings.uiScale =
            std::clamp(percent / 100.0F, AppSettings::kMinUiScale, AppSettings::kMaxUiScale);
        changed = true;
    }
    // Also when the popover was closed mid-drag, which deactivates the slider
    // without ever telling it so.
    if (!ImGui::IsItemActive()) {
        m_pendingUiScalePercent.reset();
    }

    ImGui::BeginDisabled(automatic);
    if (ImGui::Button("Match display")) {
        settings.uiScale = 0.0F;
        changed = true;
    }
    ImGui::EndDisabled();
    fieldCaption(automatic ? std::format("Following the display, at {:.0f}%.",
                                         static_cast<double>(m_state.effectiveUiScale() * 100.0F))
                           : "Set here, and not following the display.",
                 0.0F);

    ImGui::SeparatorText("Text");

    float fontSize = m_pendingFontSize.value_or(settings.fontSize);
    field("Size");
    if (ImGui::SliderFloat("##fontsize", &fontSize, AppSettings::kMinFontSize,
                           AppSettings::kMaxFontSize, "%.0f pt")) {
        m_pendingFontSize = fontSize;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        settings.fontSize =
            std::clamp(fontSize, AppSettings::kMinFontSize, AppSettings::kMaxFontSize);
        changed = true;
    }
    if (!ImGui::IsItemActive()) {
        m_pendingFontSize.reset();
    }

    // Rebuilds the font atlas, so it is kept apart from the metrics above: a
    // scale change must not pay for one.
    bool weightChanged = false;

    float weightPercent = m_pendingFontWeight.value_or(settings.fontWeight * 100.0F);
    field("Weight");
    if (ImGui::SliderFloat("##fontweight", &weightPercent, AppSettings::kMinFontWeight * 100.0F,
                           AppSettings::kMaxFontWeight * 100.0F, "%.0f%%")) {
        m_pendingFontWeight = weightPercent;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        settings.fontWeight = std::clamp(weightPercent / 100.0F, AppSettings::kMinFontWeight,
                                         AppSettings::kMaxFontWeight);
        changed = true;
        weightChanged = true;
    }
    if (!ImGui::IsItemActive()) {
        m_pendingFontWeight.reset();
    }

    if (weightChanged && m_fontWeightRequest) {
        m_fontWeightRequest(settings.fontWeight);
    }

    if (changed) {
        // Queued, not applied here: this panel is drawn inside a PushStyleVar
        // scope, and rebuilding the style under one puts the padding the pop
        // restores back over what was just written.
        m_state.refreshTheme();

        if (auto saved = m_state.saveAppSettings(); !saved) {
            toast(ToastSeverity::Error, saved.error().describe());
        }
    }
}

void MainWindow::drawThemeSection() {
    ImGui::Indent(6.0F);

    field("Theme");
    if (ImGui::BeginCombo("##theme", m_state.theme().name().c_str())) {
        for (const Theme& theme : m_state.themes()) {
            if (ImGui::Selectable(theme.name().c_str(), theme.name() == m_state.theme().name())) {
                // Live, with no restart -- that is a stated requirement, and
                // it works because the LUT and the ImGui style are both
                // rebuilt from the theme on the spot.
                m_state.setTheme(theme.name());
                m_waterfall.setColorMap(m_state.theme().waterfallColorMap());
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SeparatorText("Colormaps");

    const std::vector<ColorMap> maps = discoverColorMaps();
    const auto swatch = [](const ColorMap& map, float width) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        constexpr int kSlices = 48;
        for (int i = 0; i < kSlices; ++i) {
            const float t0 = static_cast<float>(i) / kSlices;
            const float t1 = static_cast<float>(i + 1) / kSlices;
            draw->AddRectFilled(ImVec2(origin.x + width * t0, origin.y),
                                ImVec2(origin.x + width * t1 + 1.0F, origin.y + 14.0F),
                                packed(map.sample((t0 + t1) * 0.5F)));
        }
        ImGui::Dummy(ImVec2(width, 14.0F));
    };

    for (const ColorMap& map : maps) {
        ImGui::PushID(map.name().c_str());
        const bool selected = map.name() == m_state.theme().waterfall().colorMap;

        if (ImGui::Selectable("##pick", selected, 0, ImVec2(0, 18.0F))) {
            // Editing the theme in place then re-applying is the same code
            // path as selecting a built-in, which is what makes the gradient
            // editor almost free.
            const_cast<Theme&>(m_state.theme()).waterfall().colorMap = map.name();
            const_cast<Theme&>(m_state.theme()).spectrum().fillColorMap = map.name();
            m_state.refreshTheme();
            m_waterfall.setColorMap(m_state.theme().waterfallColorMap());
        }
        ImGui::SameLine(0.0F, 4.0F);
        swatch(map, 120.0F);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(map.name().c_str());
        ImGui::PopID();
    }

    ImGui::Spacing();
    if (ImGui::Button("Gradient editor...", ImVec2(-1, 0))) {
        m_editingColorMap = m_state.theme().waterfallColorMap();
        m_showGradientEditor = true;
    }

    const float saveWidth = ImGui::CalcTextSize("Save").x + ImGui::GetStyle().FramePadding.x * 4.0F;

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s", m_newThemeName.c_str());
    field("Save as", nullptr, saveWidth);
    if (ImGui::InputTextWithHint("##themename", m_state.theme().name().c_str(), buffer,
                                 sizeof(buffer))) {
        m_newThemeName = buffer;
    }

    ImGui::SameLine();
    if (ImGui::Button("Save", ImVec2(saveWidth, 0))) {
        const std::string name = m_newThemeName.empty() ? m_state.theme().name() : m_newThemeName;

        // The live theme, not the one on disk: it carries the colour-map edits.
        Theme theme = m_state.theme();
        theme.setName(name);

        // One name, one stem, so saving under a shipped theme's name lands on
        // the same stem as the shipped file and shadows it.
        std::string stem = slugify(name);
        if (stem.empty()) {
            stem = "theme";
        }
        const std::filesystem::path path = Paths::instance().themesDir() / (stem + ".toml");

        if (auto saved = theme.saveToToml(path); saved) {
            m_state.reloadThemes();
            m_state.setTheme(name);
            m_waterfall.setColorMap(m_state.theme().waterfallColorMap());
            m_newThemeName.clear();
            toast(ToastSeverity::Success, std::format("theme written to {}", path.string()));
        } else {
            toast(ToastSeverity::Error, saved.error().describe());
        }
    }

    ImGui::Unindent(6.0F);
}

void MainWindow::drawProfilesSection() {
    ImGui::Indent(6.0F);

    if (m_newProfileName.empty()) {
        m_newProfileName = "profile";
    }

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s", m_newProfileName.c_str());
    ImGui::SetNextItemWidth(-92.0F);
    if (ImGui::InputText("##profilename", buffer, sizeof(buffer))) {
        m_newProfileName = buffer;
    }
    ImGui::SameLine();
    if (ImGui::Button("Save", ImVec2(-1, 0)) && !m_newProfileName.empty()) {
        const std::filesystem::path path =
            Paths::instance().profilesDir() / std::format("{}.toml", m_newProfileName);

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        if (auto saved = m_state.currentProfile(m_newProfileName).save(path); !saved) {
            toast(ToastSeverity::Error, saved.error().describe());
        } else {
            toast(ToastSeverity::Success, std::format("profile '{}' saved", m_newProfileName));
        }
    }

    ImGui::SeparatorText("Saved");

    const std::vector<std::string> names = m_state.profileNames();
    if (names.empty()) {
        ImGui::TextDisabled("None saved yet.");
    }

    std::string pendingDelete;
    for (const std::string& name : names) {
        ImGui::PushID(name.c_str());

        if (ImGui::Button(name.c_str(), ImVec2(-58.0F, 0))) {
            const std::filesystem::path path =
                Paths::instance().profilesDir() / std::format("{}.toml", name);

            if (auto profile = Profile::load(path); !profile) {
                toast(ToastSeverity::Error, profile.error().describe());
            } else if (auto applied = m_state.applyProfile(*profile); !applied) {
                // Partially applied is the normal case when the radio has
                // moved on, and everything else was still restored -- so this
                // reports what could not be done rather than claiming failure.
                toast(ToastSeverity::Warning,
                      std::format("'{}' loaded, but {}", name, applied.error().message()));
                m_newProfileName = name;
            } else {
                toast(ToastSeverity::Success, std::format("profile '{}' loaded", name));
                m_newProfileName = name;
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Load '%s'", name.c_str());
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            pendingDelete = name;
        }
        ImGui::PopID();
    }

    if (!pendingDelete.empty()) {
        std::error_code ec;
        std::filesystem::remove(
            Paths::instance().profilesDir() / std::format("{}.toml", pendingDelete), ec);
        toast(ToastSeverity::Success, std::format("profile '{}' deleted", pendingDelete));
    }

    ImGui::Unindent(6.0F);
}

void MainWindow::drawContributorsSection() {
    ImGui::Indent(6.0F);

    PluginManager& plugins = PluginManager::instance();
    std::vector<std::string> order = plugins.contributorOrder();

    if (order.empty()) {
        ImGui::TextDisabled("Nothing is contributing frequency data.");
        fieldCaption("Turn one on under Plugins.", 0.0F);
        ImGui::Unindent(6.0F);
        return;
    }

    fieldCaption("Drag a row to reorder.", 0.0F);
    ImGui::Spacing();

    // The drop decides, and the move happens after the loop: reordering the
    // vector being walked is how a frame comes to draw one row twice and
    // another not at all.
    int dragFrom = -1;
    int dragTo = -1;

    for (int i = 0; i < static_cast<int>(order.size()); ++i) {
        const std::string& id = order[static_cast<std::size_t>(i)];
        const Result<PluginInfo> info = plugins.info(id);

        ImGui::PushID(id.c_str());

        const std::string name = info ? info->name : id;

        // The row is one group, so the drop target below is the whole of it
        // rather than the handle alone -- a drop then lands where the operator
        // aimed instead of only on the six pixels they grabbed from.
        ImGui::BeginGroup();

        // A Selectable rather than a Text: `BeginDragDropSource` needs the
        // preceding item to have an id, and a Text submits none, so the drag
        // never started at all. The null-id fallback exists but is documented
        // not to survive the widget moving -- which is the one thing a row
        // being reordered does.
        const float handle = ImGui::GetFrameHeight();
        ImGui::Selectable(icon::glyphOr(icon::kMenu, "=").c_str(), false, ImGuiSelectableFlags_None,
                          ImVec2(handle, handle));
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("SWEEPPP_CONTRIBUTOR", &i, sizeof(i));
            ImGui::TextUnformatted(name.c_str());
            ImGui::EndDragDropSource();
        }

        ImGui::SameLine();

        bool shown = plugins.contributorShown(id);
        if (ImGui::Checkbox("##shown", &shown)) {
            if (auto changed = plugins.setContributorShown(id, shown); !changed) {
                toast(ToastSeverity::Error, changed.error().message());
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Show on the spectrum");
        }

        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(name.c_str());

        // The dataset selector, drawn by the host from what the facet reports.
        // This is where the band plan's own plan combo went, and why a
        // contributor can now ship no UI code at all.
        const std::vector<std::pair<std::string, std::string>> sets = plugins.datasets(id);
        if (sets.size() > 1) {
            // Right-aligned, so the combos line up down the section however
            // long the names beside them are.
            constexpr float kDatasetWidth = 160.0F;
            ImGui::SameLine();
            const float slack = ImGui::GetContentRegionAvail().x - kDatasetWidth;
            if (slack > 0.0F) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + slack);
            }
            ImGui::SetNextItemWidth(-1.0F);

            const auto active = static_cast<std::size_t>(plugins.activeDataset(id));
            const char* current = active < sets.size() ? sets[active].first.c_str() : "";

            if (ImGui::BeginCombo("##dataset", current)) {
                for (std::size_t d = 0; d < sets.size(); ++d) {
                    if (ImGui::Selectable(sets[d].first.c_str(), d == active)) {
                        if (auto chosen = plugins.selectDataset(id, static_cast<std::uint32_t>(d));
                            !chosen) {
                            toast(ToastSeverity::Error, chosen.error().message());
                        }
                    }
                    if (ImGui::IsItemHovered() && !sets[d].second.empty()) {
                        ImGui::SetTooltip("%s", sets[d].second.c_str());
                    }
                }
                ImGui::EndCombo();
            }
        }

        // What this contributor says it contributes -- the facet's own words,
        // which are more specific than the plugin's.
        std::string caption;
        if (info) {
            for (const PluginFacetInfo& facet : info->facets) {
                if (facet.kind == PluginFacetKind::Contributor && !facet.description.empty()) {
                    caption = facet.description;
                    break;
                }
            }
            if (caption.empty()) {
                caption = info->description;
            }
        }
        if (!caption.empty()) {
            fieldCaption(caption, 46.0F);
        }

        ImGui::EndGroup();

        // The whole row, caption included. `EndGroup` leaves the group's
        // rectangle as the last item, which is what this reads.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SWEEPPP_CONTRIBUTOR")) {
                dragFrom = *static_cast<const int*>(payload->Data);
                dragTo = i;
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::PopID();
    }

    if (dragFrom >= 0 && dragTo >= 0 && dragFrom != dragTo) {
        std::string moved = std::move(order[static_cast<std::size_t>(dragFrom)]);
        order.erase(order.begin() + dragFrom);
        order.insert(order.begin() + dragTo, std::move(moved));

        if (auto changed = plugins.setContributorOrder(order); !changed) {
            toast(ToastSeverity::Error, changed.error().message());
        }
    }

    ImGui::Unindent(6.0F);
}

void MainWindow::drawPluginsSection() {
    ImGui::Indent(6.0F);

    PluginManager& plugins = PluginManager::instance();
    const std::vector<PluginInfo> found = plugins.enumerate();

    if (found.empty()) {
        ImGui::TextDisabled("No plugins found.");
        fieldCaption("$SWEEPPP_PLUGIN_PATH adds more folders.", 0.0F);

        // The button belongs here most of all: nothing is installed, and the
        // next thing the operator wants is the folder to put something in.
        if (std::string said =
                revealButton("Open plugins folder", Paths::instance().pluginsDir(), true);
            !said.empty()) {
            toast(ToastSeverity::Info, std::move(said));
        }

        ImGui::Unindent(6.0F);
        return;
    }

    // One collapsed row per plugin: the checkbox that turns it on, and what it
    // is. Collapsed because this list is read to find one plugin, not to read
    // all of them -- and a plugin's own settings are not here at all, they get
    // a section of their own in this menu beside Display and Theme.
    for (const PluginInfo& plugin : found) {
        ImGui::PushID(plugin.path.string().c_str());

        // A plugin that would not load still gets a row, with the reason. The
        // checkbox is disabled and says why on hover -- the same idiom the FFT
        // backend combo uses for a backend this build cannot run, and for the
        // same reason: a directory that appears to contain nothing teaches an
        // operator nothing.
        bool enabled = plugin.enabled;
        ImGui::BeginDisabled(!plugin.loaded);
        if (ImGui::Checkbox("##enabled", &enabled)) {
            if (auto changed = plugins.setEnabled(plugin.id, enabled); !changed) {
                toast(ToastSeverity::Error, changed.error().message());
            } else {
                toast(ToastSeverity::Info,
                      std::format("plugin '{}' {}", plugin.id, enabled ? "enabled" : "disabled"));
            }
        }
        ImGui::EndDisabled();

        if (!plugin.loaded && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Cannot be loaded: %s", plugin.failureReason.c_str());
        }

        ImGui::SameLine();

        const std::string heading =
            plugin.loaded ? std::format("{}  {}{}", plugin.name, displayVersion(plugin.version),
                                        plugin.requiresRestart ? "   restart needed" : "")
                          : std::format("{}  (not loaded)", plugin.path.filename().string());

        if (ImGui::CollapsingHeader(heading.c_str())) {
            ImGui::Indent(6.0F);

            if (!plugin.failureReason.empty()) {
                fieldCaption(plugin.failureReason, 0.0F);
            }
            if (!plugin.description.empty()) {
                fieldCaption(plugin.description, 0.0F);
            }
            if (plugin.requiresRestart) {
                fieldCaption("Takes effect on the next start.", 0.0F);
            }

            // Wide enough for the longest label this table actually uses,
            // measured rather than assumed: the facet kinds are the widest
            // labels any readout carries, and the shared default is sized for
            // "sweep rate". Measuring also keeps it right at whatever font
            // size and DPI the operator is running.
            float labelWidth = kFieldLabelWidth;
            for (const PluginFacetInfo& facet : plugin.facets) {
                // Bounded by the view rather than by `.data()`: a string_view
                // is not required to be NUL-terminated, and these only happen
                // to be because they are literals.
                const std::string_view kind = toString(facet.kind);
                const float width = ImGui::CalcTextSize(kind.data(), kind.data() + kind.size()).x +
                                    ImGui::GetStyle().CellPadding.x * 4.0F;
                labelWidth = std::max(labelWidth, width);
            }

            if (beginReadout("##pluginfacts", labelWidth)) {
                readoutRow("id", plugin.id);

                if (!plugin.authors.empty()) {
                    std::string authors;
                    for (const PluginAuthor& author : plugin.authors) {
                        authors += (authors.empty() ? "" : ", ") + author.name;
                    }
                    readoutRow("by", authors,
                               plugin.authors.front().email.empty()
                                   ? nullptr
                                   : plugin.authors.front().email.c_str());
                }

                // Facets, active or not, each with its reason. A plugin whose
                // overlay could not register but whose data facet did is a
                // half-working plugin, and saying only that it loaded would
                // make the working half look like the whole story.
                for (const PluginFacetInfo& facet : plugin.facets) {
                    readoutRow(std::string(toString(facet.kind)).c_str(),
                               facet.failureReason.empty()
                                   ? std::format("{}{}", facet.name.empty() ? facet.id : facet.name,
                                                 facet.active ? "" : "  (inactive)")
                                   : std::format("{}  -- {}",
                                                 facet.name.empty() ? facet.id : facet.name,
                                                 facet.failureReason),
                               facet.description.empty() ? nullptr : facet.description.c_str());
                }
                ImGui::EndTable();
            }

            // Where it came from is a button rather than a row of text.
            //
            // The path was the widest thing in the panel and the least often
            // read: what an operator wants from it is almost always to *go
            // there* -- to replace the file, or to check which of two copies
            // is loaded. The full path stays in the tooltip, so nothing is
            // lost for the times they do want to read it.
            if (std::string said = revealButton("Show in folder", plugin.path); !said.empty()) {
                toast(ToastSeverity::Info, std::move(said));
            }

            // Links copy rather than open. Opening a URL means launching a
            // browser from a settings panel, which is a bigger thing to do to
            // an operator than revealing a folder they already have on disk.
            //
            // Wrapped by hand, because buttons are items rather than text and
            // ImGui will not break a run of them for you: past the right edge
            // they are not merely ugly, they are unclickable.
            //
            // Cursor plus remaining width is the content region's right edge
            // wherever the cursor happens to be, so this is the panel's edge
            // and not an accident of the button before it.
            const float linksRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
            for (const PluginLink& link : plugin.links) {
                const std::string caption =
                    std::format("{}##{}", capitalised(toString(link.type)), link.url);
                const float width = ImGui::CalcTextSize(caption.c_str(), nullptr, true).x +
                                    ImGui::GetStyle().FramePadding.x * 2.0F;

                // Onto the "Show in folder" line while there is room for it,
                // then onto lines of their own.
                ImGui::SameLine();
                if (ImGui::GetCursorPosX() + width > linksRight) {
                    ImGui::NewLine();
                }

                if (ImGui::SmallButton(caption.c_str())) {
                    ImGui::SetClipboardText(link.url.c_str());
                    toast(ToastSeverity::Info, std::format("copied {}", link.url));
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s\n\nClick to copy.", link.url.c_str());
                }
            }

            ImGui::Unindent(6.0F);
        }

        ImGui::PopID();
    }

    ImGui::Spacing();
    if (std::string said =
            revealButton("Open plugins folder", Paths::instance().pluginsDir(), true);
        !said.empty()) {
        toast(ToastSeverity::Info, std::move(said));
    }
    ImGui::Unindent(6.0F);
}

// --------------------------------------------------------------- popovers

void MainWindow::drawChartSettingsPopup() {
    const bar::PanelMetrics metrics;
    if (!ImGui::BeginPopup("##chartsettings")) {
        return;
    }

    ViewSettings& view = m_state.view();

    ImGui::SeparatorText("Trace");

    // Toggles first, then the rows that carry a value. Interleaving them puts a
    // checkbox's label and a field's label in different columns on adjacent
    // rows, and the eye loses the left edge it was reading down.
    ImGui::Checkbox("Auto display points", &view.autoPoints);

    if (view.autoPoints) {
        fieldCaption("following the plot width", 0.0F);
    } else {
        field("Points");
        ImGui::SliderInt("##displaypoints", &view.displayPoints, 64, 8192, "%d",
                         ImGuiSliderFlags_Logarithmic);
    }

    field("Smoothing", "Steadier trace; blunts transients.");
    ImGui::SliderFloat("##smoothing", &view.smoothing, 0.0F, 0.95F,
                       view.smoothing <= 0.0F ? "off" : "%.2f");

    ImGui::SeparatorText("Traces");
    {
        const std::array<Toggle, 3> traces{{{"Max hold", &view.showMaxHold},
                                            {"Min hold", &view.showMinHold},
                                            {"Average", &view.showAverage}}};
        toggleRow(traces);
    }

    ImGui::SeparatorText("Fill");

    // One control, not a checkbox plus a combo whose third entry repeated it.
    //
    // "Gradient fill" switched the fill on and off, and "Outline only" switched
    // it off again -- so the panel had two ways to say the same thing and could
    // show them disagreeing. The underlying settings are still separate because
    // profiles store them that way; what an operator picks from is the one
    // question actually being asked.
    int fill = !view.showHeatmapFill || view.fillStyle == 2 ? 0 : (view.fillStyle == 1 ? 1 : 2);
    field("Under trace", "Solid: the trace's colour.\nGradient: coloured by level.");
    if (ImGui::Combo("##fillstyle", &fill, "None\0Solid\0Gradient\0")) {
        view.showHeatmapFill = fill != 0;
        if (fill == 1) {
            view.fillStyle = 1;
        } else if (fill == 2) {
            view.fillStyle = 0;
        }
    }

    if (fill == 2) {
        field("Colours");
        if (ImGui::Button("Edit gradient...", ImVec2(-1, 0))) {
            m_editingColorMap = m_state.theme().spectrumFillColorMap();
            m_showGradientEditor = true;
        }
    }

    ImGui::SeparatorText("Levels");

    field("Y axis", "Also draggable on the plot.");
    ImGui::SetNextItemWidth(rangeItemWidth());
    ImGui::DragFloatRange2("##ylevels", &view.yMinDb, &view.yMaxDb, 0.5F, kScaleFloorDbfs,
                           kScaleCeilingDbfs, "%.0f dB", "%.0f dB");

    field("Gradient", "Shared with the waterfall.");
    ImGui::SetNextItemWidth(rangeItemWidth());
    ImGui::DragFloatRange2("##gradientlevels", &view.gradientMinDb, &view.gradientMaxDb, 0.5F,
                           kScaleFloorDbfs, kScaleCeilingDbfs, "%.0f dB", "%.0f dB");

    footerRule();
    if (ImGui::Button("Reset settings", ImVec2(-1, 0))) {
        const ViewSettings defaults;
        const std::string theme = view.themeName;
        view = defaults;
        view.themeName = theme;
    }

    ImGui::EndPopup();
}

void MainWindow::drawWaterfallSettingsPopup() {
    const bar::PanelMetrics metrics;
    if (!ImGui::BeginPopup("##waterfallsettings")) {
        return;
    }

    ViewSettings& view = m_state.view();

    ImGui::SeparatorText("Waterfall");

    ImGui::Checkbox("Pause", &view.waterfallPaused);

    ImGui::Checkbox("Peak detect", &view.waterfallPeakDetect);
    helpMarker("Show the loudest bin per pixel instead of the average. Catches narrow carriers "
               "but lifts the noise floor.");

    // The ceiling is the driver's, not a constant.
    //
    // One line is one texture row, so history depth cannot exceed the largest
    // texture the driver accepts -- and on most hardware that, rather than
    // memory, is what actually binds. Showing both, with what the choice costs,
    // is the difference between "16384 is the maximum" and knowing why.
    const std::uint32_t maxTexture = WaterfallRenderer::maxTextureSize();
    const auto depthCeiling =
        static_cast<int>(maxTexture > 0 ? std::min<std::uint32_t>(maxTexture, 65536U) : 16384U);

    const std::uint32_t bins = m_waterfall.bins() > 0 ? m_waterfall.bins() : 4096U;
    const auto costBytes =
        static_cast<std::uint64_t>(bins) * static_cast<std::uint64_t>(view.waterfallLines);

    field("History", "Live scrollback depth, not the recording.");
    // Logarithmic, because the range spans eight octaves: linear, the whole
    // useful low end -- everything under a few thousand lines -- is squeezed
    // into the first few pixels of the track.
    ImGui::SliderInt("##waterfalllines", &view.waterfallLines, 256, depthCeiling, "%d lines",
                     ImGuiSliderFlags_Logarithmic);

    {
        const double lps = m_state.stats().render.waterfallLinesPerSec;
        const std::string duration =
            lps > 0.1 ? std::format(", about {}",
                                    formatDuration(static_cast<double>(view.waterfallLines) / lps))
                      : std::string{};
        fieldCaption(
            std::format("{} at {} bins{}", toml_util::formatBytes(costBytes), bins, duration));

        const SystemInfo system = systemInfo();
        std::string limits = std::format("{} max texture", maxTexture);
        if (system.totalMemoryBytes > 0) {
            limits += std::format(" | {}{}", toml_util::formatBytes(system.totalMemoryBytes),
                                  system.unifiedMemory ? " unified" : " system RAM");
        }
        fieldCaption(limits);
    }

    ImGui::SeparatorText("Time axis");

    field("Position", "Scroll or drag the axis to move through history.");
    ImGui::Combo("##timeaxis", &view.waterfallTimeAxis, "Off\0Left\0Right\0");

    ImGui::Checkbox("Time lines", &view.waterfallTimeLines);

    ImGui::SeparatorText("Levels");

    field("Gradient", "Shared with the spectrum fill.");
    ImGui::SetNextItemWidth(rangeItemWidth());
    ImGui::DragFloatRange2("##waterfallgradient", &view.gradientMinDb, &view.gradientMaxDb, 0.5F,
                           kScaleFloorDbfs, kScaleCeilingDbfs, "%.0f dB", "%.0f dB");

    field("Colours");
    if (ImGui::Button("Edit gradient...", ImVec2(-1, 0))) {
        m_editingColorMap = m_state.theme().waterfallColorMap();
        m_showGradientEditor = true;
    }

    footerRule();
    {
        const float width = footerButtonWidth(2);
        if (ImGui::Button("Clear", ImVec2(width, 0))) {
            m_waterfall.clear();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Clear the waterfall history");
        }

        ImGui::SameLine();
        if (ImGui::Button("Reset settings", ImVec2(width, 0))) {
            const ViewSettings defaults;
            view.waterfallPaused = defaults.waterfallPaused;
            view.waterfallLines = defaults.waterfallLines;
            view.waterfallTimeAxis = defaults.waterfallTimeAxis;
            view.waterfallTimeLines = defaults.waterfallTimeLines;
            view.waterfallPeakDetect = defaults.waterfallPeakDetect;
            view.gradientMinDb = defaults.gradientMinDb;
            view.gradientMaxDb = defaults.gradientMaxDb;
        }
    }

    ImGui::EndPopup();
}

void MainWindow::drawGeneralSettingsPopup() {
    if (!ImGui::BeginPopup("##generalsettings")) {
        return;
    }
    drawGeneralSettingsBody();
    ImGui::EndPopup();
}

/// The application settings themselves, without the popup around them, so the
/// merged menu can host the same controls.
void MainWindow::drawGeneralSettingsBody() {
    ImGui::SeparatorText(std::string(productName()).c_str());

    // The commit, not just the version. Everything between two releases
    // reports the same version number, so "0.1.0" on its own names any of
    // several hundred builds -- and this is the line somebody reads out when
    // something is wrong. Selectable, because reading a hash aloud is worse
    // than pasting it.
    const std::string build = std::string(buildString());
    ImGui::TextDisabled("version %s", build.c_str());
    if (ImGui::IsItemClicked()) {
        ImGui::SetClipboardText(build.c_str());
        toast(ToastSeverity::Info, "Version copied");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Click to copy");
    }

    ImGui::TextDisabled("built %s with %s", std::string(buildDate()).c_str(),
                        std::string(buildCompiler()).c_str());
    ImGui::TextDisabled("for %s", std::string(buildPlatform()).c_str());

    // Offered only where the build can honour it. A checkbox that does
    // nothing because libcurl was not found at configure time is worse than
    // no checkbox: it says the application is checking when it is not.
    if (UpdateCheck::supported()) {
        bool check = m_state.appSettings().checkForUpdates;
        if (ImGui::Checkbox("Check for updates at start-up", &check)) {
            m_state.appSettings().checkForUpdates = check;
            if (auto saved = m_state.saveAppSettings(); !saved) {
                toast(ToastSeverity::Error, saved.error().describe());
            }
        }
    }

    // A nightly keeps its own configuration directory, so it and the release
    // it is installed beside never write into each other's files. Some people
    // want the opposite -- one set of profiles and antennas, whichever build
    // is running -- and this is where they say so. The marker is written now
    // and read at the next start; nothing already open moves.
    if (channel() != "release") {
        bool shared = Paths::usesReleaseConfig();
        if (ImGui::Checkbox("Share settings with the Sweep++ release", &shared)) {
            if (auto set = Paths::setUsesReleaseConfig(shared); !set) {
                toast(ToastSeverity::Error, set.error().describe());
            } else {
                toast(ToastSeverity::Info, "Applies the next time it starts");
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Read and write the release's configuration folder instead of "
                              "this build's own.\nTakes effect on the next start.");
        }
    }

    // Performance and History are not offered here. Both have a button in the
    // status bar, and a second way in from a settings menu is a second thing
    // to find rather than a shortcut -- they are windows an operator opens
    // while watching a sweep, not settings.
    ImGui::Separator();

    // Everything the application keeps on disk is under here: themes,
    // colormaps, band plans, profiles, presets, sessions, plugins and the log.
    // Opening it is what someone actually wants when they come looking for the
    // path, which is why it was never much use as a line of text.
    if (std::string said = revealButton("Open config folder", Paths::instance().configDir(), true);
        !said.empty()) {
        toast(ToastSeverity::Info, std::move(said));
    }
}

void MainWindow::drawGradientEditor() {
    if (!ImGui::Begin("Gradient editor", &m_showGradientEditor,
                      ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped("Drag a stop to move it, click its swatch to recolour.");
    ImGui::Separator();

    // Preview strip.
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        constexpr float kWidth = 420.0F;
        constexpr float kHeight = 32.0F;
        constexpr int kSlices = 128;

        for (int i = 0; i < kSlices; ++i) {
            const float t0 = static_cast<float>(i) / kSlices;
            const float t1 = static_cast<float>(i + 1) / kSlices;
            draw->AddRectFilled(ImVec2(origin.x + kWidth * t0, origin.y),
                                ImVec2(origin.x + kWidth * t1 + 1.0F, origin.y + kHeight),
                                packed(m_editingColorMap.sample((t0 + t1) * 0.5F)));
        }
        ImGui::Dummy(ImVec2(kWidth, kHeight));
    }

    std::vector<ColorStop> stops = m_editingColorMap.stops();
    bool changed = false;

    for (std::size_t i = 0; i < stops.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));

        ImVec4 color = toImVec4(stops[i].color);
        if (ImGui::ColorEdit4("##color", &color.x,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
            stops[i].color = Color{.r = color.x, .g = color.y, .b = color.z, .a = color.w};
            changed = true;
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0F);
        if (ImGui::SliderFloat("##pos", &stops[i].position, 0.0F, 1.0F, "%.3f")) {
            changed = true;
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("x") && stops.size() > 2) {
            stops.erase(stops.begin() + static_cast<std::ptrdiff_t>(i));
            changed = true;
            ImGui::PopID();
            break;
        }

        ImGui::PopID();
    }

    if (ImGui::Button("Add stop")) {
        stops.push_back(ColorStop{.position = 0.5F, .color = m_editingColorMap.sample(0.5F)});
        changed = true;
    }

    if (changed) {
        // Re-bakes the LUT immediately, so the preview and a live waterfall
        // update as the slider moves.
        m_editingColorMap.setStops(std::move(stops));
    }

    ImGui::Separator();

    char nameBuffer[64];
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", m_newColorMapName.c_str());
    ImGui::SetNextItemWidth(220.0F);
    if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer))) {
        m_newColorMapName = nameBuffer;
    }

    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        m_editingColorMap.setName(m_newColorMapName);
        const_cast<Theme&>(m_state.theme()).addColorMap(m_editingColorMap);
        const_cast<Theme&>(m_state.theme()).waterfall().colorMap = m_newColorMapName;
        const_cast<Theme&>(m_state.theme()).spectrum().fillColorMap = m_newColorMapName;
        m_state.refreshTheme();
        m_waterfall.setColorMap(m_state.theme().waterfallColorMap());
    }

    ImGui::SameLine();
    if (ImGui::Button("Save as colormap")) {
        m_editingColorMap.setName(m_newColorMapName);
        const auto path =
            Paths::instance().colormapsDir() / std::format("{}.toml", m_newColorMapName);
        if (auto saved = m_editingColorMap.saveToToml(path); saved) {
            toast(ToastSeverity::Success, std::format("colormap written to {}", path.string()));
        } else {
            toast(ToastSeverity::Error, saved.error().describe());
        }
    }

    ImGui::End();
}

// ------------------------------------------------------------- antennas

void MainWindow::beginEditingAntenna(const Antenna* antenna) {
    if (antenna != nullptr) {
        m_editingAntenna = *antenna;
        m_editingAntennaOriginalId = antenna->id;
    } else {
        m_editingAntenna = Antenna{};
        m_editingAntennaOriginalId.clear();
    }
    m_showAntennaEditor = true;
}

void MainWindow::drawAntennasSection() {
    AntennaLibrary& library = m_state.antennas();

    fieldCaption("Assign these to connectors in the device panel.", 0.0F);

    const float action = ImGui::GetFrameHeight();
    const float actionsWidth = (action + ImGui::GetStyle().ItemSpacing.x) * 2.0F;
    std::string pendingRemoval;

    for (const Antenna& antenna : library.entries()) {
        ImGui::PushID(antenna.id.c_str());

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(antenna.name.c_str());

        // Right-aligned, so the two buttons form a column instead of trailing
        // whatever length the name happens to be.
        ImGui::SameLine();
        const float rowRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rowRight - actionsWidth));

        if (iconButton("##edit", icon::kEdit, "e", action)) {
            beginEditingAntenna(&antenna);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(antenna.builtin ? "Edit a copy" : "Edit this antenna");
        }

        ImGui::SameLine();

        // A shipped entry is listed but not removable. The list is the same on
        // every machine, so deleting one would make an antenna silently absent
        // on this bench alone -- and the way to be rid of it is to not assign
        // it, which costs nothing.
        ImGui::BeginDisabled(antenna.builtin);
        if (iconButton("##remove", icon::kClose, "x", action)) {
            pendingRemoval = antenna.id;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(antenna.builtin ? "Built-in antennas cannot be deleted"
                                              : "Delete this antenna");
        }

        // Indented under the name it belongs to, so a list of ten reads as ten
        // entries rather than as twenty lines.
        fieldCaption(describeAntenna(antenna), 12.0F);
        ImGui::PopID();
    }

    if (library.empty()) {
        fieldCaption("No antennas yet.", 0.0F);
    }

    // Deferred: removing inside the loop would invalidate the span being
    // iterated.
    if (!pendingRemoval.empty()) {
        library.remove(pendingRemoval);
        m_state.saveAntennas();
    }

    ImGui::Separator();
    if (ImGui::Button("Add antenna", ImVec2(-1, 0))) {
        beginEditingAntenna(nullptr);
    }

    fieldCaption(
        std::format("Stored in {}", (Paths::instance().antennasDir() / "custom.toml").string()),
        0.0F);
}

void MainWindow::drawAntennaEditor() {
    if (!ImGui::Begin("Antenna", &m_showAntennaEditor, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    Antenna& antenna = m_editingAntenna;

    const auto textField = [](const char* label, std::string& value, const char* hint = nullptr) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
        ImGui::SetNextItemWidth(260.0F);
        if (hint != nullptr) {
            if (ImGui::InputTextWithHint(label, hint, buffer, sizeof(buffer))) {
                value = buffer;
            }
        } else if (ImGui::InputText(label, buffer, sizeof(buffer))) {
            value = buffer;
        }
    };

    textField("Name", antenna.name, "Diamond D-190");
    textField("Category", antenna.category, "Wideband, ADS-B, GPS");
    textField("Type", antenna.type, "discone, yagi, whip");

    // Megahertz, matching the range panel, so the two places an operator types
    // a frequency want the same number in the same unit.
    double startMHz = antenna.startHz / 1e6;
    double stopMHz = antenna.stopHz / 1e6;
    ImGui::SetNextItemWidth(120.0F);
    if (ImGui::InputDouble("##start", &startMHz, 0.0, 0.0, "%.3f")) {
        antenna.startHz = startMHz * 1e6;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("-");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0F);
    if (ImGui::InputDouble("##stop", &stopMHz, 0.0, 0.0, "%.3f")) {
        antenna.stopHz = stopMHz * 1e6;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("MHz");
    helpMarker("Usable range. A routed sweep sends only these frequencies through it.");

    ImGui::SetNextItemWidth(120.0F);
    ImGui::InputDouble("Gain (dBi)", &antenna.gainDbi, 0.0, 0.0, "%.1f");

    ImGui::Checkbox("Needs bias-T", &antenna.needsBiasT);

    textField("Notes", antenna.notes);

    ImGui::Separator();

    const bool nameEmpty = antenna.name.empty();
    const bool rangeBad = antenna.stopHz <= antenna.startHz || antenna.startHz <= 0.0;

    if (nameEmpty) {
        ImGui::TextColored(toImVec4(m_state.theme().chrome().warning), "a name is required");
    } else if (rangeBad) {
        ImGui::TextColored(toImVec4(m_state.theme().chrome().warning), "stop must be above start");
    }

    ImGui::BeginDisabled(nameEmpty || rangeBad);
    if (ImGui::Button("Save", ImVec2(120.0F, 0))) {
        AntennaLibrary& library = m_state.antennas();

        // A new entry needs an id; an existing one keeps the one assignments
        // already store, *including* when a shipped antenna is being copied --
        // the copy has to carry the shipped id or it would sit beside the
        // original rather than shadowing it.
        if (antenna.id.empty()) {
            antenna.id = library.makeId(antenna.name);
        }
        if (!m_editingAntennaOriginalId.empty() && m_editingAntennaOriginalId != antenna.id) {
            library.remove(m_editingAntennaOriginalId);
        }

        library.add(antenna);
        m_state.saveAntennas();
        m_showAntennaEditor = false;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0F, 0))) {
        m_showAntennaEditor = false;
    }

    ImGui::End();
}

// ------------------------------------------------------- fft benchmark

namespace {

/// A duration at a precision worth reading. Transforms are microseconds and
/// planners are hundreds of milliseconds, and printing either in the other's
/// unit costs the reader the comparison the table exists to make.
std::string formatBenchTime(double seconds) {
    if (seconds >= 1.0) {
        return std::format("{:.2f} s", seconds);
    }
    if (seconds >= 1e-3) {
        return std::format("{:.1f} ms", seconds * 1e3);
    }
    return std::format("{:.2f} us", seconds * 1e6);
}

/// What the results table is showing.
///
/// A selector rather than one wide table: five metrics across several backends
/// is more columns than a panel can hold, and the one that matters depends on
/// what is being asked. Per-transform answers "which is the faster
/// transform"; throughput at the worker count answers "which will sweep
/// faster", and those are not the same question.
enum class BenchMetric : int {
    Median = 0,
    Tail,
    Throughput,
    Cpu,
    Plan,
};

/// Whether a smaller number wins. CPU is deliberately absent: fewer cores is
/// not better if they bought throughput, so that column is diagnostic and gets
/// no winner highlight.
[[nodiscard]] bool lowerIsBetter(BenchMetric metric) {
    return metric != BenchMetric::Throughput && metric != BenchMetric::Cpu;
}

[[nodiscard]] double metricValue(const FftBenchmarkSample& sample, BenchMetric metric) {
    switch (metric) {
    case BenchMetric::Median:
        return sample.p50Seconds;
    case BenchMetric::Tail:
        return sample.p99Seconds;
    case BenchMetric::Throughput:
        return sample.throughputPerSecond;
    case BenchMetric::Cpu:
        return sample.cpuCores;
    case BenchMetric::Plan:
        return sample.planSeconds;
    }
    return 0.0;
}

[[nodiscard]] std::string formatMetric(const FftBenchmarkSample& sample, BenchMetric metric) {
    const double value = metricValue(sample, metric);
    switch (metric) {
    case BenchMetric::Throughput:
        return std::format("{:.0f}/s", value);
    case BenchMetric::Cpu:
        return value < 0.0 ? "-" : std::format("{:.2f}", value);
    default:
        return formatBenchTime(value);
    }
}

/// The sample for one size at one thread count, or null when there is none.
const FftBenchmarkSample* sampleAt(const FftBenchmarkEntry& entry, std::size_t size,
                                   std::size_t threads) {
    const auto found =
        std::ranges::find_if(entry.samples, [size, threads](const FftBenchmarkSample& s) {
            return s.size == size && s.threads == threads;
        });
    return found == entry.samples.end() ? nullptr : &*found;
}

/// Every (size, threads) pair any backend reported, ascending. Taken from the
/// results rather than from the config so the table matches what was actually
/// run, including a run that was cancelled part way.
std::vector<std::pair<std::size_t, std::size_t>>
benchmarkRows(const std::vector<FftBenchmarkEntry>& entries) {
    std::vector<std::pair<std::size_t, std::size_t>> rows;
    for (const FftBenchmarkEntry& entry : entries) {
        for (const FftBenchmarkSample& sample : entry.samples) {
            rows.emplace_back(sample.size, sample.threads);
        }
    }
    std::ranges::sort(rows);
    const auto duplicates = std::ranges::unique(rows);
    rows.erase(duplicates.begin(), duplicates.end());
    return rows;
}

/// The highest thread count measured -- the rows that predict a sweep.
[[nodiscard]] std::size_t
widestThreadCount(const std::vector<std::pair<std::size_t, std::size_t>>& rows) {
    std::size_t widest = 1;
    for (const auto& [size, threads] : rows) {
        widest = std::max(widest, threads);
    }
    return widest;
}

/// Tops of the size ladder the benchmark offers.
///
/// An operator picks a transform size to get a resolution bandwidth, so the
/// ladder has to reach the sizes that produce the fine ones: at 20 MS/s a
/// 1 kHz RBW needs about 30k points and at 100 MS/s about 150k, neither of
/// which the four-entry default came near.
///
/// It is not simply always the longest, because the cost is real and it is
/// FFTW's: its measuring planner takes 2.3 s at 262144 and 3.1 s at 1048576,
/// against vDSP's 10 and 46 ms. That asymmetry is itself a result worth seeing,
/// but it should not be charged to someone who wanted a quick check.
constexpr std::array<std::uint32_t, 3> kBenchLadderTops{65536, 262144, 1048576};

/// Powers of four from 1024 to `top`, which spans the range in six rows rather
/// than the eleven every power of two would need.
[[nodiscard]] std::vector<std::size_t> benchLadder(std::uint32_t top) {
    std::vector<std::size_t> sizes;
    for (std::size_t size = 1024; size <= top; size *= 4) {
        sizes.push_back(size);
    }
    return sizes;
}

} // namespace

void MainWindow::drawFftBenchmarkWindow() {
    // Sized explicitly for the same reason the Performance panel is: with no
    // ini file there is no saved geometry, and a window whose content is all
    // full-width tables auto-fits to nothing.
    ImGui::SetNextWindowSize(ImVec2(660.0F, 560.0F), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(bar::scaled(ImVec2(460.0F, 320.0F)),
                                        ImVec2(FLT_MAX, FLT_MAX));

    if (!ImGui::Begin("FFT benchmark", &m_showFftBenchmark)) {
        ImGui::End();
        return;
    }

    const ChromeTheme& chrome = m_state.theme().chrome();
    const bool running = m_fftBenchmark.running();

    const std::uint32_t workers = m_state.pipelineConfig().workerCount != 0
                                      ? m_state.pipelineConfig().workerCount
                                      : defaultWorkerCount();

    // The sample rate in force, so a transform size can be shown as the
    // resolution bandwidth it buys -- which is the unit the size was chosen in
    // and the only one the table means anything in.
    double sampleRate = m_state.sweepPlan().sampleRate;
    if (!m_state.sweeping()) {
        const ISdrDevice* device = m_state.device();
        sampleRate = device != nullptr
                         ? asDouble(device->getParameter("sample_rate").value_or(SdrValue{20e6}))
                         : 20e6;
    }
    const double enbw =
        windowEnbw(m_state.pipelineConfig().window, m_state.pipelineConfig().windowBeta);
    const auto rbwFor = [sampleRate, enbw](std::size_t size) {
        return size == 0 ? 0.0 : sampleRate * enbw / static_cast<double>(size);
    };

    ImGui::TextWrapped("Times every available backend on this machine.");

    ImGui::Spacing();

    // Depth, because the default was too short to be trusted. A median taken
    // over a few milliseconds moves by tens of percent between runs, which is
    // how a benchmark comes to disagree with the pipeline.
    constexpr std::array kDepthSeconds{0.1, 0.35, 1.0};
    ImGui::BeginDisabled(running);
    field("Depth", "Time spent on each measurement.", 0.0F);
    ImGui::Combo("##benchdepth", &m_fftBenchDepth, "Quick\0Normal\0Thorough\0");

    field("Threads", nullptr, 0.0F);
    ImGui::Checkbox("1", &m_fftBenchOneThread);
    ImGui::SameLine();
    ImGui::Checkbox(std::format("{} workers", workers).c_str(), &m_fftBenchAllWorkers);
    if (!m_fftBenchOneThread && !m_fftBenchAllWorkers) {
        // Something has to be measured; silently re-checking the one that
        // answers the more useful question beats an empty table.
        m_fftBenchAllWorkers = true;
    }

    m_fftBenchTop = std::clamp(m_fftBenchTop, 0, static_cast<int>(kBenchLadderTops.size()) - 1);
    field("Sizes", "Largest FFT size to measure.", 0.0F);
    if (ImGui::BeginCombo(
            "##benchtop",
            std::format("to {} ({})",
                        groupedCount(kBenchLadderTops[static_cast<std::size_t>(m_fftBenchTop)]),
                        toml_util::formatFrequencyShort(
                            rbwFor(kBenchLadderTops[static_cast<std::size_t>(m_fftBenchTop)])))
                .c_str())) {
        for (std::size_t i = 0; i < kBenchLadderTops.size(); ++i) {
            const bool selected = static_cast<int>(i) == m_fftBenchTop;
            if (ImGui::Selectable(
                    std::format("to {} points -- {} RBW", groupedCount(kBenchLadderTops[i]),
                                toml_util::formatFrequencyShort(rbwFor(kBenchLadderTops[i])))
                        .c_str(),
                    selected)) {
                m_fftBenchTop = static_cast<int>(i);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    // Built here as well as on Run, so the estimate below is the real one.
    FftBenchmarkConfig config = defaultFftBenchmarkConfig();
    config.secondsPerSample = kDepthSeconds[static_cast<std::size_t>(
        std::clamp(m_fftBenchDepth, 0, static_cast<int>(kDepthSeconds.size()) - 1))];
    config.sizes = benchLadder(kBenchLadderTops[static_cast<std::size_t>(m_fftBenchTop)]);
    // Always the size actually configured, wherever the ladder stops: the
    // comparison has to include the one this installation is going to run.
    config.sizes.push_back(m_state.pipelineConfig().fftSize);
    std::ranges::sort(config.sizes);
    {
        const auto duplicates = std::ranges::unique(config.sizes);
        config.sizes.erase(duplicates.begin(), duplicates.end());
    }
    config.threadCounts.clear();
    if (m_fftBenchOneThread) {
        config.threadCounts.push_back(1);
    }
    if (m_fftBenchAllWorkers && workers > 1) {
        config.threadCounts.push_back(workers);
    }
    if (config.threadCounts.empty()) {
        config.threadCounts.push_back(1);
    }

    std::size_t availableBackends = 0;
    for (const FftBackendInfo& info : FftBackendManager::instance().enumerate()) {
        availableBackends += info.available ? 1 : 0;
    }
    const std::size_t steps = fftBenchmarkStepCount(config, availableBackends);

    ImGui::Spacing();
    ImGui::BeginDisabled(running);
    if (ImGui::Button(running ? "Running..." : "Run benchmark")) {
        m_fftBenchmark.start(config);
    }
    ImGui::EndDisabled();

    if (running) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            m_fftBenchmark.cancel();
        }
    }

    ImGui::SameLine();
    if (running) {
        ImGui::TextDisabled("%s", formatBenchTime(m_fftBenchmark.elapsedSeconds()).c_str());
    } else {
        // Planning is excluded: it is measured, not predicted, and on FFTW it
        // is most of a short run.
        ImGui::TextDisabled(
            "%zu measurements, about %s plus planning", steps,
            formatBenchTime(static_cast<double>(steps) * config.secondsPerSample).c_str());
    }

    if (running) {
        const std::size_t total = m_fftBenchmark.stepsTotal();
        const float fraction =
            total == 0 ? 0.0F
                       : static_cast<float>(m_fftBenchmark.stepsDone()) / static_cast<float>(total);
        ImGui::ProgressBar(fraction, ImVec2(-1, 0), m_fftBenchmark.currentStep().c_str());
    }

    // Said while it can still be acted on rather than printed beside the
    // results: a sweep and a benchmark contend for the same cores, so numbers
    // taken during one are honest about this machine but not about the
    // backends.
    if (m_state.running()) {
        ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(chrome.warning));
        ImGui::TextWrapped("A sweep is running and will skew the results.");
        ImGui::PopStyleColor();
    }

    const std::vector<FftBenchmarkEntry> results = m_fftBenchmark.results();
    if (results.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", running ? "Measuring..." : "No results yet.");
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("Results");

    field("Show", nullptr, 0.0F);
    ImGui::Combo("##benchmetric", &m_fftBenchMetric,
                 "Per transform (p50)\0Tail (p99)\0Throughput\0CPU cores\0Plan time\0");
    const auto metric = static_cast<BenchMetric>(m_fftBenchMetric);

    const std::vector<std::pair<std::size_t, std::size_t>> rows = benchmarkRows(results);
    const std::size_t widest = widestThreadCount(rows);

    // Plan construction has no thread axis -- the pipeline builds one plan and
    // shares it -- so repeating a size's figure once per thread count would be
    // padding the table with the same number.
    const bool perThread = metric != BenchMetric::Plan;
    const auto columns = static_cast<int>(results.size()) + (perThread ? 3 : 2);

    if (ImGui::BeginTable("##benchresults", columns,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("RBW");
        if (perThread) {
            ImGui::TableSetupColumn("Threads");
        }
        for (const FftBenchmarkEntry& entry : results) {
            ImGui::TableSetupColumn(entry.backend.c_str());
        }
        ImGui::TableHeadersRow();

        std::size_t previousSize = 0;
        for (const auto& [size, threads] : rows) {
            if (!perThread && size == previousSize) {
                continue;
            }
            previousSize = size;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(groupedCount(static_cast<std::uint32_t>(size)).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(toml_util::formatFrequencyShort(rbwFor(size)).c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("at %s and the current window",
                                  toml_util::formatFrequencyShort(sampleRate).c_str());
            }
            if (perThread) {
                ImGui::TableNextColumn();
                ImGui::Text("%zu", threads);
            }

            // The best cell in the row, so the eye finds the winner without
            // comparing numbers across a table.
            double best = 0.0;
            bool haveBest = false;
            if (metric != BenchMetric::Cpu) {
                for (const FftBenchmarkEntry& entry : results) {
                    const FftBenchmarkSample* sample = sampleAt(entry, size, threads);
                    if (sample == nullptr || !sample->skipped.empty()) {
                        continue;
                    }
                    const double value = metricValue(*sample, metric);
                    if (value <= 0.0) {
                        continue;
                    }
                    if (!haveBest || (lowerIsBetter(metric) ? value < best : value > best)) {
                        best = value;
                        haveBest = true;
                    }
                }
            }

            for (const FftBenchmarkEntry& entry : results) {
                ImGui::TableNextColumn();
                const FftBenchmarkSample* sample = sampleAt(entry, size, threads);
                if (sample == nullptr) {
                    ImGui::TextDisabled("-");
                    continue;
                }
                if (!sample->skipped.empty()) {
                    ImGui::TextDisabled("-");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", sample->skipped.c_str());
                    }
                    continue;
                }

                const double value = metricValue(*sample, metric);
                const bool winner = haveBest && value == best;
                if (winner) {
                    ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(chrome.ok));
                }
                if (winner || !haveBest || value <= 0.0) {
                    ImGui::TextUnformatted(formatMetric(*sample, metric).c_str());
                } else {
                    const double factor = lowerIsBetter(metric) ? value / best : best / value;
                    ImGui::Text("%s  %.2fx", formatMetric(*sample, metric).c_str(), factor);
                }
                if (winner) {
                    ImGui::PopStyleColor();
                }

                // Everything about this cell, so switching metric is for
                // reading a column rather than for finding one number.
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("p50 %s | p99 %s | max %s\n%.0f FFT/s | %.2f cores | "
                                      "%zu runs\nplan %s",
                                      formatBenchTime(sample->p50Seconds).c_str(),
                                      formatBenchTime(sample->p99Seconds).c_str(),
                                      formatBenchTime(sample->maxSeconds).c_str(),
                                      sample->throughputPerSecond, sample->cpuCores, sample->runs,
                                      formatBenchTime(sample->planSeconds).c_str());
                }
            }
        }
        ImGui::EndTable();
    }

    for (const FftBenchmarkEntry& entry : results) {
        if (!entry.error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(chrome.danger));
            ImGui::TextWrapped("%s: %s", entry.backend.c_str(), entry.error.c_str());
            ImGui::PopStyleColor();
        }
    }

    // ------------------------------------------------------------ verdict
    //
    // Per size at the widest thread count, and never averaged into one
    // number. The backends genuinely trade places across the size range on
    // this hardware, and a single headline figure would hide exactly the
    // crossover an operator needs to know about.
    ImGui::SeparatorText("Verdict");

    const auto fastestAt = [&](std::size_t size, std::size_t threads) -> const FftBenchmarkEntry* {
        const FftBenchmarkEntry* winner = nullptr;
        double bestThroughput = 0.0;
        for (const FftBenchmarkEntry& entry : results) {
            const FftBenchmarkSample* sample = sampleAt(entry, size, threads);
            if (sample == nullptr || !sample->skipped.empty()) {
                continue;
            }
            if (sample->throughputPerSecond > bestThroughput) {
                bestThroughput = sample->throughputPerSecond;
                winner = &entry;
            }
        }
        return winner;
    };

    if (results.size() < 2) {
        ImGui::TextDisabled("Only one backend is installed; there is nothing to compare.");
    } else {
        ImGui::TextDisabled("At %zu threads -- the arrangement a sweep uses.", widest);
        for (const auto& [size, threads] : rows) {
            if (threads != widest) {
                continue;
            }
            const FftBenchmarkEntry* winner = fastestAt(size, threads);
            if (winner == nullptr) {
                continue;
            }
            const FftBenchmarkSample* best = sampleAt(*winner, size, threads);

            double runnerUp = 0.0;
            for (const FftBenchmarkEntry& entry : results) {
                if (&entry == winner) {
                    continue;
                }
                const FftBenchmarkSample* sample = sampleAt(entry, size, threads);
                if (sample != nullptr && sample->skipped.empty()) {
                    runnerUp = std::max(runnerUp, sample->throughputPerSecond);
                }
            }
            if (best == nullptr || runnerUp <= 0.0) {
                continue;
            }
            ImGui::BulletText("%s points (%s RBW): %s, by %.2fx",
                              groupedCount(static_cast<std::uint32_t>(size)).c_str(),
                              toml_util::formatFrequencyShort(rbwFor(size)).c_str(),
                              winner->backend.c_str(), best->throughputPerSecond / runnerUp);
        }
    }

    // The button switches to whichever wins at the size actually configured,
    // because that is the only size this installation is going to run.
    const FftBenchmarkEntry* forThisSize = fastestAt(m_state.pipelineConfig().fftSize, widest);
    if (forThisSize == nullptr) {
        ImGui::End();
        return;
    }

    ImGui::Spacing();
    if (m_state.fftBackendName() == forThisSize->backend) {
        ImGui::TextDisabled("%s is already selected, and wins at the configured size of %u.",
                            forThisSize->backend.c_str(), m_state.pipelineConfig().fftSize);
    } else {
        if (ImGui::Button(std::format("Use {}", forThisSize->backend).c_str())) {
            if (auto switched = m_state.setFftBackend(forThisSize->backend); !switched) {
                toast(ToastSeverity::Error,
                      std::format("could not switch to '{}': {}", forThisSize->backend,
                                  switched.error().message()));
            } else {
                toast(ToastSeverity::Info,
                      std::format("FFT backend '{}' selected", forThisSize->backend));
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("fastest at the configured size of %u",
                            m_state.pipelineConfig().fftSize);
    }

    ImGui::End();
}

// ---------------------------------------------------------- performance

void MainWindow::drawPerformancePanel() {
    // An explicit first-use size is required, not merely nicer.
    //
    // Layout persistence is owned by the profile system, so io.IniFilename is
    // null and every window opens without saved geometry. ImGui then auto-fits
    // the window to its content -- but this panel's tables, progress bars and
    // sparklines are all full-width (-1), which resolves to nothing in a
    // window that has no width yet. The two feed each other and the window
    // collapses to a sliver.
    ImGui::SetNextWindowSize(ImVec2(560.0F, 720.0F), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(bar::scaled(ImVec2(420.0F, 260.0F)),
                                        ImVec2(FLT_MAX, FLT_MAX));

    if (!ImGui::Begin("Performance", &m_showPerformance)) {
        ImGui::End();
        return;
    }

    const TelemetrySnapshot& stats = m_state.stats();
    const Telemetry::History& history = m_state.telemetry().history();
    const ChromeTheme& chrome = m_state.theme().chrome();

    std::array<float, Telemetry::kHistoryLength> buffer{};

    const auto section = [](const char* title) { ImGui::SeparatorText(title); };

    // The same, with Reset sharing the row.
    //
    // Every count in this panel is cumulative since the radio started, which is
    // what makes them worth trusting for an acceptance run -- and also what
    // makes them hard to read while changing something. One burst of drops at
    // start-up pins the percentage for the rest of the session, so "is it
    // better now" could not be answered without closing the device to get a
    // fresh baseline. This gives one without touching the radio.
    //
    // On the separator's own row rather than a row of its own: the panel is
    // dense and every figure is worth seeing at once, so a whole row spent on
    // one button pushed the rest down for nothing. Drawn after the separator
    // and then placed back on its row, so the rule runs behind it -- and as a
    // SmallButton, whose height is a line of text rather than a full frame,
    // which would stand proud of the separator and crowd the bar beneath.
    const auto sectionWithReset = [this](const char* title) {
        const std::string label = icon::glyphOr(icon::kReload, "Reset");
        const float width =
            ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0F;
        const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        const float top = ImGui::GetCursorPosY();

        ImGui::SeparatorText(title);
        const float resume = ImGui::GetCursorPosY();

        ImGui::SetCursorPos(ImVec2(std::max(right - width, 0.0F), top));
        if (ImGui::SmallButton(std::string(label).append("##perfreset").c_str())) {
            // Everything below reads `stats`, a reference to the copy this
            // zeroes, so the panel answers on this frame instead of showing the
            // old figures until the 4 Hz refresh catches up.
            m_state.resetTelemetry();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Reset the counters and graphs");
        }
        ImGui::SetCursorPosY(resume);
    };

    // Link utilisation gets a bar rather than a number: an operator seeing a
    // nearly full bar knows instantly that the cable or the port is the limit,
    // not the settings.
    sectionWithReset("Input");
    if (stats.stream.linkCapacityBytesPerSec > 0.0) {
        const auto fraction = static_cast<float>(stats.stream.linkUtilisation);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, toImVec4(fraction > 0.9F   ? chrome.danger
                                                               : fraction > 0.7F ? chrome.warning
                                                                                 : chrome.ok));
        ImGui::ProgressBar(
            fraction, ImVec2(-1, 0),
            std::format("{} of {} ({:.0f}%)", toml_util::formatByteRate(stats.stream.bytesPerSecIn),
                        toml_util::formatByteRate(stats.stream.linkCapacityBytesPerSec),
                        stats.stream.linkUtilisation * 100.0)
                .c_str());
        ImGui::PopStyleColor();
    }

    ImGui::ProgressBar(
        stats.stream.ringFillFraction, ImVec2(-1, 0),
        std::format("ring {:.0f}% full", static_cast<double>(stats.stream.ringFillFraction) * 100.0)
            .c_str());

    if (ImGui::BeginTable("##input", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        setupStatColumns();
        int count = flatten(history.measuredSps, buffer);
        statRow("Sample rate",
                std::format("{} / {}", toml_util::formatFrequencyShort(stats.stream.measuredSps),
                            toml_util::formatFrequencyShort(stats.stream.configuredSps)),
                buffer.data(), count, 0.0F, static_cast<float>(stats.stream.configuredSps * 1.2),
                &chrome.accent);

        count = flatten(history.dropRate, buffer);
        statRow("Samples dropped",
                std::format("{} ({:.3f}%)",
                            stats.stream.samplesDropped + stats.stream.samplesLostAtSource,
                            stats.stream.dropFraction * 100.0),
                buffer.data(), count, 0.0F, std::max(history.dropRate.max(), 1.0F), &chrome.danger);

        // Split out, because the two call for different remedies: discarded
        // means the host could not process what it was given, never taken means
        // it could not accept it in the first place.
        statRow("  discarded", std::format("{}", stats.stream.samplesDropped));
        statRow("  never taken", std::format("{}", stats.stream.samplesLostAtSource));

        statRow("Device overruns", std::format("{}", stats.stream.deviceOverruns));
        statRow("Ring full", std::format("{}", stats.stream.ringFullEvents));
        statRow("Pool exhausted", std::format("{}", stats.stream.poolExhaustedEvents));
        statRow("Sequence gaps", std::format("{}", stats.stream.sequenceGaps));
        ImGui::EndTable();
    }

    // Whatever the radio can say about its own condition, rendered without any
    // per-device knowledge here -- the same arrangement as the parameter panel.
    // A driver that reports nothing simply contributes no section.
    if (!m_state.health().empty()) {
        section("Device");

        if (ImGui::BeginTable("##devicehealth", 3,
                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            setupStatColumns();
            std::array<float, 240> scratch{};

            for (const AppState::HealthTrace& trace : m_state.health()) {
                const bool plottable = trace.reading.maximum > trace.reading.minimum;
                const int count = plottable ? flatten(trace.history, scratch) : 0;

                if (trace.reading.alarm) {
                    ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(m_state.theme().chrome().danger));
                }
                statRow(trace.reading.label.c_str(), trace.reading.value,
                        count > 1 ? scratch.data() : nullptr, count, trace.reading.minimum,
                        trace.reading.maximum, &m_state.theme().spectrum().traceAverage);
                if (trace.reading.alarm) {
                    ImGui::PopStyleColor();
                }
            }
            ImGui::EndTable();
        }
    }

    section("Processing");

    // The single number that answers "am I seeing everything?".
    {
        const auto fraction = static_cast<float>(stats.process.processedFraction);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, toImVec4(fraction > 0.99F  ? chrome.ok
                                                               : fraction > 0.5F ? chrome.warning
                                                                                 : chrome.danger));
        ImGui::ProgressBar(fraction, ImVec2(-1, 0),
                           std::format("{:.1f}% of delivered samples processed",
                                       stats.process.processedFraction * 100.0)
                               .c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Text("Throttle: %s", std::string(toString(stats.process.throttleReason)).c_str());
    helpMarker("every-nth: 1/N coverage requested\n"
               "cpu-limited: workers cannot keep up\n"
               "ring-full / pool-exhausted: lost before a worker\n"
               "device-overrun: dropped by the radio");

    if (ImGui::BeginTable("##processing", 3,
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        setupStatColumns();
        int count = flatten(history.fftsPerSec, buffer);
        statRow("FFT rate", std::format("{:.0f}/s", stats.process.fftsPerSec), buffer.data(), count,
                0.0F, std::max(history.fftsPerSec.max(), 1.0F), &chrome.accent);

        count = flatten(history.fftLatencyP99, buffer);
        statRow("FFT latency",
                std::format("p50 {:.0f} us | p99 {:.0f} us",
                            static_cast<double>(stats.process.fftLatencyP50Us),
                            static_cast<double>(stats.process.fftLatencyP99Us)),
                buffer.data(), count, 0.0F, std::max(history.fftLatencyP99.max(), 1.0F),
                &chrome.warning);

        statRow("Workers", std::format("{} ({:.0f}% busy)", stats.process.workerCount,
                                       stats.process.workerUtilisation * 100.0));
        statRow("FFTs computed", std::format("{}", stats.process.fftsComputed));
        statRow("FFTs skipped", std::format("{}", stats.process.fftsSkipped));
        statRow("Sweep passes", std::format("{}", stats.process.sweepPassesCompleted));
        statRow("Retunes", std::format("{:.0f}/s", stats.process.retunesPerSec));
        ImGui::EndTable();
    }

    section("Output");
    if (ImGui::BeginTable("##output", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        setupStatColumns();
        int count = flatten(history.fps, buffer);
        statRow("Render", std::format("{:.0f} FPS", stats.render.fps), buffer.data(), count, 0.0F,
                std::max(history.fps.max(), 1.0F), &chrome.ok);

        count = flatten(history.waterfallLps, buffer);
        statRow("Waterfall", std::format("{:.0f} lines/s", stats.render.waterfallLinesPerSec),
                buffer.data(), count, 0.0F, std::max(history.waterfallLps.max(), 1.0F),
                &chrome.accent);

        count = flatten(history.cpuPercent, buffer);
        statRow("CPU", std::format("{:.0f}% of one core", stats.render.cpuPercent), buffer.data(),
                count, 0.0F, std::max(history.cpuPercent.max(), 100.0F), &chrome.warning);

        statRow("Sweep speed", std::format("{:.1f} MHz/s", stats.render.sweepSpeedHzPerSec / 1e6));
        statRow("Uptime", formatDuration(stats.uptimeSeconds));
        ImGui::EndTable();
    }

    // Per consumer, so "the recorder is behind" stays distinguishable from
    // "the radio is dropping samples".
    section("Frame consumers");
    if (ImGui::BeginTable("##consumers", 4,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("Consumer");
        ImGui::TableSetupColumn("Delivered");
        ImGui::TableSetupColumn("Dropped");
        ImGui::TableSetupColumn("Max callback");
        ImGui::TableHeadersRow();

        for (const ConsumerSnapshot& consumer : m_state.displayBus().consumerStats()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(consumer.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(consumer.framesDelivered));
            ImGui::TableNextColumn();
            if (consumer.framesDropped > 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(chrome.warning));
            }
            ImGui::Text("%llu (%.2f%%)", static_cast<unsigned long long>(consumer.framesDropped),
                        consumer.dropFraction * 100.0);
            if (consumer.framesDropped > 0) {
                ImGui::PopStyleColor();
            }
            ImGui::TableNextColumn();
            ImGui::Text("%llu us", static_cast<unsigned long long>(consumer.maxCallbackUs));
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

void MainWindow::drawHistoryWindow() {
    pollHistoryOpen();
    updateHistoryTitle();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    // The root window's style, taken from the instrument rather than left at
    // ImGui's defaults. Without these the bars sit inside eight pixels of
    // padding with a rounded, bordered edge, so they read as panels floating on
    // the window instead of as the window's own chrome -- which is most of what
    // made this one look like a different application.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    // NoScrollWithMouse as well as the scrollbar NoDecoration already implies.
    // Without it the viewer still scrolled -- silently, since there was no bar
    // to show it -- and every wheel event meant for the panes moved the whole
    // window instead. This is the same set the main window uses.
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus |
                                        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar |
                                        ImGuiWindowFlags_NoScrollWithMouse;

    const bool open = ImGui::Begin("##history", nullptr, kFlags);
    ImGui::PopStyleVar(3);
    if (!open) {
        ImGui::End();
        return;
    }

    drawHistoryToolbar();

    // No rule under the bar: the toolbar carries its own background, which is
    // the edge, and the line only ever appeared with no file open -- so the
    // window gained a border at the exact moment it had least in it.
    if (m_history.isOpen()) {
        drawHistoryTimeline();
    } else if (!m_historyLoad) {
        // Nothing to draw *and* nothing on the way is the empty state; a file
        // on the way gets the progress card below instead, so the two do not
        // stack on top of each other for the length of the read.
        drawHistoryEmptyCard();
    }

    drawHistoryLoadingCard();

    ImGui::End();

    // The viewer gets the same corner. It draws the spectrum widget, which
    // raises messages of its own, and until this was here they went nowhere at
    // all.
    drawToasts();
}

void MainWindow::drawHistoryEmptyCard() {
    const ChromeTheme& chrome = m_state.theme().chrome();
    const ImGuiStyle& style = ImGui::GetStyle();

    const std::string heading = "No session open";

    // This window records nothing and sweeps nothing; it opens files. The
    // empty state used to send the operator off to start a sweep, which is a
    // thing the other window does and this one cannot.
    //
    // Broken into lines here rather than left to wrap, so the height is known
    // before the card is placed -- which is what centring it needs.
    const std::array<const char*, 2> lines{"Open a .sweeps session file to scroll",
                                           "through what was recorded."};

    float width = ImGui::CalcTextSize(heading.c_str()).x;
    for (const char* line : lines) {
        width = std::max(width, ImGui::CalcTextSize(line).x);
    }
    width = std::clamp(width + kCardPadding.x * 2.0F, 320.0F,
                       std::max(ImGui::GetMainViewport()->WorkSize.x - 64.0F, 320.0F));

    // The message that matters most when nothing is open is the one saying why
    // -- and the status bar that would otherwise carry it is only drawn once a
    // file is open, so it goes in the card. Wrapped rather than clipped: it is
    // a path and a reason, and both ends of it matter.
    const float wrap = width - kCardPadding.x * 2.0F;
    const float messageHeight =
        m_historyMessage.empty()
            ? 0.0F
            : ImGui::CalcTextSize(m_historyMessage.c_str(), nullptr, false, wrap).y +
                  style.ItemSpacing.y;

    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const float buttonHeight = ImGui::GetFontSize() + bar::framePadding().y * 2.0F;
    const float height = kCardPadding.y * 2.0F + lineHeight * static_cast<float>(lines.size() + 1) +
                         messageHeight + style.ItemSpacing.y * 2.0F + buttonHeight;

    beginCentredCard("##historyempty", ImVec2(width, height), chrome);

    centredText(heading.c_str(), chrome.text);

    if (!m_historyMessage.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(chrome.danger));
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted(m_historyMessage.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    for (const char* line : lines) {
        centredText(line, chrome.textDim);
    }

    ImGui::Dummy(ImVec2(0.0F, style.ItemSpacing.y));

    // The same action as the toolbar's chip, in the place the eye is already
    // looking. Sized to its label rather than to the card, so it reads as a
    // button and not as a banner.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, bar::framePadding());
    const std::string caption =
        std::format("{}  Open session file##histopencard", icon::glyphOr(icon::kOpen, ""));
    const float buttonWidth =
        ImGui::CalcTextSize(caption.c_str(), nullptr, true).x + bar::framePadding().x * 2.0F;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max((ImGui::GetContentRegionAvail().x - buttonWidth) * 0.5F, 0.0F));
    if (ImGui::Button(caption.c_str())) {
        promptForHistoryFile();
    }
    ImGui::PopStyleVar();

    endCentredCard();
}

void MainWindow::drawHistoryLoadingCard() {
    drawProgressCard(
        "##historyloading", m_historyLoad.has_value(),
        m_historyLoad ? std::format("Opening {}", m_historyLoad->path.filename().string())
                      : std::string{},
        "Reading the recording and its index", m_historyLoad ? m_historyLoad->startedNs : 0);
}

void MainWindow::drawSessionSaveCard() {
    drawProgressCard("##sessionsave", m_state.sessionSaveRunning(), m_state.sessionSaveLabel(),
                     m_state.sessionSaveDetail(), m_state.sessionSaveStartedNs());
}

void MainWindow::drawStartupCard() {
    // The window is up and drawing underneath, which is the point: these
    // seconds used to be spent before it existed at all, so a radio slow to
    // come up was indistinguishable from an application that failed to start.
    drawProgressCard("##devicestartup", m_state.deviceStartupRunning(),
                     m_state.deviceStartupLabel(), m_state.deviceStartupDetail(),
                     m_state.deviceStartupStartedNs());
}

void MainWindow::drawProgressCard(const char* id, bool active, const std::string& heading,
                                  const std::string& detail, std::uint64_t startedNs) {
    // A modal, not a panel drawn over the top.
    //
    // What is underneath must not be touched while it is being replaced: the
    // radio is halfway open, or the reader is about to be swapped out from
    // under the plots. A modal is how ImGui says exactly that -- it dims
    // everything behind it and takes the input -- so the viewer's file and the
    // instrument's radio wait behind the same panel.
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoScrollbar;

    // Held back so work that finishes in a few frames -- which most of it does
    // -- never flashes a panel. What this is for is the file or the radio slow
    // enough to be waited on, and on those the delay is lost in the wait.
    constexpr std::uint64_t kShowAfterNs = 150'000'000;
    const std::uint64_t elapsed = monotonicNs() - startedNs;

    if (!active || elapsed < kShowAfterNs) {
        // Whatever it was waiting for has landed. A popup can only be closed
        // from inside itself, so it is entered once more to do exactly that.
        if (ImGui::IsPopupOpen(id) && ImGui::BeginPopupModal(id, nullptr, kFlags)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        return;
    }

    const ChromeTheme& chrome = m_state.theme().chrome();
    const ImGuiStyle& style = ImGui::GetStyle();

    if (!ImGui::IsPopupOpen(id)) {
        ImGui::OpenPopup(id);
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5F,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5F),
                            ImGuiCond_Always, ImVec2(0.5F, 0.5F));

    ImGui::PushStyleColor(ImGuiCol_PopupBg, toImVec4(chrome.panelBackground));
    ImGui::PushStyleColor(ImGuiCol_Border, toImVec4(chrome.border));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, kCardRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, kCardPadding);

    if (ImGui::BeginPopupModal(id, nullptr, kFlags)) {
        constexpr float kSpinnerRadius = 18.0F;
        const std::string elapsedText =
            std::format("{:.1f} s", static_cast<double>(elapsed) * 1e-9);

        // Auto-resized to its contents, but never narrower than this: a
        // heading that grows a word must not make the panel breathe, and a
        // spinner alone in a box the width of "3.2 s" reads as a glitch.
        constexpr float kMinContentWidth = 260.0F;
        const float contentWidth =
            std::max({ImGui::CalcTextSize(heading.c_str()).x, ImGui::CalcTextSize(detail.c_str()).x,
                      kMinContentWidth});
        ImGui::Dummy(ImVec2(contentWidth, 0.0F));

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             std::max((contentWidth - kSpinnerRadius * 2.0F) * 0.5F, 0.0F));
        spinner(kSpinnerRadius, 3.0F, chrome.separator, chrome.accent);

        ImGui::Dummy(ImVec2(0.0F, style.ItemSpacing.y));
        centredText(heading.c_str(), chrome.text, contentWidth);
        centredText(detail.c_str(), chrome.textDim, contentWidth);
        centredText(elapsedText.c_str(), chrome.textDim, contentWidth);

        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

void MainWindow::promptForHistoryFile() {
    if (auto chosen = openFileDialog(Paths::instance().sessionsDir(), "Sweep session", "sweeps")) {
        beginHistoryOpen(*chosen);
    }
}

void MainWindow::beginHistoryOpen(const std::filesystem::path& path, bool reload) {
    if (path.empty() || m_historyLoad) {
        return;
    }

    m_historyMessage.clear();

    HistoryLoad load;
    load.path = path;
    load.reload = reload;
    load.startedNs = monotonicNs();
    load.future = std::async(std::launch::async, [path] {
        HistoryOpenResult result;
        auto reader = session::SessionReader::open(path, session::sweepsLogSink());
        if (!reader) {
            result.error = reader.error().describe();
        } else {
            result.reader = std::move(*reader);
        }
        return result;
    });

    m_historyLoad = std::move(load);
}

void MainWindow::pollHistoryOpen() {
    if (!m_historyLoad ||
        m_historyLoad->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }

    HistoryOpenResult result = m_historyLoad->future.get();
    const std::filesystem::path path = m_historyLoad->path;
    const bool reload = m_historyLoad->reload;
    m_historyLoad.reset();

    if (!result.reader) {
        m_historyMessage = result.error;
        return;
    }

    m_history.adopt(std::move(result.reader), path);
    m_historyMessage.clear();

    // The cached spectrum belongs to the reader that has just been replaced,
    // so it goes whatever brought the new one in. The playhead only goes when
    // this is a different file: a session reloaded to pick up its tail should
    // come back where it was.
    m_historySpectrum.clear();
    if (!reload) {
        m_historySelection.reset();
        m_historyPlaying = false;
    }
}

void MainWindow::updateHistoryTitle() {
    std::string title = std::format("{} History", productName());

    if (m_history.isOpen()) {
        const session::SessionSummary& summary = m_history.reader()->summary();
        title = std::format(
            "{} History ({}) {} {} lines", productName(), m_history.path().filename().string(),
            formatDuration(summary.durationSeconds()), groupedCount(summary.totalLines));
    } else if (m_historyLoad) {
        title = std::format("{} History (opening {})", productName(),
                            m_historyLoad->path.filename().string());
    }

    setWindowTitle(std::move(title));
}

void MainWindow::drawHistoryToolbar() {
    const ChromeTheme& chrome = m_state.theme().chrome();

    // Built to the instrument's bar, not to its own taste: same background,
    // same height, same frame padding, and the same three-zone layout of
    // identity on the left, the control you reach for most in the centre, and
    // the one destructive action pinned right. The viewer used to stack a row
    // of small buttons above a separate transport row above a duplicate of the
    // status line, which is why it read as a different application.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, toImVec4(chrome.headerBackground));
    ImGui::BeginChild("##historytoolbar", ImVec2(0, bar::toolbarHeight()), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, bar::framePadding());
    ImGui::SetCursorPos(ImVec2(8.0F, bar::padding()));

    // The whole bar is held while a file is being read. Not only the controls
    // that would start a second read: closing or seeking a view that is about
    // to be replaced by the one on its way leaves the window showing something
    // the operator did not ask for.
    ImGui::BeginDisabled(m_historyLoad.has_value());

    // The file chip, where the instrument puts the radio it is attached to.
    // Same question in both windows: what am I looking at?
    {
        const std::string caption =
            m_history.isOpen()
                ? std::format("{}  {}##histopen", icon::glyphOr(icon::kHistory, ""),
                              m_history.path().filename().string())
                : std::format("{}  Open session##histopen", icon::glyphOr(icon::kOpen, ""));

        if (ImGui::Button(caption.c_str())) {
            promptForHistoryFile();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", m_history.isOpen() ? m_history.path().string().c_str()
                                                       : "Open a saved .sweeps file");
        }
    }

    // The file being written right now is readable: tiles are self-describing
    // and the reader recovers by scan when there is no index yet, which is the
    // same path a session ended by a power cut takes. What it will not show is
    // whatever is still buffered in the writer, so it stops short of the live
    // edge rather than being wrong about it.
    if (m_state.sessionOpen()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(chrome.record));
        if (ImGui::Button(icon::glyphOr(icon::kRecord, "Live").append("##histlive").c_str())) {
            beginHistoryOpen(m_state.sessionPath());
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Open the session being recorded right now");
        }
    }

    if (m_history.isOpen()) {
        ImGui::SameLine();
        // A session being written grows after it was opened; the reader holds a
        // snapshot, so re-reading is how the tail becomes visible.
        if (ImGui::Button(icon::glyphOr(icon::kReload, "Reload").append("##histreload").c_str())) {
            beginHistoryOpen(m_history.path(), true);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Reload the file");
        }

        drawHistoryTransport();

        bar::groupSeparator(chrome.separator);

        if (ImGui::Button(
                icon::glyphOr(icon::kResetZoom, "Reset view").append("##histreset").c_str())) {
            m_history.resetView();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Back to the start");
        }

        ImGui::SameLine();
        if (ImGui::Button(icon::glyphOr(icon::kSettings, "View").append("##histcfg").c_str())) {
            ImGui::OpenPopup("##historysettings");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Overview strip and playback rate");
        }
        drawHistorySettingsPopup();

        // Close, where the instrument keeps Start/Stop: the one control whose
        // position must never move, alone at its end of the bar.
        {
            constexpr float kCloseWidth = 92.0F;
            ImGui::SameLine(std::max(ImGui::GetWindowWidth() - kCloseWidth - bar::rightMargin(),
                                     ImGui::GetCursorPosX() + ImGui::GetStyle().ItemSpacing.x));

            ImGui::PushStyleColor(ImGuiCol_Button, toImVec4(chrome.stop));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toImVec4(chrome.stop.withAlpha(0.85F)));
            if (ImGui::Button("Close", ImVec2(kCloseWidth, 0))) {
                m_history.close();
                m_historySelection.reset();
                m_historySpectrum.clear();
            }
            ImGui::PopStyleColor(2);
        }
    }

    ImGui::EndDisabled();

    ImGui::PopStyleVar(); // bar::framePadding()
    ImGui::EndChild();
}

void MainWindow::drawHistorySettingsPopup() {
    if (!ImGui::BeginPopup("##historysettings")) {
        return;
    }

    ImGui::SeparatorText("Overview strip");

    auto bandKhz = static_cast<float>(m_historyBandHz / 1e3);
    ImGui::SetNextItemWidth(180.0F);
    if (ImGui::SliderFloat("Band width", &bandKhz, 0.0F, 200000.0F,
                           bandKhz <= 0.0F ? "whole span" : "%.0f kHz",
                           ImGuiSliderFlags_Logarithmic)) {
        m_historyBandHz = static_cast<double>(bandKhz) * 1e3;
    }
    helpMarker("Spectrum shown either side of the marker.");

    ImGui::SeparatorText("Playback");
    ImGui::SetNextItemWidth(180.0F);
    ImGui::SliderFloat("Rate", &m_historySpeed, 0.1F, 16.0F, "%.2fx", ImGuiSliderFlags_Logarithmic);

    ImGui::EndPopup();
}

void MainWindow::advanceHistoryPlayback() {
    if (!m_historyPlaying || !m_historySelection) {
        m_historyLastTickNs = monotonicNs();
        return;
    }

    const std::uint64_t now = monotonicNs();
    const auto elapsed = static_cast<double>(now - m_historyLastTickNs);
    m_historyLastTickNs = now;

    // Advanced against the wall clock rather than per frame, so the playback
    // rate is the one asked for whatever the frame rate happens to be.
    const auto step = static_cast<std::uint64_t>(elapsed * static_cast<double>(m_historySpeed));
    const std::uint64_t last = m_history.reader()->summary().lastLineNs;

    if (m_historySelection->monotonicNs + step >= last) {
        m_historySelection->monotonicNs = last;
        m_historyPlaying = false;
    } else {
        m_historySelection->monotonicNs += step;
    }
    m_historySpectrum.clear();

    // The window follows the playhead once it reaches the lower third, rather
    // than recentring every frame -- a view that slides continuously is far
    // harder to read than one that scrolls a screenful at a time.
    const std::uint64_t from = m_history.viewFromNs();
    const std::uint64_t to = m_history.viewToNs();
    if (m_historySelection->monotonicNs > from + (to - from) * 2 / 3) {
        const std::uint64_t span = to - from;
        const std::uint64_t start = m_historySelection->monotonicNs - span / 3;
        m_history.setTimeRange(start, start + span);
    }
}

void MainWindow::seekHistory(std::uint64_t monotonicNs) {
    // Seeking moves the playhead in time and nothing else. The frequency being
    // examined belongs to the marker now, which seeking has no business
    // touching.
    m_historySelection = HistoryPick{.monotonicNs = monotonicNs};
    m_historySpectrum.clear();

    // Brought into view if the seek landed outside it.
    const std::uint64_t from = m_history.viewFromNs();
    const std::uint64_t to = m_history.viewToNs();
    if (monotonicNs < from || monotonicNs > to) {
        const std::uint64_t span = to - from;
        const std::uint64_t start = monotonicNs > span / 2 ? monotonicNs - span / 2 : 0;
        m_history.setTimeRange(start, start + span);
    }
}

void MainWindow::drawHistoryTransport() {
    const session::SessionSummary& summary = m_history.reader()->summary();
    const std::uint64_t span = m_history.viewToNs() - m_history.viewFromNs();
    // A step is a tenth of what is on screen, so it means the same thing at
    // every zoom level.
    const std::uint64_t step = std::max<std::uint64_t>(span / 10, 1);

    const std::uint64_t current =
        m_historySelection ? m_historySelection->monotonicNs : summary.firstLineNs;

    // Centred on the window, in the slot the instrument gives its range button:
    // the control reached for constantly belongs where the eye already is.
    //
    // Measured from the captions themselves, because the group's width depends
    // on the font and on whether the icon glyphs loaded at all -- with icons
    // these are single characters, without them they are words, and a figure
    // assuming either is wrong by a wide margin in the other case.
    //
    // Play and Pause are measured together and the wider taken, so the whole
    // group does not shuffle sideways each time playback is toggled.
    const auto buttonWidth = [](const std::string& label) {
        return ImGui::CalcTextSize(label.c_str(), nullptr, true).x +
               ImGui::GetStyle().FramePadding.x * 2.0F;
    };

    const float groupWidth = buttonWidth(icon::glyphOr(icon::kSkipStart, "|<")) +
                             buttonWidth(icon::glyphOr(icon::kRewind, "<<")) +
                             std::max(buttonWidth(icon::glyphOr(icon::kPlay, "Play")),
                                      buttonWidth(icon::glyphOr(icon::kPause, "Pause"))) +
                             buttonWidth(icon::glyphOr(icon::kForward, ">>")) +
                             buttonWidth(icon::glyphOr(icon::kSkipEnd, ">|")) + buttonWidth("-") +
                             buttonWidth("+") + ImGui::CalcTextSize("88.8x").x +
                             ImGui::GetStyle().ItemSpacing.x * 7.0F;

    ImGui::SameLine(std::max((ImGui::GetWindowWidth() - groupWidth) * 0.5F,
                             ImGui::GetCursorPosX() + bar::groupGap()));

    if (ImGui::Button(icon::glyphOr(icon::kSkipStart, "|<").append("##tstart").c_str())) {
        seekHistory(summary.firstLineNs);
    }
    ImGui::SameLine();
    if (ImGui::Button(icon::glyphOr(icon::kRewind, "<<").append("##tback").c_str())) {
        seekHistory(current > summary.firstLineNs + step ? current - step : summary.firstLineNs);
    }
    ImGui::SameLine();
    if (ImGui::Button(icon::glyphOr(m_historyPlaying ? icon::kPause : icon::kPlay,
                                    m_historyPlaying ? "Pause" : "Play")
                          .append("##tplay")
                          .c_str())) {
        m_historyPlaying = !m_historyPlaying;
        m_historyLastTickNs = monotonicNs();
        if (m_historyPlaying && !m_historySelection) {
            seekHistory(summary.firstLineNs);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(icon::glyphOr(icon::kForward, ">>").append("##tfwd").c_str())) {
        seekHistory(std::min(current + step, summary.lastLineNs));
    }
    ImGui::SameLine();
    if (ImGui::Button(icon::glyphOr(icon::kSkipEnd, ">|").append("##tend").c_str())) {
        seekHistory(summary.lastLineNs);
    }

    // Stepped rather than continuous: the useful rates are a handful of
    // recognisable multiples, and hunting for exactly 1x on a slider is a
    // worse experience than pressing a button twice.
    static constexpr std::array kRates{0.1F, 0.2F, 0.5F, 1.0F, 2.0F, 5.0F, 10.0F, 16.0F};
    const auto nearest = static_cast<std::size_t>(
        std::distance(kRates.begin(), std::ranges::min_element(kRates, {}, [this](float rate) {
                          return std::abs(rate - m_historySpeed);
                      })));

    ImGui::SameLine();
    if (ImGui::Button("-##slower") && nearest > 0) {
        m_historySpeed = kRates[nearest - 1];
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%.2gx", static_cast<double>(m_historySpeed));
    ImGui::SameLine();
    if (ImGui::Button("+##faster") && nearest + 1 < kRates.size()) {
        m_historySpeed = kRates[nearest + 1];
    }
}

void MainWindow::drawHistoryStatusBar() {
    const session::SessionSummary& summary = m_history.reader()->summary();
    const ChromeTheme& chrome = m_state.theme().chrome();

    // The instrument's status bar exactly: its background, its height, its
    // padding, and its division of labour -- a band chip and the at-a-glance
    // state on the left, fixed-width readouts on the right.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, toImVec4(chrome.headerBackground));
    ImGui::BeginChild("##historystatus", ImVec2(0, bar::statusBarHeight()), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, bar::statusFramePadding());
    ImGui::SetCursorPos(ImVec2(10.0F, bar::padding()));

    // The same chip the instrument's bar draws, asking the same question: what
    // is at this frequency? The frequency comes from whichever cursor is
    // aiming the overview strip, so the chip and the strip always agree.
    //
    // One implementation rather than a second copy here -- two bars asking the
    // same question must not be able to give different answers, and this one
    // gains the hover list the hand-written version never had.
    {
        const Marker* selected = m_state.markers().active();
        const double queryHz = selected != nullptr && selected->visible
                                   ? selected->frequencyHz
                                   : (summary.lowestHz + summary.highestHz) * 0.5;
        drawContributionChip(queryHz, chrome);
    }

    ImGui::SameLine(0.0F, 24.0F);
    ImGui::AlignTextToFramePadding();

    // The playhead readout gets a box wide enough for the longest position the
    // session can reach, and the chip is placed at the end of that box rather
    // than after the text. Laid out by text width instead, the chip stepped
    // sideways on every frame of playback -- the same reason the readouts on
    // the right are in reserved boxes.
    const double sessionSeconds = summary.durationSeconds();
    const std::string total = formatPlayhead(sessionSeconds, sessionSeconds);
    const float timeBoxX = ImGui::GetCursorPosX();
    const float timeBoxWidth =
        ImGui::CalcTextSize(bar::digitMask(std::format("{}  into  {}", total, total)).c_str()).x;

    if (m_historySelection) {
        const double into =
            static_cast<double>(static_cast<std::int64_t>(m_historySelection->monotonicNs) -
                                static_cast<std::int64_t>(summary.firstLineNs)) *
            1e-9;
        ImGui::TextDisabled("%s  into  %s", formatPlayhead(into, sessionSeconds).c_str(),
                            total.c_str());
    } else {
        ImGui::TextDisabled("Click the strip to place the playhead");
    }

    if (m_historyPlaying) {
        ImGui::SameLine(std::max(timeBoxX + timeBoxWidth + 16.0F,
                                 ImGui::GetCursorPosX() + ImGui::GetStyle().ItemSpacing.x));
        bar::statusChip(std::format("playing {:.2g}x", static_cast<double>(m_historySpeed)).c_str(),
                        chrome.ok);
    }

    // Readouts in reserved boxes, so a digit appearing in one does not shove
    // the rest sideways -- the instrument's rule, for the same reason.
    //
    // Each says what it means on hover. "LOD 1", "3 seg" and "indexed" are
    // exact and unreadable: they name the file format's own machinery, which
    // is not what an operator brings to the window. The numbers stay short --
    // that is what a status bar is for -- and the sentence is one hover away.
    const Color scanColor = summary.recoveredByScan ? chrome.warning : chrome.ok;
    std::vector<bar::Metric> metrics{
        {std::format("{} lines", groupedCount(summary.totalLines)), "8,888,888 lines", nullptr,
         "Sweep lines in the recording"},
        {std::format("{} segments", m_history.reader()->segments().size()), "888 segments", nullptr,
         "A new segment starts wherever the range or resolution changed"},
        {detailLabel(m_history.lastLod()), "1:64 detail", nullptr,
         "Detail the waterfall is drawn at. Zoom in for full detail."},
        {std::format("strip {}", toml_util::formatFrequencyShort(
                                     m_historyBandHz > 0.0 ? m_historyBandHz
                                                           : summary.highestHz - summary.lowestHz)),
         "strip 8888.8 MHz", nullptr, "Overview strip width around the marker"},
        {summary.recoveredByScan ? "Rebuilt by scan" : "Indexed", "Rebuilt by scan", &scanColor,
         summary.recoveredByScan ? "The index was missing and was rebuilt; nothing is lost"
                                 : "Index read from the file"},
    };

    bar::drawMetrics(metrics, chrome.separator);

    ImGui::PopStyleVar(); // bar::statusFramePadding()
    ImGui::EndChild();
}

void MainWindow::drawHistoryStripSplitter() {
    ImGui::InvisibleButton("##historystripsplit",
                           ImVec2(ImGui::GetContentRegionAvail().x, bar::splitterThickness()));

    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    if (ImGui::IsItemActive()) {
        // Upwards makes it taller, which is the direction the edge moves.
        m_historyOverviewHeight =
            std::clamp(m_historyOverviewHeight - ImGui::GetIO().MouseDelta.y, 28.0F, 400.0F);
    }

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float middle = (min.y + max.y) * 0.5F;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, middle), ImVec2(max.x, middle),
                                        packed(ImGui::IsItemActive() || ImGui::IsItemHovered()
                                                   ? m_state.theme().chrome().accent
                                                   : m_state.theme().chrome().border),
                                        1.0F);
}

void MainWindow::drawHistoryOverview(float height) {
    const session::SessionSummary& summary = m_history.reader()->summary();

    const double fullSpan = std::max(summary.highestHz - summary.lowestHz, 1.0);

    // Aimed by the selected marker, the same cursor the instrument uses.
    //
    // The viewer draws the same spectrum widget, so its markers are live here,
    // and there is no second notion of "the frequency I am looking at" for this
    // to disagree with.
    const Marker* marker = m_state.markers().active();
    if (marker != nullptr && !marker->visible) {
        marker = nullptr;
    }

    // With no marker there is no frequency worth centring on, and a band pinned
    // to the middle of the span lands on whatever happens to be there -- empty
    // sky, on a recording whose activity sits at one end. Showing all of it is
    // what lets an operator find the activity worth marking; the strip narrows
    // around the marker the moment one goes down.
    const double bandHz =
        marker != nullptr && m_historyBandHz > 0.0 ? std::min(m_historyBandHz, fullSpan) : fullSpan;

    const double centerHz =
        marker != nullptr ? marker->frequencyHz : (summary.lowestHz + summary.highestHz) * 0.5;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, height);
    // Wide enough to hold the view mark's minimum extent as well as to be worth
    // drawing: below that the mark's clamp would be handed a low bound above
    // its high one.
    if (size.x < 8.0F) {
        return;
    }

    const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    const std::uint32_t texture = m_history.overviewTexture(
        static_cast<std::uint32_t>(size.x * scale.x),
        static_cast<std::uint32_t>(std::max(height * scale.y, 8.0F)), centerHz, bandHz);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 far(origin.x + size.x, origin.y + size.y);

    draw->AddRectFilled(origin, far, packed(m_state.theme().spectrum().background));
    if (texture != 0) {
        draw->AddImage(static_cast<ImTextureID>(texture), origin, far);
    }

    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##historyoverview", size);

    const auto sessionSpan =
        static_cast<double>(m_history.overviewToNs() - m_history.overviewFromNs());
    if (sessionSpan <= 0.0) {
        return;
    }

    // The window currently on screen, so the strip doubles as a scrollbar: it
    // says both what the recording contains and which part of it is being
    // looked at.
    // Signed, so a window reaching outside the strip's extent reads as a
    // fraction below zero and clamps to the near edge, rather than wrapping
    // round to an enormous positive one and clamping to the far edge instead.
    const auto intoSession = [&](std::uint64_t instant) {
        return static_cast<float>(
            static_cast<double>(static_cast<std::int64_t>(instant) -
                                static_cast<std::int64_t>(m_history.overviewFromNs())) /
            sessionSpan);
    };

    const float windowFrom = intoSession(m_history.viewFromNs());
    const float windowTo = intoSession(m_history.viewToNs());

    // Horizontal only, and always the full height.
    //
    // The mark answers one question -- how much of the recording the waterfall
    // is showing -- and that is a question about time, which on this strip is
    // the horizontal axis alone. The vertical axis is the band around the
    // marker, an unrelated quantity, so boxing the mark into it made the mark
    // describe something nobody had asked about and left it clamped flat
    // against an edge whenever the view was wider than the band.
    ImVec2 markMin(origin.x + size.x * std::clamp(windowFrom, 0.0F, 1.0F), origin.y);
    ImVec2 markMax(origin.x + size.x * std::clamp(windowTo, 0.0F, 1.0F), far.y);

    // Kept visible when the window is a sliver of the session. Collapsed to a
    // hairline the mark reads as a stray rule across the strip rather than as
    // the thing that says where you are.
    constexpr float kMinExtent = 3.0F;
    if (markMax.x - markMin.x < kMinExtent) {
        const float middle = (markMin.x + markMax.x) * 0.5F;
        markMin.x = std::clamp(middle - kMinExtent * 0.5F, origin.x, far.x - kMinExtent);
        markMax.x = markMin.x + kMinExtent;
    }

    // The spectrum's trace colour, not a red of its own.
    //
    // Red was chosen to stand apart from measured data and does the opposite:
    // it is the top of most gradients, so on a busy strip the mark disappeared
    // into whatever it was drawn over. The trace colour is the one hue in this
    // window that never comes from a colormap, which makes it the one that
    // always reads as an annotation -- and it already means "the thing you are
    // looking at" everywhere else on screen.
    const Color windowColor = m_state.theme().spectrum().traceLive;
    draw->AddRectFilled(markMin, markMax, packed(windowColor.withAlpha(0.14F)));
    draw->AddRect(markMin, markMax, packed(windowColor), 0.0F, 2.0F);

    if (m_historySelection) {
        const float playhead = intoSession(m_historySelection->monotonicNs);
        if (playhead >= 0.0F && playhead <= 1.0F) {
            const float x = origin.x + size.x * playhead;
            draw->AddLine(ImVec2(x, origin.y), ImVec2(x, far.y),
                          packed(m_state.theme().chrome().warning), 1.5F);
        }
    }

    // Click or drag anywhere to go there. This is the only surface that moves
    // the recording in time -- the panes above it behave exactly like the live
    // ones, where a drag means frequency.
    if (ImGui::IsItemActive()) {
        const auto fraction = static_cast<double>(std::clamp(
            (ImGui::GetIO().MousePos.x - origin.x) / std::max(size.x, 1.0F), 0.0F, 1.0F));
        seekHistory(m_history.overviewFromNs() +
                    static_cast<std::uint64_t>(sessionSpan * fraction));
    }

    // Wheel scrolls through the recording, since this is the surface that owns
    // time. Shift makes it a coarse jump.
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0F) {
        const std::uint64_t span = m_history.viewToNs() - m_history.viewFromNs();
        const auto stride = static_cast<double>(span) * (ImGui::GetIO().KeyShift ? 1.0 : 0.25) *
                            static_cast<double>(-ImGui::GetIO().MouseWheel);
        const auto current = static_cast<double>(m_history.viewFromNs());
        const double next =
            std::clamp(current + stride, static_cast<double>(m_history.overviewFromNs()),
                       static_cast<double>(m_history.overviewToNs()));
        m_history.setTimeRange(static_cast<std::uint64_t>(next),
                               static_cast<std::uint64_t>(next) + span);
    }

    if (ImGui::IsItemHovered()) {
        // Where the pointer is, not what the file contains.
        //
        // It used to report the session's total length and the strip's band --
        // both true, neither dependent on the pointer, so moving along the
        // strip changed nothing and a wide box sat over the one thing being
        // pointed at. What a hover here is asking is "what time is this", which
        // is also exactly what clicking will do.
        const auto fraction = static_cast<double>(std::clamp(
            (ImGui::GetIO().MousePos.x - origin.x) / std::max(size.x, 1.0F), 0.0F, 1.0F));
        ImGui::SetTooltip("%s", formatDuration(sessionSpan * fraction * 1e-9).c_str());
    }
}

void MainWindow::refreshHistorySpectrum() {
    if (!m_historySelection || !m_historySpectrum.empty()) {
        return;
    }

    const std::optional<std::uint32_t> segment =
        m_history.segmentAt(m_historySelection->monotonicNs);
    if (!segment) {
        return;
    }

    auto spectrum = m_history.reader()->spectrumAt(m_historySelection->monotonicNs, *segment);
    if (!spectrum || spectrum->empty()) {
        return;
    }

    m_historySpectrum = std::move(*spectrum);
    m_historySpectrumSegment = *segment;

    const session::SegmentInfo* info = nullptr;
    for (const session::SegmentInfo& candidate : m_history.reader()->segments()) {
        if (candidate.id == m_historySpectrumSegment) {
            info = &candidate;
            break;
        }
    }
    if (info == nullptr) {
        return;
    }

    // Pushed through the same trace store the live spectrum uses, so the plot
    // above is not a lookalike -- it is the same widget, with the same traces,
    // markers, band plan and gestures, fed from a file instead of a radio.
    SpectrumFrame frame;
    frame.sequence = m_historySelection->monotonicNs;
    frame.hostTimeNs = m_historySelection->monotonicNs;
    frame.wallTimeNs = m_historySelection->monotonicNs;
    frame.startHz = info->grid.startHz;
    frame.binWidthHz = info->grid.binWidthHz;
    frame.binsDbfs = m_historySpectrum;
    frame.config = info->config;

    m_state.traces().update(frame);
}

void MainWindow::drawHistoryTimeline() {
    const session::SessionReader& reader = *m_history.reader();
    const session::SessionSummary& summary = reader.summary();

    m_history.setColorMap(m_state.theme().waterfallColorMap());
    m_history.setGradientRange(m_state.view().gradientMinDb, m_state.view().gradientMaxDb);
    m_history.setPeakDetect(m_state.view().waterfallPeakDetect);

    // Bound to the session, not to whichever segment the playhead is in.
    // Segments cover different ranges, so clamping per segment snapped the zoom
    // back every time the playhead crossed a boundary.
    m_state.setViewBounds(summary.lowestHz, summary.highestHz);

    advanceHistoryPlayback();
    refreshHistorySpectrum();

    // No file summary here. Every field it carried is in the status bar, where
    // the instrument keeps the same numbers, and printing them twice on one
    // screen was most of what made this window look unrelated to that one.

    // The spectrum reads its window from AppState, which is also what the
    // shared gestures mutate -- so handing it the history's range before and
    // taking it back after is what makes wheel-zoom and shift-drag behave
    // exactly as they do on the live plot.
    m_state.setVisibleRange(m_history.viewFromHz(), m_history.viewToHz());

    // Everything below the plots is a fixed cost and all of it has to come out
    // of the budget, or the content outruns the window and it scrolls. The
    // splitter and the status bar's own padding were the two that did not, so
    // the viewer overran by about a dozen pixels at every size.
    const float overview = m_historyOverviewHeight;
    const float footer = bar::statusBarHeight();
    const float body = ImGui::GetContentRegionAvail().y - overview - footer -
                       bar::splitterThickness() - ImGui::GetStyle().ItemSpacing.y * 3.0F;

    // A scrollable child eats the wheel before the pane inside it is asked, so
    // the time gutter's scroll never saw an event. None of these panes scroll
    // -- each draws exactly into the box it is given -- so refusing the wheel
    // costs nothing and lets it reach the surface it was aimed at.
    constexpr ImGuiWindowFlags kPaneFlags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, toImVec4(m_state.theme().spectrum().background));
    ImGui::BeginChild("##historyplots", ImVec2(0, std::max(body, 120.0F)), 0, kPaneFlags);
    {
        const float available = ImGui::GetContentRegionAvail().y;
        const float spacing = ImGui::GetStyle().ItemSpacing.y;

        constexpr float kMinPaneHeight = 70.0F;
        const float budget = available - bar::splitterThickness() - spacing * 2.0F;
        const float waterfallHeight =
            std::clamp(budget * m_state.waterfallFraction(), std::min(kMinPaneHeight, budget),
                       std::max(budget - kMinPaneHeight, 1.0F));

        ImGui::BeginChild("##historyspectrumpane", ImVec2(0, budget - waterfallHeight), 0,
                          kPaneFlags);
        drawSpectrum();
        ImGui::EndChild();

        drawPaneSplitter(budget);

        ImGui::BeginChild("##historywaterfallpane", ImVec2(0, waterfallHeight), 0, kPaneFlags);
        drawHistoryWaterfall();
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    drawHistoryStripSplitter();
    drawHistoryOverview(m_historyOverviewHeight);

    double fromHz = 0.0;
    double toHz = 0.0;
    m_state.visibleRange(fromHz, toHz);
    m_history.setFrequencyRange(fromHz, toHz);

    drawHistoryStatusBar();
}

void MainWindow::drawHistoryAxes(const ImVec2& origin, const ImVec2& size, float axisWidth,
                                 float axisHeight) {
    const ChromeTheme& chrome = m_state.theme().chrome();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // Labelled against the start of the session, not the start of the view: an
    // absolute position in the recording is what an operator wants to quote,
    // and a view-relative one changes meaning every time they scroll.
    const auto sessionStart = m_history.reader()->summary().firstLineNs;

    constexpr int kTimeLabels = 8;
    const double labelStepSeconds =
        static_cast<double>(m_history.viewToNs() - m_history.viewFromNs()) * 1e-9 / kTimeLabels;

    for (int i = 0; i <= kTimeLabels; ++i) {
        const double fraction = static_cast<double>(i) / kTimeLabels;
        const float y = origin.y + size.y * static_cast<float>(fraction);
        // Subtracted as signed. These are two unrelated points on a monotonic
        // clock, and an instant before the session start is a small negative
        // number rather than the enormous positive one unsigned wraparound
        // would make of it.
        const double intoSession =
            static_cast<double>(static_cast<std::int64_t>(m_history.timeAt(fraction)) -
                                static_cast<std::int64_t>(sessionStart)) *
            1e-9;

        const std::string label = formatTimestamp(intoSession, labelStepSeconds);
        const ImVec2 extent = ImGui::CalcTextSize(label.c_str());
        // Nudged inside the pane at the ends so the first and last labels are
        // not half-clipped by the window edge.
        const float textY = std::clamp(y - extent.y * 0.5F, origin.y, origin.y + size.y - extent.y);
        draw->AddText(ImVec2(origin.x - extent.x - 6.0F, textY), packed(chrome.textDim),
                      label.c_str());

        if (i > 0 && i < kTimeLabels) {
            draw->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + size.x, y),
                          packed(Color{1.0F, 1.0F, 1.0F, 0.12F}), 1.0F);
        }
    }

    constexpr int kFreqLabels = 6;
    for (int i = 0; i <= kFreqLabels; ++i) {
        const double fraction = static_cast<double>(i) / kFreqLabels;
        const float x = origin.x + size.x * static_cast<float>(fraction);
        const std::string label = toml_util::formatFrequencyShort(m_history.frequencyAt(fraction));
        const ImVec2 extent = ImGui::CalcTextSize(label.c_str());

        float textX = x - extent.x * 0.5F;
        textX = std::clamp(textX, origin.x, origin.x + size.x - extent.x);
        draw->AddText(ImVec2(textX, origin.y + size.y + 4.0F), packed(chrome.textDim),
                      label.c_str());
    }

    (void)axisWidth;
    (void)axisHeight;
}

} // namespace sweeppp::ui
