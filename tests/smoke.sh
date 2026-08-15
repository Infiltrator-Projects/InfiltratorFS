#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

BUILD_DIR=${1:-build}
IMAGE=${2:-/tmp/infilfs-smoke.img}

truncate -s 64M "$IMAGE"
"$BUILD_DIR/mkfs.infilfs" -L smoke-test "$IMAGE"
"$BUILD_DIR/infilfs-inspect" "$IMAGE"
rm -f "$IMAGE"
