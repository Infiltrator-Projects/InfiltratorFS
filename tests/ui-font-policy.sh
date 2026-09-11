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
windows_entry="$root/tools/windows/infiltratorfs-windows-entry.c"
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
! grep -Fq 'set_monospace(True)' "$manager"

# The Linux UI must not inherit the desktop theme. These are the canonical
# MBLINK/Mercedes colours used by the application presentation layer.
for colour in '#050608' '#0e1115' '#171b20' '#0d1014' '#eef1f3' '#98a1a9' '#353a40' '#00adef'; do
    grep -Fiq "$colour" "$manager"
done
! grep -Fq '@theme_' "$manager"
! grep -Fq '@borders' "$manager"


# The selected storage row must remain dark/metallic. Mercedes blue is an
# accent edge, never a full-row system selection fill.
grep -Fq 'background-color: #11161b;' "$manager"
grep -Fq 'background-image: linear-gradient(to right, #161c21, #0d1014);' "$manager"
grep -Fq 'border-left-width: 3px;' "$manager"
! grep -Fq 'rgba(0,173,239,0.14)' "$manager"

# Linux mirrors LINK's LinkAboutInfo contract even though this manager is
# Python/GTK3 rather than LINK's C shell. Keep the same rich About facts,
# explicit emblem and family style instead of falling back to a bare dialog.
grep -Fq 'class AboutInfo:' "$manager"
grep -Fq "mirror of LINK's LinkAboutInfo presentation contract" "$manager"
grep -Fq 'def show_common_about(' "$manager"
grep -Fq 'link-about-dialog' "$manager"
grep -Fq 'dialog.set_logo(logo)' "$manager"
grep -Fq 'theme.load_icon(name, 96' "$manager"
grep -Fq 'website_label="Project website"' "$manager"
grep -Fq 'INFILTRATORFS · NATIVE FILESYSTEM' "$manager"

grep -Fq 'AddFontMemResourceEx' "$windows"
grep -Fq 'RemoveFontMemResourceEx' "$windows"
grep -Fq 'MB Corpo S Title WEB' "$windows"
grep -Fq 'MB Corpo A Title Cond WEB' "$windows"
grep -Fq 'FW_BOLD' "$windows"
! grep -Fq 'L"Segoe UI"' "$windows"
! grep -Fq 'L"Consolas"' "$windows"

# The Win32 shell is compiled through the entry adapter. It must force the
# fixed dark MB palette rather than inheriting AppsUseLightTheme.
grep -Fq 'infs_mb_rgb_map' "$windows_entry"
grep -Fq 'infs_mb_rgb(5u, 6u, 8u)' "$windows_entry"
grep -Fq 'infs_mb_rgb(23u, 27u, 32u)' "$windows_entry"
grep -Fq 'infs_mb_rgb(238u, 241u, 243u)' "$windows_entry"
grep -Fq 'infs_mb_rgb(152u, 161u, 169u)' "$windows_entry"
grep -Fq 'L"AppsUseLightTheme"' "$windows_entry"
grep -Fq '#define RegGetValueW infs_mb_RegGetValueW' "$windows_entry"
