// SPDX-License-Identifier: GPL-3.0-or-later
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include "infilfs/storage.h"
#include "infilfs/volume.h"
#include "infiltratorfs-windows-metadata.h"
#include "../../windows-driver/infiltratorfs-native-protocol.h"

#include <intrin.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INFILFS_NATIVE_CONTROL L"\\\\.\\InfiltratorFSControl"
#define INFILFS_NATIVE_WORKERS 8u

struct native_storage_context {
    HANDLE control;
    uint64_t volume_id;
    uint64_t size_bytes;
};

struct native_volume {
    uint64_t volume_id;
    struct native_storage_context storage_context;
    struct infs_volume volume;
    int open;
    CRITICAL_SECTION lock;
    struct native_volume *next;
};

static CRITICAL_SECTION g_volume_lock;
static struct native_volume *g_volumes;
static volatile LONG g_stop;
static HANDLE g_stop_event;
static HANDLE g_worker_threads[INFILFS_NATIVE_WORKERS];
static HANDLE g_worker_controls[INFILFS_NATIVE_WORKERS];
static DWORD g_worker_count;
static SERVICE_STATUS_HANDLE g_service_status_handle;
static SERVICE_STATUS g_service_status;

static infs_status win32_error_status(DWORD error)
{
    switch (error) {
    case ERROR_SUCCESS: return INFS_STATUS_OK;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY: return INFS_STATUS_NO_MEMORY;
    case ERROR_WRITE_PROTECT: return INFS_STATUS_READ_ONLY;
    case ERROR_DISK_FULL: return INFS_STATUS_NO_SPACE;
    case ERROR_NOT_SUPPORTED: return INFS_STATUS_NOT_SUPPORTED;
    default: return INFS_STATUS_IO_ERROR;
    }
}

static infs_status native_raw_transfer(
    struct native_storage_context *ctx, int write,
    uint64_t offset, void *buffer, size_t size)
{
    if (!ctx || !ctx->control || (size && !buffer) ||
        size > INFILFS_WIN_NATIVE_IO_CHUNK)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (offset > ctx->size_bytes ||
        (uint64_t)size > ctx->size_bytes - offset)
        return INFS_STATUS_IO_ERROR;

    size_t bytes = offsetof(struct infilfs_win_native_raw_io, data) + size;
    struct infilfs_win_native_raw_io *io = calloc(1, bytes);
    if (!io)
        return INFS_STATUS_NO_MEMORY;
    io->protocol_version = INFILFS_WIN_NATIVE_PROTOCOL_VERSION;
    io->volume_id = ctx->volume_id;
    io->offset = offset;
    io->size = (uint32_t)size;
    if (write && size)
        memcpy(io->data, buffer, size);

    DWORD returned = 0;
    BOOL okay = DeviceIoControl(
        ctx->control,
        write ? IOCTL_INFILFS_NATIVE_RAW_WRITE :
                IOCTL_INFILFS_NATIVE_RAW_READ,
        io, (DWORD)bytes, io, (DWORD)bytes, &returned, NULL);
    infs_status status = okay ? INFS_STATUS_OK :
                              win32_error_status(GetLastError());
    if (okay && !write && size)
        memcpy(buffer, io->data, size);
    free(io);
    return status;
}

static infs_status native_read(
    void *opaque, uint64_t offset, void *buffer, size_t size)
{
    struct native_storage_context *ctx = opaque;
    size_t done = 0;
    while (done < size) {
        size_t chunk = size - done;
        if (chunk > INFILFS_WIN_NATIVE_IO_CHUNK)
            chunk = INFILFS_WIN_NATIVE_IO_CHUNK;
        infs_status status = native_raw_transfer(
            ctx, 0, offset + done, (uint8_t *)buffer + done, chunk);
        if (status != INFS_STATUS_OK)
            return status;
        done += chunk;
    }
    return INFS_STATUS_OK;
}

static infs_status native_write(
    void *opaque, uint64_t offset, const void *buffer, size_t size)
{
    struct native_storage_context *ctx = opaque;
    size_t done = 0;
    while (done < size) {
        size_t chunk = size - done;
        if (chunk > INFILFS_WIN_NATIVE_IO_CHUNK)
            chunk = INFILFS_WIN_NATIVE_IO_CHUNK;
        infs_status status = native_raw_transfer(
            ctx, 1, offset + done, (void *)((const uint8_t *)buffer + done),
            chunk);
        if (status != INFS_STATUS_OK)
            return status;
        done += chunk;
    }
    return INFS_STATUS_OK;
}

static infs_status native_flush(void *opaque)
{
    struct native_storage_context *ctx = opaque;
    struct infilfs_win_native_volume_info info = {0};
    info.protocol_version = INFILFS_WIN_NATIVE_PROTOCOL_VERSION;
    info.volume_id = ctx->volume_id;
    DWORD returned = 0;
    if (!DeviceIoControl(
            ctx->control, IOCTL_INFILFS_NATIVE_RAW_FLUSH,
            &info, sizeof(info), NULL, 0, &returned, NULL))
        return win32_error_status(GetLastError());
    return INFS_STATUS_OK;
}

static infs_status native_size(
    void *opaque, uint64_t *size_bytes, int *is_device)
{
    struct native_storage_context *ctx = opaque;
    if (!ctx || !size_bytes || !is_device)
        return INFS_STATUS_INVALID_ARGUMENT;
    *size_bytes = ctx->size_bytes;
    *is_device = 1;
    return INFS_STATUS_OK;
}

static infs_status native_random(void *opaque, void *buffer, size_t size)
{
    (void)opaque;
    if (!buffer && size)
        return INFS_STATUS_INVALID_ARGUMENT;
    return BCryptGenRandom(
               NULL, buffer, (ULONG)size,
               BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ?
           INFS_STATUS_OK : INFS_STATUS_IO_ERROR;
}

static infs_status native_time(
    void *opaque, struct infs_timestamp *time)
{
    FILETIME ft;
    ULARGE_INTEGER ticks;
    const uint64_t windows_epoch = UINT64_C(116444736000000000);
    (void)opaque;
    if (!time)
        return INFS_STATUS_INVALID_ARGUMENT;
    GetSystemTimePreciseAsFileTime(&ft);
    ticks.LowPart = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;
    if (ticks.QuadPart < windows_epoch)
        return INFS_STATUS_IO_ERROR;
    uint64_t unix100ns = ticks.QuadPart - windows_epoch;
    time->seconds = (int64_t)(unix100ns / UINT64_C(10000000));
    time->nanoseconds =
        (uint32_t)((unix100ns % UINT64_C(10000000)) * 100u);
    return INFS_STATUS_OK;
}

static void native_close(void *opaque)
{
    (void)opaque;
}

static const struct infs_storage_ops native_storage_ops = {
    .read_at = native_read,
    .write_at = native_write,
    .flush = native_flush,
    .get_size = native_size,
    .random_bytes = native_random,
    .current_time = native_time,
    .close = native_close,
};

static int query_volume(
    HANDLE control, uint64_t volume_id,
    struct infilfs_win_native_volume_info *info)
{
    DWORD returned = 0;
    memset(info, 0, sizeof(*info));
    uint64_t input = volume_id;
    return DeviceIoControl(
        control, IOCTL_INFILFS_NATIVE_VOLUME_QUERY,
        &input, sizeof(input), info, sizeof(*info), &returned, NULL) &&
        returned >= sizeof(*info) &&
        info->protocol_version == INFILFS_WIN_NATIVE_PROTOCOL_VERSION;
}

static struct native_volume *find_volume_locked(uint64_t volume_id)
{
    for (struct native_volume *v = g_volumes; v; v = v->next)
        if (v->volume_id == volume_id)
            return v;
    return NULL;
}

static struct native_volume *get_volume(
    HANDLE control, uint64_t volume_id)
{
    EnterCriticalSection(&g_volume_lock);
    struct native_volume *existing = find_volume_locked(volume_id);
    if (existing) {
        LeaveCriticalSection(&g_volume_lock);
        return existing;
    }

    struct infilfs_win_native_volume_info info;
    if (!query_volume(control, volume_id, &info)) {
        LeaveCriticalSection(&g_volume_lock);
        return NULL;
    }

    struct native_volume *v = calloc(1, sizeof(*v));
    if (!v) {
        LeaveCriticalSection(&g_volume_lock);
        return NULL;
    }
    v->volume_id = volume_id;
    v->storage_context.control = control;
    v->storage_context.volume_id = volume_id;
    v->storage_context.size_bytes = info.size_bytes;
    InitializeCriticalSection(&v->lock);

    struct infs_storage storage = {
        .ops = &native_storage_ops,
        .context = &v->storage_context,
    };
    infs_status status = infs_volume_open_storage(
        &v->volume, &storage, info.read_only ? 0 : 1);
    if (status != INFS_STATUS_OK) {
        DeleteCriticalSection(&v->lock);
        free(v);
        LeaveCriticalSection(&g_volume_lock);
        return NULL;
    }
    v->open = 1;
    v->next = g_volumes;
    g_volumes = v;
    LeaveCriticalSection(&g_volume_lock);
    return v;
}

static int request_path_utf8(
    const uint16_t *wide, uint32_t chars,
    char out[INFS_PATH_MAX + 1u])
{
    if (!out || chars >= INFILFS_WIN_NATIVE_PATH_CHARS)
        return 0;
    if (!chars) {
        strcpy_s(out, INFS_PATH_MAX + 1u, "/");
        return 1;
    }

    wchar_t temp[INFILFS_WIN_NATIVE_PATH_CHARS];
    memcpy(temp, wide, (size_t)chars * sizeof(uint16_t));
    temp[chars] = L'\0';
    for (uint32_t i = 0; i < chars; ++i)
        if (temp[i] == L'\\')
            temp[i] = L'/';

    int prefix = temp[0] == L'/' ? 0 : 1;
    if (prefix)
        out[0] = '/';
    int bytes = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS,
        temp, -1, out + prefix,
        (int)(INFS_PATH_MAX + 1u - (size_t)prefix),
        NULL, NULL);
    return bytes > 0;
}

static uint64_t portable_file_id(const uint8_t object_id[16])
{
    uint64_t low = 0, high = 0;
    memcpy(&low, object_id, sizeof(low));
    memcpy(&high, object_id + sizeof(low), sizeof(high));
    uint64_t id = low ^ _rotl64(high, 23);
    return id ? id : UINT64_C(1);
}

static void attrs_to_native(
    const struct infs_attributes *in,
    struct infilfs_win_native_attributes *out)
{
    memset(out, 0, sizeof(*out));
    out->file_id = portable_file_id(in->object_id);
    out->logical_size = in->logical_size;
    out->allocation_size = in->allocated_size;
    out->creation_time_100ns =
        (uint64_t)infilfs_timestamp_to_windows_ticks(&in->birth_time);
    out->access_time_100ns =
        (uint64_t)infilfs_timestamp_to_windows_ticks(&in->access_time);
    out->write_time_100ns =
        (uint64_t)infilfs_timestamp_to_windows_ticks(
            &in->modification_time);
    out->change_time_100ns =
        (uint64_t)infilfs_timestamp_to_windows_ticks(&in->change_time);
    out->link_count = (uint32_t)(
        in->link_count > UINT32_MAX ? UINT32_MAX : in->link_count);
    out->file_attributes = infilfs_portable_to_windows_attributes(
        in->portable_flags, in->object_type == INFS_OBJECT_DIRECTORY);
    if (in->object_type == INFS_OBJECT_DIRECTORY)
        out->object_type = INFILFS_WIN_NATIVE_OBJECT_DIRECTORY;
    else if (in->object_type == INFS_OBJECT_SYMLINK)
        out->object_type = INFILFS_WIN_NATIVE_OBJECT_SYMLINK;
    else
        out->object_type = INFILFS_WIN_NATIVE_OBJECT_FILE;
}

static void response_status(
    struct infilfs_win_native_response *response, infs_status status)
{
    response->protocol_version = INFILFS_WIN_NATIVE_PROTOCOL_VERSION;
    response->status = status;
}

static void dispatch_lookup(
    struct native_volume *v,
    const struct infilfs_win_native_request *request,
    struct infilfs_win_native_response *response)
{
    char path[INFS_PATH_MAX + 1u];
    struct infs_attributes attrs;
    if (!request_path_utf8(request->path, request->path_chars, path)) {
        response_status(response, INFS_STATUS_NAME_TOO_LONG);
        return;
    }
    infs_status status = infs_get_attributes(&v->volume, path, &attrs);
    response_status(response, status);
    if (status == INFS_STATUS_OK)
        attrs_to_native(&attrs, &response->attributes);
}

static void dispatch_read(
    struct native_volume *v,
    const struct infilfs_win_native_request *request,
    struct infilfs_win_native_response *response)
{
    char path[INFS_PATH_MAX + 1u];
    if (!request_path_utf8(request->path, request->path_chars, path) ||
        request->length > sizeof(response->output)) {
        response_status(response, INFS_STATUS_INVALID_ARGUMENT);
        return;
    }
    int64_t got = infs_read_file(
        &v->volume, path, response->output,
        (size_t)request->length, request->offset);
    if (got < 0) {
        response_status(response, (infs_status)got);
        return;
    }
    response->output_bytes = (uint32_t)got;
    response_status(response, INFS_STATUS_OK);
}

static void dispatch_write(
    struct native_volume *v,
    const struct infilfs_win_native_request *request,
    struct infilfs_win_native_response *response)
{
    char path[INFS_PATH_MAX + 1u];
    if (!request_path_utf8(request->path, request->path_chars, path) ||
        request->input_bytes > sizeof(request->input) ||
        request->input_bytes != request->length) {
        response_status(response, INFS_STATUS_INVALID_ARGUMENT);
        return;
    }
    int64_t wrote =
        (request->flags & INFILFS_WIN_NATIVE_REQ_WRITE_THROUGH) ?
        infs_write_file(
            &v->volume, path, request->input,
            request->input_bytes, request->offset) :
        infs_write_file_buffered(
            &v->volume, path, request->input,
            request->input_bytes, request->offset);
    response_status(
        response, wrote < 0 ? (infs_status)wrote : INFS_STATUS_OK);
}

static void dispatch_enum(
    struct native_volume *v,
    const struct infilfs_win_native_request *request,
    struct infilfs_win_native_response *response)
{
    char path[INFS_PATH_MAX + 1u];
    if (!request_path_utf8(request->path, request->path_chars, path)) {
        response_status(response, INFS_STATUS_NAME_TOO_LONG);
        return;
    }
    struct infs_dir_item *items = NULL;
    size_t count = 0;
    infs_status status = infs_list_dir(
        &v->volume, path, &items, &count);
    if (status != INFS_STATUS_OK) {
        response_status(response, status);
        return;
    }

    size_t start = (size_t)request->offset;
    size_t used = 0;
    uint32_t entries = 0;
    for (size_t i = start; i < count; ++i) {
        struct infs_attributes attrs;
        char child[INFS_PATH_MAX + 1u];
        int n = strcmp(path, "/") == 0 ?
            _snprintf_s(child, sizeof(child), _TRUNCATE,
                        "/%s", items[i].name) :
            _snprintf_s(child, sizeof(child), _TRUNCATE,
                        "%s/%s", path, items[i].name);
        if (n < 0)
            continue;
        status = infs_get_attributes(&v->volume, child, &attrs);
        if (status != INFS_STATUS_OK)
            break;

        struct infilfs_win_native_dirent entry;
        memset(&entry, 0, sizeof(entry));
        attrs_to_native(&attrs, &entry.attributes);
        int chars = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, items[i].name, -1,
            (wchar_t *)entry.name,
            INFILFS_WIN_NATIVE_NAME_CHARS);
        if (chars <= 0)
            continue;
        entry.name_chars = (uint32_t)(chars - 1);
        if (used + sizeof(entry) > sizeof(response->output))
            break;
        memcpy(response->output + used, &entry, sizeof(entry));
        used += sizeof(entry);
        entries++;
    }
    infs_free_dir_items(items);
    response->output_bytes = (uint32_t)used;
    response->entry_count = entries;
    response_status(response, status == INFS_STATUS_OK ?
                              INFS_STATUS_OK : status);
}

static void dispatch_query_security(
    struct native_volume *v,
    const struct infilfs_win_native_request *request,
    struct infilfs_win_native_response *response)
{
    char path[INFS_PATH_MAX + 1u];
    if (!request_path_utf8(request->path, request->path_chars, path)) {
        response_status(response, INFS_STATUS_NAME_TOO_LONG);
        return;
    }

    struct infs_attributes attrs;
    infs_status status = infs_get_attributes(
        &v->volume, path, &attrs);
    if (status != INFS_STATUS_OK) {
        response_status(response, status);
        return;
    }

    struct infs_security_descriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    status = infs_get_security_descriptor(
        &v->volume, path, &descriptor);
    if (status == INFS_STATUS_NOT_FOUND) {
        response_status(response, INFS_STATUS_NOT_FOUND);
        return;
    }
    if (status != INFS_STATUS_OK) {
        response_status(response, status);
        return;
    }

    void *native = NULL;
    uint32_t bytes = 0;
    status = infilfs_windows_security_from_portable(
        &v->volume, &descriptor,
        attrs.object_type == INFS_OBJECT_DIRECTORY,
        &native, &bytes);
    infs_free_security_descriptor(&descriptor);
    if (status != INFS_STATUS_OK) {
        response_status(response, status);
        return;
    }
    if (bytes > sizeof(response->output)) {
        free(native);
        response_status(response, INFS_STATUS_OVERFLOW);
        return;
    }
    memcpy(response->output, native, bytes);
    response->output_bytes = bytes;
    free(native);
    response_status(response, INFS_STATUS_OK);
}

static void dispatch_set_security(
    struct native_volume *v,
    const struct infilfs_win_native_request *request,
    struct infilfs_win_native_response *response)
{
    char path[INFS_PATH_MAX + 1u];
    if (!request_path_utf8(request->path, request->path_chars, path) ||
        request->input_bytes == 0 ||
        request->input_bytes > sizeof(request->input)) {
        response_status(response, INFS_STATUS_INVALID_ARGUMENT);
        return;
    }

    PSECURITY_DESCRIPTOR native =
        (PSECURITY_DESCRIPTOR)(void *)request->input;
    if (!IsValidSecurityDescriptor(native)) {
        response_status(response, INFS_STATUS_CORRUPT);
        return;
    }

    struct infs_attributes attrs;
    infs_status status = infs_get_attributes(
        &v->volume, path, &attrs);
    if (status != INFS_STATUS_OK) {
        response_status(response, status);
        return;
    }

    struct infs_security_descriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    status = infilfs_windows_security_to_portable(
        &v->volume, native,
        attrs.object_type == INFS_OBJECT_DIRECTORY,
        &descriptor);
    if (status == INFS_STATUS_OK)
        status = infs_set_security_descriptor(
            &v->volume, path, &descriptor);
    infs_free_security_descriptor(&descriptor);
    response_status(response, status);
}

static void dispatch_mutation(
    struct native_volume *v,
    const struct infilfs_win_native_request *request,
    struct infilfs_win_native_response *response)
{
    char path[INFS_PATH_MAX + 1u];
    char second[INFS_PATH_MAX + 1u];
    infs_status status = INFS_STATUS_INVALID_ARGUMENT;

    if (!request_path_utf8(request->path, request->path_chars, path)) {
        response_status(response, INFS_STATUS_NAME_TOO_LONG);
        return;
    }

    switch (request->opcode) {
    case INFILFS_WIN_NATIVE_OP_CREATE:
        status = infs_create_file(&v->volume, path, NULL);
        break;
    case INFILFS_WIN_NATIVE_OP_MKDIR:
        status = infs_mkdir(&v->volume, path, NULL);
        break;
    case INFILFS_WIN_NATIVE_OP_UNLINK:
        status = infs_unlink(&v->volume, path);
        break;
    case INFILFS_WIN_NATIVE_OP_RMDIR:
        status = infs_rmdir(&v->volume, path);
        break;
    case INFILFS_WIN_NATIVE_OP_TRUNCATE:
        status = infs_truncate_file(
            &v->volume, path, request->length);
        break;
    case INFILFS_WIN_NATIVE_OP_RENAME:
        if (!request_path_utf8(
                request->second_path, request->second_path_chars, second))
            status = INFS_STATUS_NAME_TOO_LONG;
        else
            status = infs_rename(&v->volume, path, second);
        break;
    default:
        break;
    }
    response_status(response, status);
}

static void dispatch_request(
    HANDLE control,
    const struct infilfs_win_native_request *request,
    struct infilfs_win_native_response *response)
{
    memset(response, 0, sizeof(*response));
    response->protocol_version = INFILFS_WIN_NATIVE_PROTOCOL_VERSION;
    response->request_id = request->request_id;

    if (request->protocol_version != INFILFS_WIN_NATIVE_PROTOCOL_VERSION) {
        response_status(response, INFS_STATUS_NOT_SUPPORTED);
        return;
    }

    struct native_volume *v = get_volume(control, request->volume_id);
    if (!v) {
        response_status(response, INFS_STATUS_IO_ERROR);
        return;
    }

    EnterCriticalSection(&v->lock);
    switch (request->opcode) {
    case INFILFS_WIN_NATIVE_OP_LOOKUP:
        dispatch_lookup(v, request, response);
        break;
    case INFILFS_WIN_NATIVE_OP_ENUMERATE:
        dispatch_enum(v, request, response);
        break;
    case INFILFS_WIN_NATIVE_OP_READ:
        dispatch_read(v, request, response);
        break;
    case INFILFS_WIN_NATIVE_OP_WRITE:
        dispatch_write(v, request, response);
        break;
    case INFILFS_WIN_NATIVE_OP_FLUSH:
        response_status(response, infs_volume_sync(&v->volume));
        break;
    case INFILFS_WIN_NATIVE_OP_QUERY_SECURITY:
        dispatch_query_security(v, request, response);
        break;
    case INFILFS_WIN_NATIVE_OP_SET_SECURITY:
        dispatch_set_security(v, request, response);
        break;
    case INFILFS_WIN_NATIVE_OP_CREATE:
    case INFILFS_WIN_NATIVE_OP_MKDIR:
    case INFILFS_WIN_NATIVE_OP_UNLINK:
    case INFILFS_WIN_NATIVE_OP_RMDIR:
    case INFILFS_WIN_NATIVE_OP_RENAME:
    case INFILFS_WIN_NATIVE_OP_TRUNCATE:
        dispatch_mutation(v, request, response);
        break;
    default:
        response_status(response, INFS_STATUS_NOT_SUPPORTED);
        break;
    }
    LeaveCriticalSection(&v->lock);
}

static DWORD WINAPI worker_main(void *opaque)
{
    size_t worker = (size_t)(uintptr_t)opaque;
    HANDLE control = CreateFileW(
        INFILFS_NATIVE_CONTROL,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (control == INVALID_HANDLE_VALUE)
        return GetLastError();
    if (worker < INFILFS_NATIVE_WORKERS)
        InterlockedExchangePointer(
            (PVOID volatile *)&g_worker_controls[worker], control);

    struct infilfs_win_native_request *request =
        malloc(sizeof(*request));
    struct infilfs_win_native_response *response =
        malloc(sizeof(*response));
    if (!request || !response) {
        free(response);
        free(request);
        CloseHandle(control);
        return ERROR_OUTOFMEMORY;
    }

    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        DWORD returned = 0;
        memset(request, 0, sizeof(*request));
        if (!DeviceIoControl(
                control, IOCTL_INFILFS_NATIVE_WAIT_REQUEST,
                NULL, 0, request, sizeof(*request), &returned, NULL)) {
            DWORD error = GetLastError();
            if (error == ERROR_OPERATION_ABORTED)
                break;
            Sleep(100);
            continue;
        }
        dispatch_request(control, request, response);
        returned = 0;
        if (!DeviceIoControl(
                control, IOCTL_INFILFS_NATIVE_COMPLETE_REQUEST,
                response, sizeof(*response), NULL, 0, &returned, NULL))
            Sleep(10);
    }

    free(response);
    free(request);
    if (worker < INFILFS_NATIVE_WORKERS)
        InterlockedExchangePointer(
            (PVOID volatile *)&g_worker_controls[worker], NULL);
    CloseHandle(control);
    return ERROR_SUCCESS;
}

static void close_volumes(void)
{
    EnterCriticalSection(&g_volume_lock);
    struct native_volume *v = g_volumes;
    g_volumes = NULL;
    LeaveCriticalSection(&g_volume_lock);
    while (v) {
        struct native_volume *next = v->next;
        if (v->open)
            infs_volume_close(&v->volume);
        DeleteCriticalSection(&v->lock);
        free(v);
        v = next;
    }
}

static void request_stop(void)
{
    if (InterlockedExchange(&g_stop, 1) != 0)
        return;
    if (g_stop_event)
        SetEvent(g_stop_event);
    for (DWORD i = 0; i < INFILFS_NATIVE_WORKERS; ++i) {
        HANDLE control = (HANDLE)InterlockedCompareExchangePointer(
            (PVOID volatile *)&g_worker_controls[i], NULL, NULL);
        if (control && control != INVALID_HANDLE_VALUE)
            (void)CancelIoEx(control, NULL);
    }
}

static int start_workers(void)
{
    g_worker_count = 0;
    for (DWORD i = 0; i < INFILFS_NATIVE_WORKERS; ++i) {
        g_worker_threads[i] = CreateThread(
            NULL, 0, worker_main, (void *)(uintptr_t)i, 0, NULL);
        if (!g_worker_threads[i])
            break;
        g_worker_count++;
    }
    return g_worker_count != 0;
}

static void wait_workers(void)
{
    if (g_worker_count)
        WaitForMultipleObjects(
            g_worker_count, g_worker_threads, TRUE, INFINITE);
    for (DWORD i = 0; i < g_worker_count; ++i) {
        if (g_worker_threads[i]) {
            CloseHandle(g_worker_threads[i]);
            g_worker_threads[i] = NULL;
        }
    }
    g_worker_count = 0;
}

static void service_report(DWORD state, DWORD error, DWORD hint)
{
    if (!g_service_status_handle)
        return;
    memset(&g_service_status, 0, sizeof(g_service_status));
    g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_service_status.dwCurrentState = state;
    g_service_status.dwWin32ExitCode = error;
    g_service_status.dwWaitHint = hint;
    g_service_status.dwControlsAccepted =
        state == SERVICE_RUNNING ?
        SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    SetServiceStatus(g_service_status_handle, &g_service_status);
}

static DWORD WINAPI service_control(
    DWORD control, DWORD event_type, void *event_data, void *context)
{
    (void)event_type;
    (void)event_data;
    (void)context;
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        service_report(SERVICE_STOP_PENDING, NO_ERROR, 5000);
        request_stop();
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        SetServiceStatus(g_service_status_handle, &g_service_status);
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static void WINAPI service_main(DWORD argc, wchar_t **argv)
{
    (void)argc;
    (void)argv;
    g_service_status_handle = RegisterServiceCtrlHandlerExW(
        L"InfiltratorFSNative", service_control, NULL);
    if (!g_service_status_handle)
        return;

    service_report(SERVICE_START_PENDING, NO_ERROR, 5000);
    g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stop_event) {
        service_report(
            SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    InterlockedExchange(&g_stop, 0);
    if (!start_workers()) {
        DWORD error = GetLastError();
        CloseHandle(g_stop_event);
        g_stop_event = NULL;
        service_report(
            SERVICE_STOPPED, error ? error : ERROR_SERVICE_NOT_ACTIVE, 0);
        return;
    }

    service_report(SERVICE_RUNNING, NO_ERROR, 0);
    WaitForSingleObject(g_stop_event, INFINITE);
    request_stop();
    wait_workers();
    close_volumes();
    CloseHandle(g_stop_event);
    g_stop_event = NULL;
    service_report(SERVICE_STOPPED, NO_ERROR, 0);
}

static BOOL WINAPI console_control(DWORD control)
{
    if (control == CTRL_C_EVENT || control == CTRL_BREAK_EVENT ||
        control == CTRL_CLOSE_EVENT || control == CTRL_SHUTDOWN_EVENT) {
        request_stop();
        return TRUE;
    }
    return FALSE;
}

static int run_console(void)
{
    g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stop_event)
        return 1;
    InterlockedExchange(&g_stop, 0);
    SetConsoleCtrlHandler(console_control, TRUE);
    if (!start_workers()) {
        CloseHandle(g_stop_event);
        g_stop_event = NULL;
        return 1;
    }

    wprintf(
        L"InfiltratorFS native filesystem service running with %lu workers.\n",
        (unsigned long)g_worker_count);
    WaitForSingleObject(g_stop_event, INFINITE);
    request_stop();
    wait_workers();
    close_volumes();
    SetConsoleCtrlHandler(console_control, FALSE);
    CloseHandle(g_stop_event);
    g_stop_event = NULL;
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    InitializeCriticalSection(&g_volume_lock);

    int result = 0;
    if (argc > 1 && _wcsicmp(argv[1], L"--console") == 0) {
        result = run_console();
    } else {
        SERVICE_TABLE_ENTRYW dispatch[] = {
            { L"InfiltratorFSNative", service_main },
            { NULL, NULL }
        };
        if (!StartServiceCtrlDispatcherW(dispatch)) {
            DWORD error = GetLastError();
            if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
                result = run_console();
            else
                result = (int)error;
        }
    }

    close_volumes();
    DeleteCriticalSection(&g_volume_lock);
    return result;
}
