#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="${1:?source tree required}"
archive="$root/assets/fonts/mb-corpo-fonts.tar.xz"
expected_archive="bdb6063f838a7fab22b4d6b412170640c69511df53aa3dfa9a4ea8431c9d8274"

test -f "$archive"
test "$(sha256sum "$archive" | awk '{print $1}')" = "$expected_archive"

manager="$root/tools/infiltratorfs-manager.c"
windows="$root/tools/windows/infiltratorfs-windows.c"
windows_entry="$root/tools/windows/infiltratorfs-windows-entry.c"
resource="$root/tools/windows/infiltratorfs-windows-fonts.rc.in"
cmake="$root/CMakeLists.txt"

for file in mb_corpo_a_cond_regular.ttf mb_corpo_s_bold.ttf mb_corpo_s_regular.ttf; do
    grep -Fq "$file" "$cmake"
done
grep -Fq 'DESTINATION share/fonts/truetype/infiltratorfs' "$cmake"
grep -Fq '@INFILFS_FONT_A_COND_REGULAR_RC@' "$resource"
grep -Fq '@INFILFS_FONT_S_BOLD_RC@' "$resource"
grep -Fq '@INFILFS_FONT_S_REGULAR_RC@' "$resource"

grep -Fq '#include <gtk/gtk.h>' "$manager"
grep -Fq '#include "infiltratr/design.h"' "$manager"
grep -Fq '#include "infiltratr/format.h"' "$manager"
grep -Fq 'infiltratr_theme_resolve' "$manager"
grep -Fq 'infiltratr_theme_mode_next' "$manager"
grep -Fq 'infiltratr_format_disk_capacity' "$manager"
grep -Fq 'MB Corpo S Title WEB' "$manager"
grep -Fq 'MB Corpo A Title Cond WEB' "$manager"
grep -Fq 'font-weight: 700' "$manager"
grep -Fq '#define ACCENT_HEX "#00adef"' "$manager"
grep -Fq 'Cycle Infiltratr Common System, Day and Night themes' "$manager"
grep -Fq 'border-left-color:' "$manager"
grep -Fq 'button, button label, button image' "$manager"
grep -Fq 'headerbar button label, headerbar button image' "$manager"
! grep -Fq 'font-family: monospace' "$manager"
! grep -Fq 'Segoe UI' "$manager"
! grep -Fq 'Sans' "$manager"
! grep -Eq 'python3|PyGObject|gi\.repository|FcConfigAppFontAddFile' "$manager"
! grep -Fq 'infiltratorfs-theme' "$manager"

grep -Fq 'gtk_about_dialog_set_logo' "$manager"
grep -Fq 'gtk_icon_theme_load_icon' "$manager"
grep -Fq '"drive-harddisk", 96' "$manager"
grep -Fq 'gtk_about_dialog_set_website_label' "$manager"
grep -Fq 'link-about-dialog' "$manager"
grep -Fq 'INFILTRATORFS · NATIVE FILESYSTEM' "$manager"

grep -Fq 'AddFontMemResourceEx' "$windows"
grep -Fq 'RemoveFontMemResourceEx' "$windows"
grep -Fq 'MB Corpo S Title WEB' "$windows"
grep -Fq 'MB Corpo A Title Cond WEB' "$windows"
grep -Fq 'FW_BOLD' "$windows"
! grep -Fq 'L"Segoe UI"' "$windows"
! grep -Fq 'L"Consolas"' "$windows"

grep -Fq '#include "infiltratr/design.h"' "$windows"
grep -Fq 'infiltratr_theme_resolve' "$windows"
grep -Fq 'INFILTRATR_THEME_SYSTEM' "$windows"
grep -Fq 'INFILTRATR_THEME_DAY' "$windows"
grep -Fq 'INFILTRATR_THEME_NIGHT' "$windows"
grep -Fq 'L"ThemeMode"' "$windows"
grep -Fq 'IDM_VIEW_THEME_SYSTEM' "$windows"
! grep -Fq 'infs_mb_rgb_map' "$windows_entry"
! grep -Fq '#define RegGetValueW' "$windows_entry"
