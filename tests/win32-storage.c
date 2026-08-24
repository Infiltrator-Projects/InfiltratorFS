// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef _WIN32
#include "infilfs/endian.h"
#include "infilfs/format_volume.h"
#include "infilfs/volume.h"
#include "infilfs/win32_io.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

static int fail(const char *message, infs_status status)
{
    fprintf(stderr, "%s: %s (%d)\n", message,
            infs_status_string(status), status);
    return 1;
}

static int verify_linux_image(const char *utf8_path)
{
    wchar_t path[32768];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path, -1,
                             path, (int)(sizeof(path) / sizeof(path[0])))) {
        fprintf(stderr, "cross-platform image path is not valid UTF-8\n");
        return 1;
    }

    struct infs_storage storage = {0};
    infs_status status = infs_win32_storage_open(&storage, path, 0, 0);
    if (status != INFS_STATUS_OK)
        return fail("open Linux-created image through Win32 backend", status);

    struct infs_volume volume;
    status = infs_volume_open_storage(&volume, &storage, 0);
    if (status != INFS_STATUS_OK) {
        infs_storage_close(&storage);
        return fail("open Linux-created InfiltratorFS image", status);
    }

    if (infs_le16_to_cpu(volume.sb.format_major) != 0u ||
        infs_le16_to_cpu(volume.sb.format_minor) != 9u) {
        infs_volume_close(&volume);
        fprintf(stderr, "Linux-created image is not Format 0.9\n");
        return 1;
    }

    static const char expected[] = "linux-to-windows-format-0.9\n";
    char buffer[sizeof(expected)] = {0};
    int64_t read_count = infs_read_file(&volume, "/linux-cross-platform.txt",
                                       buffer, sizeof(expected) - 1u, 0u);
    if (read_count != (int64_t)(sizeof(expected) - 1u) ||
        memcmp(buffer, expected, sizeof(expected) - 1u) != 0) {
        infs_volume_close(&volume);
        fprintf(stderr, "Linux-created image read-back mismatch\n");
        return 1;
    }

    infs_volume_close(&volume);
    puts("Linux-created Format 0.9 image opened successfully on Windows");
    return 0;
}

static int make_temp_file(wchar_t path[MAX_PATH], LONGLONG size)
{
    wchar_t temp_dir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp_dir) ||
        !GetTempFileNameW(temp_dir, L"IFS", 0, path))
        return 0;
    HANDLE image = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (image == INVALID_HANDLE_VALUE) {
        DeleteFileW(path);
        return 0;
    }
    LARGE_INTEGER length;
    length.QuadPart = size;
    int ok = SetFilePointerEx(image, length, NULL, FILE_BEGIN) &&
             SetEndOfFile(image);
    CloseHandle(image);
    if (!ok)
        DeleteFileW(path);
    return ok;
}

static int test_region_backend(void)
{
    wchar_t path[MAX_PATH];
    const uint64_t mib = UINT64_C(1024) * UINT64_C(1024);
    const uint64_t region_offset = mib;
    const uint64_t region_size = 64u * mib;
    const uint64_t backing_size = 66u * mib;
    if (!make_temp_file(path, (LONGLONG)backing_size)) {
        fprintf(stderr, "region temporary path creation failed\n");
        return 1;
    }

    HANDLE raw = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (raw == INVALID_HANDLE_VALUE) {
        DeleteFileW(path);
        return 1;
    }
    const unsigned char before = 0x5a;
    const unsigned char after = 0xa5;
    DWORD done = 0;
    LARGE_INTEGER pos;
    pos.QuadPart = 0;
    if (!SetFilePointerEx(raw, pos, NULL, FILE_BEGIN) ||
        !WriteFile(raw, &before, 1, &done, NULL) || done != 1) {
        CloseHandle(raw);
        DeleteFileW(path);
        return 1;
    }
    pos.QuadPart = (LONGLONG)(region_offset + region_size);
    if (!SetFilePointerEx(raw, pos, NULL, FILE_BEGIN) ||
        !WriteFile(raw, &after, 1, &done, NULL) || done != 1) {
        CloseHandle(raw);
        DeleteFileW(path);
        return 1;
    }
    CloseHandle(raw);

    struct infs_storage storage = {0};
    infs_status status = infs_win32_storage_open_region(
        &storage, path, region_offset, region_size, 1);
    if (status != INFS_STATUS_OK) {
        DeleteFileW(path);
        return fail("open bounded region", status);
    }
    uint64_t reported = 0;
    int is_device = 0;
    status = infs_storage_get_size(&storage, &reported, &is_device);
    if (status != INFS_STATUS_OK || reported != region_size || !is_device) {
        infs_storage_close(&storage);
        DeleteFileW(path);
        fprintf(stderr, "bounded region reported incorrect size\n");
        return 1;
    }
    status = infs_format_storage(&storage, "Windows Region CI");
    if (status != INFS_STATUS_OK) {
        infs_storage_close(&storage);
        DeleteFileW(path);
        return fail("format bounded region", status);
    }

    struct infs_volume volume;
    status = infs_volume_open_storage(&volume, &storage, 1);
    if (status != INFS_STATUS_OK) {
        infs_storage_close(&storage);
        DeleteFileW(path);
        return fail("open formatted bounded region", status);
    }
    struct infs_create_options options = {0};
    options.posix_permissions = 0644u;
    status = infs_create_file(&volume, "/region.txt", &options);
    if (status != INFS_STATUS_OK) {
        infs_volume_close(&volume);
        DeleteFileW(path);
        return fail("create file in bounded region", status);
    }
    static const char text[] = "bounded-partition-region\n";
    int64_t written = infs_write_file(&volume, "/region.txt", text,
                                      sizeof(text) - 1u, 0u);
    if (written != (int64_t)(sizeof(text) - 1u)) {
        infs_volume_close(&volume);
        DeleteFileW(path);
        return fail("write bounded region file",
                    written < 0 ? (infs_status)written : INFS_STATUS_IO_ERROR);
    }
    infs_volume_close(&volume);

    raw = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (raw == INVALID_HANDLE_VALUE) {
        DeleteFileW(path);
        return 1;
    }
    unsigned char check_before = 0, check_after = 0;
    pos.QuadPart = 0;
    if (!SetFilePointerEx(raw, pos, NULL, FILE_BEGIN) ||
        !ReadFile(raw, &check_before, 1, &done, NULL) || done != 1) {
        CloseHandle(raw);
        DeleteFileW(path);
        return 1;
    }
    pos.QuadPart = (LONGLONG)(region_offset + region_size);
    if (!SetFilePointerEx(raw, pos, NULL, FILE_BEGIN) ||
        !ReadFile(raw, &check_after, 1, &done, NULL) || done != 1) {
        CloseHandle(raw);
        DeleteFileW(path);
        return 1;
    }
    CloseHandle(raw);
    DeleteFileW(path);
    if (check_before != before || check_after != after) {
        fprintf(stderr, "bounded region wrote outside its partition\n");
        return 1;
    }
    puts("bounded Win32 partition-region test passed");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2)
        return verify_linux_image(argv[1]);
    if (argc != 1) {
        fprintf(stderr, "Usage: %s [linux-created-image]\n", argv[0]);
        return 2;
    }

    if (test_region_backend() != 0)
        return 1;

    wchar_t path[MAX_PATH];
    if (!make_temp_file(path, 64ll * 1024ll * 1024ll)) {
        fprintf(stderr, "temporary path creation failed\n");
        return 1;
    }

    struct infs_storage storage = {0};
    infs_status status = infs_win32_storage_open(&storage, path, 1, 0);
    if (status != INFS_STATUS_OK) {
        DeleteFileW(path);
        return fail("open image", status);
    }
    status = infs_format_storage(&storage, "Windows CI");
    if (status != INFS_STATUS_OK) {
        infs_storage_close(&storage);
        DeleteFileW(path);
        return fail("format image", status);
    }

    struct infs_volume volume;
    status = infs_volume_open_storage(&volume, &storage, 1);
    if (status != INFS_STATUS_OK) {
        infs_storage_close(&storage);
        DeleteFileW(path);
        return fail("open formatted image", status);
    }

    struct infs_create_options options = {0};
    options.posix_permissions = 0644u;
    status = infs_create_file(&volume, "/windows.txt", &options);
    if (status != INFS_STATUS_OK) {
        infs_volume_close(&volume);
        DeleteFileW(path);
        return fail("create file", status);
    }
    static const char text[] = "native Windows InfiltratorFS\n";
    int64_t written = infs_write_file(&volume, "/windows.txt", text,
                                      sizeof(text) - 1u, 0u);
    if (written != (int64_t)(sizeof(text) - 1u)) {
        infs_volume_close(&volume);
        DeleteFileW(path);
        return fail("write file",
                    written < 0 ? (infs_status)written : INFS_STATUS_IO_ERROR);
    }
    infs_volume_close(&volume);

    memset(&storage, 0, sizeof(storage));
    status = infs_win32_storage_open(&storage, path, 0, 0);
    if (status != INFS_STATUS_OK) {
        DeleteFileW(path);
        return fail("reopen image", status);
    }
    status = infs_volume_open_storage(&volume, &storage, 0);
    if (status != INFS_STATUS_OK) {
        infs_storage_close(&storage);
        DeleteFileW(path);
        return fail("reopen volume", status);
    }
    char buffer[sizeof(text)] = {0};
    int64_t read_count = infs_read_file(&volume, "/windows.txt", buffer,
                                       sizeof(text) - 1u, 0u);
    if (read_count != (int64_t)(sizeof(text) - 1u) ||
        memcmp(buffer, text, sizeof(text) - 1u) != 0) {
        infs_volume_close(&volume);
        DeleteFileW(path);
        fprintf(stderr, "read-back mismatch\n");
        return 1;
    }
    infs_volume_close(&volume);
    DeleteFileW(path);
    printf("native Win32 storage/format/read/write test passed\n");
    return 0;
}
#else
int main(void) { return 0; }
#endif
