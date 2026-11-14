#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Boot a real EFI-stub kernel and initramfs stored only on an InfiltratorFS
# image. Every firmware, disk and filesystem object here is a disposable file.
set -euo pipefail
boot=$(realpath "${1:?BOOTX64.EFI required}")
kernel=$(realpath "${2:?small EFI-stub kernel required}")
work=$(realpath -m "${3:?output directory required}")
root=$(cd "$(dirname "$0")/.." && pwd)
ovmf=${OVMF_DIR:-/usr/share/OVMF}
mkdir -p "$work/ram"/{bin,dev,proc}
cp "$(command -v busybox)" "$work/ram/bin/busybox"
ln -sf busybox "$work/ram/bin/sh"
cat > "$work/ram/init" <<'GUEST'
#!/bin/sh
/bin/busybox mount -t devtmpfs devtmpfs /dev
exec </dev/ttyS0 >/dev/ttyS0 2>&1
echo INFILTRATORFS_V2_EFI_KERNEL_PASS
/bin/busybox poweroff -f
GUEST
chmod +x "$work/ram/init"
(cd "$work/ram" && find . -print0 | cpio --null -o -H newc --quiet | gzip -1) > "$work/native.initrd"
cd "$root"
make -C module OUT="$work/module"
cc -std=c11 -O2 -ffunction-sections -fdata-sections \
    -Iinclude -Isrc/infiltratr-common/include -Iuefi/filesystems/lz4 \
    -Wl,--gc-sections tests/format-019-image.c \
    src/checksum.c src/locator.c src/status.c src/storage.c src/security.c \
    src/fs.c src/volume.c src/format_volume.c \
    src/infiltratr-common/src/arithmetic.c src/infiltratr-common/src/core.c \
    uefi/filesystems/lz4/lz4.c -o "$work/make-image"
"$work/make-image" --emit-boot-image "$work/infiltratorfs.img" "$kernel" "$work/native.initrd"
truncate -s 64M "$work/esp.img"
mkfs.fat -F32 "$work/esp.img" >/dev/null
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT ::/EFI/BOOT/filesystems_x64
mcopy -i "$work/esp.img" "$boot" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$work/esp.img" "$work/module/InfiltratorFS.so" ::/EFI/BOOT/filesystems_x64/InfiltratorFS.so
truncate -s 96M "$work/disk.img"
sgdisk --clear --new=1:2048:+64M --typecode=1:ef00 \
    --new=2:133120:+16M --typecode=2:8300 "$work/disk.img" >/dev/null
uuid=$(sgdisk --info=2 "$work/disk.img" | awk '/Partition unique GUID: / {print $4}')
[[ $uuid =~ ^[[:xdigit:]]{8}(-[[:xdigit:]]{4}){3}-[[:xdigit:]]{12}$ ]]
cat > "$work/boot.conf" <<BOOT
timeout 1
textonly true
use_nvram false
driver_diagnostics true
scanfor manual
menuentry "InfiltratorFS Common V2" {
    volume $uuid
    filesystem_module infiltratorfs
    loader \\native.img
    initrd \\native.initrd
    linux_load memory
    ostype Linux
    options "console=ttyS0 rdinit=/init panic=1"
}
BOOT
mcopy -i "$work/esp.img" "$work/boot.conf" ::/EFI/BOOT/boot.conf
dd if="$work/esp.img" of="$work/disk.img" bs=1M seek=1 conv=notrunc status=none
dd if="$work/infiltratorfs.img" of="$work/disk.img" bs=1M seek=65 conv=notrunc status=none
cp "$ovmf/OVMF_VARS_4M.fd" "$work/vars.fd"
qemu=(qemu-system-x86_64 -machine q35,accel=tcg -cpu max -m 768 -display none
    -monitor none -net none -no-reboot -serial "file:$work/serial.log"
    -drive "if=pflash,format=raw,readonly=on,file=$ovmf/OVMF_CODE_4M.fd"
    -drive "if=pflash,format=raw,file=$work/vars.fd"
    -drive "file=$work/disk.img,format=raw,if=none,id=disk"
    -device nvme,drive=disk,serial=infilv2)
if [[ -n ${QEMU_DATA_DIR:-} ]]; then qemu+=(-L "$QEMU_DATA_DIR"); fi
if ! timeout 120 "${qemu[@]}" || ! grep -Fq 'INFILTRATORFS_V2_EFI_KERNEL_PASS' "$work/serial.log"; then
    tail -n 70 "$work/serial.log" >&2; exit 1
fi
grep -Fq 'validated InfiltratorFS.so' "$work/serial.log"
grep -Fq "close /native.img: $(stat -c %s "$kernel") bytes read through module" "$work/serial.log"
grep -Fq "close /native.initrd: $(stat -c %s "$work/native.initrd") bytes read through module" "$work/serial.log"
dd if="$work/disk.img" bs=1M skip=65 count=16 status=none | cmp - "$work/infiltratorfs.img"
echo INFILTRATORFS_V2_EFI_KERNEL_PASS
