#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

build_dir="${1:-build}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkfs="$build_dir/mkfs.infilfs"
inspect="$build_dir/infilfs-inspect"
rules="$repo_root/packaging/59-infiltratorfs.rules"
udisks_patch="$repo_root/packaging/udisks2-infiltratorfs-display.patch"
libblockdev_patch="$repo_root/packaging/libblockdev-infiltratorfs.patch"
gnome_disks_patch="$repo_root/packaging/gnome-disks-infiltratorfs.patch"
mkfs_alias="$repo_root/tools/mkfs.infiltratorfs"
bootstrap="$repo_root/support/installer/bootstrap.sh"
os_helper="$repo_root/packaging/infiltratorfs-os-integration"
mint_patcher="$repo_root/packaging/patch-mintstick.py"
noble_libblockdev_patch="$repo_root/packaging/libblockdev-3.1-infiltratorfs.patch"
noble_gnome_patch="$repo_root/packaging/gnome-disks-46-infiltratorfs.patch"
noble_bundle_builder="$repo_root/packaging/build-noble-desktop-integration.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

image="$tmp/desktop.img"
plain="$tmp/plain.img"
probe="$tmp/probe.txt"

truncate -s 64M "$image"
"$mkfs" -L 'Desktop Test' "$image" >/dev/null
"$inspect" --udev "$image" > "$probe"

grep -Fxq 'ID_FS_USAGE=filesystem' "$probe"
grep -Fxq 'ID_FS_TYPE=infiltratorfs' "$probe"
grep -Fxq 'ID_FS_VERSION=0.17' "$probe"
grep -Fxq 'ID_FS_LABEL=Desktop Test' "$probe"
grep -Fxq 'ID_FS_BLOCK_SIZE=4096' "$probe"
grep -Eq '^ID_FS_UUID=[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$' "$probe"

# Standalone mkfs.infilfs and the portable formatter must advertise the same
# current Format 0.17 compression capability. Native mounted IAC1 is gated
# by incompatible feature bit 0x1000, so assert it from the actual checkpoint.
python3 - "$image" <<'PY'
import struct
import sys

with open(sys.argv[1], "rb") as image:
    checkpoint = image.read(4096)
flags = struct.unpack_from("<Q", checkpoint, 148)[0]
assert flags & 0x1000, hex(flags)
PY

truncate -s 64M "$plain"
if "$inspect" --udev "$plain" > "$tmp/plain-probe.txt" 2>/dev/null; then
    echo 'desktop-integration: non-InfiltratorFS image was identified' >&2
    exit 1
fi
[[ ! -s "$tmp/plain-probe.txt" ]]

grep -Fq 'ID_FS_TYPE}=="infiltratorfs"' "$rules"
grep -Fq 'UDISKS_MOUNT_OPTIONS_DEFAULTS' "$rules"
grep -Fq 'modprobe -q infiltratorfs' "$rules"
grep -Fq 'ENV{ID_FS_LABEL}=="", ENV{UDISKS_NAME}="InfiltratorFS"' "$rules"
grep -Fq 'UDISKS_ICON_NAME}="drive-harddisk"' "$rules"
grep -Fq 'UDISKS_SYMBOLIC_ICON_NAME}="drive-harddisk-symbolic"' "$rules"

# The installer refreshes the block-device udev database before returning.
# Because the udev rule probes InfiltratorFS using a shared advisory lock,
# require the installer to wait for those probes to drain before callers can
# immediately invoke mkfs on the same device.
grep -Fq 'udevadm trigger --subsystem-match=block --action=change' "$bootstrap"
grep -Fq 'udevadm settle --timeout=30' "$bootstrap"

# libudisks2 currently owns the long human-readable filesystem description
# consumed by GNOME Disks.  Keep the minimal upstream mapping in-tree so the
# project cannot regress into pretending that udev metadata alone controls the
# "Contents" string.
test -s "$udisks_patch"
grep -Fq '"filesystem", "infiltratorfs",     "*"' "$udisks_patch"
grep -Fq 'InfiltratorFS (format %s)' "$udisks_patch"
grep -Fq '"filesystem", "infiltratorfs",     NULL' "$udisks_patch"

# Formatting requires all three desktop-stack boundaries. libblockdev owns
# capability discovery and mkfs execution, GNOME Disks owns its explicit
# "Other" list, and libudisks2 owns the human-readable contents label.
test -x "$mkfs_alias"
test -s "$libblockdev_patch"
test -s "$gnome_disks_patch"
grep -Fq 'BD_FS_TECH_INFILTRATORFS' "$libblockdev_patch"
grep -Fq 'mkfs.infiltratorfs' "$libblockdev_patch"
grep -Fq '"--force"' "$libblockdev_patch"
grep -Fq 'options->label' "$libblockdev_patch"
grep -Fq 'GDU_OTHER_FS_TYPE_INFILTRATORFS' "$gnome_disks_patch"
grep -Fq '"infiltratorfs"' "$gnome_disks_patch"

# Public Ubuntu 24.04 / Linux Mint 22.x packages carry ABI-matched downstream
# desktop binaries and restore the distro originals with dpkg-divert.
for integration_file in "$os_helper" "$mint_patcher" "$noble_libblockdev_patch" \
                        "$noble_gnome_patch" "$noble_bundle_builder"; do
    test -s "$integration_file"
done
bash -n "$os_helper"
bash -n "$noble_bundle_builder"
python3 - "$mint_patcher" <<'PY'
import ast
import pathlib
import sys
ast.parse(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
PY
grep -Fq 'dpkg-divert --package "$OWNER" --add --rename' "$os_helper"
grep -Fq 'dpkg-divert --package "$OWNER" --remove --rename' "$os_helper"
grep -Fq 'restart_udisks' "$os_helper"
grep -Fq 'libblockdev-fs3' "$os_helper"
grep -Fq 'src/disks/gducreateotherpage.c' "$noble_gnome_patch"
grep -Fq '{"infiltratorfs", N_("InfiltratorFS' "$noble_gnome_patch"
grep -Fq 'src/disks/gduwindow.c' "$noble_gnome_patch"
grep -Fq 'src/disks/gduvolumegrid.c' "$noble_gnome_patch"
grep -Fq 'InfiltratorFS (format %s)' "$noble_gnome_patch"
grep -Fq 'BD_FS_TECH_INFILTRATORFS' "$noble_libblockdev_patch"

# Mintstick is patched from its locally installed stock files.  Exercise the
# exact upstream filesystem, device-name and command-execution anchors so
# digit-ending block devices cannot regress to a false-success format.
cat > "$tmp/mintstick.py" <<'MINT_UI'
            self.fsmodel.append(["fat32", "FAT32", 11, True, True])
            self.fsmodel.append(["exfat", "exFAT", 11, False, False])
            self.fsmodel.append(["ntfs", "NTFS", 32, False, False])
            self.fsmodel.append(["ext4", "EXT4", 16, False, False])
                            name = block.get_property('device')
                            name = ''.join([i for i in name if not i.isdigit()])
            usb_path = ''.join([i for i in a if not i.isdigit()])
MINT_UI
cat > "$tmp/raw_format.py" <<'MINT_FORMAT'
def execute(command):
    syslog.syslog(str(command))
    call(command)
    call(["sync"])

    partition_path = "%s1" % device_path
    if fstype == "fat32":
        partition_type = "fat32"
    elif fstype == "exfat":
        partition_type = "ntfs"
    elif fstype == "ntfs":
        partition_type = "ntfs"
    elif fstype == "ext4":
        partition_type = "ext4"

    execute(["parted", device_path, "mkpart", "primary", partition_type, "1MiB", "100%"])

    # Call wipefs on the new partitions to avoid problems with old filesystem signatures
    if fstype == "fat32":
        pass
    elif fstype == "ext4":
        execute(["mkfs.ext4", "-E", "root_owner=%s:%s" % (uid, gid), "-L", volume_label, partition_path])

    parser.add_argument("-f", "--filesystem", help="File system type", action="store",
                        type=str, choices=("fat32", "exfat", "ntfs", "ext4"), required=True)
MINT_FORMAT
python3 "$mint_patcher" "$tmp/mintstick.py" "$tmp/raw_format.py" \
    "$tmp/mintstick-patched.py" "$tmp/raw-format-patched.py"
grep -Fq '["infiltratorfs", "InfiltratorFS", 63, False, False]' "$tmp/mintstick-patched.py"
grep -Fq "name.startswith(('/dev/mmcblk', '/dev/nvme'))" "$tmp/mintstick-patched.py"
grep -Fq "name = name.rstrip('0123456789')" "$tmp/mintstick-patched.py"
grep -Fq "a.startswith(('/dev/mmcblk', '/dev/nvme'))" "$tmp/mintstick-patched.py"
grep -Fq "usb_path = a.rstrip('0123456789')" "$tmp/mintstick-patched.py"
grep -Fq 'partition_suffix = "p1" if device_path[-1].isdigit() else "1"' "$tmp/raw-format-patched.py"
grep -Fq 'Command failed with exit status' "$tmp/raw-format-patched.py"
grep -Fq 'execute(["partprobe", device_path])' "$tmp/raw-format-patched.py"
grep -Fq 'execute(["udevadm", "settle", "--timeout=10"])' "$tmp/raw-format-patched.py"
grep -Fq 'mkfs.infiltratorfs", "--force", "-L"' "$tmp/raw-format-patched.py"
grep -Fq '"ext4", "infiltratorfs"' "$tmp/raw-format-patched.py"

# Linux Mint 22.x packages can still carry the pre-August-2026 Mintstick
# partition start spelling ("1").  It must patch just as safely as current
# upstream's "1MiB" spelling so package upgrades do not strand dpkg.
sed 's/partition_type, "1MiB", "100%"/partition_type, "1", "100%"/' \
    "$tmp/raw_format.py" > "$tmp/raw-format-legacy.py"
python3 "$mint_patcher" "$tmp/mintstick.py" "$tmp/raw-format-legacy.py" \
    "$tmp/mintstick-legacy-patched.py" "$tmp/raw-format-legacy-patched.py"
grep -Fq 'partition_type, "1", "100%"' "$tmp/raw-format-legacy-patched.py"
grep -Fq 'execute(["partprobe", device_path])' "$tmp/raw-format-legacy-patched.py"
grep -Fq 'execute(["udevadm", "settle", "--timeout=10"])' "$tmp/raw-format-legacy-patched.py"

# Prove the conventional helper expected by libblockdev forwards every
# argument byte-for-byte to the qualified formatter installed beside it.
cp "$mkfs_alias" "$tmp/mkfs.infiltratorfs"
cat > "$tmp/mkfs.infilfs" <<'MOCK_MKFS'
#!/usr/bin/env bash
printf '%s\n' "$@" > "$INFILFS_MKFS_ALIAS_ARGS"
MOCK_MKFS
chmod 0755 "$tmp/mkfs.infiltratorfs" "$tmp/mkfs.infilfs"
INFILFS_MKFS_ALIAS_ARGS="$tmp/mkfs-alias.args" \
    "$tmp/mkfs.infiltratorfs" "$image" --force -L 'GNOME Disks Label'
mapfile -t alias_args < "$tmp/mkfs-alias.args"
expected_alias_args=("$image" --force -L 'GNOME Disks Label')
[[ "${alias_args[*]}" == "${expected_alias_args[*]}" ]]

echo 'desktop-integration: PASS'
