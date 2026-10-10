// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sweeppp::ui {

/// One piece of a release's notes, in reading order.
struct ChangelogBlock {
    enum class Kind : std::uint8_t { Heading, Paragraph, Bullet, Image };
    Kind kind = Kind::Paragraph;
    std::string text;  ///< `**bold**` kept; the drawing decides what it means
    std::string image; ///< Repository-relative path, for an image
};

/// One `## [version] - date` section of CHANGELOG.md.
struct ChangelogRelease {
    std::string version; ///< "0.2.0", or "Unreleased"
    std::string date;
    std::vector<ChangelogBlock> blocks;

    [[nodiscard]] bool unreleased() const noexcept { return version == "Unreleased"; }
};

/// The releases in CHANGELOG.md, newest first as the file has them.
///
/// Reads the part of Markdown the file is written in: `## [version] - date`
/// starts a release, `###` a heading, `- ` a bullet (indented lines continue
/// it), `![alt](path)` on its own line an image, and anything else a
/// paragraph. What comes before the first release is the file's preamble.
[[nodiscard]] std::vector<ChangelogRelease> parseChangelog(std::string_view markdown);

/// What to show after an update: the releases newer than `seen`, up to and
/// including `current`, newest first. With nothing seen yet -- a first
/// install -- the newest release `current` includes. Unreleased never counts.
[[nodiscard]] std::vector<const ChangelogRelease*>
releasesSince(std::span<const ChangelogRelease> releases, std::string_view seen,
              std::string_view current);

/// `**bold**` spans split out, in order. Even indices are plain text, odd ones
/// were inside the markers.
[[nodiscard]] std::vector<std::string_view> emphasisRuns(std::string_view text);

} // namespace sweeppp::ui
