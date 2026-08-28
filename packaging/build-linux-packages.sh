#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

build_dir="${1:-build}"
dist_dir="${2:-dist}"
version="$(sed -n 's/^project(InfiltratorFS VERSION \([^ ]*\) LANGUAGES C)$/\1/p' "$repo_root/CMakeLists.txt")"
[[ -n "$version" ]] || { echo "Could not determine InfiltratorFS version from CMakeLists.txt" >&2; exit 1; }

package_version="${INFILTRATORFS_PACKAGE_VERSION:-$version}"
build_identity="${INFILTRATORFS_BUILD_IDENTITY:-generic-apt}"
emit_run="${INFILTRATORFS_EMIT_RUN:-1}"

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
[[ -f src/infiltratr-common/CMakeLists.txt ]] || {
    echo "The pinned Infiltratr Common submodule is not initialised." >&2
    exit 1
}

mkdir -p "$dist_dir"
architecture="$(dpkg --print-architecture)"
deb_name="infiltratorfs_${package_version}_${architecture}.deb"
run_name="infiltratorfs-${version}-linux-native.run"
package_root="$(mktemp -d)"
payload="$(mktemp)"
trap 'rm -rf "$package_root" "$payload"' EXIT
chmod 0755 "$package_root"

cmake --install "$build_dir" --prefix "$package_root/usr"
# Native Linux is the product path from 0.17.0 onward. Never allow an
# opportunistically-built legacy FUSE adapter into a release package.
rm -f "$package_root/usr/bin/infilfs-fuse"
install -d "$package_root/usr/share/doc/infiltratorfs"
install -m 0644 LICENSE "$package_root/usr/share/doc/infiltratorfs/copyright"
install -m 0644 README.md "$package_root/usr/share/doc/infiltratorfs/README.md"

# DKMS source must be self-contained. Every RW composition file is required;
# omitting an implementation include makes host-side DKMS builds fail even
# though the repository build itself succeeds.
dkms_root="$package_root/usr/src/infiltratorfs-$package_version"
install -d "$dkms_root"
for file in Makefile infiltratorfs.c infiltratorfs_format.h \
            infiltratorfs_index_tree.inc infiltratorfs_directory_tree.inc \
            infiltratorfs_rw.inc \
            infiltratorfs_rw_legacy.inc infiltratorfs_rw_data.inc \
            infiltratorfs_rw_namespace.inc infiltratorfs_rw_read_cache.inc \
            infiltratorfs_pagecache.inc infiltratorfs_linux_meta.inc; do
    install -m 0644 "kernel/$file" "$dkms_root/$file"
done
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
cat > "$package_root/DEBIAN/control" <<EOF
Package: infiltratorfs
Version: ${package_version}
Section: utils
Priority: optional
Architecture: ${architecture}
Maintainer: The First Infiltrator
X-InfiltratorFS-Build: ${build_identity}
Depends: dkms, kmod, policykit-1, util-linux, xdg-utils, zenity
Recommends: linux-headers-generic, udev, udisks2
Installed-Size: ${installed_size}
Homepage: https://github.com/The-First-Infiltrator/InfiltratorFS
Description: native Linux InfiltratorFS filesystem and tools
 InfiltratorFS formatter, inspector, scrubber, forensic scanner,
 native mount/fsck helpers, direct-image utility, Linux Mint desktop manager,
 and the native Linux VFS kernel driver installed through DKMS.
 FUSE is not used or required by this package.
 Use only with disposable or backed-up media while the filesystem is pre-1.0.
EOF

cat > "$package_root/DEBIAN/preinst" <<EOF
#!/bin/sh
set -e
module='infiltratorfs'
version='${package_version}'

active=''
if command -v findmnt >/dev/null 2>&1; then
    active="\$(findmnt -rn -t infiltratorfs,fuse.infilfs-fuse 2>/dev/null || true)"
else
    active="\$(grep -E ' (infiltratorfs|fuse\\.infilfs-fuse) ' /proc/self/mounts 2>/dev/null || true)"
fi
if [ -n "\$active" ]; then
    echo 'InfiltratorFS: unmount all InfiltratorFS volumes before upgrading the driver.' >&2
    echo "\$active" >&2
    exit 1
fi

if command -v dkms >/dev/null 2>&1; then
    dkms status -m "\$module" 2>/dev/null | while IFS= read -r line; do
        case "\$line" in "\$module"/*) ;; *) continue ;; esac
        head="\${line%%,*}"; head="\${head%%:*}"; old_version="\${head#\${module}/}"
        if [ -n "\$old_version" ] && [ "\$old_version" != "\$version" ]; then
            echo "InfiltratorFS: removing stale DKMS registration \$old_version."
            dkms remove -m "\$module" -v "\$old_version" --all || true
            rm -rf "/usr/src/\$module-\$old_version" "/var/lib/dkms/\$module/\$old_version" || true
        fi
    done
fi
exit 0
EOF
chmod 0755 "$package_root/DEBIAN/preinst"

cat > "$package_root/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e
module='infiltratorfs'
version='${package_version}'
kernel="\$(uname -r)"

if [ ! -f "/lib/modules/\$kernel/build/Makefile" ]; then
    echo "InfiltratorFS: matching kernel headers are required for \$kernel." >&2
    echo "Install linux-headers-\$kernel and configure this package again." >&2
    exit 1
fi
if ! dkms status -m "\$module" -v "\$version" 2>/dev/null | grep -q .; then
    dkms add -m "\$module" -v "\$version"
fi
dkms build -m "\$module" -v "\$version" -k "\$kernel"
dkms install -m "\$module" -v "\$version" -k "\$kernel" --force
depmod -a
modprobe -r infiltratorfs 2>/dev/null || true
modprobe infiltratorfs
grep -qw infiltratorfs /proc/filesystems || {
    echo 'InfiltratorFS: native kernel filesystem did not register.' >&2
    exit 1
}
rm -f /usr/bin/infilfs-fuse
if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || true
    udevadm trigger --subsystem-match=block --action=change || true
    udevadm settle --timeout=30 || true
fi
exit 0
EOF
chmod 0755 "$package_root/DEBIAN/postinst"

cat > "$package_root/DEBIAN/prerm" <<EOF
#!/bin/sh
set -e
if [ "\${1:-}" = remove ] && command -v dkms >/dev/null 2>&1; then
    dkms remove -m infiltratorfs -v '${package_version}' --all || true
fi
exit 0
EOF
chmod 0755 "$package_root/DEBIAN/prerm"

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

dpkg-deb --root-owner-group --build "$package_root" "$dist_dir/$deb_name"
contents="$dist_dir/package-contents.txt"
dpkg-deb --contents "$dist_dir/$deb_name" > "$contents"
[[ "$(awk 'NR == 1 { print $1 }' "$contents")" == "drwxr-xr-x" ]] || {
    echo "Debian archive root must be mode 0755." >&2
    exit 1
}
for required in \
    'usr/bin/infiltratorfs-manager$' \
    'usr/bin/infilfs-forensic$' \
    'usr/sbin/mount.infiltratorfs$' \
    'usr/sbin/fsck.infiltratorfs$' \
    'usr/lib/infiltratorfs/infiltratorfs-manager-helper$' \
    'usr/lib/udev/rules.d/59-infiltratorfs.rules$' \
    'usr/share/applications/infiltratorfs-manager.desktop$' \
    "usr/src/infiltratorfs-${package_version}/dkms.conf$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs.c$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_format.h$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_rw.inc$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_rw_legacy.inc$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_rw_data.inc$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_rw_namespace.inc$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_rw_read_cache.inc$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_pagecache.inc$" \
    "usr/src/infiltratorfs-${package_version}/infiltratorfs_linux_meta.inc$"; do
    grep -q "$required" "$contents"
done
if grep -q 'usr/bin/infilfs-fuse$' "$contents"; then
    echo 'Native release package unexpectedly contains infilfs-fuse.' >&2
    exit 1
fi
test "$(dpkg-deb --field "$dist_dir/$deb_name" Version)" = "$package_version"
depends="$(dpkg-deb --field "$dist_dir/$deb_name" Depends)"
for dependency in dkms kmod policykit-1 util-linux xdg-utils zenity; do
    grep -Eq "(^|, )${dependency}([ ,]|$)" <<<"$depends"
done
if grep -Eqi '(^|[, ])(fuse3|libfuse3-3)([, ]|$)' <<<"$depends"; then
    echo 'Native release package unexpectedly depends on FUSE.' >&2
    exit 1
fi
dpkg-deb --ctrl-tarfile "$dist_dir/$deb_name" | tar -xOf - ./postinst | grep -Fq 'modprobe infiltratorfs'
rm -f "$contents"

if [[ "$emit_run" = 0 ]]; then
    printf 'Built InfiltratorFS Debian package:\n  %s\n' "$deb_name"
    exit 0
fi

# The .run payload contains source, but its bootstrap deliberately disables the
# optional PkgConfig/FUSE discovery and removes any legacy infilfs-fuse binary.
source_epoch="$(git log -1 --format=%ct 2>/dev/null || date +%s)"
tar --sort=name --mtime="@${source_epoch}" --owner=0 --group=0 --numeric-owner \
    --exclude='./.git' --exclude='*/.git' --exclude='./build*' --exclude='./dist' \
    -czf "$payload" .

cat > "$dist_dir/$run_name" <<'RUN_HEADER'
#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail
self="$0"
marker='__INFILTRATORFS_NATIVE_PAYLOAD__'
payload_start="$(awk -v marker="$marker" '$0 == marker { print NR + 1; exit }' "$self")"
[[ -n "$payload_start" ]] || { echo "Installer payload marker not found." >&2; exit 1; }

verify_installer() {
    local verify_root
    test "$(head -n 1 "$self")" = '#!/usr/bin/env bash'
    tail -n +"$payload_start" "$self" | gzip -t
    verify_root="$(mktemp -d)"
    trap 'rm -rf "$verify_root"' RETURN
    tail -n +"$payload_start" "$self" | tar --no-same-owner -xzf - -C "$verify_root"
    for required in CMakeLists.txt README.md support/installer/bootstrap.sh \
        src/infiltratr-common/CMakeLists.txt kernel/Makefile kernel/infiltratorfs.c \
        kernel/infiltratorfs_format.h kernel/infiltratorfs_rw.inc \
        kernel/infiltratorfs_rw_legacy.inc kernel/infiltratorfs_rw_data.inc \
        kernel/infiltratorfs_rw_namespace.inc kernel/infiltratorfs_rw_read_cache.inc \
        kernel/infiltratorfs_pagecache.inc kernel/infiltratorfs_linux_meta.inc; do
        test -f "$verify_root/$required"
    done
    test -x "$verify_root/support/installer/bootstrap.sh"
    bash -n "$verify_root/support/installer/bootstrap.sh"
    rm -rf "$verify_root"
    trap - RETURN
}

case "${1:-}" in
    --verify)
        [[ $# -eq 1 ]] || exit 2
        verify_installer
        echo 'InfiltratorFS native installer verified.'
        exit 0
        ;;
    --dry-run)
        [[ $# -eq 1 ]] || exit 2
        ;;
    --build-only)
        [[ $# -eq 2 ]] || { echo 'Usage: infiltratorfs-<version>-linux-native.run --build-only OUTPUT.deb' >&2; exit 2; }
        ;;
    '')
        [[ $# -eq 0 ]] || exit 2
        ;;
    --help|-h)
        echo 'Usage: infiltratorfs-<version>-linux-native.run [--verify|--dry-run|--build-only OUTPUT.deb]'
        exit 0
        ;;
    *) exit 2 ;;
esac
verify_installer
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/source"
tail -n +"$payload_start" "$self" | tar --no-same-owner -xzf - -C "$work/source"
"$work/source/support/installer/bootstrap.sh" "$@"
exit 0
__INFILTRATORFS_NATIVE_PAYLOAD__
RUN_HEADER
cat "$payload" >> "$dist_dir/$run_name"
chmod 0755 "$dist_dir/$run_name"

"$dist_dir/$run_name" --verify
"$dist_dir/$run_name" --dry-run > "$dist_dir/native-installer-dry-run.txt"
grep -Fq 'Dry run only; no packages will be installed' "$dist_dir/native-installer-dry-run.txt"
grep -Fq 'Native kernel module commands:' "$dist_dir/native-installer-dry-run.txt"
grep -Fq 'The completed installation is Debian-managed as infiltratorfs' "$dist_dir/native-installer-dry-run.txt"
rm -f "$dist_dir/native-installer-dry-run.txt"

(
    cd "$dist_dir"
    sha256sum "$deb_name" > "$deb_name.sha256"
    sha256sum "$run_name" > "$run_name.sha256"
)
printf 'Built native Linux release packages:\n  %s\n  %s\n' "$deb_name" "$run_name"
