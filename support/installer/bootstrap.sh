#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT}/build-native"

declare -a missing_packages=()

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
    command -v update-desktop-database >/dev/null 2>&1 ||
        add_missing_package desktop-file-utils
    if ! command -v pkg-config >/dev/null 2>&1 ||
        ! pkg-config --exists fuse3; then
        add_missing_package libfuse3-dev
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

check_requirements

if [[ "${1:-}" == '--dry-run' ]]; then
    printf 'InfiltratorFS native build installer\n'
    printf 'Dry run only; no packages will be installed and no files will be changed.\n'
    if (( ${#missing_packages[@]} > 0 )); then
        printf 'Missing build or runtime requirements:\n'
        printf '  %s\n' "${missing_packages[@]}"
        printf 'Commands that would install them:\n'
        print_package_commands
    else
        printf 'All required build and runtime packages are already available.\n'
    fi
    print_build_commands
    exit 0
fi

if (( $# > 0 )); then
    echo "Usage: bootstrap.sh [--dry-run]" >&2
    exit 2
fi

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

printf '\nInfiltratorFS native compilation and installation complete.\n'
printf 'Launch "InfiltratorFS Manager" from the application menu.\n'
