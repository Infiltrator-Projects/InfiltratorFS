#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

build_dir="${1:-build}"
dist_dir="${2:-dist}"
version="$(bash "$repo_root/tools/project-version.sh" "$repo_root/CMakeLists.txt")"

package_version="${INFILTRATORFS_PACKAGE_VERSION:-$version}"
build_identity="${INFILTRATORFS_BUILD_IDENTITY:-generic-apt}"
emit_run="${INFILTRATORFS_EMIT_RUN:-1}"
integration_bundle="${INFILTRATORFS_OS_INTEGRATION_BUNDLE_DIR:-}"
require_integration="${INFILTRATORFS_REQUIRE_OS_INTEGRATION:-0}"
integration_enabled=0
[[ "$package_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+(\+native[0-9]+)?$ ]] || {
    echo "Invalid InfiltratorFS package version: $package_version" >&2
    exit 1
}
case "$build_identity" in
    generic-apt|native-local) ;;
    *) echo "Invalid InfiltratorFS build identity: $build_identity" >&2; exit 1 ;;
esac
case "$emit_run" in
    0|1) ;;
    *) echo "INFILTRATORFS_EMIT_RUN must be 0 or 1." >&2; exit 1 ;;
esac
case "$require_integration" in
    0|1) ;;
    *) echo "INFILTRATORFS_REQUIRE_OS_INTEGRATION must be 0 or 1." >&2; exit 1 ;;
esac
if [[ -n "$integration_bundle" ]]; then
    for required in gnome-disks libbd_fs.so manifest; do
        [[ -s "$integration_bundle/$required" ]] || {
            echo "Desktop integration bundle is missing $required." >&2
            exit 1
        }
    done
    integration_enabled=1
elif [[ "$require_integration" = 1 ]]; then
    echo "This package build requires the Ubuntu/Mint desktop integration bundle." >&2
    exit 1
fi
[[ -f src/infiltratr-common/CMakeLists.txt ]] || {
    echo "The pinned Libraries submodule is not initialised." >&2
    exit 1
}

mkdir -p "$dist_dir"
architecture="$(dpkg --print-architecture)"
deb_name="infiltratorfs_${package_version}_${architecture}.deb"
run_name="infiltratorfs-${version}-linux-native.run"
package_root="$(mktemp -d)"
trap 'rm -rf "$package_root"' EXIT
chmod 0755 "$package_root"

cmake --install "$build_dir" --prefix "$package_root/usr"
install -d "$package_root/usr/share/doc/infiltratorfs"
install -m 0644 LICENSE "$package_root/usr/share/doc/infiltratorfs/copyright"
install -m 0644 README.md "$package_root/usr/share/doc/infiltratorfs/README.md"
if [[ "$integration_enabled" = 1 ]]; then
    install -d "$package_root/usr/lib/infiltratorfs/os-integration"
    install -m 0755 "$integration_bundle/gnome-disks" "$package_root/usr/lib/infiltratorfs/os-integration/gnome-disks"
    install -m 0644 "$integration_bundle/libbd_fs.so" "$package_root/usr/lib/infiltratorfs/os-integration/libbd_fs.so"
    install -m 0644 "$integration_bundle/manifest" "$package_root/usr/lib/infiltratorfs/os-integration/manifest"
fi
# DKMS source must be self-contained. One manifest/stager owns the exact source
# set used by packages, hosted DKMS reproduction and destructive qualification.
dkms_root="$package_root/usr/src/infiltratorfs-$package_version"
bash packaging/stage-kernel-dkms-source.sh "$dkms_root"
cat > "$dkms_root/dkms.conf" <<EOF
PACKAGE_NAME="infiltratorfs"
PACKAGE_VERSION="${package_version}"
BUILT_MODULE_NAME[0]="infiltratorfs"
DEST_MODULE_LOCATION[0]="/updates/dkms"
AUTOINSTALL="yes"
MAKE[0]="make KDIR=/lib/modules/\${kernelver}/build"
CLEAN="make KDIR=/lib/modules/\${kernelver}/build clean"
EOF

install -d "$package_root/DEBIAN"
installed_size="$(du -sk "$package_root/usr" | cut -f1)"
desktop_depends=""
desktop_recommends=", udisks2"
desktop_identity="core-only"
if [[ "$integration_enabled" = 1 ]]; then
    desktop_depends=", udisks2, gnome-disk-utility (>= 46~), gnome-disk-utility (<< 47~), libblockdev-fs3 (>= 3.1~), libblockdev-fs3 (<< 3.2~)"
    desktop_recommends=""
    desktop_identity="ubuntu24.04-mint22-bundled"
fi
cat > "$package_root/DEBIAN/control" <<EOF
Package: infiltratorfs
Version: ${package_version}
Section: utils
Priority: optional
Architecture: ${architecture}
Maintainer: The First Infiltrator
X-InfiltratorFS-Build: ${build_identity}
X-InfiltratorFS-Desktop-Integration: ${desktop_identity}
Depends: dkms, initramfs-tools, kmod, polkitd, pkexec, util-linux, xdg-utils, fontconfig, libssl3t64 | libssl3, libgtk-3-0t64 | libgtk-3-0, libglib2.0-0t64 | libglib2.0-0${desktop_depends}
Recommends: linux-headers-generic, udev${desktop_recommends}
Installed-Size: ${installed_size}
Homepage: https://github.com/Infiltrator-Projects/InfiltratorFS
Description: native Linux InfiltratorFS filesystem and tools
 InfiltratorFS formatter, inspector, scrubber, forensic scanner,
 native mount/fsck helpers, direct-image utility, GTK desktop manager,
 and the native Linux VFS kernel driver installed through DKMS.
 FUSE is not used or required by this package.
 Use only with disposable or backed-up media while the filesystem is pre-1.0.
EOF

cat > "$package_root/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
depmod -a 2>/dev/null || true
if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || true
    udevadm trigger --subsystem-match=block --action=change || true
fi
exit 0
EOF
chmod 0755 "$package_root/DEBIAN/postrm"

# preinst/postinst/prerm have one source of truth. Do not shadow these templates
# with generated copies in this builder: package behaviour must be validated
# against the exact scripts that are shipped.
for maintainer in preinst postinst prerm; do
    sed "s/@PACKAGE_VERSION@/${package_version}/g" \
        "packaging/debian/${maintainer}.in" > "$package_root/DEBIAN/$maintainer"
    chmod 0755 "$package_root/DEBIAN/$maintainer"
done

dpkg-deb --root-owner-group --build "$package_root" "$dist_dir/$deb_name"
contents="$dist_dir/package-contents.txt"
dpkg-deb --contents "$dist_dir/$deb_name" > "$contents"
[[ "$(awk 'NR == 1 { print $1 }' "$contents")" == "drwxr-xr-x" ]] || {
    echo "Debian archive root must be mode 0755." >&2
    exit 1
}
for required in \
    'usr/bin/infiltratorfs-manager$' \
    'usr/bin/infiltratorfs-theme$' \
    'usr/bin/infilfs-forensic$' \
    'usr/bin/infilfs-optimize$' \
    'usr/bin/infiltratorfs-resize$' \
    'usr/bin/infiltratorfs-quota$' \
    'usr/bin/mkfs.infiltratorfs$' \
    'usr/sbin/mount.infiltratorfs$' \
    'usr/sbin/fsck.infiltratorfs$' \
    'usr/share/initramfs-tools/hooks/infiltratorfs$' \
    'usr/lib/infiltratorfs/infiltratorfs-manager-helper$' \
    'usr/lib/infiltratorfs/infiltratorfs-os-integration$' \
    'usr/lib/udev/rules.d/59-infiltratorfs.rules$' \
    'usr/share/applications/infiltratorfs-manager.desktop$' \
    'usr/share/icons/hicolor/scalable/apps/infiltratorfs-manager.svg$' \
    'usr/share/app-install/icons/infiltratorfs.svg$' \
    'usr/lib/infiltratorfs/infiltratorfs-format-partition.nemo_action$' \
    'usr/share/fonts/truetype/infiltratorfs/mb_corpo_a_cond_regular.ttf$' \
    'usr/share/fonts/truetype/infiltratorfs/mb_corpo_s_bold.ttf$' \
    'usr/share/fonts/truetype/infiltratorfs/mb_corpo_s_regular.ttf$' \
    "usr/src/infiltratorfs-${package_version}/dkms.conf$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_core.c$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_cpu.c$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_shared_ownership.c$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_name_policy.c$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_crypto.c$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_security.c$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_internal.h$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_format.h$" \
    "usr/src/infiltratorfs-${package_version}/iac1.h$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_allocation_map.c$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_allocation_publish.c$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_parallel_alloc.c$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_extent_tree.c$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_rw.inc$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_rw_legacy.inc$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_checksum_cache.c$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_locator_cache.c$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_rw_data.inc$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_rw_namespace.inc$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_rw_namespace_exchange.inc$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_read_cache.c$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_pagecache.c$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_linux_meta_codec.c$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_linux_meta.inc$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_resize.c$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_quota.inc$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_defrag.inc$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_ioctl.h$"; do
    grep -q "$required" "$contents"
done
test "$(dpkg-deb --field "$dist_dir/$deb_name" X-InfiltratorFS-Desktop-Integration)" = "$desktop_identity"
if [[ "$integration_enabled" = 1 ]]; then
    for required in gnome-disks libbd_fs.so manifest; do
        grep -q "usr/lib/infiltratorfs/os-integration/${required}$" "$contents"
    done
else
    if grep -Eq 'usr/lib/infiltratorfs/os-integration/(gnome-disks|libbd_fs\.so|manifest)$' "$contents"; then
        echo 'Core-only InfiltratorFS package unexpectedly contains a desktop integration bundle.' >&2
        exit 1
    fi
fi
if grep -q 'usr/bin/infilfs-fuse$' "$contents"; then
    echo 'Native release package unexpectedly contains infilfs-fuse.' >&2
    exit 1
fi
if grep -q 'usr/lib/infiltratorfs/patch-mintstick.py$' "$contents"; then
    echo 'Release package must not install the retired whole-device Mintstick patcher.' >&2
    exit 1
fi
if grep -q 'usr/share/nemo/actions/infiltratorfs-format-partition.nemo_action$' "$contents"; then
    echo 'Release package must not install a second visible Nemo Format action.' >&2
    exit 1
fi
test "$(dpkg-deb --field "$dist_dir/$deb_name" Version)" = "$package_version"
depends="$(dpkg-deb --field "$dist_dir/$deb_name" Depends)"
if [[ "$integration_enabled" = 1 ]]; then
    for dependency in udisks2 gnome-disk-utility libblockdev-fs3; do
        grep -Eq "(^|, )${dependency}([ ,|]|$)" <<<"$depends" || {
            echo "Integrated package must depend on distro-owned ${dependency}." >&2
            exit 1
        }
    done
fi
for dependency in dkms initramfs-tools kmod polkitd pkexec util-linux xdg-utils fontconfig libgtk-3-0t64 libglib2.0-0t64; do
    grep -Eq "(^|, )${dependency}([ ,|]|$)" <<<"$depends"
done
if grep -Eq '(^|, )python3([ ,]|$)|python3-gi|gir1\.2-gtk-3\.0' <<<"$depends"; then
    echo 'Native release package must not depend on Python/PyGObject for its Manager.' >&2
    exit 1
fi
if grep -Eqi '(^|[, ])(fuse3|libfuse3-3)([, ]|$)' <<<"$depends"; then
    echo 'Native release package unexpectedly depends on FUSE.' >&2
    exit 1
fi
preinst_text="$(dpkg-deb --ctrl-tarfile "$dist_dir/$deb_name" | tar -xOf - ./preinst)"
grep -Fq 'root_fstype="$(findmnt -n -o FSTYPE / 2>/dev/null || true)"' <<<"$preinst_text"
grep -Fq 'umount -a -t infiltratorfs' <<<"$preinst_text"
grep -Fq 'live root remains mounted' <<<"$preinst_text"
grep -Fq 'clean automatic unmount complete' <<<"$preinst_text"
grep -Fq 'volume is still busy' <<<"$preinst_text"
# preinst runs before the replacement is unpacked, built or qualified. A
# failed package transaction must not first destroy its working DKMS fallback.
grep -Fq 'Keep the previous DKMS registration' <<<"$preinst_text"
if grep -Eq 'dkms[[:space:]]+remove|rm[[:space:]]+-rf' <<<"$preinst_text"; then
    echo 'preinst destroys previous DKMS state before replacement qualification.' >&2
    exit 1
fi
if awk '
    /^[[:space:]]*umount[[:space:]]/ {
        for (field = 2; field <= NF; ++field)
            if ($field ~ /^-[^-]*[fl]/ || $field == "--force" ||
                $field == "--lazy")
                unsafe = 1
    }
    END { exit unsafe ? 0 : 1 }
' <<<"$preinst_text"; then
    echo 'Debian preinst must never force or lazily detach mounted InfiltratorFS volumes.' >&2
    exit 1
fi
postinst_text="$(dpkg-deb --ctrl-tarfile "$dist_dir/$deb_name" | tar -xOf - ./postinst)"
grep -Fq 'update-initramfs -u -k "$kernel"' <<<"$postinst_text"
grep -Fq 'update-initramfs -c -k "$kernel"' <<<"$postinst_text"
grep -Fq 'offline/chroot root target staged' <<<"$postinst_text"
grep -Fq 'root-volume upgrade staged safely' <<<"$postinst_text"
grep -Fq 'modprobe --dry-run --verbose "$module"' <<<"$postinst_text"
grep -Fq 'module loading is administratively disabled' <<<"$postinst_text"
grep -Fq 'infiltratorfs-os-integration install' <<<"$postinst_text"
prerm_text="$(dpkg-deb --ctrl-tarfile "$dist_dir/$deb_name" | tar -xOf - ./prerm)"
grep -Fq 'refusing to remove the filesystem package while / is mounted as InfiltratorFS' <<<"$prerm_text"
grep -Fq 'infiltratorfs-os-integration remove' <<<"$prerm_text"
rm -f "$contents"

if [[ "$emit_run" = 0 ]]; then
    printf 'Built InfiltratorFS Debian package:\n  %s\n' "$deb_name"
    exit 0
fi

# The .run is deliberately a small text-only, source-fetching bootstrap.
# Git commit, recursive gitlinks and external desktop assets are fixed at
# package-generation time. Neither source nor executable extras are attached.
command -v git >/dev/null 2>&1 || {
    echo 'git is required to pin native installer source.' >&2
    exit 1
}
source_sha="$(git rev-parse --verify HEAD)"
[[ "$source_sha" =~ ^[0-9a-f]{40}$ ]] || {
    echo 'Native installer source must be a full Git commit identity.' >&2
    exit 1
}
desktop_source_tag="$(head -n 1 .github/desktop-integration-release | tr -d '[:space:]')"
[[ "$desktop_source_tag" =~ ^desktop-integration-ubuntu24\.04-[A-Za-z0-9.+_-]+$ ]] || {
    echo 'The pinned desktop integration release identity is invalid.' >&2
    exit 1
}
desktop_tag=""
gnome_disks_sha=""
libbd_fs_sha=""
manifest_sha=""
if [[ "$integration_enabled" = 1 ]]; then
    desktop_tag="$desktop_source_tag"
    gnome_disks_sha="$(sha256sum "$integration_bundle/gnome-disks" | cut -d' ' -f1)"
    libbd_fs_sha="$(sha256sum "$integration_bundle/libbd_fs.so" | cut -d' ' -f1)"
    manifest_sha="$(sha256sum "$integration_bundle/manifest" | cut -d' ' -f1)"
fi
template="support/installer/native-release.run.in"
test -s "$template"
sed -e "s/@SOURCE_SHA@/$source_sha/g" \
    -e "s/@RELEASE_VERSION@/$version/g" \
    -e "s/@DESKTOP_SOURCE_TAG@/$desktop_source_tag/g" \
    -e "s/@DESKTOP_TAG@/$desktop_tag/g" \
    -e "s/@GNOME_DISKS_SHA256@/$gnome_disks_sha/g" \
    -e "s/@LIBBD_FS_SHA256@/$libbd_fs_sha/g" \
    -e "s/@MANIFEST_SHA256@/$manifest_sha/g" \
    "$template" > "$dist_dir/$run_name"
chmod 0755 "$dist_dir/$run_name"
bash -n "$dist_dir/$run_name"
"$dist_dir/$run_name" --verify
"$dist_dir/$run_name" --dry-run > "$dist_dir/native-installer-dry-run.txt"
grep -Fq 'Dry run only; no packages will be installed' "$dist_dir/native-installer-dry-run.txt"
grep -Fq 'Native build commands:' "$dist_dir/native-installer-dry-run.txt"
grep -Fq 'Native kernel module commands:' "$dist_dir/native-installer-dry-run.txt"
grep -Fq 'The completed installation is Debian-managed as infiltratorfs' "$dist_dir/native-installer-dry-run.txt"
bash tests/native-installer-source-policy.sh "$dist_dir/$run_name" "$source_sha" "$version"
rm -f "$dist_dir/native-installer-dry-run.txt"

(
    cd "$dist_dir"
    sha256sum "$deb_name" > "$deb_name.sha256"
    sha256sum "$run_name" > "$run_name.sha256"
)
printf 'Built native Linux release packages:\n  %s\n  %s\n' "$deb_name" "$run_name"
