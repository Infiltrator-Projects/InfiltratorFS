// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef _WIN32
#include "infilfs/win32_partition_io.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <bcrypt.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>

struct infs_win32_partition_context {
    HANDLE handle;
    uint64_t base_offset;
    uint64_t region_size;
};

static infs_status status_from_win32(DWORD error)
{
    switch (error) {
    case ERROR_SUCCESS: return INFS_STATUS_OK;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_NAME: return INFS_STATUS_INVALID_ARGUMENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_WRITE_PROTECT: return INFS_STATUS_READ_ONLY;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL: return INFS_STATUS_NO_SPACE;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return INFS_STATUS_NOT_FOUND;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS: return INFS_STATUS_ALREADY_EXISTS;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY: return INFS_STATUS_NO_MEMORY;
    case ERROR_BUSY:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION: return INFS_STATUS_BUSY;
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_FUNCTION: return INFS_STATUS_NOT_SUPPORTED;
    case ERROR_OPERATION_ABORTED: return INFS_STATUS_INTERRUPTED;
    case ERROR_ARITHMETIC_OVERFLOW: return INFS_STATUS_OVERFLOW;
    default: return INFS_STATUS_IO_ERROR;
    }
}

static infs_status checked_offset(
    const struct infs_win32_partition_context *ctx,
    uint64_t offset, size_t size, uint64_t *absolute)
{
    if (!ctx || !absolute)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (offset > ctx->region_size ||
        (uint64_t)size > ctx->region_size - offset)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (ctx->base_offset > UINT64_MAX - offset)
        return INFS_STATUS_OVERFLOW;
    *absolute = ctx->base_offset + offset;
    if (*absolute > INT64_MAX)
        return INFS_STATUS_OVERFLOW;
    return INFS_STATUS_OK;
}

static infs_status partition_read_at(void *context, uint64_t offset,
                                     void *buffer, size_t size)
{
    struct infs_win32_partition_context *ctx = context;
    uint64_t absolute = 0;
    infs_status status = checked_offset(ctx, offset, size, &absolute);
    if (status != INFS_STATUS_OK)
        return status;

    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)absolute;
    if (!SetFilePointerEx(ctx->handle, pos, NULL, FILE_BEGIN))
        return status_from_win32(GetLastError());

    uint8_t *out = buffer;
    while (size) {
        DWORD chunk = size > UINT32_C(0x40000000) ?
            UINT32_C(0x40000000) : (DWORD)size;
        DWORD done = 0;
        if (!ReadFile(ctx->handle, out, chunk, &done, NULL))
            return status_from_win32(GetLastError());
        if (!done)
            return INFS_STATUS_IO_ERROR;
        out += done;
        size -= done;
    }
    return INFS_STATUS_OK;
}

static infs_status partition_write_at(void *context, uint64_t offset,
                                      const void *buffer, size_t size)
{
    struct infs_win32_partition_context *ctx = context;
    uint64_t absolute = 0;
    infs_status status = checked_offset(ctx, offset, size, &absolute);
    if (status != INFS_STATUS_OK)
        return status;

    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)absolute;
    if (!SetFilePointerEx(ctx->handle, pos, NULL, FILE_BEGIN))
        return status_from_win32(GetLastError());

    const uint8_t *in = buffer;
    while (size) {
        DWORD chunk = size > UINT32_C(0x40000000) ?
            UINT32_C(0x40000000) : (DWORD)size;
        DWORD done = 0;
        if (!WriteFile(ctx->handle, in, chunk, &done, NULL))
            return status_from_win32(GetLastError());
        if (!done)
            return INFS_STATUS_IO_ERROR;
        in += done;
        size -= done;
    }
    return INFS_STATUS_OK;
}

static infs_status partition_flush(void *context)
{
    struct infs_win32_partition_context *ctx = context;
    if (!FlushFileBuffers(ctx->handle))
        return status_from_win32(GetLastError());
    return INFS_STATUS_OK;
}

static infs_status partition_get_size(void *context, uint64_t *size_bytes,
                                      int *is_device)
{
    struct infs_win32_partition_context *ctx = context;
    if (!ctx || !size_bytes || !is_device)
        return INFS_STATUS_INVALID_ARGUMENT;
    *size_bytes = ctx->region_size;
    *is_device = 1;
    return INFS_STATUS_OK;
}

static infs_status partition_random(void *context, void *buffer, size_t size)
{
    (void)context;
    uint8_t *out = buffer;
    while (size) {
        ULONG chunk = size > ULONG_MAX ? ULONG_MAX : (ULONG)size;
        NTSTATUS result = BCryptGenRandom(NULL, out, chunk,
                                         BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (result < 0)
            return INFS_STATUS_IO_ERROR;
        out += chunk;
        size -= chunk;
    }
    return INFS_STATUS_OK;
}

static infs_status partition_time(void *context, int64_t *time_ns)
{
    (void)context;
    if (!time_ns)
        return INFS_STATUS_INVALID_ARGUMENT;
    FILETIME file_time;
    GetSystemTimePreciseAsFileTime(&file_time);
    ULARGE_INTEGER ticks;
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    const uint64_t unix_epoch_ticks = UINT64_C(116444736000000000);
    if (ticks.QuadPart < unix_epoch_ticks)
        return INFS_STATUS_OVERFLOW;
    uint64_t unix_ticks = ticks.QuadPart - unix_epoch_ticks;
    if (unix_ticks > (uint64_t)INT64_MAX / 100u)
        return INFS_STATUS_OVERFLOW;
    *time_ns = (int64_t)(unix_ticks * 100u);
    return INFS_STATUS_OK;
}

static void partition_close(void *context)
{
    struct infs_win32_partition_context *ctx = context;
    if (!ctx)
        return;
    if (ctx->handle != INVALID_HANDLE_VALUE)
        CloseHandle(ctx->handle);
    free(ctx);
}

static const struct infs_storage_ops partition_ops = {
    partition_read_at,
    partition_write_at,
    partition_flush,
    partition_get_size,
    partition_random,
    partition_time,
    partition_close
};

static int parse_physical_drive_number(const wchar_t *path, DWORD *disk_number)
{
    static const wchar_t prefix[] = L"\\\\.\\PhysicalDrive";
    const size_t prefix_len = (sizeof(prefix) / sizeof(prefix[0])) - 1u;
    if (!path || _wcsnicmp(path, prefix, prefix_len) != 0)
        return 0;
    wchar_t *end = NULL;
    unsigned long value = wcstoul(path + prefix_len, &end, 10);
    if (end == path + prefix_len || !end || *end != L'\0' || value > 0xfffffffful)
        return 0;
    *disk_number = (DWORD)value;
    return 1;
}

static DRIVE_LAYOUT_INFORMATION_EX *read_drive_layout(HANDLE disk)
{
    size_t capacity = 64u * 1024u;
    for (unsigned attempt = 0; attempt < 6u; ++attempt) {
        if (capacity > UINT32_MAX)
            return NULL;
        DRIVE_LAYOUT_INFORMATION_EX *layout = malloc(capacity);
        if (!layout)
            return NULL;
        DWORD returned = 0;
        if (DeviceIoControl(disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                            NULL, 0, layout, (DWORD)capacity,
                            &returned, NULL))
            return layout;
        DWORD error = GetLastError();
        free(layout);
        if (error != ERROR_INSUFFICIENT_BUFFER && error != ERROR_MORE_DATA)
            return NULL;
        capacity *= 2u;
    }
    return NULL;
}

static int resolve_partition_device(const wchar_t *physical_path,
                                    uint64_t base_offset,
                                    uint64_t region_size,
                                    wchar_t *partition_path,
                                    size_t partition_path_count)
{
    DWORD disk_number = 0;
    if (!parse_physical_drive_number(physical_path, &disk_number))
        return 0;

    HANDLE metadata = CreateFileW(physical_path, 0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (metadata == INVALID_HANDLE_VALUE)
        return 0;
    DRIVE_LAYOUT_INFORMATION_EX *layout = read_drive_layout(metadata);
    CloseHandle(metadata);
    if (!layout)
        return 0;

    DWORD partition_number = 0;
    for (DWORD i = 0; i < layout->PartitionCount; ++i) {
        const PARTITION_INFORMATION_EX *part = &layout->PartitionEntry[i];
        if (part->PartitionNumber == 0 ||
            part->StartingOffset.QuadPart < 0 ||
            part->PartitionLength.QuadPart <= 0)
            continue;
        if ((uint64_t)part->StartingOffset.QuadPart == base_offset &&
            (uint64_t)part->PartitionLength.QuadPart == region_size) {
            partition_number = part->PartitionNumber;
            break;
        }
    }
    free(layout);
    if (!partition_number)
        return 0;

    return _snwprintf_s(partition_path, partition_path_count, _TRUNCATE,
                        L"\\\\?\\GLOBALROOT\\Device\\Harddisk%lu\\Partition%lu",
                        (unsigned long)disk_number,
                        (unsigned long)partition_number) >= 0;
}

infs_status infs_win32_storage_open_partition_region(
    struct infs_storage *storage,
    const wchar_t *path,
    uint64_t base_offset,
    uint64_t region_size,
    int writable)
{
    if (!storage || !path || !path[0] || !region_size ||
        base_offset > UINT64_MAX - region_size)
        return INFS_STATUS_INVALID_ARGUMENT;
    storage->ops = NULL;
    storage->context = NULL;

    DWORD access = GENERIC_READ | (writable ? GENERIC_WRITE : 0u);
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    DWORD flags = FILE_ATTRIBUTE_NORMAL | (writable ? FILE_FLAG_WRITE_THROUGH : 0u);

    HANDLE handle = CreateFileW(path, access, share, NULL, OPEN_EXISTING,
                                flags, NULL);
    uint64_t actual_base = base_offset;
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD first_error = GetLastError();
        wchar_t partition_path[256];
        if (!resolve_partition_device(path, base_offset, region_size,
                                      partition_path,
                                      sizeof(partition_path) / sizeof(partition_path[0])))
            return status_from_win32(first_error);
        handle = CreateFileW(partition_path, access, share, NULL,
                             OPEN_EXISTING, flags, NULL);
        if (handle == INVALID_HANDLE_VALUE)
            return status_from_win32(GetLastError());
        actual_base = 0;
    }

    struct infs_win32_partition_context *ctx = calloc(1u, sizeof(*ctx));
    if (!ctx) {
        CloseHandle(handle);
        return INFS_STATUS_NO_MEMORY;
    }
    ctx->handle = handle;
    ctx->base_offset = actual_base;
    ctx->region_size = region_size;
    storage->ops = &partition_ops;
    storage->context = ctx;
    return INFS_STATUS_OK;
}
#endif
