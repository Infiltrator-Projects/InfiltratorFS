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
    fprintf(stderr, "%s: %s (%d)\n", message, infs_status_string(status), status);
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
        infs_le16_to_cpu(volume.sb.format_minor) != 8u) {
        infs_volume_close(&volume);
        fprintf(stderr, "Linux-created image is not Format 0.8\n");
        return 1;
    }

    static const char expected[] = "linux-to-windows-format-0.8\n";
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
    puts("Linux-created Format 0.8 image opened successfully on Windows");
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

    wchar_t temp_dir[MAX_PATH];
    wchar_t path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp_dir) ||
        !GetTempFileNameW(temp_dir, L"IFS", 0, path)) {
        fprintf(stderr, "temporary path creation failed\n");
        return 1;
    }

    HANDLE image = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (image == INVALID_HANDLE_VALUE) {
        DeleteFileW(path);
        return 1;
    }
    LARGE_INTEGER length;
    length.QuadPart = 64ll * 1024ll * 1024ll;
    if (!SetFilePointerEx(image, length, NULL, FILE_BEGIN) || !SetEndOfFile(image)) {
        CloseHandle(image);
        DeleteFileW(path);
        return 1;
    }
    CloseHandle(image);

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
        return fail("write file", written < 0 ? (infs_status)written : INFS_STATUS_IO_ERROR);
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
