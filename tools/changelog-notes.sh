#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# One release's section of CHANGELOG.md, as GitHub release notes:
#
#   tools/changelog-notes.sh <version> <owner/repo> <ref>
#
# Images become absolute URLs at <ref>, since a release page has no directory
# for a relative path to start from. Fails when the version has no section, so
# a tag cannot be released without its notes.

set -euo pipefail

version="$1"
repo="$2"
ref="$3"
root="$(cd "$(dirname "$0")/.." && pwd)"

notes="$(awk -v version="$version" '
    /^## / {
        if (found) exit
        found = index($0, "## [" version "]") == 1
        next
    }
    found && !/^\[[^]]*\]: / { print }
' "$root/CHANGELOG.md")"

if [[ -z "${notes//[[:space:]]/}" ]]; then
    echo "CHANGELOG.md has no section for $version" >&2
    exit 1
fi

printf '%s\n' "$notes" |
    sed -E "s#\]\((resources/[^)]+)\)#](https://raw.githubusercontent.com/$repo/$ref/\1)#g"
