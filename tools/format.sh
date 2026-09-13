#!/usr/bin/env bash
# Apply .clang-format across the tree, or verify it with --check.
#
# Runs from anywhere: every path below is resolved against the repository root
# rather than the working directory.
#
# Written for bash 3.2, which is what macOS ships: no mapfile, and no bare
# expansion of a possibly-empty array under `set -u`.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
    echo "error: $CLANG_FORMAT not found." >&2
    echo "       macOS: brew install clang-format   Debian: apt install clang-format" >&2
    exit 127
fi

mode=fix
case "${1:-}" in
    --check) mode=check ;;
    --fix | "") ;;
    *)
        echo "usage: tools/format.sh [--check | --fix]" >&2
        exit 2
        ;;
esac

# The vendored expected.hpp is excluded by .clang-format-ignore, which
# clang-format applies itself; it is pruned here too so --check does not report
# a file it would then refuse to rewrite.
files=()
while IFS= read -r f; do
    files+=("$f")
done < <(
    find . -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.c' \) \
        -not -path './.git/*' \
        -not -path './build/*' \
        -not -path '*/libsweepsfile/build/*' \
        -not -path '*/vendor/*' |
        sort
)

if [ "${#files[@]}" -eq 0 ]; then
    echo "error: no sources found under $ROOT" >&2
    exit 1
fi

if [ "$mode" = check ]; then
    if ! "$CLANG_FORMAT" --dry-run --Werror --style=file "${files[@]}"; then
        echo >&2
        echo "formatting differs -- run tools/format.sh to fix" >&2
        exit 1
    fi
    echo "format OK (${#files[@]} files)"
else
    "$CLANG_FORMAT" -i --style=file "${files[@]}"
    echo "formatted ${#files[@]} files"
fi
