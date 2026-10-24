// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * InfiltratorFS-owned UEFI Simple File System bridge.
 *
 * The filesystem semantics and on-disk parsing remain in the canonical
 * portable InfiltratorFS core. This file only adapts that core to FSW/UEFI.
 */

#include "fsw_infiltratorfs.h"
#include "fsw_efi.h"

#include "infilfs/status.h"
#include "infilfs/storage.h"

extern EFI_GUID gMyEfiBlockIoProtocolGuid;

struct fsw_infiltratorfs_storage_context {
    struct fsw_infiltratorfs_volume *vol;
};

static fsw_status_t fsw_infiltratorfs_volume_mount(struct fsw_infiltratorfs_volume *vol);
static void fsw_infiltratorfs_volume_free(struct fsw_infiltratorfs_volume *vol);
static fsw_status_t fsw_infiltratorfs_volume_stat(struct fsw_infiltratorfs_volume *vol,
                                                  struct fsw_volume_stat *sb);
static fsw_status_t fsw_infiltratorfs_dnode_fill(struct fsw_infiltratorfs_volume *vol,
                                                 struct fsw_infiltratorfs_dnode *dno);
static void fsw_infiltratorfs_dnode_free(struct fsw_infiltratorfs_volume *vol,
                                         struct fsw_infiltratorfs_dnode *dno);
static fsw_status_t fsw_infiltratorfs_dnode_stat(struct fsw_infiltratorfs_volume *vol,
                                                 struct fsw_infiltratorfs_dnode *dno,
                                                 struct fsw_dnode_stat *sb);
static fsw_status_t fsw_infiltratorfs_get_extent(struct fsw_infiltratorfs_volume *vol,
                                                 struct fsw_infiltratorfs_dnode *dno,
                                                 struct fsw_extent *extent);
static fsw_status_t fsw_infiltratorfs_dir_lookup(struct fsw_infiltratorfs_volume *vol,
                                                 struct fsw_infiltratorfs_dnode *dno,
                                                 struct fsw_string *lookup_name,
                                                 struct fsw_infiltratorfs_dnode **child_dno);
static fsw_status_t fsw_infiltratorfs_dir_read(struct fsw_infiltratorfs_volume *vol,
                                               struct fsw_infiltratorfs_dnode *dno,
                                               struct fsw_shandle *shand,
                                               struct fsw_infiltratorfs_dnode **child_dno);
static fsw_status_t fsw_infiltratorfs_readlink(struct fsw_infiltratorfs_volume *vol,
                                               struct fsw_infiltratorfs_dnode *dno,
                                               struct fsw_string *link);

struct fsw_fstype_table FSW_FSTYPE_TABLE_NAME(infiltratorfs) = {
    { FSW_STRING_TYPE_ISO88591, 13, 13, "infiltratorfs" },
    sizeof(struct fsw_infiltratorfs_volume),
    sizeof(struct fsw_infiltratorfs_dnode),

    fsw_infiltratorfs_volume_mount,
    fsw_infiltratorfs_volume_free,
    fsw_infiltratorfs_volume_stat,
    fsw_infiltratorfs_dnode_fill,
    fsw_infiltratorfs_dnode_free,
    fsw_infiltratorfs_dnode_stat,
    fsw_infiltratorfs_get_extent,
    fsw_infiltratorfs_dir_lookup,
    fsw_infiltratorfs_dir_read,
    fsw_infiltratorfs_readlink,
};

static size_t infs_fsw_cstrlen(const char *text)
{
    size_t len = 0;
    if (!text)
        return 0;
    while (text[len] != 0)
        ++len;
    return len;
}

static fsw_u64 infs_fsw_path_id(const char *path)
{
    fsw_u64 hash = 1469598103934665603ULL;
    const unsigned char *p = (const unsigned char *)path;

    while (p && *p) {
        hash ^= (fsw_u64)*p++;
        hash *= 1099511628211ULL;
    }
    return hash ? hash : 1;
}

static fsw_status_t infs_fsw_map_status(infs_status status)
{
    switch (status) {
    case INFS_STATUS_OK:
        return FSW_SUCCESS;
    case INFS_STATUS_NO_MEMORY:
        return FSW_OUT_OF_MEMORY;
    case INFS_STATUS_IO_ERROR:
        return FSW_IO_ERROR;
    case INFS_STATUS_NOT_FOUND:
        return FSW_NOT_FOUND;
    case INFS_STATUS_NOT_SUPPORTED:
        return FSW_UNSUPPORTED;
    case INFS_STATUS_CORRUPT:
    case INFS_STATUS_LOOP_DETECTED:
        return FSW_VOLUME_CORRUPTED;
    default:
        return FSW_UNKNOWN_ERROR;
    }
}

static int infs_fsw_type(uint16_t type)
{
    switch (type) {
    case INFS_OBJECT_DIRECTORY:
        return FSW_DNODE_TYPE_DIR;
    case INFS_OBJECT_FILE:
        return FSW_DNODE_TYPE_FILE;
    case INFS_OBJECT_SYMLINK:
        return FSW_DNODE_TYPE_SYMLINK;
    default:
        return FSW_DNODE_TYPE_SPECIAL;
    }
}

static fsw_status_t infs_fsw_storage_read_at(void *opaque, uint64_t offset,
                                             void *buffer, size_t size)
{
    struct fsw_infiltratorfs_storage_context *context =
        (struct fsw_infiltratorfs_storage_context *)opaque;
    FSW_VOLUME_DATA *host;
    EFI_STATUS status;

    if (!context || !context->vol || (!buffer && size != 0))
        return INFS_STATUS_INVALID_ARGUMENT;
    if (size == 0)
        return INFS_STATUS_OK;

    host = (FSW_VOLUME_DATA *)context->vol->g.host_data;
    if (!host || !host->DiskIo)
        return INFS_STATUS_IO_ERROR;

    status = refit_call5_wrapper(host->DiskIo->ReadDisk,
                                 host->DiskIo,
                                 host->MediaId,
                                 (UINT64)offset,
                                 (UINTN)size,
                                 (VOID *)buffer);
    host->LastIOStatus = status;
    return EFI_ERROR(status) ? INFS_STATUS_IO_ERROR : INFS_STATUS_OK;
}

static fsw_status_t infs_fsw_storage_get_size(void *opaque, uint64_t *size_bytes,
                                              int *is_device)
{
    struct fsw_infiltratorfs_storage_context *context =
        (struct fsw_infiltratorfs_storage_context *)opaque;
    FSW_VOLUME_DATA *host;
    EFI_BLOCK_IO *block_io = NULL;
    EFI_STATUS status;
    UINT64 blocks;
    UINT64 block_size;

    if (!context || !context->vol || !size_bytes || !is_device)
        return INFS_STATUS_INVALID_ARGUMENT;

    host = (FSW_VOLUME_DATA *)context->vol->g.host_data;
    if (!host)
        return INFS_STATUS_IO_ERROR;

    status = refit_call3_wrapper(BS->HandleProtocol,
                                 host->Handle,
                                 &gMyEfiBlockIoProtocolGuid,
                                 (VOID **)&block_io);
    if (EFI_ERROR(status) || !block_io || !block_io->Media)
        return INFS_STATUS_IO_ERROR;

    blocks = (UINT64)block_io->Media->LastBlock + 1ULL;
    block_size = (UINT64)block_io->Media->BlockSize;
    if (block_size == 0 || blocks > (~(UINT64)0) / block_size)
        return INFS_STATUS_OVERFLOW;

    *size_bytes = blocks * block_size;
    *is_device = 1;
    return INFS_STATUS_OK;
}

static void infs_fsw_storage_close(void *opaque)
{
    if (opaque)
        fsw_free(opaque);
}

static const struct infs_storage_ops infs_fsw_storage_ops = {
    infs_fsw_storage_read_at,
    NULL,
    NULL,
    NULL,
    NULL,
    infs_fsw_storage_get_size,
    NULL,
    NULL,
    infs_fsw_storage_close,
};

static fsw_status_t infs_fsw_set_path(struct fsw_infiltratorfs_dnode *dno,
                                      const char *path)
{
    size_t len;
    char *copy;
    fsw_status_t status;

    if (!dno || !path)
        return FSW_UNKNOWN_ERROR;
    len = infs_fsw_cstrlen(path);
    if (len >= INFS_PATH_MAX)
        return FSW_UNSUPPORTED;

    status = fsw_alloc((int)len + 1, (void **)&copy);
    if (status)
        return status;
    if (len)
        fsw_memcpy(copy, path, len);
    copy[len] = 0;

    if (dno->path)
        fsw_free(dno->path);
    dno->path = copy;
    dno->attributes_valid = 0;
    return FSW_SUCCESS;
}

static fsw_status_t infs_fsw_child_path(struct fsw_infiltratorfs_dnode *parent,
                                        struct fsw_string *name,
                                        char **path_out)
{
    struct fsw_string utf8;
    size_t parent_len;
    size_t name_len;
    size_t total;
    int add_slash;
    char *path;
    fsw_status_t status;

    if (!parent || !parent->path || !name || !path_out)
        return FSW_UNKNOWN_ERROR;

    utf8.type = FSW_STRING_TYPE_EMPTY;
    utf8.len = utf8.size = 0;
    utf8.data = NULL;
    status = fsw_strdup_coerce(&utf8, FSW_STRING_TYPE_UTF8, name);
    if (status)
        return status;

    parent_len = infs_fsw_cstrlen(parent->path);
    name_len = (size_t)utf8.size;
    add_slash = parent_len > 0 && parent->path[parent_len - 1] != '/';
    total = parent_len + (add_slash ? 1u : 0u) + name_len;
    if (total >= INFS_PATH_MAX) {
        fsw_strfree(&utf8);
        return FSW_UNSUPPORTED;
    }

    status = fsw_alloc((int)total + 1, (void **)&path);
    if (status) {
        fsw_strfree(&utf8);
        return status;
    }

    if (parent_len)
        fsw_memcpy(path, parent->path, parent_len);
    if (add_slash)
        path[parent_len++] = '/';
    if (name_len)
        fsw_memcpy(path + parent_len, utf8.data, name_len);
    path[total] = 0;

    fsw_strfree(&utf8);
    *path_out = path;
    return FSW_SUCCESS;
}

static fsw_status_t infs_fsw_create_child(struct fsw_infiltratorfs_volume *vol,
                                          struct fsw_infiltratorfs_dnode *parent,
                                          struct fsw_string *name,
                                          const char *path,
                                          struct fsw_infiltratorfs_dnode **child_out)
{
    struct infs_lookup lookup;
    struct fsw_infiltratorfs_dnode *child;
    fsw_status_t status;
    infs_status infs_result;

    infs_result = infs_lookup_path(&vol->infs, path, &lookup);
    if (infs_result != INFS_STATUS_OK)
        return infs_fsw_map_status(infs_result);

    status = fsw_dnode_create_with_tree(parent,
                                        infs_fsw_path_id(path),
                                        (fsw_u64)lookup.block,
                                        infs_fsw_type(lookup.type),
                                        name,
                                        &child);
    if (status)
        return status;

    if (!child->path) {
        status = infs_fsw_set_path(child, path);
        if (status) {
            fsw_dnode_release((struct fsw_dnode *)child);
            return status;
        }
    }
    *child_out = child;
    return FSW_SUCCESS;
}

static fsw_status_t fsw_infiltratorfs_volume_mount(struct fsw_infiltratorfs_volume *vol)
{
    struct fsw_infiltratorfs_storage_context *context = NULL;
    struct infs_storage storage;
    struct infs_lookup root_lookup;
    struct fsw_infiltratorfs_dnode *root;
    struct fsw_string label;
    size_t label_len = 0;
    fsw_status_t status;
    infs_status infs_result;

    infs_fsw_uefi_trace("INFS_CORE_MOUNT_ENTER\r\n");
    fsw_set_blocksize(vol, INFS_BLOCK_SIZE, INFS_BLOCK_SIZE);
    infs_fsw_uefi_trace("INFS_CORE_SETBLOCK_DONE\r\n");

    status = fsw_alloc_zero(sizeof(*context), (void **)&context);
    if (status)
        return status;
    context->vol = vol;
    infs_fsw_uefi_trace("INFS_CORE_CONTEXT_READY\r\n");

    storage.ops = &infs_fsw_storage_ops;
    storage.context = context;
    infs_fsw_uefi_trace("INFS_CORE_OPEN_BEGIN\r\n");
    infs_result = infs_volume_open_storage(&vol->infs, &storage, 0);
    infs_fsw_uefi_trace(infs_result == INFS_STATUS_OK ?
                         "INFS_CORE_OPEN_SUCCESS\r\n" :
                         "INFS_CORE_OPEN_REJECT\r\n");
    if (infs_result != INFS_STATUS_OK) {
        infs_storage_close(&storage);
        return infs_fsw_map_status(infs_result);
    }
    vol->infs_open = 1;

    while (label_len < INFS_LABEL_MAX && vol->infs.sb.label[label_len] != 0)
        ++label_len;
    label.type = FSW_STRING_TYPE_UTF8;
    label.len = label.size = (int)label_len;
    label.data = vol->infs.sb.label;
    status = fsw_strdup_coerce(&vol->g.label, vol->g.host_string_type, &label);
    if (status)
        return status;

    infs_fsw_uefi_trace("INFS_CORE_LOOKUP_ROOT_BEGIN\r\n");
    infs_result = infs_lookup_path(&vol->infs, "/", &root_lookup);
    infs_fsw_uefi_trace(infs_result == INFS_STATUS_OK ?
                         "INFS_CORE_LOOKUP_ROOT_OK\r\n" :
                         "INFS_CORE_LOOKUP_ROOT_FAIL\r\n");
    if (infs_result != INFS_STATUS_OK)
        return infs_fsw_map_status(infs_result);

    status = fsw_dnode_create_root(vol, (fsw_u64)root_lookup.block, &vol->g.root);
    if (status)
        return status;

    root = (struct fsw_infiltratorfs_dnode *)vol->g.root;
    status = infs_fsw_set_path(root, "/");
    if (status)
        return status;

    return FSW_SUCCESS;
}

static void fsw_infiltratorfs_volume_free(struct fsw_infiltratorfs_volume *vol)
{
    if (vol->infs_open) {
        infs_volume_close(&vol->infs);
        vol->infs_open = 0;
    }
}

static fsw_status_t fsw_infiltratorfs_volume_stat(struct fsw_infiltratorfs_volume *vol,
                                                  struct fsw_volume_stat *sb)
{
    uint64_t total = vol->infs.size_bytes;
    uint64_t free_blocks = infs_le64_to_cpu(vol->infs.sb.free_blocks);

    if (!sb)
        return FSW_UNKNOWN_ERROR;
    sb->total_bytes = (fsw_u64)total;
    if (free_blocks > (~(uint64_t)0) / INFS_BLOCK_SIZE)
        return FSW_VOLUME_CORRUPTED;
    sb->free_bytes = (fsw_u64)(free_blocks * INFS_BLOCK_SIZE);
    return FSW_SUCCESS;
}

static fsw_status_t fsw_infiltratorfs_dnode_fill(struct fsw_infiltratorfs_volume *vol,
                                                 struct fsw_infiltratorfs_dnode *dno)
{
    infs_status result;

    if (!dno || !dno->path)
        return FSW_VOLUME_CORRUPTED;
    if (dno->attributes_valid)
        return FSW_SUCCESS;

    result = infs_get_attributes(&vol->infs, dno->path, &dno->attributes);
    if (result != INFS_STATUS_OK)
        return infs_fsw_map_status(result);

    dno->g.type = infs_fsw_type(dno->attributes.object_type);
    dno->g.size = (fsw_u64)dno->attributes.logical_size;
    dno->attributes_valid = 1;
    return FSW_SUCCESS;
}

static void fsw_infiltratorfs_dnode_free(struct fsw_infiltratorfs_volume *vol,
                                         struct fsw_infiltratorfs_dnode *dno)
{
    (void)vol;
    if (dno->path) {
        fsw_free(dno->path);
        dno->path = NULL;
    }
}

static fsw_u32 infs_fsw_time32(const struct infs_timestamp *time)
{
    if (!time || time->seconds <= 0)
        return 0;
    if ((uint64_t)time->seconds > 0xffffffffULL)
        return 0xffffffffU;
    return (fsw_u32)time->seconds;
}

static fsw_status_t fsw_infiltratorfs_dnode_stat(struct fsw_infiltratorfs_volume *vol,
                                                 struct fsw_infiltratorfs_dnode *dno,
                                                 struct fsw_dnode_stat *sb)
{
    fsw_status_t status;
    fsw_u16 mode;

    status = fsw_infiltratorfs_dnode_fill(vol, dno);
    if (status)
        return status;

    sb->used_bytes = (fsw_u64)dno->attributes.allocated_size;
    mode = (fsw_u16)(dno->attributes.posix_permissions & 07777u);
    switch (dno->g.type) {
    case FSW_DNODE_TYPE_DIR:
        mode |= S_IFDIR;
        break;
    case FSW_DNODE_TYPE_SYMLINK:
        mode |= S_IFLNK;
        break;
    case FSW_DNODE_TYPE_FILE:
        mode |= S_IFREG;
        break;
    default:
        break;
    }
    fsw_store_attr_posix(sb, mode);
    fsw_store_time_posix(sb, FSW_DNODE_STAT_CTIME,
                         infs_fsw_time32(&dno->attributes.change_time));
    fsw_store_time_posix(sb, FSW_DNODE_STAT_MTIME,
                         infs_fsw_time32(&dno->attributes.modification_time));
    fsw_store_time_posix(sb, FSW_DNODE_STAT_ATIME,
                         infs_fsw_time32(&dno->attributes.access_time));
    return FSW_SUCCESS;
}

static fsw_status_t fsw_infiltratorfs_get_extent(struct fsw_infiltratorfs_volume *vol,
                                                 struct fsw_infiltratorfs_dnode *dno,
                                                 struct fsw_extent *extent)
{
    uint64_t offset;
    uint64_t remaining;
    size_t requested;
    void *buffer;
    int64_t read_result;
    fsw_status_t status;

    if (!extent || !dno || !dno->path)
        return FSW_UNKNOWN_ERROR;
    if (extent->log_start > (~(uint64_t)0) / INFS_BLOCK_SIZE)
        return FSW_VOLUME_CORRUPTED;

    offset = (uint64_t)extent->log_start * INFS_BLOCK_SIZE;
    if (offset >= dno->g.size)
        return FSW_NOT_FOUND;

    status = fsw_alloc(INFS_BLOCK_SIZE, &buffer);
    if (status)
        return status;
    fsw_memzero(buffer, INFS_BLOCK_SIZE);

    remaining = dno->g.size - offset;
    requested = remaining < INFS_BLOCK_SIZE ? (size_t)remaining : INFS_BLOCK_SIZE;
    read_result = infs_read_file(&vol->infs, dno->path, buffer, requested, offset);
    if (read_result < 0) {
        fsw_free(buffer);
        return infs_fsw_map_status((infs_status)read_result);
    }
    if ((size_t)read_result != requested) {
        fsw_free(buffer);
        return FSW_VOLUME_CORRUPTED;
    }

    extent->type = FSW_EXTENT_TYPE_BUFFER;
    extent->log_count = 1;
    extent->buffer = buffer;
    extent->phys_start = 0;
    return FSW_SUCCESS;
}

static fsw_status_t fsw_infiltratorfs_dir_lookup(struct fsw_infiltratorfs_volume *vol,
                                                 struct fsw_infiltratorfs_dnode *dno,
                                                 struct fsw_string *lookup_name,
                                                 struct fsw_infiltratorfs_dnode **child_dno)
{
    char *path = NULL;
    fsw_status_t status;

    status = infs_fsw_child_path(dno, lookup_name, &path);
    if (status)
        return status;
    status = infs_fsw_create_child(vol, dno, lookup_name, path, child_dno);
    fsw_free(path);
    return status;
}

static fsw_status_t fsw_infiltratorfs_dir_read(struct fsw_infiltratorfs_volume *vol,
                                               struct fsw_infiltratorfs_dnode *dno,
                                               struct fsw_shandle *shand,
                                               struct fsw_infiltratorfs_dnode **child_dno)
{
    struct infs_dir_item *items = NULL;
    size_t count = 0;
    size_t index;
    struct fsw_string name;
    char *path = NULL;
    fsw_status_t status;
    infs_status result;

    if (!dno || !dno->path || !shand)
        return FSW_UNKNOWN_ERROR;

    result = infs_list_dir(&vol->infs, dno->path, &items, &count);
    if (result != INFS_STATUS_OK)
        return infs_fsw_map_status(result);

    index = (size_t)shand->pos;
    if (index >= count) {
        infs_free_dir_items(items);
        return FSW_NOT_FOUND;
    }

    name.type = FSW_STRING_TYPE_UTF8;
    name.len = name.size = (int)infs_fsw_cstrlen(items[index].name);
    name.data = items[index].name;

    status = infs_fsw_child_path(dno, &name, &path);
    if (!status)
        status = infs_fsw_create_child(vol, dno, &name, path, child_dno);
    if (!status)
        shand->pos++;

    if (path)
        fsw_free(path);
    infs_free_dir_items(items);
    return status;
}

static fsw_status_t fsw_infiltratorfs_readlink(struct fsw_infiltratorfs_volume *vol,
                                               struct fsw_infiltratorfs_dnode *dno,
                                               struct fsw_string *link)
{
    char *target = NULL;
    size_t length = 0;
    struct fsw_string source;
    fsw_status_t status;
    infs_status result;

    if (!dno || !dno->path || !link)
        return FSW_UNKNOWN_ERROR;

    status = fsw_alloc(INFS_PATH_MAX + 1, (void **)&target);
    if (status)
        return status;

    result = infs_read_symlink(&vol->infs, dno->path, target,
                               INFS_PATH_MAX + 1, &length);
    if (result != INFS_STATUS_OK) {
        fsw_free(target);
        return infs_fsw_map_status(result);
    }
    if (length > INFS_PATH_MAX) {
        fsw_free(target);
        return FSW_VOLUME_CORRUPTED;
    }

    source.type = FSW_STRING_TYPE_UTF8;
    source.len = source.size = (int)length;
    source.data = target;
    status = fsw_strdup_coerce(link, vol->g.host_string_type, &source);
    fsw_free(target);
    return status;
}
