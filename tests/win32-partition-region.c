// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef _WIN32
#include "infilfs/format_volume.h"
#include "infilfs/storage.h"
#include "infilfs/volume.h"
#include "infilfs/win32_partition_io.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    return 1;
}

int main(void)
{
    wchar_t temp_dir[MAX_PATH];
    wchar_t path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp_dir) ||
        !GetTempFileNameW(temp_dir, L"IFR", 0, path))
        return fail("temporary path creation failed");

    const uint64_t file_size = UINT64_C(40) * 1024u * 1024u;
    const uint64_t base = UINT64_C(4) * 1024u * 1024u;
    const uint64_t region = UINT64_C(32) * 1024u * 1024u;
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DeleteFileW(path);
        return fail("temporary file open failed");
    }
    LARGE_INTEGER length;
    length.QuadPart = (LONGLONG)file_size;
    if (!SetFilePointerEx(file, length, NULL, FILE_BEGIN) || !SetEndOfFile(file)) {
        CloseHandle(file);
        DeleteFileW(path);
        return fail("temporary file sizing failed");
    }

    static const uint8_t before[16] = {
        0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
        0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xf0,0x0f
    };
    static const uint8_t after[16] = {
        0x0f,0xf0,0xee,0xdd,0xcc,0xbb,0xaa,0x99,
        0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11
    };
    DWORD done = 0;
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)(base - sizeof(before));
    if (!SetFilePointerEx(file, pos, NULL, FILE_BEGIN) ||
        !WriteFile(file, before, sizeof(before), &done, NULL) ||
        done != sizeof(before)) {
        CloseHandle(file);
        DeleteFileW(path);
        return fail("before sentinel write failed");
    }
    pos.QuadPart = (LONGLONG)(base + region);
    if (!SetFilePointerEx(file, pos, NULL, FILE_BEGIN) ||
        !WriteFile(file, after, sizeof(after), &done, NULL) ||
        done != sizeof(after)) {
        CloseHandle(file);
        DeleteFileW(path);
        return fail("after sentinel write failed");
    }
    CloseHandle(file);

    struct infs_storage storage = {0};
    infs_status status = infs_win32_storage_open_partition_region(
        &storage, path, base, region, 1);
    if (status != INFS_STATUS_OK) {
        DeleteFileW(path);
        return fail("bounded region open failed");
    }
    status = infs_format_storage(&storage, "PartitionRegionCI");
    if (status != INFS_STATUS_OK) {
        infs_storage_close(&storage);
        DeleteFileW(path);
        return fail("bounded region format failed");
    }
    struct infs_volume volume;
    status = infs_volume_open_storage(&volume, &storage, 1);
    if (status != INFS_STATUS_OK) {
        infs_storage_close(&storage);
        DeleteFileW(path);
        return fail("formatted bounded region open failed");
    }
    infs_volume_close(&volume);

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DeleteFileW(path);
        return fail("temporary file reopen failed");
    }
    uint8_t check[16];
    pos.QuadPart = (LONGLONG)(base - sizeof(before));
    if (!SetFilePointerEx(file, pos, NULL, FILE_BEGIN) ||
        !ReadFile(file, check, sizeof(check), &done, NULL) ||
        done != sizeof(check) || memcmp(check, before, sizeof(before)) != 0) {
        CloseHandle(file);
        DeleteFileW(path);
        return fail("before sentinel changed");
    }
    pos.QuadPart = (LONGLONG)(base + region);
    if (!SetFilePointerEx(file, pos, NULL, FILE_BEGIN) ||
        !ReadFile(file, check, sizeof(check), &done, NULL) ||
        done != sizeof(check) || memcmp(check, after, sizeof(after)) != 0) {
        CloseHandle(file);
        DeleteFileW(path);
        return fail("after sentinel changed");
    }
    CloseHandle(file);
    DeleteFileW(path);
    puts("Win32 partition-region bounded storage test passed");
    return 0;
}
#else
int main(void) { return 0; }
#endif
