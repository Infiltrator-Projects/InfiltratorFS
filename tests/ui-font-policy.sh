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

# The Linux UI consumes Common's System/Day/Night semantic palette while
# retaining the product-local blue accent and bundled typography.
grep -Fq 'THEME_MODES = ("system", "day", "night")' "$manager"
grep -Fq 'infiltrator-design-v1.json' "$manager"
grep -Fq 'infiltratorfs-theme' "$manager"
grep -Fq 'Cycle Infiltratr Common System, Day and Night themes' "$manager"
grep -Fq 'border-left-color: {accent};' "$manager"
grep -Fq 'THEME_ACCENT = "#00adef"' "$manager"

# GTK child labels/images must inherit each button state's foreground.  A
# universal direct colour caused the night headerbar to render pale text/icons
# on the pale Common button surface, making the top bar effectively unreadable.
if sed -n '/^\* {{$/,/^}}$/p' "$manager" | grep -Fq 'color:'; then
    echo 'ui-font-policy: universal GTK foreground overrides button contrast' >&2
    exit 1
fi
grep -Fq 'button, button label, button image {{' "$manager"
grep -Fq 'headerbar button label, headerbar button image {{' "$manager"
grep -Fq 'headerbar .title, headerbar label.title {{' "$manager"
grep -Fq 'headerbar .subtitle, headerbar label.subtitle {{' "$manager"

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

# The Win32 shell resolves System/Day/Night through Common and leaves only
# the product-local filesystem UI policy in the Windows adapter.
grep -Fq '#include "infiltratr/design.h"' "$windows"
grep -Fq 'infiltratr_theme_resolve' "$windows"
grep -Fq 'INFILTRATR_THEME_SYSTEM' "$windows"
grep -Fq 'INFILTRATR_THEME_DAY' "$windows"
grep -Fq 'INFILTRATR_THEME_NIGHT' "$windows"
grep -Fq 'L"ThemeMode"' "$windows"
grep -Fq 'IDM_VIEW_THEME_SYSTEM' "$windows"
! grep -Fq 'infs_mb_rgb_map' "$windows_entry"
! grep -Fq '#define RegGetValueW' "$windows_entry"
