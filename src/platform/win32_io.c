// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef _WIN32
#include "infilfs/win32_io.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <bcrypt.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>

struct infs_win32_storage_context {
    HANDLE handle;
    int locked;
    int is_device;
    int is_volume;
    int has_region;
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

static int path_is_device(const wchar_t *path)
{
    return wcsncmp(path, L"\\\\.\\", 4u) == 0 ||
           wcsncmp(path, L"\\\\?\\Volume{", 11u) == 0;
}

static int path_is_volume(const wchar_t *path)
{
    if (wcsncmp(path, L"\\\\?\\Volume{", 11u) == 0)
        return 1;
    if (wcsncmp(path, L"\\\\.\\", 4u) != 0)
        return 0;
    wchar_t letter = path[4];
    return ((letter >= L'A' && letter <= L'Z') ||
            (letter >= L'a' && letter <= L'z')) &&
           path[5] == L':' && path[6] == L'\0';
}

static infs_status checked_absolute_offset(
    const struct infs_win32_storage_context *win,
    uint64_t offset, size_t size, uint64_t *absolute)
{
    if (!win || !absolute)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (win->has_region) {
        if (offset > win->region_size ||
            (uint64_t)size > win->region_size - offset)
            return INFS_STATUS_INVALID_ARGUMENT;
        if (win->base_offset > UINT64_MAX - offset)
            return INFS_STATUS_OVERFLOW;
        *absolute = win->base_offset + offset;
    } else {
        *absolute = offset;
    }
    if (*absolute > INT64_MAX)
        return INFS_STATUS_OVERFLOW;
    return INFS_STATUS_OK;
}

static infs_status win32_seek(HANDLE handle, uint64_t offset)
{
    if (offset > INT64_MAX)
        return INFS_STATUS_OVERFLOW;
    LARGE_INTEGER position;
    position.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(handle, position, NULL, FILE_BEGIN))
        return status_from_win32(GetLastError());
    return INFS_STATUS_OK;
}

static infs_status win32_read_at(void *context, uint64_t offset,
                                 void *buffer, size_t size)
{
    struct infs_win32_storage_context *win = context;
    uint64_t absolute = 0;
    infs_status status = checked_absolute_offset(win, offset, size, &absolute);
    if (status != INFS_STATUS_OK)
        return status;
    status = win32_seek(win->handle, absolute);
    if (status != INFS_STATUS_OK)
        return status;
    uint8_t *out = buffer;
    while (size) {
        DWORD chunk = size > UINT32_C(0x40000000) ?
            UINT32_C(0x40000000) : (DWORD)size;
        DWORD done = 0;
        if (!ReadFile(win->handle, out, chunk, &done, NULL))
            return status_from_win32(GetLastError());
        if (done == 0)
            return INFS_STATUS_IO_ERROR;
        out += done;
        size -= done;
    }
    return INFS_STATUS_OK;
}

static infs_status win32_write_at(void *context, uint64_t offset,
                                  const void *buffer, size_t size)
{
    struct infs_win32_storage_context *win = context;
    uint64_t absolute = 0;
    infs_status status = checked_absolute_offset(win, offset, size, &absolute);
    if (status != INFS_STATUS_OK)
        return status;
    status = win32_seek(win->handle, absolute);
    if (status != INFS_STATUS_OK)
        return status;
    const uint8_t *in = buffer;
    while (size) {
        DWORD chunk = size > UINT32_C(0x40000000) ?
            UINT32_C(0x40000000) : (DWORD)size;
        DWORD done = 0;
        if (!WriteFile(win->handle, in, chunk, &done, NULL))
            return status_from_win32(GetLastError());
        if (done == 0)
            return INFS_STATUS_IO_ERROR;
        in += done;
        size -= done;
    }
    return INFS_STATUS_OK;
}

static infs_status win32_flush(void *context)
{
    struct infs_win32_storage_context *win = context;
    if (!FlushFileBuffers(win->handle))
        return status_from_win32(GetLastError());
    return INFS_STATUS_OK;
}

static infs_status volume_size_from_partition(HANDLE handle,
                                              uint64_t *size_bytes)
{
    PARTITION_INFORMATION_EX part;
    DWORD returned = 0;
    if (!DeviceIoControl(handle, IOCTL_DISK_GET_PARTITION_INFO_EX,
                         NULL, 0, &part, sizeof(part), &returned, NULL))
        return status_from_win32(GetLastError());
    if (part.PartitionLength.QuadPart <= 0)
        return INFS_STATUS_CORRUPT;
    *size_bytes = (uint64_t)part.PartitionLength.QuadPart;
    return INFS_STATUS_OK;
}

static infs_status volume_size_from_length_info(HANDLE handle,
                                                uint64_t *size_bytes)
{
    GET_LENGTH_INFORMATION length;
    DWORD returned = 0;
    if (!DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO,
                         NULL, 0, &length, sizeof(length), &returned, NULL))
        return status_from_win32(GetLastError());
    if (length.Length.QuadPart <= 0)
        return INFS_STATUS_CORRUPT;
    *size_bytes = (uint64_t)length.Length.QuadPart;
    return INFS_STATUS_OK;
}

static infs_status volume_size_from_geometry(HANDLE handle,
                                             uint64_t *size_bytes)
{
    uint8_t buffer[sizeof(DISK_GEOMETRY_EX) + 1024u];
    DISK_GEOMETRY_EX *geometry = (DISK_GEOMETRY_EX *)buffer;
    DWORD returned = 0;
    if (!DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                         NULL, 0, buffer, (DWORD)sizeof(buffer),
                         &returned, NULL))
        return status_from_win32(GetLastError());
    if (geometry->DiskSize.QuadPart <= 0)
        return INFS_STATUS_CORRUPT;
    *size_bytes = (uint64_t)geometry->DiskSize.QuadPart;
    return INFS_STATUS_OK;
}

static infs_status volume_size_from_extents(HANDLE handle,
                                            uint64_t *size_bytes)
{
    size_t capacity = sizeof(VOLUME_DISK_EXTENTS) +
                      7u * sizeof(DISK_EXTENT);
    for (unsigned attempt = 0; attempt < 8u; ++attempt) {
        if (capacity > (size_t)UINT32_MAX)
            return INFS_STATUS_OVERFLOW;
        VOLUME_DISK_EXTENTS *extents = malloc(capacity);
        if (!extents)
            return INFS_STATUS_NO_MEMORY;
        DWORD returned = 0;
        if (DeviceIoControl(handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                            NULL, 0, extents, (DWORD)capacity,
                            &returned, NULL)) {
            uint64_t total = 0;
            DWORD count = extents->NumberOfDiskExtents;
            for (DWORD i = 0; i < count; ++i) {
                LONGLONG length = extents->Extents[i].ExtentLength.QuadPart;
                if (length <= 0 || (uint64_t)length > UINT64_MAX - total) {
                    free(extents);
                    return INFS_STATUS_CORRUPT;
                }
                total += (uint64_t)length;
            }
            free(extents);
            if (!total)
                return INFS_STATUS_CORRUPT;
            *size_bytes = total;
            return INFS_STATUS_OK;
        }
        DWORD error = GetLastError();
        free(extents);
        if (error != ERROR_MORE_DATA)
            return status_from_win32(error);
        if (capacity > (size_t)UINT32_MAX / 2u)
            return INFS_STATUS_OVERFLOW;
        capacity *= 2u;
    }
    return INFS_STATUS_NOT_SUPPORTED;
}

static infs_status device_size_best_effort(HANDLE handle, int is_volume,
                                           uint64_t *size_bytes)
{
    infs_status first_error = INFS_STATUS_NOT_SUPPORTED;
    infs_status status;

    if (is_volume) {
        status = volume_size_from_partition(handle, size_bytes);
        if (status == INFS_STATUS_OK)
            return INFS_STATUS_OK;
        first_error = status;
    }

    status = volume_size_from_length_info(handle, size_bytes);
    if (status == INFS_STATUS_OK)
        return INFS_STATUS_OK;
    if (first_error == INFS_STATUS_NOT_SUPPORTED)
        first_error = status;

    status = volume_size_from_geometry(handle, size_bytes);
    if (status == INFS_STATUS_OK)
        return INFS_STATUS_OK;
    if (first_error == INFS_STATUS_NOT_SUPPORTED)
        first_error = status;

    if (is_volume) {
        status = volume_size_from_extents(handle, size_bytes);
        if (status == INFS_STATUS_OK)
            return INFS_STATUS_OK;
        if (first_error == INFS_STATUS_NOT_SUPPORTED)
            first_error = status;
    }

    LARGE_INTEGER length;
    if (GetFileSizeEx(handle, &length) && length.QuadPart > 0) {
        *size_bytes = (uint64_t)length.QuadPart;
        return INFS_STATUS_OK;
    }
    return first_error;
}

static infs_status win32_get_size(void *context, uint64_t *size_bytes,
                                  int *is_device)
{
    struct infs_win32_storage_context *win = context;
    if (!size_bytes || !is_device)
        return INFS_STATUS_INVALID_ARGUMENT;

    if (win->has_region) {
        *size_bytes = win->region_size;
        *is_device = 1;
        return INFS_STATUS_OK;
    }

    if (win->is_device) {
        infs_status status = device_size_best_effort(
            win->handle, win->is_volume, size_bytes);
        if (status == INFS_STATUS_OK)
            *is_device = 1;
        return status;
    }

    LARGE_INTEGER length;
    if (!GetFileSizeEx(win->handle, &length))
        return status_from_win32(GetLastError());
    if (length.QuadPart < 0)
        return INFS_STATUS_CORRUPT;
    *size_bytes = (uint64_t)length.QuadPart;
    *is_device = 0;
    return INFS_STATUS_OK;
}

static infs_status win32_random(void *context, void *buffer, size_t size)
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

static infs_status win32_time(void *context, struct infs_timestamp *time)
{
    (void)context;
    if (!time)
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
    time->seconds = (int64_t)(unix_ticks / UINT64_C(10000000));
    time->nanoseconds = (uint32_t)((unix_ticks % UINT64_C(10000000)) * 100u);
    return INFS_STATUS_OK;
}

static void win32_close(void *context)
{
    struct infs_win32_storage_context *win = context;
    if (!win)
        return;
    if (win->handle != INVALID_HANDLE_VALUE) {
        if (win->locked) {
            DWORD returned = 0;
            DeviceIoControl(win->handle, FSCTL_UNLOCK_VOLUME,
                            NULL, 0, NULL, 0, &returned, NULL);
        }
        CloseHandle(win->handle);
    }
    free(win);
}

static const struct infs_storage_ops win32_storage_ops = {
    win32_read_at,
    win32_write_at,
    win32_flush,
    win32_get_size,
    win32_random,
    win32_time,
    win32_close
};

static infs_status open_common(struct infs_storage *storage,
                               const wchar_t *path, int writable,
                               int lock_and_dismount,
                               int has_region,
                               uint64_t base_offset,
                               uint64_t region_size)
{
    if (!storage || !path || !path[0] || (has_region && !region_size))
        return INFS_STATUS_INVALID_ARGUMENT;
    storage->ops = NULL;
    storage->context = NULL;

    struct infs_win32_storage_context *win = calloc(1u, sizeof(*win));
    if (!win)
        return INFS_STATUS_NO_MEMORY;
    win->handle = INVALID_HANDLE_VALUE;
    win->is_device = path_is_device(path) || has_region;
    win->is_volume = path_is_volume(path) && !has_region;
    win->has_region = has_region;
    win->base_offset = base_offset;
    win->region_size = region_size;

    DWORD access = GENERIC_READ | (writable ? GENERIC_WRITE : 0u);
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    /* Transaction durability is expressed by explicit FlushFileBuffers calls
     * at InfiltratorFS publication barriers. FILE_FLAG_WRITE_THROUGH on every
     * ordinary block write duplicates those barriers and catastrophically
     * penalises removable media. */
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    win->handle = CreateFileW(path, access, share, NULL, OPEN_EXISTING,
                              flags, NULL);
    if (win->handle == INVALID_HANDLE_VALUE) {
        infs_status status = status_from_win32(GetLastError());
        free(win);
        return status;
    }

    if (has_region) {
        uint64_t backing_size = 0;
        infs_status size_status;
        if (path_is_device(path))
            size_status = device_size_best_effort(
                win->handle, path_is_volume(path), &backing_size);
        else {
            LARGE_INTEGER length;
            if (!GetFileSizeEx(win->handle, &length))
                size_status = status_from_win32(GetLastError());
            else if (length.QuadPart <= 0)
                size_status = INFS_STATUS_CORRUPT;
            else {
                backing_size = (uint64_t)length.QuadPart;
                size_status = INFS_STATUS_OK;
            }
        }
        if (size_status != INFS_STATUS_OK ||
            base_offset > backing_size ||
            region_size > backing_size - base_offset) {
            CloseHandle(win->handle);
            free(win);
            return size_status != INFS_STATUS_OK ?
                size_status : INFS_STATUS_INVALID_ARGUMENT;
        }
    }

    if (win->is_device && !has_region && writable && lock_and_dismount) {
        DWORD returned = 0;
        if (!DeviceIoControl(win->handle, FSCTL_LOCK_VOLUME,
                             NULL, 0, NULL, 0, &returned, NULL)) {
            infs_status status = status_from_win32(GetLastError());
            CloseHandle(win->handle);
            free(win);
            return status;
        }
        win->locked = 1;
        if (!DeviceIoControl(win->handle, FSCTL_DISMOUNT_VOLUME,
                             NULL, 0, NULL, 0, &returned, NULL)) {
            DWORD error = GetLastError();
            if (error != ERROR_NOT_SUPPORTED && error != ERROR_INVALID_FUNCTION) {
                DeviceIoControl(win->handle, FSCTL_UNLOCK_VOLUME,
                                NULL, 0, NULL, 0, &returned, NULL);
                CloseHandle(win->handle);
                free(win);
                return status_from_win32(error);
            }
        }
    }

    storage->ops = &win32_storage_ops;
    storage->context = win;
    return INFS_STATUS_OK;
}

infs_status infs_win32_storage_open(struct infs_storage *storage,
                                    const wchar_t *path, int writable,
                                    int lock_and_dismount)
{
    return open_common(storage, path, writable, lock_and_dismount,
                       0, 0, 0);
}

infs_status infs_win32_storage_open_region(struct infs_storage *storage,
                                           const wchar_t *path,
                                           uint64_t base_offset,
                                           uint64_t region_size,
                                           int writable)
{
    return open_common(storage, path, writable, 0,
                       1, base_offset, region_size);
}
#endif
