#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT}/build"

printf 'InfiltratorFS native build installer\n'
printf 'Source: %s\n' "$ROOT"

for command in cmake ctest pkg-config; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Missing required command: $command" >&2
        exit 1
    fi
done

if ! pkg-config --exists fuse3; then
    echo "Missing libfuse3 development files." >&2
    echo "Install fuse3 and libfuse3-dev before building." >&2
    exit 1
fi

cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure

sudo cmake --install "$BUILD_DIR" --prefix /usr

printf '\nInfiltratorFS native installation complete.\n'
