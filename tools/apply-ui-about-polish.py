#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""One-shot behaviour-preserving Linux Manager presentation cleanup."""
from pathlib import Path
import re

root = Path(__file__).resolve().parent.parent
manager_path = root / "tools" / "infiltratorfs-manager"
policy_path = root / "tests" / "ui-font-policy.sh"
manager = manager_path.read_text(encoding="utf-8")

old_selection = '''.device-list row:selected {
    background-image: linear-gradient(to right, rgba(0,173,239,0.14), rgba(0,173,239,0.035));
    border-color: #293943;
    border-left-color: #00adef;
}
.device-list row:selected .device-name { color: #eef1f3; }
.device-list row:selected .device-meta { color: #9fdcf1; }
'''
new_selection = '''.device-list row:selected,
.device-list row:selected:hover {
    background-color: #11161b;
    background-image: linear-gradient(to right, #161c21, #0d1014);
    border-color: #293943;
    border-left-width: 3px;
    border-left-color: #00adef;
}
.device-list row:selected .device-name { color: #eef1f3; }
.device-list row:selected .device-meta { color: #98a1a9; }
'''
if old_selection not in manager:
    raise SystemExit("current device selection CSS did not match expected source")
manager = manager.replace(old_selection, new_selection, 1)

about_contract = '''\n\n@dataclass(frozen=True)\nclass AboutInfo:\n    \"\"\"Python GTK3 mirror of LINK's LinkAboutInfo presentation contract.\"\"\"\n\n    product_name: str\n    subtitle: str\n    version: str\n    description: str\n    release_date: str = \"\"\n    authors: tuple[str, ...] = ()\n    copyright: str = \"\"\n    website: str = \"\"\n    license_name: str = \"\"\n    license_text: str = \"\"\n    credits: str = \"\"\n\n'''
needle = "\n\ndef discover_partitions() -> list[Target]:\n"
if "class AboutInfo:" not in manager:
    if needle not in manager:
        raise SystemExit("could not locate Target/About contract insertion point")
    manager = manager.replace(needle, about_contract + needle.lstrip("\n"), 1)

common_about = r'''

def _about_logo():
    """Return the explicit product emblem used by the shared LINK-style About UI."""
    theme = Gtk.IconTheme.get_default()
    if theme is None:
        return None
    for name in ("drive-harddisk", "drive-harddisk-symbolic", "media-floppy"):
        try:
            return theme.load_icon(name, 96, Gtk.IconLookupFlags.FORCE_SIZE)
        except GLib.Error:
            continue
    return None


def show_common_about(parent, info: AboutInfo) -> None:
    """Render the LINK About contract with the native GTK3 shell."""
    comments = [part for part in (info.subtitle, info.description) if part]
    if info.release_date:
        comments.append(f"Released: {info.release_date}")
    if info.credits:
        comments.append(f"Credits: {info.credits}")

    dialog = Gtk.AboutDialog(
        transient_for=parent,
        modal=True,
        program_name=info.product_name,
        version=info.version,
        comments="\n\n".join(comments),
        website=info.website or None,
        website_label="Project website" if info.website else None,
        copyright=info.copyright or None,
    )
    dialog.set_title(f"About {info.product_name}")
    dialog.set_destroy_with_parent(True)
    dialog.set_resizable(True)
    dialog.set_default_size(560, 520)
    dialog.set_size_request(520, 480)
    dialog.get_style_context().add_class("link-about-dialog")
    if info.authors:
        dialog.set_authors(list(info.authors))
    if info.license_text:
        dialog.set_license(info.license_text)
        dialog.set_wrap_license(True)
    elif info.license_name:
        dialog.set_license(info.license_name)
    logo = _about_logo()
    if logo is not None:
        dialog.set_logo(logo)
    dialog.run()
    dialog.destroy()

'''
css_marker = '\n\nCSS = b"""\n'
if "def show_common_about(" not in manager:
    if css_marker not in manager:
        raise SystemExit("could not locate common About insertion point")
    manager = manager.replace(css_marker, common_about + css_marker, 1)

about_css = '''
.link-about-dialog {
    background-color: #050608;
    color: #eef1f3;
}
.link-about-dialog label {
    color: #eef1f3;
}
.link-about-dialog image {
    margin-top: 12px;
    margin-bottom: 8px;
}
'''
if ".link-about-dialog {" not in manager:
    css_end = '\n"""\n\n\nclass Manager(Gtk.ApplicationWindow):'
    if css_end not in manager:
        raise SystemExit("could not locate CSS terminator")
    manager = manager.replace(css_end, "\n" + about_css + '"""\n\n\nclass Manager(Gtk.ApplicationWindow):', 1)

pattern = re.compile(
    r"\n    def about\(self, \*_\):\n"
    r"        dialog = Gtk\.AboutDialog\(.*?\n"
    r"        dialog\.run\(\)\n"
    r"        dialog\.destroy\(\)\n",
    re.S,
)
replacement = '''
    def about(self, *_):
        info = AboutInfo(
            product_name=APP_NAME,
            subtitle="INFILTRATORFS · NATIVE FILESYSTEM",
            version=package_version(),
            description=(
                "Native Linux management for InfiltratorFS volumes. "
                "Uses the native VFS/DKMS driver; FUSE is not the product path."
            ),
            authors=("Shannon Smith",),
            copyright="Copyright © 2026 Shannon Smith",
            website="https://github.com/Infiltrator-Projects/InfiltratorFS",
            license_name="GPL-3.0-or-later",
            license_text=(
                "InfiltratorFS is free software licensed under the GNU General Public "
                "License version 3 or, at your option, any later version "
                "(GPL-3.0-or-later). See LICENSE in the source package for the complete licence text."
            ),
            credits="Shannon Smith — Author and project maintainer",
        )
        show_common_about(self, info)
'''
manager, changed = pattern.subn("\n" + replacement, manager, count=1)
if changed != 1:
    raise SystemExit(f"expected to replace one About handler, replaced {changed}")

manager_path.write_text(manager, encoding="utf-8")

policy = policy_path.read_text(encoding="utf-8")
anchor = "! grep -Fq '@borders' \"$manager\"\n"
checks = '''

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
'''
if "The selected storage row must remain dark/metallic" not in policy:
    if anchor not in policy:
        raise SystemExit("could not locate Linux UI policy insertion point")
    policy = policy.replace(anchor, anchor + checks, 1)
    policy_path.write_text(policy, encoding="utf-8")

print("Applied LINK-style About contract and restrained storage selection polish")
