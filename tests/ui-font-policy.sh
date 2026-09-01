#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:?source tree required}"
archive="$root/assets/fonts/mb-corpo-fonts.tar.xz"
expected_archive="bdb6063f838a7fab22b4d6b412170640c69511df53aa3dfa9a4ea8431c9d8274"

test -f "$archive"
test "$(sha256sum "$archive" | awk '{print $1}')" = "$expected_archive"

manager="$root/tools/infiltratorfs-manager"
windows="$root/tools/windows/infiltratorfs-windows.c"
resource="$root/tools/windows/infiltratorfs-windows-fonts.rc.in"
cmake="$root/CMakeLists.txt"

for file in mb_corpo_a_cond_regular.ttf mb_corpo_s_bold.ttf mb_corpo_s_regular.ttf; do
    grep -Fq "$file" "$cmake"
done

grep -Fq '@INFILFS_FONT_A_COND_REGULAR_RC@' "$resource"
grep -Fq '@INFILFS_FONT_S_BOLD_RC@' "$resource"
grep -Fq '@INFILFS_FONT_S_REGULAR_RC@' "$resource"

grep -Fq 'FcConfigAppFontAddFile' "$manager"
grep -Fq 'MB Corpo S Title WEB' "$manager"
grep -Fq 'MB Corpo A Title Cond WEB' "$manager"
grep -Fq 'font-weight: 700' "$manager"
! grep -Fq 'font-family: monospace' "$manager"

grep -Fq 'AddFontMemResourceEx' "$windows"
grep -Fq 'RemoveFontMemResourceEx' "$windows"
grep -Fq 'MB Corpo S Title WEB' "$windows"
grep -Fq 'MB Corpo A Title Cond WEB' "$windows"
grep -Fq 'FW_BOLD' "$windows"
! grep -Fq 'L"Segoe UI"' "$windows"
! grep -Fq 'L"Consolas"' "$windows"
