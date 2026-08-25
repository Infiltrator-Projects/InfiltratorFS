#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT}/build-native"
VERSION="$(sed -n 's/^project(InfiltratorFS VERSION \([^ ]*\) LANGUAGES C)$/\1/p' "$ROOT/CMakeLists.txt")"
KERNEL_RELEASE="$(uname -r)"
DKMS_SOURCE="/usr/src/infiltratorfs-${VERSION}"

declare -a missing_packages=()

if [[ -z "$VERSION" ]]; then
    echo "Could not determine InfiltratorFS version from CMakeLists.txt" >&2
    exit 1
fi

add_missing_package() {
    local candidate existing
    candidate="$1"
    for existing in "${missing_packages[@]:-}"; do
        [[ "$existing" == "$candidate" ]] && return 0
    done
    missing_packages+=("$candidate")
}

check_requirements() {
    command -v cc >/dev/null 2>&1 || add_missing_package build-essential
    command -v make >/dev/null 2>&1 || add_missing_package build-essential
    command -v cmake >/dev/null 2>&1 || add_missing_package cmake
    command -v pkg-config >/dev/null 2>&1 || add_missing_package pkg-config
    command -v fusermount3 >/dev/null 2>&1 || add_missing_package fuse3
    command -v pkexec >/dev/null 2>&1 || add_missing_package policykit-1
    command -v lsblk >/dev/null 2>&1 || add_missing_package util-linux
    command -v findmnt >/dev/null 2>&1 || add_missing_package util-linux
    command -v mountpoint >/dev/null 2>&1 || add_missing_package util-linux
    command -v xdg-open >/dev/null 2>&1 || add_missing_package xdg-utils
    command -v zenity >/dev/null 2>&1 || add_missing_package zenity
    command -v dkms >/dev/null 2>&1 || add_missing_package dkms
    command -v modprobe >/dev/null 2>&1 || add_missing_package kmod
    command -v depmod >/dev/null 2>&1 || add_missing_package kmod
    command -v update-desktop-database >/dev/null 2>&1 ||
        add_missing_package desktop-file-utils
    if ! command -v pkg-config >/dev/null 2>&1 ||
        ! pkg-config --exists fuse3; then
        add_missing_package libfuse3-dev
    fi
    if [[ ! -f "/lib/modules/${KERNEL_RELEASE}/build/Makefile" ]]; then
        add_missing_package "linux-headers-${KERNEL_RELEASE}"
    fi
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
    printf '  make -C %q KDIR=%q\n' "$ROOT/kernel" "/lib/modules/${KERNEL_RELEASE}/build"
    printf '  sudo install DKMS source under %q\n' "$DKMS_SOURCE"
    printf '  sudo dkms add -m infiltratorfs -v %q\n' "$VERSION"
    printf '  sudo dkms build -m infiltratorfs -v %q -k %q\n' "$VERSION" "$KERNEL_RELEASE"
    printf '  sudo dkms install -m infiltratorfs -v %q -k %q --force\n' "$VERSION" "$KERNEL_RELEASE"
}

run_as_root() {
    if (( EUID == 0 )); then
        "$@"
    else
        sudo "$@"
    fi
}

remove_stale_dkms_versions() {
    local line head old_version
    declare -A seen=()

    command -v dkms >/dev/null 2>&1 || return 0
    while IFS= read -r line; do
        [[ "$line" == infiltratorfs/* ]] || continue
        head="${line%%,*}"
        head="${head%%:*}"
        old_version="${head#infiltratorfs/}"
        [[ -n "$old_version" && "$old_version" != "$VERSION" ]] || continue
        [[ -z "${seen[$old_version]:-}" ]] || continue
        seen[$old_version]=1

        printf 'Removing stale InfiltratorFS DKMS registration %s before package/header changes.\n' "$old_version"
        run_as_root dkms remove -m infiltratorfs -v "$old_version" --all || true
        run_as_root rm -rf "/usr/src/infiltratorfs-${old_version}" "/var/lib/dkms/infiltratorfs/${old_version}" || true
    done < <(dkms status -m infiltratorfs 2>/dev/null || true)
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
MAKE[0]="make KDIR=/lib/modules/\${kernelver}/build"
CLEAN="make KDIR=/lib/modules/\${kernelver}/build clean"
EOF

    make -C "$ROOT/kernel" KDIR="/lib/modules/${KERNEL_RELEASE}/build"

    if dkms status -m infiltratorfs -v "$VERSION" 2>/dev/null | grep -q .; then
        run_as_root dkms remove -m infiltratorfs -v "$VERSION" --all || true
    fi

    run_as_root rm -rf "$DKMS_SOURCE"
    run_as_root install -d "$DKMS_SOURCE"
    run_as_root install -m 0644 "$ROOT/kernel/Makefile" "$DKMS_SOURCE/Makefile"
    run_as_root install -m 0644 "$ROOT/kernel/infiltratorfs.c" "$DKMS_SOURCE/infiltratorfs.c"
    run_as_root install -m 0644 "$ROOT/kernel/infiltratorfs_format.h" "$DKMS_SOURCE/infiltratorfs_format.h"
    run_as_root install -m 0644 "$dkms_conf" "$DKMS_SOURCE/dkms.conf"

    run_as_root dkms add -m infiltratorfs -v "$VERSION"
    run_as_root dkms build -m infiltratorfs -v "$VERSION" -k "$KERNEL_RELEASE"
    run_as_root dkms install -m infiltratorfs -v "$VERSION" -k "$KERNEL_RELEASE" --force
    run_as_root depmod -a
}

check_requirements

if [[ "${1:-}" == '--dry-run' ]]; then
    printf 'InfiltratorFS native build installer\n'
    printf 'Dry run only; no packages will be installed and no files will be changed.\n'
    printf 'Older InfiltratorFS DKMS registrations would be removed before any package/header installation.\n'
    if (( ${#missing_packages[@]} > 0 )); then
        printf 'Missing build or runtime requirements:\n'
        printf '  %s\n' "${missing_packages[@]}"
        printf 'Commands that would install them:\n'
        print_package_commands
    else
        printf 'All required build and runtime packages are already available.\n'
    fi
    print_build_commands
    print_kernel_commands
    exit 0
fi

if (( $# > 0 )); then
    echo "Usage: bootstrap.sh [--dry-run]" >&2
    exit 2
fi

remove_stale_dkms_versions

printf 'InfiltratorFS native build installer\n'
printf 'Source: %s\n' "$ROOT"

if (( ${#missing_packages[@]} > 0 )); then
    printf 'Missing build or runtime requirements:\n'
    printf '  %s\n' "${missing_packages[@]}"
    printf 'The installer needs to run:\n'
    print_package_commands

    if [[ ! -t 0 ]]; then
        echo "Cannot request permission to install packages without an interactive terminal." >&2
        exit 1
    fi
    read -r -p 'Install the missing requirements now? [y/N] ' reply
    case "$reply" in
        y|Y|yes|YES|Yes)
            ;;
        *)
            echo "Installation cancelled." >&2
            exit 1
            ;;
    esac

    if ! command -v apt-get >/dev/null 2>&1; then
        echo "apt-get is unavailable; install the listed packages manually." >&2
        exit 1
    fi
    if (( EUID == 0 )); then
        apt-get update
        apt-get install -y "${missing_packages[@]}"
    else
        if ! command -v sudo >/dev/null 2>&1; then
            echo "sudo is unavailable; install the listed packages manually." >&2
            exit 1
        fi
        sudo apt-get update
        sudo apt-get install -y "${missing_packages[@]}"
    fi

    missing_packages=()
    check_requirements
    if (( ${#missing_packages[@]} > 0 )); then
        printf 'Requirements are still missing after installation:\n' >&2
        printf '  %s\n' "${missing_packages[@]}" >&2
        exit 1
    fi
fi

cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure

if (( EUID == 0 )); then
    cmake --install "$BUILD_DIR" --prefix /usr
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
    fi
else
    sudo cmake --install "$BUILD_DIR" --prefix /usr
    if command -v update-desktop-database >/dev/null 2>&1; then
        sudo update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
    fi
fi

install_kernel_module

printf '\nInfiltratorFS native compilation and installation complete.\n'
printf 'The read-only native Linux module is installed through DKMS for kernel %s.\n' "$KERNEL_RELEASE"
printf 'Launch "InfiltratorFS Manager" from the application menu.\n'
