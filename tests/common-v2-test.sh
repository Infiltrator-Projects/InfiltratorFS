#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
make -C module OUT="$work/module"
module="$work/module/InfiltratorFS.so"
readelf -d "$module" > "$work/dynamic"
if grep -Eq '\((NEEDED|TEXTREL)\)' "$work/dynamic"; then
    echo 'Firmware module must not depend on a host library or text relocation.' >&2; exit 1
fi
readelf -r "$module" > "$work/relocations"
if grep 'R_X86_64_' "$work/relocations" | grep -v 'R_X86_64_RELATIVE' > "$work/unexpected"; then
    cat "$work/unexpected" >&2; exit 1
fi
objdump -d "$module" > "$work/disassembly"
if grep -Eq '%(xmm|ymm|zmm|st\()' "$work/disassembly"; then
    echo 'Firmware module contains vector or floating-point registers.' >&2; exit 1
fi
cc -std=c11 -O2 -ffunction-sections -fdata-sections \
    -Iinclude -Isrc/infiltratr-common/include -Iuefi/filesystems/lz4 \
    -Wl,--gc-sections tests/format-019-image.c \
    src/checksum.c src/locator.c src/status.c src/storage.c src/security.c \
    src/fs.c src/volume.c src/format_volume.c \
    src/infiltratr-common/src/arithmetic.c src/infiltratr-common/src/core.c \
    uefi/filesystems/lz4/lz4.c -o "$work/make-image"
"$work/make-image" --emit-populated-image "$work/populated.img"
cc -std=c11 -Wall -Wextra -Werror -Imodule/common -Isrc/infiltratr-common/include \
    tests/common-v2-image.c -ldl -o "$work/test-image"
"$work/test-image" "$module" "$work/populated.img"
