#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root="${1:?repository root required}"
hook="$root/packaging/initramfs/infiltratorfs-hook"
build="$root/packaging/build-linux-packages.sh"
fsck_source="$root/tools/fsck.infiltratorfs.c"
cmake="$root/CMakeLists.txt"
pre="$root/packaging/debian/preinst.in"
post="$root/packaging/debian/postinst.in"
prerm="$root/packaging/debian/prerm.in"

bash -n "$hook" "$pre" "$post" "$prerm" "$build"
grep -Fq '. /usr/share/initramfs-tools/hook-functions' "$hook"
grep -Fq 'manual_add_modules infiltratorfs' "$hook"
grep -Fq 'copy_exec /usr/bin/infilfs-inspect /usr/bin' "$hook"
grep -Fq '59-infiltratorfs.rules' "$hook"
grep -Fq 'copy_exec /usr/sbin/fsck.infiltratorfs /usr/sbin' "$hook"
! grep -Fq '/usr/bin/infilfs-scrub' "$hook"

# fsck is one native executable. Plain invocation calls infs_check directly;
# --scrub calls the authoritative scrub APIs directly. There is no shell
# wrapper or standalone installed scrub program in this contract.
grep -Fq 'add_executable(fsck.infiltratorfs tools/fsck.infiltratorfs.c)' "$cmake"
grep -Fq 'install(TARGETS fsck.infiltratorfs' "$cmake"
! grep -Fq 'add_executable(infilfs-scrub ' "$cmake"
! grep -Fq 'tools/fsck.infiltratorfs ' "$cmake"
grep -Fq 'infs_check(&vol, &report)' "$fsck_source"
grep -Fq 'infs_scrub_with_progress(&vol, &report, scrub_progress, NULL)' "$fsck_source"
grep -Fq 'infs_scrub_online(&vol, &report)' "$fsck_source"
grep -Fq 'infs_snapshot_scrub(&vol, snapshot_name, &report)' "$fsck_source"
grep -Fq 'strcmp(arg, "--scrub")' "$fsck_source"
! grep -Fq 'exec' "$fsck_source"

# The retired standalone CLI must not remain in the source tree.
[[ ! -e "$root/tools/infilfs-scrub.c" ]]
[[ ! -e "$root/tools/fsck.infiltratorfs" ]]

grep -Fq 'packaging/debian/${maintainer}.in' "$build"
grep -Fq 'usr/share/initramfs-tools/hooks/infiltratorfs$' "$build"
grep -Fq 'root_active=1' "$pre"
grep -Fq 'keeping the live root driver mounted' "$pre"
grep -Fq 'removing existing DKMS registration $old_version before installing $version' "$pre"
! grep -Fq '[ "$old_version" != "$version" ]' "$pre"
grep -Fq 'offline_target=1' "$post"
grep -Fq '[ -f "/lib/modules/$running_kernel/build/Makefile" ]' "$post"
grep -Fq '[ -f "$modules_dir/build/Makefile" ]' "$post"
grep -Fq 'dkms build -m "$module" -v "$version" -k "$kernel"' "$post"
grep -Fq 'dkms install -m "$module" -v "$version" -k "$kernel" --force' "$post"
grep -Fq 'update-initramfs -u -k "$kernel"' "$post"
grep -Fq 'update-initramfs -c -k "$kernel"' "$post"
grep -Fq 'offline/chroot root target staged for kernel(s)' "$post"
grep -Fq 'live driver remains loaded until reboot' "$post"
grep -Fq '/run/reboot-required' "$post"
grep -Fq 'refusing to remove the filesystem package while / is mounted as InfiltratorFS' "$prerm"
grep -Fq 'MODULE_ALIAS_FS(INFILTRATORFS_NAME)' "$root/kernel/infiltratorfs_core.c"
grep -Fq '.get_inode_acl = infilfs_posix_acl_get' "$root/kernel/infiltratorfs_rw.inc"
grep -Fq '.set_acl = infilfs_posix_acl_set' "$root/kernel/infiltratorfs_rw.inc"
grep -Fq 'infilfs_ns_reserved_linux_meta_name' "$root/kernel/infiltratorfs_rw_namespace.inc"
echo 'root-volume integration policy: PASS'
