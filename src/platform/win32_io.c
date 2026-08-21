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
    infs_status status = win32_seek(win->handle, offset);
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
    infs_status status = win32_seek(win->handle, offset);
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

static infs_status win32_get_size(void *context, uint64_t *size_bytes,
                                  int *is_device)
{
    struct infs_win32_storage_context *win = context;
    if (!size_bytes || !is_device)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (win->is_device) {
        GET_LENGTH_INFORMATION length;
        DWORD returned = 0;
        if (!DeviceIoControl(win->handle, IOCTL_DISK_GET_LENGTH_INFO,
                             NULL, 0, &length, sizeof(length), &returned, NULL))
            return status_from_win32(GetLastError());
        if (length.Length.QuadPart < 0)
            return INFS_STATUS_CORRUPT;
        *size_bytes = (uint64_t)length.Length.QuadPart;
        *is_device = 1;
        return INFS_STATUS_OK;
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

static infs_status win32_time(void *context, int64_t *time_ns)
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

infs_status infs_win32_storage_open(struct infs_storage *storage,
                                    const wchar_t *path, int writable,
                                    int lock_and_dismount)
{
    if (!storage || !path || !path[0])
        return INFS_STATUS_INVALID_ARGUMENT;
    storage->ops = NULL;
    storage->context = NULL;

    struct infs_win32_storage_context *win = calloc(1u, sizeof(*win));
    if (!win)
        return INFS_STATUS_NO_MEMORY;
    win->handle = INVALID_HANDLE_VALUE;
    win->is_device = path_is_device(path);

    DWORD access = GENERIC_READ | (writable ? GENERIC_WRITE : 0u);
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    DWORD flags = FILE_ATTRIBUTE_NORMAL | (writable ? FILE_FLAG_WRITE_THROUGH : 0u);
    win->handle = CreateFileW(path, access, share, NULL, OPEN_EXISTING,
                              flags, NULL);
    if (win->handle == INVALID_HANDLE_VALUE) {
        infs_status status = status_from_win32(GetLastError());
        free(win);
        return status;
    }

    if (win->is_device && writable && lock_and_dismount) {
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
#endif
