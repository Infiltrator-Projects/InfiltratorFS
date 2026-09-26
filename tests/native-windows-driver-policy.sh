#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
driver="$root/windows-driver/infiltratorfs-native.c"
protocol="$root/windows-driver/infiltratorfs-native-protocol.h"
service="$root/tools/windows/infiltratorfs-native-service.c"
project="$root/windows-driver/infiltratorfs-native.vcxproj"
inf="$root/windows-driver/infiltratorfs-native.inf"
roadmap="$root/docs/ROADMAP.md"

fail() {
    printf 'native-windows-driver-policy: %s\n' "$*" >&2
    exit 1
}

for file in "$driver" "$protocol" "$service" "$project" "$inf"; do
    test -s "$file" || fail "missing native Windows driver component: $file"
done

grep -Fq 'NTSTATUS DriverEntry(' "$driver" || fail 'kernel filesystem driver has no DriverEntry'
grep -Fq 'IoRegisterFileSystem' "$driver" || fail 'kernel driver is not registered as a filesystem'
grep -Fq 'IRP_MN_MOUNT_VOLUME' "$driver" || fail 'kernel driver has no mount-volume path'
grep -Fq 'CcInitializeCacheMap' "$driver" || fail 'kernel driver does not integrate with Cache Manager'
grep -Fq 'CcCopyRead' "$driver" || fail 'cached read path is missing'
grep -Fq 'CcCopyWrite' "$driver" || fail 'cached write path is missing'
grep -Fq 'IRP_PAGING_IO' "$driver" || fail 'paging I/O path is missing'
grep -Fq 'IRP_MJ_QUERY_SECURITY' "$driver" || fail 'native security query dispatch is missing'
grep -Fq 'IRP_MJ_SET_SECURITY' "$driver" || fail 'native security mutation dispatch is missing'
grep -Fq 'INFILFS_WIN_NATIVE_OP_QUERY_SECURITY' "$service" || fail 'portable-core security query service is missing'
grep -Fq 'INFILFS_WIN_NATIVE_OP_SET_SECURITY' "$service" || fail 'portable-core security mutation service is missing'
grep -Fq 'IoBuildSynchronousFsdRequest' "$driver" || fail 'raw target-device I/O path is missing'
grep -Fq 'IOCTL_INFILFS_NATIVE_WAIT_REQUEST' "$driver" || fail 'kernel-to-portable-core request path is missing'
grep -Fq 'IOCTL_INFILFS_NATIVE_COMPLETE_REQUEST' "$service" || fail 'portable-core service completion path is missing'
grep -Fq 'infs_volume_open_storage' "$service" || fail 'native service does not use the authoritative portable core'
grep -Fq 'IOCTL_INFILFS_NATIVE_RAW_READ' "$service" || fail 'native service does not route raw reads through the driver'
grep -Fq 'IOCTL_INFILFS_NATIVE_RAW_WRITE' "$service" || fail 'native service does not route raw writes through the driver'
grep -Fq '<PlatformToolset>WindowsKernelModeDriver10.0</PlatformToolset>' "$project" || fail 'native driver project is not a WDK kernel-driver project'
grep -Fq 'Class=FileSystem' "$inf" || fail 'native driver INF is not a filesystem driver package'
grep -Fq 'ServiceType=2' "$inf" || fail 'native driver INF is not installing a filesystem driver service'

if grep -Eiq 'winfsp|dokan|fuse' "$driver" "$service" "$protocol"; then
    fail 'native Windows path acquired an external filesystem-framework dependency'
fi

grep -Fq -- '- [ ] Native Windows filesystem driver with Cache Manager/I/O Manager integration.' "$roadmap" || fail 'native driver must remain unchecked until WDK build and mounted qualification pass'

printf 'native-windows-driver-policy: PASS\n'
