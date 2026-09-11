// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef _WIN32
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

#include "infilfs/volume.h"
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

/*
 * Canonical MBLINK visual contract for the native Windows shell.
 *
 * The UI implementation in infiltratorfs-windows.c intentionally uses a
 * small set of platform colours and native controls.  Keep the implementation
 * untouched and map those platform colour slots onto the same fixed Mercedes
 * palette used by MBLINK instead of inheriting the user's Windows light/dark
 * application theme.
 *
 *   background  #050608
 *   chrome      #0E1115
 *   panel       #171B20
 *   silver      #EEF1F3
 *   muted       #98A1A9
 *   border      #353A40
 *   active      #00ADEF
 */
static COLORREF infs_mb_rgb(BYTE red, BYTE green, BYTE blue)
{
    return (COLORREF)((DWORD)red |
                      ((DWORD)green << 8u) |
                      ((DWORD)blue << 16u));
}

static COLORREF infs_mb_rgb_map(BYTE red, BYTE green, BYTE blue)
{
    if ((red == 31u && green == 31u && blue == 31u) ||
        (red == 246u && green == 247u && blue == 249u))
        return infs_mb_rgb(5u, 6u, 8u);          /* #050608 background */
    if ((red == 43u && green == 43u && blue == 43u) ||
        (red == 255u && green == 255u && blue == 255u))
        return infs_mb_rgb(23u, 27u, 32u);       /* #171B20 panel */
    if ((red == 245u && green == 245u && blue == 245u) ||
        (red == 32u && green == 33u && blue == 36u))
        return infs_mb_rgb(238u, 241u, 243u);    /* #EEF1F3 silver */
    if ((red == 184u && green == 184u && blue == 184u) ||
        (red == 95u && green == 99u && blue == 104u))
        return infs_mb_rgb(152u, 161u, 169u);    /* #98A1A9 muted */
    return infs_mb_rgb(red, green, blue);
}

/* Force only the app-theme query to dark. Other registry reads retain their
 * normal behaviour if the included Windows shell adds any in the future. */
static LONG WINAPI infs_mb_RegGetValueW(
    HKEY key,
    LPCWSTR subkey,
    LPCWSTR value_name,
    DWORD flags,
    LPDWORD type,
    PVOID data,
    LPDWORD data_size)
{
    LONG status = RegGetValueW(key, subkey, value_name, flags,
                               type, data, data_size);
    if (status == ERROR_SUCCESS && value_name != NULL && data != NULL &&
        data_size != NULL && *data_size >= sizeof(DWORD) &&
        wcscmp(value_name, L"AppsUseLightTheme") == 0) {
        *(DWORD *)data = 0u;
    }
    return status;
}

/* The transfer application is a bulk-copy adapter, so keep a much larger
 * bounded transaction open than the interactive Linux/FUSE default. Without
 * this policy, infs_write_file_buffered() immediately publishes every 4 MiB
 * copy chunk and rewrites the multi-megabyte allocation bitmap each time.
 * A 256 MiB publication window keeps the generic crash-consistent transaction
 * machinery intact while removing that pathological Windows write
 * amplification. copy_host_file() still performs an explicit sync at the end
 * of each file, so a completed file is durable before the next one starts. */
#define INFS_WINDOWS_TRANSFER_PUBLISH_BYTES \
    (UINT64_C(256) * 1024u * 1024u)

static infs_status infs_windows_volume_open_storage(
    struct infs_volume *vol, struct infs_storage *storage, int writable)
{
    infs_status status = infs_volume_open_storage(vol, storage, writable);
    if (status != INFS_STATUS_OK || !writable)
        return status;

    status = infs_volume_set_deferred_publish(
        vol, 1, INFS_WINDOWS_TRANSFER_PUBLISH_BYTES);
    if (status != INFS_STATUS_OK) {
        infs_volume_close(vol);
        return status;
    }
    return INFS_STATUS_OK;
}

#define CreateFileW infs_discovery_CreateFileW
#define RegGetValueW infs_mb_RegGetValueW
#undef RGB
#define RGB(red, green, blue) \
    infs_mb_rgb_map((BYTE)(red), (BYTE)(green), (BYTE)(blue))
#define infs_win32_storage_open_region infs_win32_storage_open_partition_region
#define infs_volume_open_storage infs_windows_volume_open_storage
#include "infiltratorfs-windows.c"
#endif
