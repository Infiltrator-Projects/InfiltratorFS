// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef _WIN32
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

#include "infilfs/win32_partition_io.h"

/* Some SD/card-reader stacks allow metadata IOCTLs on PhysicalDriveN but
 * reject a GENERIC_READ open used only for enumeration. Retry discovery opens
 * with zero desired access; the actual filesystem probe uses the dedicated
 * partition-region backend below and still requires real data access. */
static HANDLE WINAPI infs_discovery_CreateFileW(
    LPCWSTR path,
    DWORD desired_access,
    DWORD share_mode,
    LPSECURITY_ATTRIBUTES security_attributes,
    DWORD creation_disposition,
    DWORD flags_and_attributes,
    HANDLE template_file)
{
    HANDLE handle = CreateFileW(path, desired_access, share_mode,
                                security_attributes, creation_disposition,
                                flags_and_attributes, template_file);
    if (handle != INVALID_HANDLE_VALUE || !path)
        return handle;

    static const wchar_t prefix[] = L"\\\\.\\PhysicalDrive";
    const size_t prefix_len = (sizeof(prefix) / sizeof(prefix[0])) - 1u;
    if (_wcsnicmp(path, prefix, prefix_len) == 0 &&
        desired_access == GENERIC_READ) {
        handle = CreateFileW(path, 0, share_mode,
                             security_attributes, creation_disposition,
                             flags_and_attributes, template_file);
    }
    return handle;
}

#define CreateFileW infs_discovery_CreateFileW
#define infs_win32_storage_open_region infs_win32_storage_open_partition_region
#include "infiltratorfs-windows.c"
#endif
