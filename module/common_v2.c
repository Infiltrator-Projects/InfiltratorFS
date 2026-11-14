// SPDX-License-Identifier: GPL-3.0-or-later
/* Common V2 adapter over the InfiltratorFS portable format engine.
 * This interface exposes read-only access. Its firmware profile requires
 * the matching freestanding build and independent firmware qualification. */
#include <infiltratr/filesystem_module_v2.h>
#include "infilfs/fs.h"
#include "infilfs/endian.h"
#include "infilfs/volume.h"
#include <limits.h>
#include <string.h>
#ifdef INFS_COMMON_V2_FIRMWARE
void infs_v2_set_host(const IfsFsHostRuntimeV2 *host);
#define USE_HOST(host) infs_v2_set_host(host)
#else
#define USE_HOST(host) ((void)(host))
#endif
struct v2_volume {
    const IfsFsHostRuntimeV2 *host;
    struct infs_volume volume;
};
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
    default: return IFS_FS_INVALID;
    }
}
static infs_status storage_read(void *opaque, uint64_t offset, void *buffer, size_t length)
{
    const IfsFsHostRuntimeV2 *host = opaque;
    unsigned char *destination = buffer;
    if (length && !buffer) return INFS_STATUS_INVALID_ARGUMENT;
    while (length) {
        uint32_t chunk = length > UINT32_MAX ? UINT32_MAX : (uint32_t)length;
        if (ifs_fs_host_read_bytes_v2(host, offset, destination, chunk) != IFS_FS_OK)
            return INFS_STATUS_IO_ERROR;
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
static const struct infs_storage_ops storage_ops = {
    .read_at = storage_read, .get_size = storage_size
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
        /* Recover through the owner's checkpoint reader if the primary
         * signature is damaged but a backup remains valid. */
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
        return result == INFS_STATUS_IO_ERROR ? IFS_FS_IO_ERROR : IFS_FS_NOT_FOUND;
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
    if (!(host->capabilities & IFS_FS_HOST_READ_ONLY)) return IFS_FS_ACCESS;
    struct v2_volume *v = host->allocate(host->context, sizeof(*v));
    if (!v) return IFS_FS_NO_MEMORY;
    memset(v, 0, sizeof(*v)); v->host = host;
    struct infs_storage storage = { &storage_ops, (void *)host };
    infs_status status = infs_volume_open_storage(&v->volume, &storage, 0);
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
    USE_HOST(host); infs_volume_close(&v->volume); host->release(host->context, v);
    return IFS_FS_OK;
}
static int stat_path(void *volume, const char *path, IfsFsEntryV2 *entry)
{
    struct v2_volume *v = volume; struct infs_attributes attr;
    if (!v || !valid_path(path) || !entry || entry->structure_bytes < sizeof(*entry) || !entry->name)
        return IFS_FS_INVALID;
    USE_HOST(v->host);
    infs_status status = infs_get_attributes(&v->volume, path, &attr);
    if (status != INFS_STATUS_OK) return map_status(status);
    const char *leaf = strrchr(path, '/'); leaf = leaf ? leaf + 1 : path;
    size_t length = strlen(leaf);
    if (length >= entry->name_capacity || length > UINT32_MAX) return IFS_FS_INVALID;
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
    if (!v || !valid_path(path) || !entry) return IFS_FS_INVALID;
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
    size_t length = 0;
    while (length < INFS_LABEL_MAX && v->volume.sb.label[length]) ++length;
    if (length >= capacity) return IFS_FS_INVALID;
    memcpy(out, v->volume.sb.label, length); out[length] = 0; *bytes = (uint32_t)length;
    return IFS_FS_OK;
}
static const IfsFsCoreOpsV2 core = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(core), IFS_FS_GROUP_CORE, 0,
        IFS_FS_CORE_PROBE | IFS_FS_CORE_MOUNT | IFS_FS_CORE_STAT | IFS_FS_CORE_LIST | IFS_FS_CORE_READ},
    .probe = probe, .mount = mount_volume, .unmount = unmount_volume,
    .stat = stat_path, .directory_entry = list_path, .read = read_file
};
static const IfsFsVolumeOpsV2 volume_ops = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(volume_ops), IFS_FS_GROUP_VOLUME, 0,
        IFS_FS_VOLUME_QUERY | IFS_FS_VOLUME_LABEL_GET}, .query = query_volume, .label = label
};
static const IfsFsGroupHeaderV2 *query(uint32_t group, uint32_t low, uint32_t high)
{
    if (low > IFS_FS_GROUP_ABI_V2 || high < IFS_FS_GROUP_ABI_V2) return NULL;
    if (group == IFS_FS_GROUP_CORE) return &core.header;
    if (group == IFS_FS_GROUP_VOLUME) return &volume_ops.header;
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
        .group_mask = IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE) | IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME),
        .query_group = query
    };
    return &api;
}
