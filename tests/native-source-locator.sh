#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Shared source-ownership discovery for native-kernel policy tests.
#
# Policy tests should validate behaviour and ownership, not pin an implementation
# to a temporary filename.  This helper locates the unique .c/.inc owner of a
# definition marker so moving a qualified function into a compiled component
# does not require rewriting unrelated tests.

native_source_candidates() {
    local kernel="$1"
    find "$kernel" -maxdepth 1 -type f \
        \( -name '*.c' -o -name '*.inc' \) -print0
}

native_unique_source_fixed() {
    local kernel="$1"
    local marker="$2"
    local file
    local -a matches=()

    while IFS= read -r -d '' file; do
        if grep -Fq -- "$marker" "$file"; then
            matches+=("$file")
        fi
    done < <(native_source_candidates "$kernel")

    if (( ${#matches[@]} != 1 )); then
        printf 'native source locator: expected one owner for fixed marker %q, found %d\n' \
            "$marker" "${#matches[@]}" >&2
        printf '  %s\n' "${matches[@]}" >&2
        return 1
    fi
    printf '%s\n' "${matches[0]}"
}

native_unique_source_regex() {
    local kernel="$1"
    local regex="$2"
    local file
    local -a matches=()

    while IFS= read -r -d '' file; do
        if grep -Eq -- "$regex" "$file"; then
            matches+=("$file")
        fi
    done < <(native_source_candidates "$kernel")

    if (( ${#matches[@]} != 1 )); then
        printf 'native source locator: expected one owner for regex %q, found %d\n' \
            "$regex" "${#matches[@]}" >&2
        printf '  %s\n' "${matches[@]}" >&2
        return 1
    fi
    printf '%s\n' "${matches[0]}"
}
