#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:?Usage: build-noble-desktop-packages.sh OUTPUT_DIR}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
for command in apt-get dpkg-buildpackage dpkg-deb dpkg-parsechangelog patch python3 sha256sum; do
    command -v "$command" >/dev/null 2>&1 || { echo "Missing required command: $command" >&2; exit 1; }
done
if [[ -r /etc/os-release ]]; then . /etc/os-release; fi
[[ "${ID:-}" == ubuntu && "${VERSION_ID:-}" == 24.04 ]] || {
    echo "Desktop integration packages are built only on Ubuntu 24.04 (Linux Mint 22.x base)." >&2; exit 1; }
mkdir -p "$out"; out="$(cd "$out" && pwd)"; cd "$work"
apt-get source libblockdev gnome-disk-utility >/dev/null
libsrc="$(find "$work" -maxdepth 1 -type d -name 'libblockdev-*' | sort | head -n1)"
gdusrc="$(find "$work" -maxdepth 1 -type d -name 'gnome-disk-utility-*' | sort | head -n1)"
[[ -n "$libsrc" && -n "$gdusrc" ]] || { echo "Could not locate extracted Ubuntu desktop sources." >&2; exit 1; }
libver="$(dpkg-parsechangelog -l"$libsrc/debian/changelog" -SVersion)"
gduver="$(dpkg-parsechangelog -l"$gdusrc/debian/changelog" -SVersion)"
case "$libver" in 3.1.*) ;; *) echo "Unsupported Ubuntu libblockdev source: $libver" >&2; exit 1 ;; esac
case "$gduver" in 46.*) ;; *) echo "Unsupported Ubuntu GNOME Disks source: $gduver" >&2; exit 1 ;; esac
patch -d "$libsrc" -p1 --forward < "$repo_root/packaging/libblockdev-3.1-infiltratorfs.patch"
patch -d "$gdusrc" -p1 --forward < "$repo_root/packaging/gnome-disks-46-infiltratorfs.patch"
(cd "$libsrc"; DEB_BUILD_OPTIONS="${DEB_BUILD_OPTIONS:-nocheck}" dpkg-buildpackage -b -uc -us)
(cd "$gdusrc"; DEB_BUILD_OPTIONS="${DEB_BUILD_OPTIONS:-nocheck}" dpkg-buildpackage -b -uc -us)
architecture="$(dpkg --print-architecture)"
libdeb="$(find "$work" -maxdepth 1 -type f -name "libblockdev-fs3_*_${architecture}.deb" | sort | tail -n1)"
gdudeb="$(find "$work" -maxdepth 1 -type f -name "gnome-disk-utility_*_${architecture}.deb" | sort | tail -n1)"
[[ -s "$libdeb" && -s "$gdudeb" ]] || { echo "Patched Ubuntu desktop packages were not produced." >&2; exit 1; }

repackage_replacement() {
    local source_deb="$1" replacement="$2" original="$3" root base_version replacement_version output
    root="$(mktemp -d "$work/repack.XXXXXX")"
    dpkg-deb -R "$source_deb" "$root"
    base_version="$(dpkg-deb --field "$source_deb" Version)"
    replacement_version="${base_version}+infiltratorfs1"
    python3 - "$root/DEBIAN/control" "$replacement" "$replacement_version" "$original" "$base_version" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
replacement, version, original, original_version = sys.argv[2:]
lines = path.read_text(encoding="utf-8").splitlines()
fields, order, current = {}, [], None
for line in lines:
    if line[:1].isspace() and current:
        fields[current] += "\n" + line
        continue
    if ":" not in line:
        continue
    key, value = line.split(":", 1)
    current = key
    if key not in fields:
        order.append(key)
    fields[key] = value.lstrip()
fields["Package"] = replacement
fields["Version"] = version
for key, value in (("Provides", f"{original} (= {original_version})"), ("Conflicts", original), ("Replaces", original)):
    existing = fields.get(key, "")
    fields[key] = ", ".join(part for part in (existing, value) if part)
    if key not in order:
        order.append(key)
path.write_text("\n".join(f"{key}: {fields[key]}" for key in order) + "\n", encoding="utf-8")
PY
    output="$out/${replacement}_${replacement_version}_${architecture}.deb"
    dpkg-deb --root-owner-group --build "$root" "$output" >/dev/null
    printf '%s\n' "$replacement_version"
}
lib_replacement_version="$(repackage_replacement "$libdeb" infiltratorfs-libblockdev-fs3 libblockdev-fs3)"
gdu_replacement_version="$(repackage_replacement "$gdudeb" infiltratorfs-gnome-disk-utility gnome-disk-utility)"

meta_version="1.0.0+ubuntu24.04.1"
meta_root="$(mktemp -d "$work/meta.XXXXXX")"; install -d "$meta_root/DEBIAN"
cat > "$meta_root/DEBIAN/control" <<EOF
Package: infiltratorfs-desktop-integration
Version: ${meta_version}
Section: utils
Priority: optional
Architecture: all
Maintainer: The First Infiltrator
Depends: infiltratorfs-libblockdev-fs3 (= ${lib_replacement_version}), infiltratorfs-gnome-disk-utility (= ${gdu_replacement_version}), udisks2
Recommends: infiltratorfs
Homepage: https://github.com/Infiltrator-Projects/InfiltratorFS
Description: GNOME Disks and UDisks integration for InfiltratorFS
 ABI-matched Ubuntu 24.04 / Linux Mint 22.x desktop integration packages.
EOF
cat > "$meta_root/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v systemctl >/dev/null 2>&1; then systemctl restart udisks2.service >/dev/null 2>&1 || true; fi
if command -v gdbus >/dev/null 2>&1; then
    result="$(gdbus call --system --dest org.freedesktop.UDisks2 --object-path /org/freedesktop/UDisks2/Manager --method org.freedesktop.UDisks2.Manager.CanFormat infiltratorfs 2>/dev/null || true)"
    case "$result" in *true*) ;; *) echo 'InfiltratorFS: warning: UDisks does not currently advertise InfiltratorFS formatting.' >&2 ;; esac
fi
exit 0
EOF
cat > "$meta_root/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if command -v systemctl >/dev/null 2>&1; then systemctl try-restart udisks2.service >/dev/null 2>&1 || true; fi
exit 0
EOF
chmod 0755 "$meta_root/DEBIAN/postinst" "$meta_root/DEBIAN/postrm"
dpkg-deb --root-owner-group --build "$meta_root" "$out/infiltratorfs-desktop-integration_${meta_version}_all.deb" >/dev/null
patch_sha256="$(cat "$repo_root/packaging/libblockdev-3.1-infiltratorfs.patch" "$repo_root/packaging/gnome-disks-46-infiltratorfs.patch" "$repo_root/packaging/build-noble-desktop-packages.sh" | sha256sum | awk '{print $1}')"
cat > "$out/infiltratorfs-desktop-integration.manifest" <<EOF
format=2
target=ubuntu-24.04-linuxmint-22.x
libblockdev_source_version=${libver}
gnome_disk_utility_source_version=${gduver}
libblockdev_replacement_version=${lib_replacement_version}
gnome_disk_utility_replacement_version=${gdu_replacement_version}
meta_version=${meta_version}
patch_sha256=${patch_sha256}
EOF
for deb in "$out"/*.deb; do dpkg-deb --info "$deb" >/dev/null; done
printf 'Built managed InfiltratorFS desktop integration packages in %s\n' "$out"
