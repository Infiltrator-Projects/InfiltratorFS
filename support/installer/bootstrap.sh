#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT}/build-native"
VERSION="$(sed -n 's/^project(InfiltratorFS VERSION \([^ ]*\) LANGUAGES C)$/\1/p' "$ROOT/CMakeLists.txt")"
NATIVE_PACKAGE_VERSION="${VERSION}+native1"
KERNEL_RELEASE="$(uname -r)"
PACKAGE_DIR="$BUILD_DIR/native-package"
MODE="install"
OUTPUT_PATH=""
declare -a missing_packages=()

[[ -n "$VERSION" ]] || { echo "Could not determine InfiltratorFS version from CMakeLists.txt" >&2; exit 1; }

case "${1:-}" in
    --dry-run)
        [[ $# -eq 1 ]] || { echo 'Usage: bootstrap.sh [--dry-run|--build-only OUTPUT.deb]' >&2; exit 2; }
        MODE="dry-run"
        ;;
    --build-only)
        [[ $# -eq 2 ]] || { echo 'Usage: bootstrap.sh --build-only OUTPUT.deb' >&2; exit 2; }
        MODE="build-only"
        OUTPUT_PATH="$2"
        ;;
    '')
        [[ $# -eq 0 ]] || exit 2
        ;;
    *)
        echo 'Usage: bootstrap.sh [--dry-run|--build-only OUTPUT.deb]' >&2
        exit 2
        ;;
esac

add_missing_package() {
    local candidate="$1" existing
    for existing in "${missing_packages[@]:-}"; do
        [[ "$existing" == "$candidate" ]] && return 0
    done
    missing_packages+=("$candidate")
}

check_requirements() {
    command -v cc >/dev/null 2>&1 || add_missing_package build-essential
    command -v make >/dev/null 2>&1 || add_missing_package build-essential
    command -v cmake >/dev/null 2>&1 || add_missing_package cmake
    command -v dpkg >/dev/null 2>&1 || add_missing_package dpkg
    command -v dpkg-deb >/dev/null 2>&1 || add_missing_package dpkg
    command -v apt-get >/dev/null 2>&1 || add_missing_package apt
    command -v pkexec >/dev/null 2>&1 || add_missing_package policykit-1
    command -v findmnt >/dev/null 2>&1 || add_missing_package util-linux
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
}

run_as_root() {
    if (( EUID == 0 )); then
        "$@"
    elif command -v sudo >/dev/null 2>&1; then
        sudo "$@"
    else
        echo 'Administrator access is required and sudo is unavailable.' >&2
        return 1
    fi
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

refresh_desktop_storage() {
    command -v udevadm >/dev/null 2>&1 || return 0
    run_as_root udevadm control --reload-rules || true
    run_as_root udevadm trigger --subsystem-match=block --action=change || true
    run_as_root udevadm settle --timeout=30 || true
}

print_package_commands() {
    printf '  sudo apt-get update\n'
    printf '  sudo apt-get install -y'
    printf ' %q' "${missing_packages[@]}"
    printf '\n'
}

print_build_commands() {
    printf 'Native build commands:\n'
    printf '  cmake -S %q -B %q -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE=%q\n' \
        "$ROOT" "$BUILD_DIR" '-O3 -DNDEBUG -march=native -mtune=native'
    printf '  cmake --build %q --parallel\n' "$BUILD_DIR"
    printf '  ctest --test-dir %q --output-on-failure\n' "$BUILD_DIR"
    printf '  INFILTRATORFS_PACKAGE_VERSION=%q INFILTRATORFS_BUILD_IDENTITY=native-local INFILTRATORFS_EMIT_RUN=0 bash %q %q %q\n' \
        "$NATIVE_PACKAGE_VERSION" "$ROOT/packaging/build-linux-packages.sh" "$BUILD_DIR" "$PACKAGE_DIR"
}

print_kernel_commands() {
    printf 'Native kernel module commands:\n'
    printf '  Installing infiltratorfs %q triggers DKMS to compile the native VFS driver locally for kernel %q.\n' \
        "$NATIVE_PACKAGE_VERSION" "$KERNEL_RELEASE"
}

check_requirements

if [[ "$MODE" == dry-run ]]; then
    printf 'InfiltratorFS %s native local-machine build installer\n' "$VERSION"
    printf 'Native package version: %s\n' "$NATIVE_PACKAGE_VERSION"
    printf 'Dry run only; no packages will be installed and no files will be changed.\n'
    printf 'Any active native or legacy FUSE InfiltratorFS mount must be unmounted first.\n'
    if (( ${#missing_packages[@]} > 0 )); then
        printf 'Missing build or runtime requirements:\n  %s\n' "${missing_packages[@]}"
        printf 'Commands that would install them:\n'
        print_package_commands
    else
        printf 'All required native build and runtime packages are already available.\n'
    fi
    print_build_commands
    print_kernel_commands
    printf 'The completed installation is Debian-managed as infiltratorfs %s.\n' "$NATIVE_PACKAGE_VERSION"
    exit 0
fi

if (( ${#missing_packages[@]} > 0 )); then
    if [[ "$MODE" == build-only ]]; then
        printf 'Requirements missing for native package build:\n  %s\n' "${missing_packages[@]}" >&2
        exit 1
    fi
    printf 'Missing build or runtime requirements:\n  %s\n' "${missing_packages[@]}"
    printf 'The installer needs to run:\n'
    print_package_commands
    [[ -t 0 ]] || { echo "Cannot request permission without an interactive terminal." >&2; exit 1; }
    read -r -p 'Install the missing requirements now? [y/N] ' reply
    case "$reply" in y|Y|yes|YES|Yes) ;; *) echo "Installation cancelled." >&2; exit 1 ;; esac
    run_as_root apt-get update
    run_as_root apt-get install -y "${missing_packages[@]}"
    missing_packages=()
    check_requirements
    (( ${#missing_packages[@]} == 0 )) || { printf 'Requirements still missing:\n  %s\n' "${missing_packages[@]}" >&2; exit 1; }
fi

if [[ "$MODE" == install ]]; then
    require_no_active_infiltratorfs_mounts
fi

printf 'InfiltratorFS %s native local-machine compilation\n' "$VERSION"
printf 'Debian package identity: infiltratorfs %s\n' "$NATIVE_PACKAGE_VERSION"

rm -rf "$BUILD_DIR"
cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -march=native -mtune=native"
cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure

rm -rf "$PACKAGE_DIR"
INFILTRATORFS_PACKAGE_VERSION="$NATIVE_PACKAGE_VERSION" \
INFILTRATORFS_BUILD_IDENTITY=native-local \
INFILTRATORFS_EMIT_RUN=0 \
    bash "$ROOT/packaging/build-linux-packages.sh" "$BUILD_DIR" "$PACKAGE_DIR"

ARCH="$(dpkg --print-architecture)"
PACKAGE="$PACKAGE_DIR/infiltratorfs_${NATIVE_PACKAGE_VERSION}_${ARCH}.deb"
[[ -s "$PACKAGE" ]] || { echo "Native Debian package was not produced: $PACKAGE" >&2; exit 1; }
[[ "$(dpkg-deb -f "$PACKAGE" Package)" == infiltratorfs ]] || { echo 'Native package name is invalid.' >&2; exit 1; }
[[ "$(dpkg-deb -f "$PACKAGE" Version)" == "$NATIVE_PACKAGE_VERSION" ]] || { echo 'Native package version is invalid.' >&2; exit 1; }
[[ "$(dpkg-deb -f "$PACKAGE" X-InfiltratorFS-Build)" == native-local ]] || { echo 'Native package build identity is invalid.' >&2; exit 1; }

if [[ "$MODE" == build-only ]]; then
    mkdir -p "$(dirname "$OUTPUT_PATH")"
    install -m 0644 "$PACKAGE" "$OUTPUT_PATH"
    printf 'Native InfiltratorFS Debian package created: %s\n' "$OUTPUT_PATH"
    exit 0
fi

(
    cd "$PACKAGE_DIR"
    run_as_root apt-get install -y "./$(basename "$PACKAGE")"
)
refresh_desktop_storage

installed_version="$(dpkg-query -W -f='${Version}' infiltratorfs 2>/dev/null || true)"
installed_build="$(dpkg-query -W -f='${X-InfiltratorFS-Build}' infiltratorfs 2>/dev/null || true)"
[[ "$installed_version" == "$NATIVE_PACKAGE_VERSION" ]] || {
    printf 'Native package installation verification failed: expected %s, found %s\n' "$NATIVE_PACKAGE_VERSION" "$installed_version" >&2
    exit 1
}
[[ "$installed_build" == native-local ]] || {
    printf 'Native package build identity verification failed: %s\n' "$installed_build" >&2
    exit 1
}
grep -qw infiltratorfs /proc/filesystems || {
    echo 'InfiltratorFS native VFS driver is not registered after package installation.' >&2
    exit 1
}

printf '\nInfiltratorFS native installation complete.\n'
printf 'Installed package: infiltratorfs %s\n' "$installed_version"
printf 'Build: Native / local machine compile\n'
printf 'Kernel: %s\n' "$KERNEL_RELEASE"
printf 'Driver: Native Linux VFS / DKMS\n'
printf 'APT owns the installation and will offer only a genuinely newer release.\n'
printf 'Launch "InfiltratorFS Manager" from the application menu.\n'
