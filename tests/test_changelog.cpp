// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sweeppp/core/Version.hpp>
#include <sweeppp/ui/Changelog.hpp>

using namespace sweeppp;
using namespace sweeppp::ui;
using Kind = ChangelogBlock::Kind;

namespace {

constexpr std::string_view kSample = R"(# Changelog

Anything before the first release is preamble.

## [Unreleased]

- Not out yet.

## [0.2.0] - 2026-10-12

A paragraph that runs
over two lines.

### New

- **Shared servers.** One client controls,
  the rest watch -- see [the guide](docs/user-guide.md).
- `serve --web` serves a browser UI.

![Two clients](resources/whats-new/shared.png)

### Fixed

- The waterfall draws on Wayland.

## [0.1.0] - 2026-09-13

- The first release.

[0.1.0]: https://github.com/example/releases/tag/v0.1.0
)";

} // namespace

TEST_CASE("a changelog reads as releases, headings, bullets and images") {
    const std::vector<ChangelogRelease> releases = parseChangelog(kSample);
    REQUIRE(releases.size() == 3);

    CHECK(releases[0].unreleased());
    CHECK(releases[1].version == "0.2.0");
    CHECK(releases[1].date == "2026-10-12");
    CHECK(releases[2].version == "0.1.0");

    const std::vector<ChangelogBlock>& blocks = releases[1].blocks;
    REQUIRE(blocks.size() == 7);
    CHECK(blocks[0].kind == Kind::Paragraph);
    CHECK(blocks[0].text == "A paragraph that runs over two lines.");
    CHECK(blocks[1].kind == Kind::Heading);
    CHECK(blocks[1].text == "New");
    CHECK(blocks[2].kind == Kind::Bullet);
    // A continued bullet joins up, a link keeps its text, bold is kept.
    CHECK(blocks[2].text ==
          "**Shared servers.** One client controls, the rest watch -- see the guide.");
    CHECK(blocks[3].text == "serve --web serves a browser UI.");
    CHECK(blocks[4].kind == Kind::Image);
    CHECK(blocks[4].text == "Two clients");
    CHECK(blocks[4].image == "resources/whats-new/shared.png");
    CHECK(blocks[5].kind == Kind::Heading);
    CHECK(blocks[6].text == "The waterfall draws on Wayland.");

    // The link reference at the end is not a block.
    REQUIRE(releases[2].blocks.size() == 1);
    CHECK(releases[2].blocks[0].text == "The first release.");
}

TEST_CASE("after an update, every release since the one seen, and on a first install the newest") {
    const std::vector<ChangelogRelease> releases = parseChangelog(kSample);
    const auto versions = [](const std::vector<const ChangelogRelease*>& chosen) {
        std::vector<std::string> out;
        for (const ChangelogRelease* release : chosen) {
            out.push_back(release->version);
        }
        return out;
    };

    CHECK(versions(releasesSince(releases, "0.1.0", "0.2.0")) == std::vector<std::string>{"0.2.0"});
    CHECK(versions(releasesSince(releases, "0.0.9", "0.2.0")) ==
          std::vector<std::string>{"0.2.0", "0.1.0"});
    CHECK(releasesSince(releases, "0.2.0", "0.2.0").empty());

    // Nothing newer than what is running, and never Unreleased.
    CHECK(versions(releasesSince(releases, "0.0.9", "0.1.0")) == std::vector<std::string>{"0.1.0"});

    // First install: the release the running version belongs to.
    CHECK(versions(releasesSince(releases, "", "0.2.0")) == std::vector<std::string>{"0.2.0"});
    CHECK(versions(releasesSince(releases, "", "0.1.5")) == std::vector<std::string>{"0.1.0"});
}

TEST_CASE("bold runs alternate with plain ones") {
    const std::vector<std::string_view> runs = emphasisRuns("**Shared servers.** One controls.");
    REQUIRE(runs.size() == 3);
    CHECK(runs[0].empty());
    CHECK(runs[1] == "Shared servers.");
    CHECK(runs[2] == " One controls.");
    CHECK(emphasisRuns("plain") == std::vector<std::string_view>{"plain"});
}

TEST_CASE("the repository's own changelog reads, and has this version and every image it names") {
    const std::filesystem::path root = SWEEPPP_SOURCE_DIR;
    std::ifstream in(root / "CHANGELOG.md");
    REQUIRE(in.good());
    std::stringstream text;
    text << in.rdbuf();

    const std::vector<ChangelogRelease> releases = parseChangelog(text.str());
    REQUIRE(!releases.empty());

    bool hasThisVersion = false;
    for (const ChangelogRelease& release : releases) {
        hasThisVersion = hasThisVersion || release.version == versionString();
        CHECK_FALSE(release.blocks.empty());
        for (const ChangelogBlock& block : release.blocks) {
            if (block.kind == Kind::Image) {
                CAPTURE(block.image);
                CHECK(std::filesystem::exists(root / block.image));
            }
        }
    }
    CAPTURE(versionString());
    CHECK(hasThisVersion);
}
