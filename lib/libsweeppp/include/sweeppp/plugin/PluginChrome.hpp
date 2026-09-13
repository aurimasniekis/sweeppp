// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <imgui.h>

/// The bar's furniture, for whoever draws in it.
///
/// A plugin with a toolbar button is drawing into the application's own bar,
/// and a button that sizes, anchors and tints itself by its own taste is how a
/// bar comes to read as two bars. The host's `BarChrome.hpp` takes its panel
/// metrics from here for that reason: one definition, reached by the window
/// that owns the bar and by the plugins that appear in it.
///
/// Only what crosses that boundary is here. The bar's own heights, its status
/// chips and its text fitting stay in the application -- a plugin does not lay
/// the bar out, it puts one button in it.
namespace sweeppp::plugin::chrome {

/// Frame padding for a bar -- the application's own, and any a plugin draws.
///
/// Larger than a panel's, and the reason is what each is for: a bar control is
/// aimed at while the eye is on the plot, so it gets a bigger hit target; a
/// panel row is read, and has to fit dozens of its kind.
///
/// Here rather than in the application because a plugin can now have a bar of
/// its own -- a menu bar across its window -- and one that sized itself by its
/// own taste would read as a second application's bar sitting inside this one.
/// The application's `BarChrome.hpp` takes its value from here, which is the
/// same trade it already makes in the other direction for the panel metrics
/// below: one definition, reached from both sides.
inline constexpr ImVec2 kBarFramePadding{10.0F, 7.0F};

/// Pushes the bar's metrics for a scope, restoring them on the way out.
///
/// A scope guard because the height of a window's menu bar and of its title bar
/// are both decided inside `ImGui::Begin` from whatever padding is current --
/// so it has to be pushed BEFORE the Begin, which is outside the body it
/// applies to.
class BarMetrics {
public:
    BarMetrics() { ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, kBarFramePadding); }
    ~BarMetrics() { ImGui::PopStyleVar(); }

    BarMetrics(const BarMetrics&) = delete;
    BarMetrics& operator=(const BarMetrics&) = delete;
    BarMetrics(BarMetrics&&) = delete;
    BarMetrics& operator=(BarMetrics&&) = delete;
};

/// Metrics for a panel surface -- a popover, or any window of settings rows.
///
/// Smaller than the bar's, and the difference is not cosmetic. A popover opened
/// from the toolbar is drawn while the bar's frame padding is still pushed, so
/// it inherits it: every checkbox comes out a 29px square beside a 15px label,
/// every slider carries half its height in padding, and a panel of twenty rows
/// grows by a third for nothing. A bar control is aimed at while the eye is on
/// the plot; a panel row is read. They are not the same target.
inline constexpr ImVec2 kPanelFramePadding{7.0F, 4.0F};
inline constexpr ImVec2 kPanelItemSpacing{8.0F, 5.0F};
inline constexpr ImVec2 kPanelWindowPadding{12.0F, 10.0F};

/// Panel metrics for the scope of a popover, restored on the way out.
///
/// A scope guard rather than a call at the top of the body because
/// WindowPadding is read by Begin: it has to be pushed *before* BeginPopup,
/// which is outside the body, and popped after EndPopup even when the popup
/// turned out to be closed.
class PanelMetrics {
public:
    PanelMetrics() {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, kPanelFramePadding);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, kPanelItemSpacing);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, kPanelWindowPadding);
    }
    ~PanelMetrics() { ImGui::PopStyleVar(3); }

    PanelMetrics(const PanelMetrics&) = delete;
    PanelMetrics& operator=(const PanelMetrics&) = delete;
    PanelMetrics(PanelMetrics&&) = delete;
    PanelMetrics& operator=(PanelMetrics&&) = delete;
};

/// Places the next popover with its top-left corner at `at`, kept on screen.
///
/// Width as a narrow band rather than "whatever the content asks for": every
/// panel hanging off this bar shares one layout, and sizing each to its own
/// longest label makes them read as unrelated windows that happen to open near
/// each other.
///
/// The height ceiling is what is left below the corner, because a popup taller
/// than the window cannot be scrolled back to -- and a tree of five hundred
/// allocations is exactly that tall if nothing stops it. The corner is pushed
/// back up the screen when that leaves too little to be worth opening, so a
/// popover asked for near the bottom edge is a panel rather than a sliver.
inline void anchorPopoverAt(ImVec2 at, float minWidth = 360.0F, float maxWidth = 440.0F) {
    constexpr float kMargin = 12.0F;

    /// The shortest a tree is worth opening at. Below this the panel stops
    /// being a list and becomes two rows and a scrollbar.
    constexpr float kFloor = 320.0F;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float left = viewport->WorkPos.x;
    const float top = viewport->WorkPos.y;
    const float right = left + viewport->WorkSize.x;
    const float bottom = top + viewport->WorkSize.y;

    // Clamped against the width actually asked for, not the width the content
    // turns out to want: the position is set before the popup is begun, so its
    // real size is a frame away and clamping to the ceiling is what keeps a
    // panel opened near the right edge fully on screen.
    ImVec2 pos(std::clamp(at.x, left + kMargin, std::max(right - maxWidth - kMargin, left)),
               std::clamp(at.y, top + kMargin, std::max(bottom - kFloor - kMargin, top)));

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(minWidth, 0.0F), ImVec2(maxWidth, std::max(bottom - pos.y - kMargin, kFloor)));
}

/// Hangs the next popover off the item just submitted.
inline void anchorPopoverUnderItem(float minWidth = 360.0F, float maxWidth = 440.0F) {
    anchorPopoverAt(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 4.0F), minWidth,
                    maxWidth);
}

/// Where a toolbar popover hangs from, for as long as it stays open.
///
/// Two answers, because there are two ways to ask for it and they want
/// opposite things. A right-click on the button hangs the panel under the
/// button: the operator is looking there, and a panel that appeared anywhere
/// else would have to be found. A keystroke has no button under the pointer at
/// all -- the eye is on the spectrum, which is what the operator was watching
/// when they reached for the key -- so it opens at the pointer instead, where
/// the attention already is rather than at the top of the window.
///
/// It is a member and not a local because the choice has to outlive the frame
/// that made it: the position is set on every frame the popup is open, and
/// placing it under the button on frame two would snap it back the moment it
/// appeared.
class PopoverAnchor {
public:
    /// A right-click on the item just submitted opens it under that item.
    /// Call directly after the button, in place of `OpenPopupOnItemClick`.
    void openOnItemClick(const char* popupId) {
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            m_atPointer = false;
            ImGui::OpenPopup(popupId);
        }
    }

    /// A keystroke asked for it -- the flag the facet's `open_panel` set,
    /// which this clears.
    ///
    /// The keystroke is a toggle, like the click it sits beside: it opens the
    /// popover when it is shut and shuts it when it is already open. A key
    /// that only ever opens cannot be taken back the way it was made.
    ///
    /// Closing cannot happen here -- `CloseCurrentPopup` is only valid inside
    /// the popup's own Begin/End -- so this returns "close yourself", which
    /// the caller carries into the body:
    ///
    ///     const bool shut = m_popover.takeRequest(kPopupId, m_panelRequested);
    ///     ...
    ///     if (ImGui::BeginPopup(kPopupId)) {
    ///         if (shut) { ImGui::CloseCurrentPopup(); }
    ///         drawTree();
    ///         ImGui::EndPopup();
    ///     }
    ///
    /// Call it in the window the popup is begun in: the id is hashed against
    /// whatever id stack is current, and anywhere else is a different popup.
    [[nodiscard]] bool takeRequest(const char* popupId, bool& requested) {
        if (!requested) {
            return false;
        }
        requested = false;

        if (ImGui::IsPopupOpen(popupId)) {
            return true;
        }

        // Offset off the pointer rather than under it, so the popup does not
        // open with the cursor already inside it -- which puts a row under a
        // hand that has not moved yet, and the first flick of the mouse ticks
        // something the operator never aimed at.
        m_pointer = ImVec2(ImGui::GetMousePos().x + 12.0F, ImGui::GetMousePos().y + 12.0F);
        m_atPointer = true;
        ImGui::OpenPopup(popupId);
        return false;
    }

    /// Places the popover about to be begun. After the button, before Begin,
    /// every frame -- `SetNextWindowPos` only holds for the next window.
    void place(float minWidth = 360.0F, float maxWidth = 440.0F) const {
        if (m_atPointer) {
            anchorPopoverAt(m_pointer, minWidth, maxWidth);
        } else {
            anchorPopoverUnderItem(minWidth, maxWidth);
        }
    }

private:
    ImVec2 m_pointer{};
    bool m_atPointer = false;
};

/// A bar button that is the state as well as the switch.
///
/// What makes it worth having beside a keyboard shortcut that already toggles
/// the same thing: the key says nothing back, and this says what the plot is
/// showing without the operator having to look at the plot and infer it from
/// an empty band.
///
/// So the two states are told apart four ways at once, not one -- an accent
/// wash, an accent rule under it, full-strength text and a raised face against
/// a flat one. One cue is a shade of grey away from its opposite in a dark
/// theme, at a glance, from across a bench. Four is not subtle, which is the
/// point: "are the allocations on" must be answerable without clicking
/// anything to find out.
///
/// The accent is the theme's, taken from the colour its ticks and slider grabs
/// already use, so this reads as part of the application rather than as a
/// plugin's own idea of "on".
///
/// Returns true on the frame it was clicked with the left button. The right
/// button is left alone: that is the settings gesture, and `OpenPopupOnItemClick`
/// belongs to the caller because only it knows which popup to open.
[[nodiscard]] inline bool toolbarToggle(const char* label, bool on) {
    const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    const auto wash = [accent](float alpha) { return ImVec4(accent.x, accent.y, accent.z, alpha); };

    // A wash rather than the flat accent: the label has to stay readable over
    // it in every theme, and text over a fully saturated accent is legible in
    // about half of them.
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, wash(0.34F));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, wash(0.48F));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, wash(0.62F));
    } else {
        // No face at all when off, so it does not read as a button waiting to
        // be pressed *back* -- it reads as a control that is out.
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }

    const bool clicked = ImGui::Button(label);

    ImGui::PopStyleColor(on ? 3 : 2);

    if (on) {
        // The rule sits inside the button's own rectangle rather than under
        // it: the bar is only as tall as its controls plus padding, and two
        // pixels below the frame is either the plot or nothing.
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(min.x + 3.0F, max.y - 3.0F),
                                                  ImVec2(max.x - 3.0F, max.y - 1.0F),
                                                  ImGui::GetColorU32(accent), 1.0F);
    }
    return clicked;
}

} // namespace sweeppp::plugin::chrome
