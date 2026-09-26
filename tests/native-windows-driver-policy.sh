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
grep -Fq 'FileLinkInformation' "$driver" || fail 'native hard-link set-information path is missing'
grep -Fq 'FileBasicInformation' "$driver" || fail 'native basic metadata set-information path is missing'
grep -Fq 'INFILFS_WIN_NATIVE_OP_SET_BASIC' "$service" || fail 'portable-core basic metadata mutation path is missing'
grep -Fq 'INFILFS_WIN_NATIVE_OP_LINK' "$service" || fail 'portable-core hard-link service path is missing'
grep -Fq 'ObReferenceObjectByHandle' "$driver" || fail 'root-relative rename/link target resolution is missing'
grep -Fq 'IRP_MJ_SHUTDOWN' "$driver" || fail 'shutdown durability dispatch is missing'
grep -Fq 'IoRegisterShutdownNotification' "$driver" || fail 'filesystem is not registered for shutdown durability'
grep -Fq 'IRP_MJ_QUERY_SECURITY' "$driver" || fail 'native security query dispatch is missing'
grep -Fq 'IRP_MJ_SET_SECURITY' "$driver" || fail 'native security mutation dispatch is missing'
grep -Fq 'INFILFS_WIN_NATIVE_OP_QUERY_SECURITY' "$service" || fail 'portable-core security query service is missing'
grep -Fq 'INFILFS_WIN_NATIVE_OP_SET_SECURITY' "$service" || fail 'portable-core security mutation service is missing'
grep -Fq 'IoBuildSynchronousFsdRequest' "$driver" || fail 'raw target-device I/O path is missing'
grep -Fq 'IOCTL_INFILFS_NATIVE_WAIT_REQUEST' "$driver" || fail 'kernel-to-portable-core request path is missing'
grep -Fq 'IOCTL_INFILFS_NATIVE_COMPLETE_REQUEST' "$service" || fail 'portable-core service completion path is missing'
grep -Fq 'Signal while QueueLock still protects Item lifetime' "$driver" || fail 'request completion is not lifetime-safe against timeout'
grep -Fq 'ReclaimSemaphore = Item->Active ? FALSE : TRUE' "$driver" || fail 'timed-out pending requests can leak semaphore credits'
grep -Fq 'Timeout.QuadPart = -(LONGLONG)1 * 10 * 1000 * 1000' "$driver" || fail 'service wait is still unbounded'
grep -Fq 'InfilfsAbortServiceRequests' "$driver" || fail 'driver unload does not wake queued service callers'
grep -Fq 'InfilfsQueryPortableVolumeState' "$driver" || fail 'volume information is not sourced from authoritative portable state'
grep -Fq 'State.free_blocks' "$driver" || fail 'native Windows free-space reporting is not authoritative'
grep -Fq 'INFILFS_WIN_NATIVE_OP_QUERY_VOLUME' "$service" || fail 'portable service volume-state query is missing'
grep -Fq 'native_worker_budget' "$service" || fail 'native Windows worker pool is not derived from physical-core N-1 policy'
grep -Fq 'IOCTL_DISK_IS_WRITABLE' "$driver" || fail 'native Windows mount does not probe target writability'
grep -Fq 'Volume->ReadOnly = InfilfsTargetReadOnly(Target)' "$driver" || fail 'native Windows volume ignores lower-device read-only state'
grep -Fq 'Info->Characteristics |= FILE_READ_ONLY_DEVICE' "$driver" || fail 'native Windows read-only state is not reported through FileFsDeviceInformation'
grep -Fq 'Info->SupportsObjects = FALSE' "$driver" || fail 'native Windows advertises object-ID support without object-ID FSCTLs'
if grep -Fq 'FILE_SUPPORTS_REPARSE_POINTS' "$driver" ||
   grep -Fq 'FILE_SUPPORTS_SPARSE_FILES' "$driver"; then
    fail 'driver advertises Windows FSCTL contracts it does not implement'
fi
grep -Fq 'infs_volume_open_storage' "$service" || fail 'native service does not use the authoritative portable core'
grep -Fq 'IOCTL_INFILFS_NATIVE_RAW_READ' "$service" || fail 'native service does not route raw reads through the driver'
grep -Fq 'IOCTL_INFILFS_NATIVE_RAW_WRITE' "$service" || fail 'native service does not route raw writes through the driver'
grep -Fq '<PlatformToolset>WindowsKernelModeDriver10.0</PlatformToolset>' "$project" || fail 'native driver project is not a WDK kernel-driver project'
grep -Fq 'Class=FileSystem' "$inf" || fail 'native driver INF is not a filesystem driver package'
grep -Fq 'ServiceType=2' "$inf" || fail 'native driver INF is not installing a filesystem driver service'

if grep -Eiq 'winfsp|dokan|fuse' "$driver" "$service" "$protocol"; then
    fail 'native Windows path acquired an external filesystem-framework dependency'
fi

grep -Fq -- '- [x] Native Windows filesystem driver implementation with Cache Manager/I/O Manager integration.' "$roadmap" || fail 'native Windows driver implementation is not recorded complete'
grep -Fq -- '- [ ] Mounted native Windows driver qualification on Windows' "$roadmap" || fail 'mounted Windows qualification must remain open until a real mount/Verifier pass'

printf 'native-windows-driver-policy: PASS\n'
