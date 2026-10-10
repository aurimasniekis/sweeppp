// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/ui/Changelog.hpp"

#include "sweeppp/core/Version.hpp"

#include <algorithm>

namespace sweeppp::ui {
namespace {

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

/// Inline Markdown reduced to what a window can show: a link keeps its text,
/// code keeps its letters, and `**bold**` stays for the drawing to read.
std::string plain(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '`') {
            continue;
        }
        if (c == '[') {
            const auto close = text.find("](", i);
            const auto end = close == std::string_view::npos ? close : text.find(')', close);
            if (end != std::string_view::npos) {
                out += text.substr(i + 1, close - i - 1);
                i = end;
                continue;
            }
        }
        out += c;
    }
    return out;
}

ChangelogRelease releaseFrom(std::string_view heading) {
    ChangelogRelease release;
    std::string_view rest = trim(heading);
    if (rest.starts_with('[')) {
        const auto close = rest.find(']');
        release.version = std::string(
            rest.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1));
        rest = close == std::string_view::npos ? std::string_view{} : rest.substr(close + 1);
    } else {
        const auto space = rest.find(' ');
        release.version = std::string(rest.substr(0, space));
        rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space);
    }
    rest = trim(rest);
    if (rest.starts_with('-') || rest.starts_with("—")) {
        rest = trim(rest.substr(rest.starts_with('-') ? 1 : std::string_view("—").size()));
    }
    release.date = std::string(rest);
    return release;
}

} // namespace

std::vector<ChangelogRelease> parseChangelog(std::string_view markdown) {
    std::vector<ChangelogRelease> releases;
    // Whether the next indented or plain line belongs to the last block.
    bool continuing = false;

    while (!markdown.empty()) {
        const auto newline = markdown.find('\n');
        const std::string_view line = markdown.substr(0, newline);
        markdown =
            newline == std::string_view::npos ? std::string_view{} : markdown.substr(newline + 1);
        const std::string_view text = trim(line);

        if (line.starts_with("## ")) {
            releases.push_back(releaseFrom(line.substr(3)));
            continuing = false;
            continue;
        }
        if (releases.empty()) {
            continue;
        }
        std::vector<ChangelogBlock>& blocks = releases.back().blocks;

        // A link reference definition, "[0.1.0]: https://...", is for GitHub.
        if (text.empty() || (text.starts_with('[') && text.find("]:") != std::string_view::npos)) {
            continuing = false;
            continue;
        }
        if (text.starts_with("### ")) {
            blocks.push_back({.kind = ChangelogBlock::Kind::Heading,
                              .text = plain(trim(text.substr(4))),
                              .image = {}});
            continuing = false;
            continue;
        }
        if (text.starts_with("![")) {
            const auto close = text.find("](");
            const auto end = close == std::string_view::npos ? close : text.find(')', close);
            if (end != std::string_view::npos) {
                blocks.push_back({.kind = ChangelogBlock::Kind::Image,
                                  .text = std::string(text.substr(2, close - 2)),
                                  .image = std::string(text.substr(close + 2, end - close - 2))});
                continuing = false;
                continue;
            }
        }
        if (line.starts_with("- ") || line.starts_with("* ")) {
            blocks.push_back({.kind = ChangelogBlock::Kind::Bullet,
                              .text = plain(trim(line.substr(2))),
                              .image = {}});
            continuing = true;
            continue;
        }
        if (continuing && !blocks.empty()) {
            blocks.back().text += ' ';
            blocks.back().text += plain(text);
            continue;
        }
        blocks.push_back(
            {.kind = ChangelogBlock::Kind::Paragraph, .text = plain(text), .image = {}});
        continuing = true;
    }
    return releases;
}

std::vector<const ChangelogRelease*> releasesSince(std::span<const ChangelogRelease> releases,
                                                   std::string_view seen,
                                                   std::string_view current) {
    std::vector<const ChangelogRelease*> out;
    for (const ChangelogRelease& release : releases) {
        if (release.unreleased() || compareVersions(release.version, current) > 0) {
            continue;
        }
        if (seen.empty() || compareVersions(release.version, seen) > 0) {
            out.push_back(&release);
        }
    }
    std::ranges::stable_sort(out, [](const ChangelogRelease* a, const ChangelogRelease* b) {
        return compareVersions(a->version, b->version) > 0;
    });
    if (seen.empty() && out.size() > 1) {
        out.resize(1);
    }
    return out;
}

std::vector<std::string_view> emphasisRuns(std::string_view text) {
    std::vector<std::string_view> runs;
    while (true) {
        const auto marker = text.find("**");
        runs.push_back(text.substr(0, marker));
        if (marker == std::string_view::npos) {
            break;
        }
        text = text.substr(marker + 2);
    }
    return runs;
}

} // namespace sweeppp::ui
