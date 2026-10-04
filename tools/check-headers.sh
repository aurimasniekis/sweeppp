#!/usr/bin/env bash
# Verify every source file carries the SPDX header for the licence zone it is in.
#
# Two zones, and the boundary is the point: lib/libsweepsfile/ is MIT and
# everything else is GPL-3.0-or-later. A file that drifts across that boundary
# with the wrong tag is the failure this catches -- see THIRD_PARTY.md.
#
#   tools/check-headers.sh         report files that are missing or mistagged
#   tools/check-headers.sh --fix   prepend the correct header where it is absent
#
# Only the licence identifier is verified, never the copyright line: a file
# contributed by someone else, under their own name, passes. What fails is a
# file whose licence does not match the zone it sits in.
#
# Written for bash 3.2, which is what macOS ships: no mapfile.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MIT_ZONE="lib/libsweepsfile"
VENDOR="lib/libsweepsfile/include/sweeps/vendor"

fix=0
case "${1:-}" in
    --fix) fix=1 ;;
    "") ;;
    *)
        echo "usage: tools/check-headers.sh [--fix]" >&2
        exit 2
        ;;
esac

# Whoever runs --fix is the one who gets named. Resolved from git rather than
# baked in, so a contributor stamps themselves and not the repository owner;
# SPDX_COPYRIGHT overrides it for the case where git is not the source of truth.
COPYRIGHT=""
if [ "$fix" -eq 1 ]; then
    if [ -n "${SPDX_COPYRIGHT:-}" ]; then
        COPYRIGHT="$SPDX_COPYRIGHT"
    else
        name="$(git config user.name || true)"
        email="$(git config user.email || true)"
        if [ -z "$name" ] || [ -z "$email" ]; then
            echo "error: --fix needs an author, and git config has none." >&2
            echo "       set git config user.name and user.email," >&2
            echo "       or pass SPDX_COPYRIGHT='2027 Name <mail@example.org>'" >&2
            exit 1
        fi
        COPYRIGHT="$(date +%Y) $name <$email>"
    fi
fi

files=()
while IFS= read -r f; do
    files+=("$f")
done < <(
    find . -type f \
        \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.c' -o -name '*.py' \
        -o -name '*.ts' -o -name '*.tsx' -o -name '*.js' -o -name '*.css' -o -name '*.html' \) \
        -not -path './.git/*' \
        -not -path './build/*' \
        -not -path '*/node_modules/*' \
        -not -path './web/dist/*' \
        -not -path './site/*' \
        -not -path '*/libsweepsfile/build/*' |
        sed 's|^\./||' | sort
)

missing=0
mistagged=0
fixed=0

for f in "${files[@]}"; do
    case "$f" in
        "$VENDOR"/*) want="CC0-1.0" ;;
        "$MIT_ZONE"/*) want="MIT" ;;
        *) want="GPL-3.0-or-later" ;;
    esac

    # The tag after whichever comment opener the language uses: //, #, /* or
    # <!--, with any closer after it dropped.
    have="$(sed -n 's|^[/#*<!-]*[[:space:]]*SPDX-License-Identifier:[[:space:]]*\([^[:space:]]*\).*|\1|p' "$f" | head -1)"

    if [ -n "$have" ]; then
        if [ "$have" != "$want" ]; then
            echo "mistagged: $f -- has '$have', zone requires '$want'"
            mistagged=$((mistagged + 1))
        fi
        continue
    fi

    if [ "$fix" -eq 1 ]; then
        e=""
        case "$f" in
            *.py) c="#" ;;
            *.css) c="/*" e=" */" ;;
            *.html) c="<!--" e=" -->" ;;
            *) c="//" ;;
        esac
        tmp="$(mktemp)"
        # Vendored code keeps its own provenance: tag the licence, claim nothing.
        if [ "$want" = "CC0-1.0" ]; then
            printf '%s SPDX-License-Identifier: %s%s\n\n' "$c" "$want" "$e" > "$tmp"
        else
            printf '%s SPDX-FileCopyrightText: %s%s\n%s SPDX-License-Identifier: %s%s\n\n' \
                "$c" "$COPYRIGHT" "$e" "$c" "$want" "$e" > "$tmp"
        fi
        cat "$f" >> "$tmp"
        mv "$tmp" "$f"
        fixed=$((fixed + 1))
    else
        echo "missing:   $f -- needs '$want'"
        missing=$((missing + 1))
    fi
done

if [ "$fix" -eq 1 ]; then
    echo "added $fixed header(s) across ${#files[@]} files"
    if [ "$mistagged" -gt 0 ]; then
        echo "$mistagged file(s) carry the wrong licence for their zone and were left alone" >&2
        exit 1
    fi
    exit 0
fi

if [ "$missing" -eq 0 ] && [ "$mistagged" -eq 0 ]; then
    echo "SPDX OK (${#files[@]} files)"
    exit 0
fi

echo >&2
echo "$missing missing, $mistagged mistagged -- run tools/check-headers.sh --fix" >&2
exit 1
