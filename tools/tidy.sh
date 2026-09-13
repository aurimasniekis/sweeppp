#!/usr/bin/env bash
# Run clang-tidy over the tree against build/<preset>/compile_commands.json.
#
#   tools/tidy.sh                  every translation unit
#   tools/tidy.sh lib/libsweeppp   only paths matching a substring
#   tools/tidy.sh --fix            apply the fixes clang-tidy can make itself
#
# PRESET=<name> selects the build tree the compile database comes from.
#
# Written for bash 3.2, which is what macOS ships: no mapfile, and no bare
# expansion of a possibly-empty array under `set -u`.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"
PRESET="${PRESET:-dev}"
BUILD_DIR="build/$PRESET"

if ! command -v "$CLANG_TIDY" >/dev/null 2>&1; then
    echo "error: $CLANG_TIDY not found." >&2
    echo "       macOS: brew install llvm   Debian: apt install clang-tidy" >&2
    exit 127
fi

if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
    echo "error: no compile database at $BUILD_DIR/compile_commands.json" >&2
    echo "       run: make build PRESET=$PRESET" >&2
    exit 1
fi

args=()
filter=""
for arg in "$@"; do
    case "$arg" in
        --fix)
            args+=(--fix --fix-errors)
            ;;
        -*)
            echo "usage: tools/tidy.sh [--fix] [path-substring]" >&2
            exit 2
            ;;
        *) filter="$arg" ;;
    esac
done

# Homebrew clang-tidy does not share Apple Clang's implicit SDK search path, so
# without this every standard header comes back "file not found" and the real
# diagnostics are buried. The compile database is produced by /usr/bin/c++,
# which records no -isysroot of its own.
if [ "$(uname -s)" = Darwin ] && command -v xcrun >/dev/null 2>&1; then
    args+=(--extra-arg="-isysroot$(xcrun --show-sdk-path)")
fi

# Only translation units: clang-tidy reaches headers through the TUs that
# include them, filtered by HeaderFilterRegex in .clang-tidy.
files=()
while IFS= read -r f; do
    files+=("$f")
done < <(
    find . -type f \( -name '*.cpp' -o -name '*.c' \) \
        -not -path './.git/*' \
        -not -path './build/*' \
        -not -path '*/libsweepsfile/build/*' \
        -not -path '*/vendor/*' |
        sort
)

if [ -n "$filter" ]; then
    filtered=()
    for f in "${files[@]}"; do
        case "$f" in
            *"$filter"*) filtered+=("$f") ;;
        esac
    done
    files=(${filtered[@]+"${filtered[@]}"})
fi

# Keep only what this build actually compiles. A source the database has never
# heard of -- MappedFileWindows.cpp on a Unix build, a plugin switched off at
# configure time -- is otherwise linted with guessed flags, and reports a
# missing <windows.h> rather than anything about the code.
present=()
for f in "${files[@]}"; do
    if grep -qF "$ROOT/${f#./}" "$BUILD_DIR/compile_commands.json"; then
        present+=("$f")
    fi
done
skipped=$((${#files[@]} - ${#present[@]}))
files=(${present[@]+"${present[@]}"})
if [ "$skipped" -gt 0 ]; then
    echo "skipping $skipped source(s) absent from the compile database"
fi

if [ "${#files[@]}" -eq 0 ]; then
    echo "error: no translation units matched${filter:+ for '$filter'}" >&2
    exit 1
fi

jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# --fix rewrites files, and two jobs fixing one shared header race each other.
case " ${args[*]-} " in
    *" --fix "*) jobs=1 ;;
esac

echo "clang-tidy: ${#files[@]} files, $jobs job(s), database $BUILD_DIR"

status=0
printf '%s\0' "${files[@]}" |
    xargs -0 -n1 -P "$jobs" "$CLANG_TIDY" -p "$BUILD_DIR" --quiet ${args[@]+"${args[@]}"} ||
    status=$?

if [ "$status" -ne 0 ]; then
    echo >&2
    echo "clang-tidy reported findings (exit $status)" >&2
fi
exit "$status"
