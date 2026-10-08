// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <imgui.h>
#include <string>
#include <sweeppp/plugin/PluginChrome.hpp>
#include <sweeppp/ui/ColorMap.hpp>
#include <vector>

/// The instrument's bar furniture, and the text-fitting rules its readouts
/// share.
///
/// These began as file-local constants in the main window, which is why the
/// session viewer -- built in a different translation unit -- ended up with a
/// toolbar and status bar that shared none of its measurements and read as a
/// different application. A bar is a house style, not a private detail of
/// whoever draws it first, so it lives where both can reach it. The same goes
/// for how a readout holds still while its digits change, which every one of
/// them has to do the same way.
namespace sweeppp::ui::bar {

/// The same text with every digit at its widest, for reserving a box that the
/// numbers cannot then push out of.
///
/// A box measured from its own live text is remeasured every time a digit
/// appears, so everything laid out beside it steps sideways several times a
/// second -- and a readout that will not hold still cannot be read.
[[nodiscard]] inline std::string digitMask(std::string text) {
    for (char& character : text) {
        if (character >= '0' && character <= '9') {
            character = '8';
        }
    }
    return text;
}

/// Text cut to the width available, with an ellipsis where it was cut.
///
/// Whatever draws it clips at its own edge regardless; cutting here is what
/// says the text goes on rather than leaving a half-finished word hard against
/// whatever comes next.
[[nodiscard]] inline std::string ellipsised(std::string text, float available) {
    if (available <= 0.0F || ImGui::CalcTextSize(text.c_str()).x <= available) {
        return text;
    }

    const float ellipsis = ImGui::CalcTextSize("...").x;
    while (!text.empty() && ImGui::CalcTextSize(text.c_str()).x + ellipsis > available) {
        text.pop_back();
    }

    // Never mid-codepoint: these strings carry '·' and accented letters, and
    // half a UTF-8 sequence draws as a replacement box. Dropping the whole
    // character costs at most one more than the width demanded.
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0U) == 0x80U) {
        text.pop_back();
    }
    if (!text.empty() && (static_cast<unsigned char>(text.back()) & 0x80U) != 0U) {
        text.pop_back();
    }

    return text + "...";
}

/// The interface scale every metric below is measured in.
///
/// One number, set once per frame from the Appearance setting, because the
/// alternative is what this file already had: figures in pixels that were
/// right on the display they were chosen on. ImGui's own ScaleAllSizes reaches
/// the style, but a bar that pushes its own padding, a pane divider and a
/// margin are ours, and none of them is in the style.
[[nodiscard]] inline float& scaleRef() noexcept {
    static float value = 1.0F;
    return value;
}

[[nodiscard]] inline float scale() noexcept {
    return scaleRef();
}

/// Set from the one place that knows it, before anything is drawn.
inline void setScale(float value) noexcept {
    scaleRef() = value > 0.0F ? value : 1.0F;
}

/// A figure in the points these were written in, at the scale in force.
[[nodiscard]] inline float scaled(float points) noexcept {
    return points * scale();
}

[[nodiscard]] inline ImVec2 scaled(const ImVec2& points) noexcept {
    return {points.x * scale(), points.y * scale()};
}

/// Vertical breathing room inside the toolbar and status bar, above and below.
[[nodiscard]] inline float padding() noexcept {
    return scaled(6.0F);
}

/// Height of the draggable divider between stacked panes.
///
/// Thin enough not to break the black surface the panes share, wide enough to
/// be a realistic mouse target.
[[nodiscard]] inline float splitterThickness() noexcept {
    return scaled(7.0F);
}

/// Frame padding used inside the toolbar and status bar.
///
/// Larger than the sidebar's. These are the controls reached for while watching
/// the plot rather than while reading the panel, so they get a bigger hit
/// target; the sidebar stays compact because it has to fit dozens of rows.
/// Pushed as a style var for the bar's scope, so every control in it grows
/// together instead of each needing an explicit size.
///
/// From the plugin header, for the same reason the panel metrics below come
/// from it: a plugin with a bar of its own has to be able to size it the way
/// this one is sized, and two definitions of that would drift.
[[nodiscard]] inline ImVec2 framePadding() noexcept {
    return scaled(plugin::chrome::kBarFramePadding);
}

[[nodiscard]] inline ImVec2 statusFramePadding() noexcept {
    return scaled(ImVec2(9.0F, 5.0F));
}

/// Panel metrics, from the header a plugin's own popover reads them out of.
///
/// A popover hanging off this bar looks the same whether the window or a plugin
/// opened it, which it cannot do if each has its own idea of a panel's padding.
/// The reasoning behind the numbers is at their definition.
[[nodiscard]] inline ImVec2 panelFramePadding() noexcept {
    return scaled(plugin::chrome::kPanelFramePadding);
}

[[nodiscard]] inline ImVec2 panelItemSpacing() noexcept {
    return scaled(plugin::chrome::kPanelItemSpacing);
}

[[nodiscard]] inline ImVec2 panelWindowPadding() noexcept {
    return scaled(plugin::chrome::kPanelWindowPadding);
}

/// The plugin header's guard, at this application's scale.
///
/// Not `using plugin::chrome::PanelMetrics`: that one pushes the figures as
/// written, so every popover kept its start-up size while the rest of the
/// interface grew around it.
class PanelMetrics {
public:
    PanelMetrics() {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, panelFramePadding());
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, panelItemSpacing());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, panelWindowPadding());
    }
    ~PanelMetrics() { ImGui::PopStyleVar(3); }

    PanelMetrics(const PanelMetrics&) = delete;
    PanelMetrics& operator=(const PanelMetrics&) = delete;
    PanelMetrics(PanelMetrics&&) = delete;
    PanelMetrics& operator=(PanelMetrics&&) = delete;
};

/// Gap either side of the "|" that divides groups of controls.
[[nodiscard]] inline float groupGap() noexcept {
    return scaled(16.0F);
}

/// Column reserved for a status dot, and the gap between status readouts.
[[nodiscard]] inline float dotColumn() noexcept {
    return scaled(15.0F);
}

[[nodiscard]] inline float separatorGap() noexcept {
    return scaled(18.0F);
}

/// Margin holding the right-most control off the window edge.
[[nodiscard]] inline float rightMargin() noexcept {
    return scaled(12.0F);
}

/// Bar heights derived from the font and the padding above, never hard-coded.
///
/// A fixed pixel height silently clips its own buttons the moment the font, the
/// DPI or the frame padding changes -- and all three do.
[[nodiscard]] inline float height(const ImVec2& barFramePadding) {
    return ImGui::GetFontSize() + barFramePadding.y * 2.0F + padding() * 2.0F;
}

[[nodiscard]] inline float toolbarHeight() {
    return height(framePadding());
}
[[nodiscard]] inline float statusBarHeight() {
    return height(statusFramePadding());
}

/// Framebuffer pixels per point in the window being drawn.
///
/// The viewport's own where the platform keeps one, the display's otherwise.
/// Without multi-viewports -- Wayland -- ImGui never sets the main viewport's
/// and it reads zero, which draws everything sized by it at no size at all.
[[nodiscard]] inline ImVec2 framebufferScale(const ImGuiViewport* viewport) {
    if (viewport != nullptr && viewport->FramebufferScale.x > 0.0F &&
        viewport->FramebufferScale.y > 0.0F) {
        return viewport->FramebufferScale;
    }
    return ImGui::GetIO().DisplayFramebufferScale;
}

/// Puts the next modal prompt at the top of the main window, under the
/// toolbar, every frame.
///
/// Every frame, and pinned to the main viewport: placed only as it appears,
/// a prompt is placed before its size is known, lands partly outside the
/// window, and -- with panels able to tear off into windows of their own -- is
/// given one, at the top of the screen.
inline void placePrompt() {
    const ImGuiViewport* main = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(main->ID);
    ImGui::SetNextWindowPos(ImVec2(main->WorkPos.x + (main->WorkSize.x * 0.5F),
                                   main->WorkPos.y + toolbarHeight() + scaled(12.0F)),
                            ImGuiCond_Always, ImVec2(0.5F, 0.0F));
}

[[nodiscard]] inline ImVec4 toVec4(const Color& color) {
    return {color.r, color.g, color.b, color.a};
}

/// Coloured status dot plus text, for the badges in the toolbar and status bar.
inline void statusChip(const char* text, const Color& color, const char* tooltip = nullptr) {
    const float dotRadius = scaled(4.0F);

    // Aligned to frame padding so the chip sits on the buttons' baseline rather
    // than riding above them.
    ImGui::AlignTextToFramePadding();

    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(dotColumn(), ImGui::GetTextLineHeight()));
    ImGui::SameLine(0.0F, 0.0F);
    ImGui::TextUnformatted(text);

    // The dot is centred on the text's *actual* rectangle, read back after the
    // text has been submitted.
    //
    // Centring it on GetCursorScreenPos() instead looks right and is not:
    // AlignTextToFramePadding() records a baseline offset that ImGui applies
    // when the next item is submitted, and does not move the cursor. Predicting
    // the text's position therefore misses by exactly FramePadding.y, and the
    // dot floats above the words.
    const ImVec2 textMin = ImGui::GetItemRectMin();
    const ImVec2 textMax = ImGui::GetItemRectMax();

    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(start.x + dotRadius + 1.0F, (textMin.y + textMax.y) * 0.5F), dotRadius,
        color.packed());

    if (tooltip != nullptr && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
}

/// The "|" that separates groups of controls on a bar.
inline void groupSeparator(const Color& color) {
    ImGui::SameLine(0.0F, groupGap());
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(color));
    ImGui::TextUnformatted("|");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0F, groupGap());
}

/// One readout in a status bar's right-hand group.
///
/// `widest` is the widest the field can ever get, and the box is reserved at
/// that width. Laid out any other way, a digit appearing in one number shoves
/// every number after it sideways, and reading any of them means tracking a
/// value that will not hold still.
struct Metric {
    std::string text;
    const char* widest;
    const Color* dot;

    /// What the readout means, on hover.
    ///
    /// A status bar has room for a number and a word, which is enough to be
    /// read at a glance and not enough to be understood the first time. The
    /// explanation belongs with the field rather than in a manual nobody has
    /// open while looking at the bar.
    const char* tooltip = nullptr;
};

/// Draws metrics right-aligned in reserved boxes, divided by separators, and
/// returns the x the group started at so a caller can place a message before it.
inline float metricsGroupX(const std::vector<Metric>& metrics) {
    float width = 0.0F;
    for (const Metric& metric : metrics) {
        width +=
            ImGui::CalcTextSize(metric.widest).x + (metric.dot != nullptr ? dotColumn() : 0.0F);
    }
    if (!metrics.empty()) {
        width += separatorGap() * static_cast<float>(metrics.size() - 1);
    }
    return ImGui::GetWindowWidth() - width - rightMargin();
}

inline void drawMetrics(const std::vector<Metric>& metrics, const Color& separator) {
    float x = metricsGroupX(metrics);

    for (std::size_t i = 0; i < metrics.size(); ++i) {
        const Metric& metric = metrics[i];
        const float boxWidth =
            ImGui::CalcTextSize(metric.widest).x + (metric.dot != nullptr ? dotColumn() : 0.0F);

        ImGui::SameLine(std::max(x, ImGui::GetCursorPosX() + 8.0F));

        if (metric.dot != nullptr) {
            statusChip(metric.text.c_str(), *metric.dot, metric.tooltip);
        } else {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(metric.text.c_str());
            if (metric.tooltip != nullptr && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", metric.tooltip);
            }
        }

        x += boxWidth;

        if (i + 1 < metrics.size()) {
            ImGui::SameLine(x + separatorGap() * 0.5F - ImGui::CalcTextSize("|").x * 0.5F);
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, toVec4(separator));
            ImGui::TextUnformatted("|");
            ImGui::PopStyleColor();
            x += separatorGap();
        }
    }
}

} // namespace sweeppp::ui::bar
