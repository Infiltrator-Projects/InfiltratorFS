#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

build_dir="${1:-build}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

truncate -s 64M "$tmp/portable.img"
"$build_dir/mkfs.infilfs" -L PortableCore "$tmp/portable.img" >/dev/null
"$build_dir/infilfs-portable-core" "$tmp/portable.img"
