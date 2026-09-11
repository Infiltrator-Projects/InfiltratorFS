#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:-.}"

fail() {
    echo "native kernel repository hygiene policy: $*" >&2
    exit 1
}

tracked_generated="$(
    git -C "$root" ls-files kernel | \
    grep -E '^kernel/(Module\.symvers|modules\.order|.*\.ko|.*\.mod|.*\.mod\.c|.*\.o|.*\.cmd)$' \
    || true
)"

if [[ -n "$tracked_generated" ]]; then
    printf '%s\n' "$tracked_generated" >&2
    fail 'generated Kbuild artifacts are tracked in the repository'
fi

for pattern in '*.ko' '*.mod' '*.mod.c' '*.cmd' 'Module.symvers' 'modules.order'; do
    grep -Fxq "$pattern" "$root/.gitignore" || \
        fail ".gitignore no longer protects $pattern"
done

printf 'Native kernel repository hygiene policy guard passed.\n'
