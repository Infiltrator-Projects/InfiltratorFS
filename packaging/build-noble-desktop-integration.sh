#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:?Usage: build-noble-desktop-integration.sh OUTPUT_DIR}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

for command in apt-get dpkg-buildpackage dpkg-deb dpkg-parsechangelog patch sha256sum; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "Missing required command: $command" >&2
        exit 1
    }
done

if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
fi
[[ "${ID:-}" == ubuntu && "${VERSION_ID:-}" == 24.04 ]] || {
    echo "The bundled desktop ABI is built only on Ubuntu 24.04 (Linux Mint 22.x base)." >&2
    exit 1
}

mkdir -p "$out"
out="$(cd "$out" && pwd)"
cd "$work"
apt-get source libblockdev gnome-disk-utility >/dev/null

libsrc="$(find "$work" -maxdepth 1 -type d -name 'libblockdev-*' | sort | head -n1)"
gdusrc="$(find "$work" -maxdepth 1 -type d -name 'gnome-disk-utility-*' | sort | head -n1)"
[[ -n "$libsrc" && -n "$gdusrc" ]] || {
    echo "Could not locate extracted Ubuntu desktop sources." >&2
    exit 1
}

libver="$(dpkg-parsechangelog -l"$libsrc/debian/changelog" -SVersion)"
gduver="$(dpkg-parsechangelog -l"$gdusrc/debian/changelog" -SVersion)"
case "$libver" in
    3.1.*) ;;
    *) echo "Unsupported Ubuntu libblockdev source: $libver" >&2; exit 1 ;;
esac
case "$gduver" in
    46.*) ;;
    *) echo "Unsupported Ubuntu GNOME Disks source: $gduver" >&2; exit 1 ;;
esac

patch -d "$libsrc" -p1 --forward < "$repo_root/packaging/libblockdev-3.1-infiltratorfs.patch"
patch -d "$gdusrc" -p1 --forward < "$repo_root/packaging/gnome-disks-46-infiltratorfs.patch"

(
    cd "$libsrc"
    DEB_BUILD_OPTIONS="${DEB_BUILD_OPTIONS:-nocheck}" dpkg-buildpackage -b -uc -us
)
(
    cd "$gdusrc"
    DEB_BUILD_OPTIONS="${DEB_BUILD_OPTIONS:-nocheck}" dpkg-buildpackage -b -uc -us
)

extract="$work/extract"
mkdir -p "$extract/libblockdev" "$extract/gdu"

libdeb="$(find "$work" -maxdepth 1 -type f -name 'libblockdev-fs3_*_*.deb' | sort | tail -n1)"
gdudeb="$(find "$work" -maxdepth 1 -type f -name 'gnome-disk-utility_*_*.deb' | sort | tail -n1)"
[[ -s "$libdeb" && -s "$gdudeb" ]] || {
    echo "Patched Ubuntu desktop packages were not produced." >&2
    exit 1
}

dpkg-deb -x "$libdeb" "$extract/libblockdev"
dpkg-deb -x "$gdudeb" "$extract/gdu"

plugin="$(find "$extract/libblockdev" -type f -name 'libbd_fs.so.*' | sort | tail -n1)"
gnome_disks="$extract/gdu/usr/bin/gnome-disks"
[[ -s "$plugin" && -x "$gnome_disks" ]] || {
    echo "Could not locate patched desktop binaries." >&2
    exit 1
}

install -m 0644 "$plugin" "$out/libbd_fs.so"
install -m 0755 "$gnome_disks" "$out/gnome-disks"

grep -aFq 'infiltratorfs' "$out/libbd_fs.so"
grep -aFq 'InfiltratorFS' "$out/gnome-disks"

cat > "$out/manifest" <<EOF
format=1
target=ubuntu-24.04-linuxmint-22.x
libblockdev_source_version=$libver
gnome_disk_utility_source_version=$gduver
EOF
(
    cd "$out"
    sha256sum gnome-disks libbd_fs.so >> manifest
)

printf 'Built InfiltratorFS desktop integration bundle:\n  %s\n' "$out"
