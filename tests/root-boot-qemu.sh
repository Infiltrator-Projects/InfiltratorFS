#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-build-root-boot}"
module="${2:-kernel/infiltratorfs.ko}"
work="${RUNNER_TEMP:-/tmp}/infiltratorfs-root-boot"
disk="$work/root-boot.raw"
mnt="$work/mnt"
loop=""
qemu_pid=""

cleanup() {
    set +e
    [[ -n "$qemu_pid" ]] && kill -9 "$qemu_pid" 2>/dev/null
    sync
    for p in "$mnt/run" "$mnt/sys" "$mnt/proc" "$mnt/dev/pts" "$mnt/dev" \
             "$mnt/boot/efi" "$mnt/boot" "$mnt"; do
        mountpoint -q "$p" && umount -l "$p"
    done
    [[ -n "$loop" ]] && losetup -d "$loop" 2>/dev/null
    rmmod infiltratorfs 2>/dev/null
}
trap cleanup EXIT

[[ $EUID -eq 0 ]] || { echo "root boot qualification requires root" >&2; exit 2; }
for cmd in qemu-system-x86_64 qemu-img debootstrap sfdisk losetup mkfs.vfat mkfs.ext4 \
           grub-install chroot timeout; do command -v "$cmd" >/dev/null; done

rm -rf "$work"
mkdir -p "$work" "$mnt"
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --parallel
bash "$root/packaging/build-linux-packages.sh" "$build" "$work/base-dist"
INFILTRATORFS_PACKAGE_VERSION=0.18.44+rootci1 \
INFILTRATORFS_EMIT_RUN=0 \
bash "$root/packaging/build-linux-packages.sh" "$build" "$work/upgrade-dist"
base_deb="$(find "$work/base-dist" -name 'infiltratorfs_*.deb' -print -quit)"
upgrade_deb="$(find "$work/upgrade-dist" -name 'infiltratorfs_*.deb' -print -quit)"
[[ -s "$base_deb" && -s "$upgrade_deb" ]]

truncate -s 10G "$disk"
sfdisk "$disk" >/dev/null <<'EOF'
label: gpt
size=256M,type=U
size=768M,type=L
type=L
EOF
loop="$(losetup --find --show -P "$disk")"
mkfs.vfat -F32 "${loop}p1" >/dev/null
mkfs.ext4 -F "${loop}p2" >/dev/null
"$build/mkfs.infilfs" -L RootBoot "${loop}p3" >/dev/null

insmod "$module"
mount -t infiltratorfs "${loop}p3" "$mnt"
mkdir -p "$mnt/boot"
mount "${loop}p2" "$mnt/boot"
mkdir -p "$mnt/boot/efi"
mount "${loop}p1" "$mnt/boot/efi"

debootstrap --variant=minbase noble "$mnt" http://archive.ubuntu.com/ubuntu
cp "$base_deb" "$mnt/root/infiltratorfs-base.deb"
cp "$upgrade_deb" "$mnt/root/infiltratorfs-upgrade.deb"

mount --bind /dev "$mnt/dev"
mount --bind /dev/pts "$mnt/dev/pts"
mount -t proc proc "$mnt/proc"
mount -t sysfs sys "$mnt/sys"
mount --bind /run "$mnt/run"
cp /etc/resolv.conf "$mnt/etc/resolv.conf"

root_uuid="$(blkid -s UUID -o value "${loop}p3")"
boot_uuid="$(blkid -s UUID -o value "${loop}p2")"
efi_uuid="$(blkid -s UUID -o value "${loop}p1")"
cat >"$mnt/etc/fstab" <<EOF
UUID=$root_uuid / infiltratorfs defaults 0 0
UUID=$boot_uuid /boot ext4 defaults 0 2
UUID=$efi_uuid /boot/efi vfat umask=0077 0 1
EOF
echo infiltrator-root-ci > "$mnt/etc/hostname"
cat >"$mnt/etc/hosts" <<'EOF'
127.0.0.1 localhost
127.0.1.1 infiltrator-root-ci
EOF

chroot "$mnt" /bin/bash -eux <<'CHROOT'
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  linux-image-generic linux-headers-generic systemd-sysv initramfs-tools \
  grub-efi-amd64-bin grub2-common dkms kmod acl attr libcap2-bin rsync \
  ca-certificates passwd util-linux
dpkg -i /root/infiltratorfs-base.deb || apt-get -f install -y
dpkg -i /root/infiltratorfs-base.deb
cat >/etc/default/grub <<'EOF'
GRUB_DEFAULT=0
GRUB_TIMEOUT=1
GRUB_CMDLINE_LINUX_DEFAULT="console=ttyS0,115200n8"
GRUB_TERMINAL=console
EOF
grub-install --target=x86_64-efi --efi-directory=/boot/efi \
  --bootloader-id=InfiltratorRootCI --removable --no-nvram
update-initramfs -c -k all || update-initramfs -u -k all
update-grub
CHROOT

cat >"$mnt/usr/local/sbin/infiltrator-root-ci" <<'GUEST'
#!/bin/bash
set -euo pipefail
exec >/dev/ttyS0 2>&1
fstype="$(findmnt -n -o FSTYPE /)"
[[ "$fstype" == infiltratorfs ]] || { echo "ROOT_FSTYPE_FAIL:$fstype"; poweroff -f; }
phase="$(cat /var/lib/infiltrator-root-phase 2>/dev/null || echo 1)"
mkdir -p /var/lib/infiltrator-root-ci
if [[ "$phase" == 1 ]]; then
    echo "ROOT_BOOT_PASS"
    printf root-persistent >/var/lib/infiltrator-root-ci/persistent
    mkdir -p /home/root-ci /var/lib/infiltrator-root-ci/acl
    touch /var/lib/infiltrator-root-ci/acl/file
    chown 12345:23456 /var/lib/infiltrator-root-ci/acl/file
    chmod 0640 /var/lib/infiltrator-root-ci/acl/file
    setfacl -m u:12345:r-- /var/lib/infiltrator-root-ci/acl/file
    setfattr -n user.rootci -v yes /var/lib/infiltrator-root-ci/acl/file
    cp /bin/true /var/lib/infiltrator-root-ci/cap
    setcap cap_net_bind_service=ep /var/lib/infiltrator-root-ci/cap
    useradd -m rootcitest
    journalctl --sync || true

    mkdir -p /tmp/rootci-pkg/DEBIAN
    cat >/tmp/rootci-pkg/DEBIAN/control <<'EOF'
Package: rootci-workload
Version: 1.0
Architecture: all
Maintainer: CI <ci@example.invalid>
Description: InfiltratorFS root workload package
EOF
    mkdir -p /tmp/rootci-pkg/usr/share/rootci
    echo package-data >/tmp/rootci-pkg/usr/share/rootci/data
    dpkg-deb -b /tmp/rootci-pkg /tmp/rootci-workload.deb
    dpkg -i /tmp/rootci-workload.deb
    dpkg -r rootci-workload

    dpkg -i /root/infiltratorfs-upgrade.deb
    update-initramfs -u -k all
    dpkg --audit
    echo 2 >/var/lib/infiltrator-root-phase
    sync
    echo "ROOT_UPGRADE_STAGED_PASS"
    systemctl reboot
elif [[ "$phase" == 2 ]]; then
    [[ "$(dpkg-query -W -f='${Version}' infiltratorfs)" == "0.18.44+rootci1" ]]
    [[ "$(cat /var/lib/infiltrator-root-ci/persistent)" == root-persistent ]]
    getfacl -n /var/lib/infiltrator-root-ci/acl/file | grep -Eq '^user:12345:r--$'
    [[ "$(getfattr --only-values -n user.rootci /var/lib/infiltrator-root-ci/acl/file)" == yes ]]
    getcap /var/lib/infiltrator-root-ci/cap | grep -Fq cap_net_bind_service
    echo 3 >/var/lib/infiltrator-root-phase
    sync
    echo "ROOT_CRASH_READY"
    i=0
    while :; do
        printf '%08d\n' "$i" >>/var/lib/infiltrator-root-ci/crash-stream
        i=$((i+1))
        if (( i % 128 == 0 )); then sync; fi
    done
else
    [[ "$(findmnt -n -o FSTYPE /)" == infiltratorfs ]]
    [[ "$(dpkg-query -W -f='${Version}' infiltratorfs)" == "0.18.44+rootci1" ]]
    dpkg --audit
    [[ "$(cat /var/lib/infiltrator-root-ci/persistent)" == root-persistent ]]
    getfacl -n /var/lib/infiltrator-root-ci/acl/file | grep -Eq '^user:12345:r--$'
    getcap /var/lib/infiltrator-root-ci/cap | grep -Fq cap_net_bind_service
    echo "ROOT_RECOVERY_PASS"
    poweroff
fi
GUEST
chmod +x "$mnt/usr/local/sbin/infiltrator-root-ci"
cat >"$mnt/etc/systemd/system/infiltrator-root-ci.service" <<'EOF'
[Unit]
Description=InfiltratorFS root boot qualification
After=local-fs.target
Before=multi-user.target
[Service]
Type=oneshot
ExecStart=/usr/local/sbin/infiltrator-root-ci
TimeoutStartSec=0
[Install]
WantedBy=multi-user.target
EOF
ln -sf ../infiltrator-root-ci.service \
  "$mnt/etc/systemd/system/multi-user.target.wants/infiltrator-root-ci.service"

sync
for p in "$mnt/run" "$mnt/sys" "$mnt/proc" "$mnt/dev/pts" "$mnt/dev" \
         "$mnt/boot/efi" "$mnt/boot" "$mnt"; do
    mountpoint -q "$p" && umount "$p"
done
losetup -d "$loop"
loop=""
rmmod infiltratorfs

ovmf_code="$(find /usr/share/OVMF -maxdepth 1 -type f -name 'OVMF_CODE*.fd' | head -n1)"
ovmf_vars_template="$(find /usr/share/OVMF -maxdepth 1 -type f -name 'OVMF_VARS*.fd' | head -n1)"
[[ -s "$ovmf_code" && -s "$ovmf_vars_template" ]]
cp "$ovmf_vars_template" "$work/OVMF_VARS.fd"

boot_once() {
    local log="$1"
    : >"$log"
    qemu-system-x86_64 -machine q35,accel=tcg -m 2048 -smp 2 \
      -nographic -serial mon:stdio \
      -drive if=pflash,format=raw,readonly=on,file="$ovmf_code" \
      -drive if=pflash,format=raw,file="$work/OVMF_VARS.fd" \
      -drive format=raw,file="$disk",if=virtio \
      -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
      >"$log" 2>&1 &
    qemu_pid=$!
}

boot_once "$work/boot12.log"
for _ in $(seq 1 360); do
    grep -Fq ROOT_CRASH_READY "$work/boot12.log" && break
    kill -0 "$qemu_pid" 2>/dev/null || { cat "$work/boot12.log"; exit 1; }
    sleep 2
done
grep -Fq ROOT_BOOT_PASS "$work/boot12.log"
grep -Fq ROOT_UPGRADE_STAGED_PASS "$work/boot12.log"
grep -Fq ROOT_CRASH_READY "$work/boot12.log"
kill -9 "$qemu_pid"
wait "$qemu_pid" 2>/dev/null || true
qemu_pid=""

loop="$(losetup --find --show -P "$disk")"
"$build/infilfs-scrub" "${loop}p3" | tee "$work/post-crash-scrub.txt"
grep -Fq 'Result:              CLEAN' "$work/post-crash-scrub.txt"
losetup -d "$loop"; loop=""

boot_once "$work/boot3.log"
for _ in $(seq 1 240); do
    grep -Fq ROOT_RECOVERY_PASS "$work/boot3.log" && break
    kill -0 "$qemu_pid" 2>/dev/null || break
    sleep 2
done
grep -Fq ROOT_RECOVERY_PASS "$work/boot3.log" || {
    cat "$work/boot3.log"
    exit 1
}
wait "$qemu_pid" 2>/dev/null || true
qemu_pid=""

loop="$(losetup --find --show -P "$disk")"
"$build/infilfs-scrub" "${loop}p3" | tee "$work/final-scrub.txt"
grep -Fq 'Result:              CLEAN' "$work/final-scrub.txt"
echo 'Real UEFI InfiltratorFS root boot/upgrade/crash/recovery qualification: PASS'
