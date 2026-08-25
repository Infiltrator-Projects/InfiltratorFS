#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT}/build-native"
VERSION="$(sed -n 's/^project(InfiltratorFS VERSION \([^ ]*\) LANGUAGES C)$/\1/p' "$ROOT/CMakeLists.txt")"
KERNEL_RELEASE="$(uname -r)"
DKMS_SOURCE="/usr/src/infiltratorfs-${VERSION}"
declare -a missing_packages=()

[[ -n "$VERSION" ]] || { echo "Could not determine InfiltratorFS version from CMakeLists.txt" >&2; exit 1; }

add_missing_package() {
    local candidate="$1" existing
    for existing in "${missing_packages[@]:-}"; do
        [[ "$existing" == "$candidate" ]] && return 0
    done
    missing_packages+=("$candidate")
}

detect_kernel_compiler() {
    local compiler_name=""
    local compiler_path=""

    # Kbuild warns when the module compiler executable differs from the one
    # recorded in the running kernel even when both are the same GCC release.
    # Prefer the exact recorded cross-prefixed compiler when it is installed.
    compiler_name="$(grep -oE '[[:alnum:]_.+-]+-gcc-[0-9]+' /proc/version 2>/dev/null | head -n1 || true)"
    if [[ -n "$compiler_name" ]]; then
        compiler_path="$(command -v "$compiler_name" 2>/dev/null || true)"
    fi
    if [[ -z "$compiler_path" ]]; then
        compiler_path="$(command -v cc 2>/dev/null || true)"
    fi
    printf '%s\n' "$compiler_path"
}

KERNEL_CC="$(detect_kernel_compiler)"

check_requirements() {
    command -v cc >/dev/null 2>&1 || add_missing_package build-essential
    command -v make >/dev/null 2>&1 || add_missing_package build-essential
    command -v cmake >/dev/null 2>&1 || add_missing_package cmake
    command -v pkexec >/dev/null 2>&1 || add_missing_package policykit-1
    command -v lsblk >/dev/null 2>&1 || add_missing_package util-linux
    command -v findmnt >/dev/null 2>&1 || add_missing_package util-linux
    command -v mountpoint >/dev/null 2>&1 || add_missing_package util-linux
    command -v mount >/dev/null 2>&1 || add_missing_package util-linux
    command -v umount >/dev/null 2>&1 || add_missing_package util-linux
    command -v xdg-open >/dev/null 2>&1 || add_missing_package xdg-utils
    command -v zenity >/dev/null 2>&1 || add_missing_package zenity
    command -v dkms >/dev/null 2>&1 || add_missing_package dkms
    command -v modprobe >/dev/null 2>&1 || add_missing_package kmod
    command -v depmod >/dev/null 2>&1 || add_missing_package kmod
    command -v pahole >/dev/null 2>&1 || add_missing_package dwarves
    command -v udevadm >/dev/null 2>&1 || add_missing_package udev
    command -v udisksctl >/dev/null 2>&1 || add_missing_package udisks2
    command -v update-desktop-database >/dev/null 2>&1 || add_missing_package desktop-file-utils
    [[ -f "/lib/modules/${KERNEL_RELEASE}/build/Makefile" ]] || add_missing_package "linux-headers-${KERNEL_RELEASE}"
    [[ -n "$KERNEL_CC" ]] || add_missing_package build-essential
}

print_package_commands() {
    printf '  sudo apt-get update\n'
    printf '  sudo apt-get install -y'
    printf ' %q' "${missing_packages[@]}"
    printf '\n'
}

print_build_commands() {
    printf 'Native build commands:\n'
    printf '  cmake -S %q -B %q -DCMAKE_BUILD_TYPE=Release\n' "$ROOT" "$BUILD_DIR"
    printf '  cmake --build %q --parallel\n' "$BUILD_DIR"
    printf '  ctest --test-dir %q --output-on-failure\n' "$BUILD_DIR"
    printf '  sudo cmake --install %q --prefix /usr\n' "$BUILD_DIR"
}

print_kernel_commands() {
    printf 'Native kernel module commands:\n'
    printf '  make -C %q KDIR=%q CC=%q\n' "$ROOT/kernel" "/lib/modules/${KERNEL_RELEASE}/build" "$KERNEL_CC"
    printf '  sudo dkms add -m infiltratorfs -v %q\n' "$VERSION"
    printf '  sudo dkms build -m infiltratorfs -v %q -k %q\n' "$VERSION" "$KERNEL_RELEASE"
    printf '  sudo dkms install -m infiltratorfs -v %q -k %q --force\n' "$VERSION" "$KERNEL_RELEASE"
    printf '  sudo modprobe infiltratorfs\n'
}

run_as_root() {
    if (( EUID == 0 )); then "$@"; else sudo "$@"; fi
}

require_no_active_infiltratorfs_mounts() {
    local active
    active="$(findmnt -rn -t infiltratorfs,fuse.infilfs-fuse 2>/dev/null || true)"
    if [[ -n "$active" ]]; then
        echo "InfiltratorFS: active mounts must be unmounted before upgrading the driver:" >&2
        printf '%s\n' "$active" >&2
        exit 1
    fi
}

remove_stale_dkms_versions() {
    local line head old_version
    declare -A seen=()
    command -v dkms >/dev/null 2>&1 || return 0
    while IFS= read -r line; do
        [[ "$line" == infiltratorfs/* ]] || continue
        head="${line%%,*}"; head="${head%%:*}"; old_version="${head#infiltratorfs/}"
        [[ -n "$old_version" && "$old_version" != "$VERSION" ]] || continue
        [[ -z "${seen[$old_version]:-}" ]] || continue
        seen[$old_version]=1
        printf 'Removing stale InfiltratorFS DKMS registration %s.\n' "$old_version"
        run_as_root dkms remove -m infiltratorfs -v "$old_version" --all || true
        run_as_root rm -rf "/usr/src/infiltratorfs-${old_version}" "/var/lib/dkms/infiltratorfs/${old_version}" || true
    done < <(dkms status -m infiltratorfs 2>/dev/null || true)
}

remove_conflicting_debian_package() {
    local status version
    command -v dpkg-query >/dev/null 2>&1 || return 0
    status="$(dpkg-query -W -f='${db:Status-Abbrev} ${Version}\n' infiltratorfs 2>/dev/null || true)"
    [[ "$status" == ii\ * ]] || return 0
    version="${status#ii }"
    printf 'Removing Debian package record for InfiltratorFS %s before unmanaged .run installation.\n' "$version"
    run_as_root dpkg --purge infiltratorfs
}

install_kernel_module() {
    local dkms_conf="$BUILD_DIR/infiltratorfs-dkms.conf"
    mkdir -p "$BUILD_DIR"
    cat > "$dkms_conf" <<EOF
PACKAGE_NAME="infiltratorfs"
PACKAGE_VERSION="${VERSION}"
BUILT_MODULE_NAME[0]="infiltratorfs"
DEST_MODULE_LOCATION[0]="/updates/dkms"
AUTOINSTALL="yes"
MAKE[0]="make CC=${KERNEL_CC} KDIR=/lib/modules/\${kernelver}/build"
CLEAN="make KDIR=/lib/modules/\${kernelver}/build clean"
EOF

    make -C "$ROOT/kernel" KDIR="/lib/modules/${KERNEL_RELEASE}/build" CC="$KERNEL_CC"
    if dkms status -m infiltratorfs -v "$VERSION" 2>/dev/null | grep -q .; then
        run_as_root dkms remove -m infiltratorfs -v "$VERSION" --all || true
    fi
    run_as_root rm -rf "$DKMS_SOURCE"
    run_as_root install -d "$DKMS_SOURCE"
    for file in Makefile infiltratorfs.c infiltratorfs_format.h infiltratorfs_rw.inc \
                infiltratorfs_rw_legacy.inc infiltratorfs_rw_data.inc; do
        run_as_root install -m 0644 "$ROOT/kernel/$file" "$DKMS_SOURCE/$file"
    done
    run_as_root install -m 0644 "$dkms_conf" "$DKMS_SOURCE/dkms.conf"
    run_as_root dkms add -m infiltratorfs -v "$VERSION"
    run_as_root dkms build -m infiltratorfs -v "$VERSION" -k "$KERNEL_RELEASE"
    run_as_root dkms install -m infiltratorfs -v "$VERSION" -k "$KERNEL_RELEASE" --force
    run_as_root depmod -a
    run_as_root modprobe -r infiltratorfs 2>/dev/null || true
    run_as_root modprobe infiltratorfs
    grep -qw infiltratorfs /proc/filesystems || {
        echo "InfiltratorFS native module loaded but filesystem registration is missing." >&2
        exit 1
    }
}

refresh_desktop_storage() {
    command -v udevadm >/dev/null 2>&1 || return 0
    run_as_root udevadm control --reload-rules || true
    run_as_root udevadm trigger --subsystem-match=block --action=change || true
}

check_requirements

if [[ "${1:-}" == '--dry-run' ]]; then
    printf 'InfiltratorFS native kernel build installer\n'
    printf 'Dry run only; no packages will be installed and no files will be changed.\n'
    printf 'Older InfiltratorFS DKMS registrations would be removed before installation.\n'
    printf 'Any active native or legacy FUSE InfiltratorFS mount must be unmounted first.\n'
    if (( ${#missing_packages[@]} > 0 )); then
        printf 'Missing build or runtime requirements:\n  %s\n' "${missing_packages[@]}"
        printf 'Commands that would install them:\n'; print_package_commands
    else
        printf 'All required native build and runtime packages are already available.\n'
    fi
    print_build_commands
    print_kernel_commands
    printf 'The legacy /usr/bin/infilfs-fuse executable would be removed if present.\n'
    exit 0
fi

(( $# == 0 )) || { echo "Usage: bootstrap.sh [--dry-run]" >&2; exit 2; }
require_no_active_infiltratorfs_mounts
remove_stale_dkms_versions
remove_conflicting_debian_package

if (( ${#missing_packages[@]} > 0 )); then
    printf 'Missing build or runtime requirements:\n  %s\n' "${missing_packages[@]}"
    printf 'The installer needs to run:\n'; print_package_commands
    [[ -t 0 ]] || { echo "Cannot request permission without an interactive terminal." >&2; exit 1; }
    read -r -p 'Install the missing requirements now? [y/N] ' reply
    case "$reply" in y|Y|yes|YES|Yes) ;; *) echo "Installation cancelled." >&2; exit 1 ;; esac
    command -v apt-get >/dev/null 2>&1 || { echo "apt-get is unavailable." >&2; exit 1; }
    run_as_root apt-get update
    run_as_root apt-get install -y "${missing_packages[@]}"
    missing_packages=(); check_requirements
    (( ${#missing_packages[@]} == 0 )) || { printf 'Requirements still missing:\n  %s\n' "${missing_packages[@]}" >&2; exit 1; }
fi

printf 'InfiltratorFS %s native kernel installation\n' "$VERSION"
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure
run_as_root cmake --install "$BUILD_DIR" --prefix /usr
run_as_root rm -f /usr/bin/infilfs-fuse
install_kernel_module
refresh_desktop_storage
if command -v update-desktop-database >/dev/null 2>&1; then
    run_as_root update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
fi
[[ ! -e /usr/bin/infilfs-fuse ]] || { echo "Legacy FUSE executable was not removed." >&2; exit 1; }

printf '\nInfiltratorFS native installation complete.\n'
printf 'Kernel: %s\n' "$KERNEL_RELEASE"
printf 'Filesystem driver: infiltratorfs (native kernel VFS)\n'
printf 'FUSE runtime: not installed or used by InfiltratorFS\n'
printf 'Launch "InfiltratorFS Manager" from the application menu.\n'
