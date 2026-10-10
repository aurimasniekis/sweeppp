// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// What's new: the changelog's newest entries, opened on its own after an update
// and on a fresh install, and from the menu at any time.

#include "BarChrome.hpp"
#include "FileDialog.hpp"
#include "MainWindow.hpp"

#include <algorithm>
#include <fstream>
#include <imgui_internal.h>
#include <sstream>
#include <sweeppp/core/Log.hpp>
#include <sweeppp/core/Paths.hpp>
#include <sweeppp/core/Version.hpp>

namespace sweeppp::ui {
namespace {

/// CHANGELOG.md shipped with the resources, or the repository's own beside
/// them when running from a build tree.
std::filesystem::path changelogPath() {
    const std::filesystem::path resources = Paths::instance().resourcesDir();
    for (const std::filesystem::path& candidate :
         {resources / "CHANGELOG.md", resources.parent_path() / "CHANGELOG.md"}) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

/// An image path as the changelog writes it, relative to the repository,
/// found among the installed resources.
std::filesystem::path imagePath(std::string_view image) {
    constexpr std::string_view kPrefix = "resources/";
    const std::filesystem::path resources = Paths::instance().resourcesDir();
    if (image.starts_with(kPrefix)) {
        return resources / image.substr(kPrefix.size());
    }
    return resources.parent_path() / image;
}

std::vector<ChangelogRelease> readChangelog() {
    const std::filesystem::path path = changelogPath();
    if (path.empty()) {
        logWarn("ui", "no CHANGELOG.md beside the resources");
        return {};
    }
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return parseChangelog(text.str());
}

/// A bold lead -- "**Several panels at once.** Up to nine..." -- on a line of
/// its own, and the rest wrapped beneath it. Bold anywhere else reads as text.
void drawRich(std::string_view text) {
    const std::vector<std::string_view> runs = emphasisRuns(text);
    std::size_t first = 0;
    if (runs.size() >= 3 && runs[0].empty()) {
        // No bold face is loaded, so the lead is drawn twice a pixel apart:
        // set apart from the text by weight, not by a colour that reads as a link.
        const std::string_view lead = runs[1];
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float wrap =
            ImGui::CalcWrapWidthForPos(at, ImGui::GetCurrentWindow()->DC.TextWrapPos);
        ImGui::TextWrapped("%.*s", static_cast<int>(lead.size()), lead.data());
        ImGui::GetWindowDrawList()->AddText(
            ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(at.x + 1.0F, at.y),
            ImGui::GetColorU32(ImGuiCol_Text), lead.data(), lead.data() + lead.size(), wrap);
        first = 2;
    }
    std::string rest;
    for (std::size_t i = first; i < runs.size(); ++i) {
        rest += runs[i];
    }
    const std::size_t start = rest.find_first_not_of(' ');
    if (start != std::string::npos) {
        ImGui::TextWrapped("%s", rest.c_str() + start);
    }
}

} // namespace

void MainWindow::offerWhatsNew() {
    if (m_whatsNewSettled) {
        return;
    }
    // Not over the radio still opening, nor over the chooser a first run opens
    // by itself: that is the thing to answer first, and a modal on top of it
    // would close it.
    if (m_state.deviceStartupRunning() ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        return;
    }
    m_whatsNewSettled = true;

    AppSettings& settings = m_state.appSettings();
    if (settings.whatsNewSeen == versionString()) {
        return;
    }
    openWhatsNew(true);
    if (!m_showWhatsNew) {
        // Nothing to say for this version: noted, so it is not looked for again.
        settings.whatsNewSeen = std::string(versionString());
        (void)m_state.saveAppSettings();
    }
}

void MainWindow::openWhatsNew(bool sinceSeen) {
    m_changelog = readChangelog();
    m_whatsNew.clear();
    if (sinceSeen) {
        m_whatsNew =
            releasesSince(m_changelog, m_state.appSettings().whatsNewSeen, versionString());
    } else {
        // From the menu: everything, and on a nightly what it carries beyond
        // the last release too.
        for (const ChangelogRelease& release : m_changelog) {
            if (!release.unreleased() || channel() == "nightly") {
                m_whatsNew.push_back(&release);
            }
        }
    }
    m_showWhatsNew = !m_whatsNew.empty();
}

void MainWindow::drawWhatsNew() {
    constexpr const char* kId = "What's new";

    const ImGuiViewport* main = ImGui::GetMainViewport();
    ImGui::OpenPopup(kId);
    ImGui::SetNextWindowViewport(main->ID);
    ImGui::SetNextWindowPos(main->GetCenter(), ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(ImVec2(std::min(bar::scaled(760.0F), main->WorkSize.x * 0.92F),
                                    std::min(bar::scaled(820.0F), main->WorkSize.y * 0.88F)),
                             ImGuiCond_Always);

    const auto gap = [](float points) { ImGui::Dummy(ImVec2(0.0F, bar::scaled(points))); };

    // The notes and the footer pad themselves, so the scroll bar can sit on the
    // window's edge rather than inside a margin.
    bool open = true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    const bool visible = ImGui::BeginPopupModal(
        kId, &open,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!visible) {
        // The title bar's X closes the popup itself, and reports it only here.
        if (!open) {
            closeWhatsNew();
        }
        return;
    }

    const ImVec2 footerPadding = bar::scaled(ImVec2(20.0F, 12.0F));
    const float footer = ImGui::GetFrameHeight() + (footerPadding.y * 2.0F);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, bar::scaled(ImVec2(20.0F, 16.0F)));
    const bool notes = ImGui::BeginChild(
        "##notes", ImVec2(0.0F, -(footer + (ImGui::GetStyle().ItemSpacing.y * 2.0F) + 1.0F)),
        ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    if (notes) {
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        const float fontSize = ImGui::GetFontSize();
        for (std::size_t r = 0; r < m_whatsNew.size(); ++r) {
            const ChangelogRelease& release = *m_whatsNew[r];
            if (r > 0) {
                gap(16.0F);
                ImGui::Separator();
                gap(16.0F);
            }

            ImGui::PushFont(nullptr, fontSize * 1.4F);
            ImGui::TextUnformatted(
                release.unreleased()
                    ? "Unreleased"
                    : std::format("{} {}", productName(), release.version).c_str());
            ImGui::PopFont();
            if (!release.date.empty()) {
                ImGui::SameLine(0.0F, bar::scaled(10.0F));
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", release.date.c_str());
            }
            gap(4.0F);

            for (const ChangelogBlock& block : release.blocks) {
                switch (block.kind) {
                case ChangelogBlock::Kind::Heading:
                    gap(10.0F);
                    ImGui::PushFont(nullptr, fontSize * 1.15F);
                    ImGui::TextUnformatted(block.text.c_str());
                    ImGui::PopFont();
                    gap(2.0F);
                    break;
                case ChangelogBlock::Kind::Paragraph:
                    drawRich(block.text);
                    gap(6.0F);
                    break;
                case ChangelogBlock::Kind::Bullet:
                    ImGui::Indent(bar::scaled(4.0F));
                    ImGui::Bullet();
                    ImGui::SameLine(0.0F, bar::scaled(8.0F));
                    ImGui::BeginGroup();
                    drawRich(block.text);
                    ImGui::EndGroup();
                    ImGui::Unindent(bar::scaled(4.0F));
                    gap(6.0F);
                    break;
                case ChangelogBlock::Kind::Image: {
                    auto& texture = m_whatsNewImages[block.image];
                    if (!texture) {
                        texture = ImageTexture::load(imagePath(block.image));
                    }
                    if (!texture) {
                        break;
                    }
                    // As wide as the notes, never wider than the picture.
                    const float imageWidth = std::min(width, static_cast<float>(texture->width()));
                    const float height = imageWidth * static_cast<float>(texture->height()) /
                                         static_cast<float>(texture->width());
                    gap(8.0F);
                    ImGui::Image(static_cast<ImTextureID>(texture->id()),
                                 ImVec2(imageWidth, height));
                    if (!block.text.empty()) {
                        ImGui::TextDisabled("%s", block.text.c_str());
                    }
                    gap(8.0F);
                    break;
                }
                }
            }
        }
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();

    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, footerPadding);
    ImGui::BeginChild("##footer", ImVec2(0.0F, footer), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
#ifdef SWEEPPP_RELEASES_PAGE
    if (ImGui::Button("All releases")) {
        (void)openInBrowser(SWEEPPP_RELEASES_PAGE);
    }
    ImGui::SameLine();
#endif
    const float closeWidth = bar::scaled(110.0F);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - closeWidth);
    if (ImGui::Button("Close", ImVec2(closeWidth, 0.0F)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        open = false;
    }
    ImGui::EndChild();

    if (!open) {
        ImGui::CloseCurrentPopup();
        closeWhatsNew();
    }
    ImGui::EndPopup();
}

void MainWindow::closeWhatsNew() {
    m_showWhatsNew = false;
    m_whatsNew.clear();
    m_whatsNewImages.clear();
    AppSettings& settings = m_state.appSettings();
    settings.whatsNewSeen = std::string(versionString());
    (void)m_state.saveAppSettings();
}

} // namespace sweeppp::ui
