#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root="${1:-.}"
test -f "$root/tests/dependency-identity-policy.cpp"
test ! -e "$root/tests/dependency-identity-policy.py"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/infilfs-dependency-policy.XXXXXXXX")"
trap 'rm -rf -- "$scratch"' EXIT
c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
    "$root/tests/dependency-identity-policy.cpp" -o "$scratch/dependency-identity-policy"
"$scratch/dependency-identity-policy" "$root"
