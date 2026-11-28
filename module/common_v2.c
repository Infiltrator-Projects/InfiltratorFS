// SPDX-License-Identifier: GPL-3.0-or-later
/* Common V2 adapter over the InfiltratorFS portable format engine.
 * Writable access requires exclusive durable host I/O, a clock and entropy. */
#include <infiltratr/filesystem_module_v2.h>
#include <infiltratr/filesystem_block_map_v2.h>
#include "infilfs/fs.h"
#include "infilfs/endian.h"
#include "infilfs/locator.h"
#include "infilfs/volume.h"
#ifndef INFS_COMMON_V2_FIRMWARE
#include "infilfs/check.h"
#endif
#include <limits.h>
#include <stdatomic.h>
#include <string.h>
#ifdef INFS_COMMON_V2_FIRMWARE
void infs_v2_set_host(const IfsFsHostRuntimeV2 *host);
#define USE_HOST(host) infs_v2_set_host(host)
#else
#define USE_HOST(host) ((void)(host))
#endif
struct v2_volume {
    /* Own the size-versioned host descriptor for the full mount lifetime.
     * Callback context itself remains owned by the V2 host lease. */
    IfsFsHostRuntimeV2 retained_host;
    const IfsFsHostRuntimeV2 *host;
    struct infs_volume volume;
};

/* Opaque generation handles are never allocator addresses. The registry
 * and portable engine are protected by a nonblocking operation lease:
 * a recursive callback must not tear down a live volume. The firmware
 * allocator has a shared host binding, so even distinct mounts are
 * serialized until that runtime becomes per-volume. */
enum { MAX_V2_SESSIONS = 32 };
static struct {
    uintptr_t token;
    struct v2_volume *volume;
} sessions[MAX_V2_SESSIONS];
static uintptr_t sequence;
static atomic_flag operation_gate=ATOMIC_FLAG_INIT;
static int enter_operation(void)
{
    return !atomic_flag_test_and_set_explicit(&operation_gate,memory_order_acquire);
}
static int leave_operation(int rc)
{
    USE_HOST(NULL);
    atomic_flag_clear_explicit(&operation_gate,memory_order_release);
    return rc;
}
static struct v2_volume *lookup_volume(void *handle)
{
    const uintptr_t token=(uintptr_t)handle;
    if (!token) return NULL;
    for (unsigned i=0;i<MAX_V2_SESSIONS;++i)
        if (sessions[i].token==token) return sessions[i].volume;
    return NULL;
}
static int map_status(infs_status status)
{
    switch (status) {
    case INFS_STATUS_OK: return IFS_FS_OK;
    case INFS_STATUS_NOT_FOUND: return IFS_FS_NOT_FOUND;
    case INFS_STATUS_IO_ERROR: return IFS_FS_IO_ERROR;
    case INFS_STATUS_CORRUPT: return IFS_FS_CORRUPT;
    case INFS_STATUS_NO_MEMORY: return IFS_FS_NO_MEMORY;
    case INFS_STATUS_NO_SPACE: return IFS_FS_NO_SPACE;
    case INFS_STATUS_BUSY: return IFS_FS_BUSY;
    case INFS_STATUS_READ_ONLY: return IFS_FS_ACCESS;
    case INFS_STATUS_NOT_SUPPORTED: return IFS_FS_UNSUPPORTED;
    case INFS_STATUS_ALREADY_EXISTS: return IFS_FS_EXISTS;
    default: return IFS_FS_INVALID;
    }
}
static infs_status storage_read(void *opaque, uint64_t offset, void *buffer, size_t length)
{
    const IfsFsHostRuntimeV2 *host = opaque;
    unsigned char *destination = buffer;
    if (!host || (length && !buffer) || offset>host->volume_bytes ||
        length>host->volume_bytes-offset) return INFS_STATUS_INVALID_ARGUMENT;
    while (length) {
        uint32_t chunk = length > UINT32_MAX ? UINT32_MAX : (uint32_t)length;
        int result = ifs_fs_host_read_bytes_v2(host, offset, destination, chunk);
        if (result != IFS_FS_OK)
            return result == IFS_FS_NO_MEMORY ? INFS_STATUS_NO_MEMORY :
                   result == IFS_FS_INVALID ? INFS_STATUS_INVALID_ARGUMENT :
                   INFS_STATUS_IO_ERROR;
        offset += chunk; destination += chunk; length -= chunk;
    }
    return INFS_STATUS_OK;
}
static infs_status storage_size(void *opaque, uint64_t *bytes, int *is_device)
{
    const IfsFsHostRuntimeV2 *host = opaque;
    if (!host || !bytes || !is_device) return INFS_STATUS_INVALID_ARGUMENT;
    *bytes = host->volume_bytes; *is_device = 1;
    return INFS_STATUS_OK;
}
static infs_status storage_write(void *opaque, uint64_t offset, const void *buffer, size_t length)
{
    const IfsFsHostRuntimeV2 *host = opaque;
    const unsigned char *source = buffer;
    if (!host || (length && !buffer) || offset>host->volume_bytes ||
        length>host->volume_bytes-offset) return INFS_STATUS_INVALID_ARGUMENT;
    while (length) {
        uint32_t chunk = length > UINT32_MAX ? UINT32_MAX : (uint32_t)length;
        int result = ifs_fs_host_write_bytes_v2(host, offset, source, chunk);
        if (result != IFS_FS_OK)
            return result == IFS_FS_NO_MEMORY ? INFS_STATUS_NO_MEMORY :
                   result == IFS_FS_INVALID ? INFS_STATUS_INVALID_ARGUMENT :
                   INFS_STATUS_IO_ERROR;
        offset += chunk; source += chunk; length -= chunk;
    }
    return INFS_STATUS_OK;
}
static infs_status storage_flush(void *opaque)
{
    const IfsFsHostRuntimeV2 *host = opaque;
    return host && host->flush && !host->flush(host->context) ? INFS_STATUS_OK : INFS_STATUS_IO_ERROR;
}
static infs_status storage_random(void *opaque, void *buffer, size_t length)
{
    const IfsFsHostRuntimeV2 *host = opaque;
    if (length > UINT32_MAX) return INFS_STATUS_INVALID_ARGUMENT;
    return ifs_fs_host_random_bytes_v2(host, buffer, (uint32_t)length) == IFS_FS_OK ?
        INFS_STATUS_OK : INFS_STATUS_IO_ERROR;
}
static infs_status storage_time(void *opaque, struct infs_timestamp *time)
{
    const IfsFsHostRuntimeV2 *host = opaque;
    if (!host || !time || !(host->capabilities & IFS_FS_HOST_CLOCK) || !host->realtime_ns)
        return INFS_STATUS_INVALID_ARGUMENT;
    uint64_t now = host->realtime_ns(host->context);
    time->seconds = (int64_t)(now / UINT64_C(1000000000));
    time->nanoseconds = (uint32_t)(now % UINT64_C(1000000000));
    return INFS_STATUS_OK;
}
static const struct infs_storage_ops storage_ops = {
    .read_at = storage_read, .write_at = storage_write, .flush = storage_flush,
    .get_size = storage_size, .random_bytes = storage_random, .current_time = storage_time
};
static int valid_path(const char *path)
{
    if (!path || path[0] != '/') return 0;
    for (size_t i = 0; i <= INFS_PATH_MAX; ++i)
        if (!path[i]) return 1;
    return 0;
}
static int probe(const IfsFsHostRuntimeV2 *host, uint32_t *variant)
{
    uint16_t major, minor;
    if (!ifs_fs_host_v2_valid(host) || !variant) return IFS_FS_INVALID;
    struct infs_storage storage = { &storage_ops, (void *)host };
    infs_status result = infs_probe_format_signature(&storage, &major, &minor);
    if (result == INFS_STATUS_NOT_FOUND) {
        /* Distinguish foreign/blank media from recognized but damaged
         * InfiltratorFS. Locator signatures are evidence of format identity,
         * not authority to mount; the core must still validate the graph.
         * Read both independently so failure of one does not hide recovery
         * through the other. Without either signature, there is no reliable
         * basis for identifying a destroyed filesystem as InfiltratorFS. */
        const uint64_t positions[] = { INFS_LOCATOR_BLOCK_A, INFS_LOCATOR_BLOCK_B };
        int recognized = 0;
        infs_status read_error = INFS_STATUS_OK;
        for (unsigned i = 0; i < 2u; ++i) {
            uint8_t prefix[16];
            uint64_t offset = positions[i] * INFS_BLOCK_SIZE;
            if (offset > host->volume_bytes ||
                sizeof(prefix) > host->volume_bytes - offset)
                continue;
            infs_status status = infs_storage_read(&storage, offset,
                                                   prefix, sizeof(prefix));
            if (status != INFS_STATUS_OK) {
                if (read_error == INFS_STATUS_OK) read_error = status;
                continue;
            }
            if (memcmp(prefix, "INFSL019", 8u) == 0)
                recognized = 1;
        }
        if (!recognized)
            return read_error == INFS_STATUS_OK ? IFS_FS_NOT_FOUND :
                   map_status(read_error);
        if (host->volume_bytes / INFS_BLOCK_SIZE < INFS_LOCATOR_MIN_VOLUME_BLOCKS)
            return IFS_FS_CORRUPT;
        USE_HOST(host);
        struct infs_volume *volume = host->allocate(host->context, sizeof(*volume));
        if (!volume) return IFS_FS_NO_MEMORY;
        memset(volume, 0, sizeof(*volume));
        result = infs_volume_open_storage(volume, &storage, 0);
        if (result == INFS_STATUS_OK) {
            infs_volume_close(volume);
            *variant = ((uint32_t)INFS_FORMAT_MAJOR << 16) | INFS_FORMAT_MINOR;
        }
        host->release(host->context, volume);
        if (result == INFS_STATUS_OK) return IFS_FS_OK;
        return map_status(result);
    }
    if (result != INFS_STATUS_OK) return map_status(result);
    if (major != INFS_FORMAT_MAJOR || minor != INFS_FORMAT_MINOR) return IFS_FS_UNSUPPORTED;
    *variant = ((uint32_t)major << 16) | minor;
    return IFS_FS_OK;
}
static int mount_volume(const IfsFsHostRuntimeV2 *host, void **out, IfsFsVolumeInfoV2 *info)
{
    uint32_t variant;
    if (!out || !info || info->structure_bytes < sizeof(*info)) return IFS_FS_INVALID;
    *out = NULL; USE_HOST(host);
    int result = probe(host, &variant);
    if (result != IFS_FS_OK) return result;
    int writable = !(host->capabilities & IFS_FS_HOST_READ_ONLY);
    if (writable) {
        unsigned char entropy;
        if (!(host->capabilities & IFS_FS_HOST_CLOCK) ||
            ifs_fs_host_random_bytes_v2(host, &entropy, 1) != IFS_FS_OK)
            return IFS_FS_UNSUPPORTED;
    }
    struct v2_volume *v = host->allocate(host->context, sizeof(*v));
    if (!v) return IFS_FS_NO_MEMORY;
    memset(v, 0, sizeof(*v));
    const size_t host_bytes=host->structure_bytes<sizeof(v->retained_host)?
        host->structure_bytes:sizeof(v->retained_host);
    memcpy(&v->retained_host,host,host_bytes);
    v->retained_host.structure_bytes=(uint32_t)host_bytes;
    v->host=&v->retained_host;
    USE_HOST(v->host);
    struct infs_storage storage = { &storage_ops, (void *)v->host };
    infs_status status = infs_volume_open_storage(&v->volume, &storage, writable);
    if (status != INFS_STATUS_OK) { host->release(host->context, v); return map_status(status); }
    *info = (IfsFsVolumeInfoV2){.structure_bytes = sizeof(*info), .variant = variant,
        .max_name_bytes = INFS_NAME_MAX, .block_bytes = INFS_BLOCK_SIZE,
        .total_bytes = infs_le64_to_cpu(v->volume.sb.total_blocks) * INFS_BLOCK_SIZE,
        .free_bytes = infs_le64_to_cpu(v->volume.sb.free_blocks) * INFS_BLOCK_SIZE};
    *out = v; return IFS_FS_OK;
}
static int unmount_volume(void *volume)
{
    struct v2_volume *v = volume;
    if (!v) return IFS_FS_INVALID;
    const IfsFsHostRuntimeV2 *host = v->host;
    USE_HOST(host);
    infs_volume_close(&v->volume);
    /* The release callback must run while its retained descriptor lives. */
    host->release(host->context, v);
    return IFS_FS_OK;
}
static int stat_path(void *volume, const char *path, IfsFsEntryV2 *entry)
{
    struct v2_volume *v = volume; struct infs_attributes attr;
    if (!v || !valid_path(path) || !entry || entry->structure_bytes < sizeof(*entry) || !entry->name)
        return IFS_FS_INVALID;
    entry->name_bytes=0u;
    USE_HOST(v->host);
    infs_status status = infs_get_attributes(&v->volume, path, &attr);
    if (status != INFS_STATUS_OK) return map_status(status);
    const char *leaf = strrchr(path, '/'); leaf = leaf ? leaf + 1 : path;
    size_t length = strlen(leaf);
    if (length > UINT32_MAX) return IFS_FS_INVALID;
    if (length >= entry->name_capacity) return IFS_FS_NO_SPACE;
    memcpy(entry->name, leaf, length + 1); entry->name_bytes = (uint32_t)length;
    entry->size_bytes = attr.logical_size; entry->attributes = 0;
    switch (attr.object_type) {
    case INFS_OBJECT_FILE: entry->type = IFS_FS_ENTRY_FILE; break;
    case INFS_OBJECT_DIRECTORY: entry->type = IFS_FS_ENTRY_DIRECTORY; break;
    case INFS_OBJECT_SYMLINK: entry->type = IFS_FS_ENTRY_LINK; break;
    default: return IFS_FS_UNSUPPORTED;
    }
    return IFS_FS_OK;
}
static int list_path(void *volume, const char *path, uint32_t index, IfsFsEntryV2 *entry)
{
    struct v2_volume *v = volume; struct infs_dir_item *items = NULL; size_t count = 0;
    if (!v || !valid_path(path) || !entry || entry->structure_bytes<sizeof(*entry) ||
        !entry->name) return IFS_FS_INVALID;
    entry->name_bytes=0u;
    USE_HOST(v->host);
    infs_status result = infs_list_dir(&v->volume, path, &items, &count);
    if (result != INFS_STATUS_OK) return map_status(result);
    if (index >= count) { infs_free_dir_items(items); return IFS_FS_END; }
    size_t parent = strlen(path), length = strlen(items[index].name);
    char child[INFS_PATH_MAX + 1]; int slash = parent && path[parent - 1] != '/';
    if (parent + (size_t)slash + length >= sizeof(child)) {
        infs_free_dir_items(items); return IFS_FS_INVALID;
    }
    memcpy(child, path, parent); if (slash) child[parent++] = '/';
    memcpy(child + parent, items[index].name, length + 1); infs_free_dir_items(items);
    return stat_path(v, child, entry);
}
static int read_file(void *volume, const char *path, uint64_t offset, void *buffer, uint32_t length)
{
    struct v2_volume *v = volume;
    if (!v || !valid_path(path) || (!buffer && length)) return IFS_FS_INVALID;
    USE_HOST(v->host);
    int64_t result = infs_read_file(&v->volume, path, buffer, length, offset);
    if (result < 0) return map_status((infs_status)result);
    return (uint64_t)result == length ? IFS_FS_OK : IFS_FS_IO_ERROR;
}
static int write_file(void *volume, const char *path, uint64_t offset, const void *buffer, uint32_t length)
{
    struct v2_volume *v = volume;
    if (!v || !valid_path(path) || (!buffer && length)) return IFS_FS_INVALID;
    if (!v->volume.writable) return IFS_FS_ACCESS;
    USE_HOST(v->host);
    int64_t result = infs_write_file(&v->volume, path, buffer, length, offset);
    if (result < 0) return map_status((infs_status)result);
    return (uint64_t)result == length ? IFS_FS_OK : IFS_FS_IO_ERROR;
}
static int create_path(void *volume, const char *path, uint32_t type)
{
    struct v2_volume *v = volume;
    if (!v || !valid_path(path)) return IFS_FS_INVALID;
    if (!v->volume.writable) return IFS_FS_ACCESS;
    USE_HOST(v->host);
    struct infs_create_options options = {0};
    options.posix_permissions = type == IFS_FS_ENTRY_DIRECTORY ? 0755u : 0644u;
    if (type == IFS_FS_ENTRY_FILE) return map_status(infs_create_file(&v->volume, path, &options));
    if (type == IFS_FS_ENTRY_DIRECTORY) return map_status(infs_mkdir(&v->volume, path, &options));
    return IFS_FS_UNSUPPORTED;
}
static int remove_path(void *volume, const char *path)
{
    struct v2_volume *v = volume;
    struct infs_attributes attr;
    if (!v || !valid_path(path)) return IFS_FS_INVALID;
    if (!v->volume.writable) return IFS_FS_ACCESS;
    USE_HOST(v->host);
    infs_status result = infs_get_attributes(&v->volume, path, &attr);
    if (result != INFS_STATUS_OK) return map_status(result);
    return map_status(attr.object_type == INFS_OBJECT_DIRECTORY ?
                      infs_rmdir(&v->volume, path) : infs_unlink(&v->volume, path));
}
static int rename_path(void *volume, const char *old_path, const char *new_path)
{
    struct v2_volume *v = volume;
    if (!v || !valid_path(old_path) || !valid_path(new_path)) return IFS_FS_INVALID;
    if (!v->volume.writable) return IFS_FS_ACCESS;
    USE_HOST(v->host);
    return map_status(infs_rename(&v->volume, old_path, new_path));
}
static int truncate_path(void *volume, const char *path, uint64_t bytes)
{
    struct v2_volume *v = volume;
    if (!v || !valid_path(path)) return IFS_FS_INVALID;
    if (!v->volume.writable) return IFS_FS_ACCESS;
    USE_HOST(v->host);
    return map_status(infs_truncate_file(&v->volume, path, bytes));
}
static int sync_volume(void *volume)
{
    struct v2_volume *v = volume;
    if (!v) return IFS_FS_INVALID;
    if (!v->volume.writable) return IFS_FS_ACCESS;
    USE_HOST(v->host);
    return map_status(infs_volume_sync(&v->volume));
}
static int query_volume(void *volume, IfsFsVolumeInfoV2 *info)
{
    struct v2_volume *v = volume;
    if (!v || !info || info->structure_bytes < sizeof(*info)) return IFS_FS_INVALID;
    *info = (IfsFsVolumeInfoV2){.structure_bytes = sizeof(*info),
        .variant = (INFS_FORMAT_MAJOR << 16) | INFS_FORMAT_MINOR,
        .max_name_bytes = INFS_NAME_MAX, .block_bytes = INFS_BLOCK_SIZE,
        .total_bytes = infs_le64_to_cpu(v->volume.sb.total_blocks) * INFS_BLOCK_SIZE,
        .free_bytes = infs_le64_to_cpu(v->volume.sb.free_blocks) * INFS_BLOCK_SIZE};
    return IFS_FS_OK;
}
static int label(void *volume, char *out, uint32_t capacity, uint32_t *bytes)
{
    struct v2_volume *v = volume;
    if (!v || !out || !bytes) return IFS_FS_INVALID;
    *bytes=0u;
    size_t length = 0;
    while (length < INFS_LABEL_MAX && v->volume.sb.label[length]) ++length;
    if (length >= capacity) return IFS_FS_NO_SPACE;
    memcpy(out, v->volume.sb.label, length); out[length] = 0; *bytes = (uint32_t)length;
    return IFS_FS_OK;
}
/* The exported wrappers are the only entry to the registry. Internal
 * operations can call each other without recursively acquiring the gate. */
static int v2_probe(const IfsFsHostRuntimeV2 *host,uint32_t *variant)
{
    if (variant) *variant=0u;
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(probe(host,variant));
}
static int v2_mount(const IfsFsHostRuntimeV2 *host,void **out,IfsFsVolumeInfoV2 *info)
{
    if (!out) return IFS_FS_INVALID;
    *out=NULL;
    if (!enter_operation()) return IFS_FS_BUSY;
    unsigned slot=0;
    while (slot<MAX_V2_SESSIONS && sessions[slot].token) ++slot;
    if (slot==MAX_V2_SESSIONS || sequence==UINTPTR_MAX)
        return leave_operation(IFS_FS_BUSY);
    /* Consume a generation only when reserving it; never recycle a token. */
    uintptr_t token=++sequence ^ (uintptr_t)(void *)sessions;
    if (!token) {
        if (sequence==UINTPTR_MAX) return leave_operation(IFS_FS_BUSY);
        token=++sequence ^ (uintptr_t)(void *)sessions;
    }
    void *native=NULL;
    int rc=mount_volume(host,&native,info);
    if (rc!=IFS_FS_OK) return leave_operation(rc);
    sessions[slot].volume=native;
    sessions[slot].token=token;
    *out=(void *)token;
    return leave_operation(IFS_FS_OK);
}
static int v2_unmount(void *handle)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    struct v2_volume *v=lookup_volume(handle);
    if (!v) return leave_operation(IFS_FS_INVALID);
    for (unsigned i=0;i<MAX_V2_SESSIONS;++i)
        if (sessions[i].token==(uintptr_t)handle) {
            sessions[i].token=0;
            sessions[i].volume=NULL;
            break;
        }
    return leave_operation(unmount_volume(v));
}
static int v2_stat_path(void *handle,const char *path,IfsFsEntryV2 *entry)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(stat_path(lookup_volume(handle),path,entry));
}
static int v2_list_path(void *handle,const char *path,uint32_t index,IfsFsEntryV2 *entry)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(list_path(lookup_volume(handle),path,index,entry));
}
static int v2_read_file(void *handle,const char *path,uint64_t offset,void *buffer,uint32_t length)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(read_file(lookup_volume(handle),path,offset,buffer,length));
}
static int v2_write_file(void *handle,const char *path,uint64_t offset,const void *buffer,uint32_t length)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(write_file(lookup_volume(handle),path,offset,buffer,length));
}
static int v2_create_path(void *handle,const char *path,uint32_t type)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(create_path(lookup_volume(handle),path,type));
}
static int v2_remove_path(void *handle,const char *path)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(remove_path(lookup_volume(handle),path));
}
static int v2_rename_path(void *handle,const char *old_path,const char *new_path)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(rename_path(lookup_volume(handle),old_path,new_path));
}
static int v2_truncate_path(void *handle,const char *path,uint64_t bytes)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(truncate_path(lookup_volume(handle),path,bytes));
}
static int v2_query_volume(void *handle,IfsFsVolumeInfoV2 *info)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(query_volume(lookup_volume(handle),info));
}
static int v2_sync_volume(void *handle)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(sync_volume(lookup_volume(handle)));
}
static int v2_label(void *handle,char *out,uint32_t capacity,uint32_t *bytes)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(label(lookup_volume(handle),out,capacity,bytes));
}

/* Common V2 carries portable signed nanoseconds, whereas native timestamps
 * store a signed seconds component plus normalized unsigned nanoseconds.
 * Reject unrepresentable values rather than wrapping the epoch. */
static int time_ns(const struct infs_timestamp *t,int64_t *out)
{
    const int64_t scale=INT64_C(1000000000);
    if (t->nanoseconds>=1000000000u) return 0;
    if (t->seconds>=0) {
        if (t->seconds>(INT64_MAX-(int64_t)t->nanoseconds)/scale)
            return 0;
        *out=t->seconds*scale+t->nanoseconds;
    } else {
        const int64_t tail=(int64_t)t->nanoseconds-scale;
        if (t->seconds+1<INT64_MIN/scale) return 0;
        const int64_t base=(t->seconds+1)*scale;
        if (base<INT64_MIN-tail) return 0;
        *out=base+tail;
    }
    return 1;
}
static uint32_t mode_type(uint16_t type)
{
    switch(type) {
    case INFS_OBJECT_FILE: return 0100000u;
    case INFS_OBJECT_DIRECTORY: return 0040000u;
    case INFS_OBJECT_SYMLINK: return 0120000u;
    default: return 0u;
    }
}
static int metadata_get(void *handle,const char *path,IfsFsMetadataV2 *out)
{
    if (!out || out->structure_bytes<sizeof(*out) || !valid_path(path))
        return IFS_FS_INVALID;
    if (!enter_operation()) return IFS_FS_BUSY;
    struct v2_volume *v=lookup_volume(handle);
    if (!v) return leave_operation(IFS_FS_INVALID);
    USE_HOST(v->host);
    struct infs_attributes attr;
    infs_status status=infs_get_attributes(&v->volume,path,&attr);
    if (status!=INFS_STATUS_OK) return leave_operation(map_status(status));
    uint32_t type=mode_type(attr.object_type);
    if (!type) return leave_operation(IFS_FS_UNSUPPORTED);
    IfsFsMetadataV2 result={.structure_bytes=sizeof(result),
        .mask=IFS_FS_META_GET_MODE,
        .mode=type|(attr.posix_permissions&07777u),
        .owner=attr.posix_uid,.group=attr.posix_gid};
    /* The ABI reports all four timestamps together. Leave the entire
     * optional time group absent if any value exceeds its signed range. */
    if (time_ns(&attr.birth_time,&result.created_ns) &&
        time_ns(&attr.modification_time,&result.modified_ns) &&
        time_ns(&attr.access_time,&result.accessed_ns) &&
        time_ns(&attr.change_time,&result.changed_ns))
        result.mask|=IFS_FS_META_GET_TIMES;
    else
        result.created_ns=result.modified_ns=result.accessed_ns=result.changed_ns=0;
    *out=result;
    return leave_operation(IFS_FS_OK);
}
static int metadata_set(void *handle,const char *path,const IfsFsMetadataV2 *in)
{
    if (!in || in->structure_bytes<sizeof(*in) || !valid_path(path))
        return IFS_FS_INVALID;
    /* Mode and ownership are a single native mutation. SET_TIMES is
     * deliberately not advertised until combined atomic updates qualify. */
    if (in->mask!=IFS_FS_META_SET_MODE) return IFS_FS_UNSUPPORTED;
    if (in->mode&~0177777u) return IFS_FS_INVALID;
    if (!enter_operation()) return IFS_FS_BUSY;
    struct v2_volume *v=lookup_volume(handle);
    if (!v) return leave_operation(IFS_FS_INVALID);
    if (!v->volume.writable) return leave_operation(IFS_FS_ACCESS);
    USE_HOST(v->host);
    struct infs_attributes attr;
    infs_status status=infs_get_attributes(&v->volume,path,&attr);
    if (status!=INFS_STATUS_OK) return leave_operation(map_status(status));
    const uint32_t type=in->mode&0170000u;
    if (type && type!=mode_type(attr.object_type))
        return leave_operation(IFS_FS_INVALID);
    status=infs_set_posix_compat(&v->volume,path,
        INFS_POSIX_SET_PERMISSIONS|INFS_POSIX_SET_UID|INFS_POSIX_SET_GID,
        in->mode&07777u,in->owner,in->group);
    return leave_operation(map_status(status));
}
/* Links are native namespace objects. Preserve the caller's target
 * literally (including relative paths), leaving target validation to
 * the filesystem engine rather than resolving it through the host OS. */
static int metadata_symlink(void *handle,const char *path,const char *target)
{
    if (!valid_path(path) || !target) return IFS_FS_INVALID;
    if (!enter_operation()) return IFS_FS_BUSY;
    struct v2_volume *v=lookup_volume(handle);
    if (!v) return leave_operation(IFS_FS_INVALID);
    if (!v->volume.writable) return leave_operation(IFS_FS_ACCESS);
    USE_HOST(v->host);
    const struct infs_create_options options={.posix_permissions=0777u};
    return leave_operation(map_status(infs_create_symlink(&v->volume,path,target,&options)));
}
static int metadata_readlink(void *handle,const char *path,char *out,
                             uint32_t capacity,uint32_t *bytes)
{
    if (!bytes) return IFS_FS_INVALID;
    *bytes=0u;
    if (!valid_path(path) || (!out && capacity)) return IFS_FS_INVALID;
    if (!enter_operation()) return IFS_FS_BUSY;
    struct v2_volume *v=lookup_volume(handle);
    if (!v) return leave_operation(IFS_FS_INVALID);
    USE_HOST(v->host);
    size_t length=0;
    infs_status rc=infs_read_symlink(&v->volume,path,out,capacity,&length);
    if (rc==INFS_STATUS_OVERFLOW) return leave_operation(IFS_FS_NO_SPACE);
    if (rc!=INFS_STATUS_OK) return leave_operation(map_status(rc));
    if (length>UINT32_MAX) return leave_operation(IFS_FS_NO_SPACE);
    *bytes=(uint32_t)length;
    return leave_operation(IFS_FS_OK);
}
static int metadata_hardlink(void *handle,const char *existing,const char *new_path)
{
    if (!valid_path(existing) || !valid_path(new_path)) return IFS_FS_INVALID;
    if (!enter_operation()) return IFS_FS_BUSY;
    struct v2_volume *v=lookup_volume(handle);
    if (!v) return leave_operation(IFS_FS_INVALID);
    if (!v->volume.writable) return leave_operation(IFS_FS_ACCESS);
    USE_HOST(v->host);
    /* Common consumers expect creation, not the native engine's idempotent
     * linking of an already-linked name. Hold the operation lease across
     * both lookup and mutation so an existing destination is never replaced. */
    struct infs_lookup destination;
    infs_status rc=infs_lookup_path(&v->volume,new_path,&destination);
    if (rc==INFS_STATUS_OK) return leave_operation(IFS_FS_EXISTS);
    if (rc!=INFS_STATUS_NOT_FOUND) return leave_operation(map_status(rc));
    return leave_operation(map_status(infs_link_file(&v->volume,existing,new_path)));
}
static const IfsFsMetadataOpsV2 metadata_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(metadata_ops),IFS_FS_GROUP_METADATA,0,
        IFS_FS_META_GET_MODE|IFS_FS_META_GET_TIMES|IFS_FS_META_SET_MODE|
        IFS_FS_META_SYMLINK|IFS_FS_META_READLINK|IFS_FS_META_HARDLINK},
    .get=metadata_get,.set=metadata_set,
    .symlink=metadata_symlink,.readlink=metadata_readlink,.hardlink=metadata_hardlink
};

static const IfsFsCoreOpsV2 core = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(core), IFS_FS_GROUP_CORE, 0,
        IFS_FS_CORE_PROBE | IFS_FS_CORE_MOUNT | IFS_FS_CORE_STAT | IFS_FS_CORE_LIST | IFS_FS_CORE_READ |
        IFS_FS_CORE_WRITE | IFS_FS_CORE_CREATE | IFS_FS_CORE_REMOVE | IFS_FS_CORE_RENAME | IFS_FS_CORE_TRUNCATE},
    .probe = v2_probe, .mount = v2_mount, .unmount = v2_unmount,
    .stat = v2_stat_path, .directory_entry = v2_list_path, .read = v2_read_file, .write = v2_write_file,
    .create = v2_create_path, .remove = v2_remove_path, .rename = v2_rename_path, .truncate = v2_truncate_path
};
#ifndef INFS_COMMON_V2_FIRMWARE
/* Fast structural check and deep payload scrub are distinct operations.
 * The canonical check validates checkpoint replicas, permanent recovery
 * locators, allocation ownership and namespace reachability. Its report
 * stops at the first invalid stage: return a conservative count of one
 * for a structural failure rather than inventing a total error count.
 * Neither check nor scrub performs repairs or changes media. */
static int v2_check(void *handle,uint32_t flags,uint64_t *errors)
{
    if (!errors) return IFS_FS_INVALID;
    *errors=0u;
    if (flags) return IFS_FS_UNSUPPORTED;
    if (!enter_operation()) return IFS_FS_BUSY;
    struct v2_volume *v=lookup_volume(handle);
    if (!v) return leave_operation(IFS_FS_INVALID);
    USE_HOST(v->host);
    struct infs_check_report report={0};
    infs_status rc=infs_check(&v->volume,&report);
    if (rc==INFS_STATUS_CORRUPT) *errors=1u;
    return leave_operation(map_status(rc));
}
/* Scrub the mounted committed generation and its retained snapshots using
 * the authoritative native integrity walk. The operation gate keeps this
 * view stable against concurrent provider mutations; scrub never repairs
 * or writes media. Report discovered errors even if the native traversal
 * completed successfully. Firmware deliberately does not advertise scrub. */
static int v2_scrub(void *handle,uint32_t flags,uint64_t *errors)
{
    if (!errors) return IFS_FS_INVALID;
    *errors=0;
    if (flags) return IFS_FS_UNSUPPORTED;
    if (!enter_operation()) return IFS_FS_BUSY;
    struct v2_volume *v=lookup_volume(handle);
    if (!v) return leave_operation(IFS_FS_INVALID);
    USE_HOST(v->host);
    struct infs_scrub_report report={0};
    infs_status rc=infs_scrub(&v->volume,&report);
    *errors=report.checksum_errors>UINT64_MAX-report.metadata_errors ?
        UINT64_MAX : report.checksum_errors+report.metadata_errors;
    if (rc!=INFS_STATUS_OK) return leave_operation(map_status(rc));
    return leave_operation(*errors ? IFS_FS_CORRUPT : IFS_FS_OK);
}
#endif
/* Mount reconstructs the complete allocation bitmap from the selected
 * checkpoint and verifies its free count and reserved metadata. Delayed
 * frees and unconfirmed commits must never appear as reusable storage. */
static uint32_t allocation_state(const void *context, uint64_t block)
{
    const struct infs_volume *volume = context;
    return volume->bitmap[block/8] & (1u << (block%8)) ?
        IFS_FS_SPACE_ALLOCATED : IFS_FS_SPACE_FREE;
}

static int v2_allocation_map(void *handle, uint64_t cursor, IfsFsAllocationRangeV2 *out,
                             uint32_t capacity, uint32_t *written)
{
    if (!written) return IFS_FS_INVALID;
    *written = 0;
    if (!enter_operation()) return IFS_FS_BUSY;
    struct v2_volume *v = lookup_volume(handle);
    if (!v) return leave_operation(IFS_FS_INVALID);
    if (v->volume.reopen_required_status != INFS_STATUS_OK)
        return leave_operation(map_status(v->volume.reopen_required_status));
    if (v->volume.tx_active) return leave_operation(IFS_FS_BUSY);
    USE_HOST(v->host);
    uint64_t blocks = infs_le64_to_cpu(v->volume.sb.total_blocks);
    if (!v->volume.bitmap || (blocks/8 + (blocks%8 != 0)) > v->volume.bitmap_bytes)
        return leave_operation(IFS_FS_CORRUPT);
    return leave_operation(ifs_fs_block_map_page_v2(v->host, 0, blocks, INFS_BLOCK_SIZE,
        &v->volume, allocation_state, cursor, out, capacity, written));
}
static const IfsFsAllocationOpsV2 allocation_ops = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(allocation_ops),
               IFS_FS_GROUP_ALLOCATION, 0, IFS_FS_ALLOCATION_MAP},
    .map = v2_allocation_map
};

static const IfsFsVolumeOpsV2 volume_ops = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(volume_ops), IFS_FS_GROUP_VOLUME, 0,
        IFS_FS_VOLUME_QUERY | IFS_FS_VOLUME_LABEL_GET | IFS_FS_VOLUME_SYNC
#ifndef INFS_COMMON_V2_FIRMWARE
        | IFS_FS_VOLUME_SCRUB | IFS_FS_VOLUME_CHECK
#endif
    },
    .query = v2_query_volume, .label = v2_label, .sync = v2_sync_volume
#ifndef INFS_COMMON_V2_FIRMWARE
    , .scrub = v2_scrub, .check = v2_check
#endif
};
static const IfsFsGroupHeaderV2 *query(uint32_t group, uint32_t low, uint32_t high)
{
    if (low > IFS_FS_GROUP_ABI_V2 || high < IFS_FS_GROUP_ABI_V2) return NULL;
    if (group == IFS_FS_GROUP_CORE) return &core.header;
    if (group == IFS_FS_GROUP_VOLUME) return &volume_ops.header;
    if (group == IFS_FS_GROUP_ALLOCATION) return &allocation_ops.header;
    if (group == IFS_FS_GROUP_METADATA) return &metadata_ops.header;
    return NULL;
}
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{
    static const IfsFsModuleApiV2 api = {
        .abi_version = IFS_FS_COMMON_ABI_V2, .structure_bytes = sizeof(api),
        .format_id = "infiltratorfs", .runtime_profiles =
#ifdef INFS_COMMON_V2_FIRMWARE
            IFS_FS_PROFILE_FREESTANDING | IFS_FS_PROFILE_FIRMWARE,
#else
            IFS_FS_PROFILE_HOSTED,
#endif
        .group_mask = IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE) | IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME) |
                      IFS_FS_GROUP_BIT(IFS_FS_GROUP_METADATA) | IFS_FS_GROUP_BIT(IFS_FS_GROUP_ALLOCATION),
        .query_group = query
    };
    return &api;
}
