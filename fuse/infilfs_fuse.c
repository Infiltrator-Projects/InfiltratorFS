// SPDX-License-Identifier: GPL-3.0-or-later
#define FUSE_USE_VERSION 35
#include <fuse3/fuse.h>

#include "infilfs/checksum.h"
#include "infilfs/format.h"
#include "infilfs/fs.h"
#include "infilfs/posix_io.h"
#include "infilfs/volume.h"

#include "infilfs/endian.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <unistd.h>

#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE 0x01
#endif
#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE 0x02
#endif

#define INFS_NS_PER_SECOND INT64_C(1000000000)
#define INFS_HANDLE_MAGIC UINT64_C(0x494e4653484e444c)
#define INFS_ORPHAN_PREFIX ".infilfs-open-handles-"
#define INFS_LINUX_META_DIRECTORY "/.infilfs-posix-meta"
#define INFS_LINUX_META_MAGIC "INPSXM01"
#define INFS_LINUX_META_VERSION UINT32_C(1)
#define INFS_LINUX_META_MAX (UINT32_C(1024) * 1024u)

struct infs_linux_meta_header {
    uint8_t magic[8];
    uint32_t version;
    uint32_t special_mode;
    uint64_t special_rdev;
    uint32_t xattr_bytes;
    uint32_t reserved;
};

struct infs_linux_xattr_record {
    uint16_t name_length;
    uint16_t reserved;
    uint32_t value_length;
};

_Static_assert(sizeof(struct infs_linux_meta_header) == 32u,
               "Linux metadata header layout changed");
_Static_assert(sizeof(struct infs_linux_xattr_record) == 8u,
               "Linux xattr record layout changed");

static struct infs_volume g_volume;

struct infs_open_handle {
    uint64_t magic;
    uint8_t object_id[16];
    char path[INFS_PATH_MAX + 1u];
    int orphaned;
    struct infs_open_handle *next;
};

static struct infs_open_handle *g_handles;
static char g_orphan_directory[INFS_PATH_MAX + 1u];
static unsigned g_orphan_sequence;

static struct infs_open_handle *handle_from_info(struct fuse_file_info *fi)
{
    if (!fi || fi->fh == 0)
        return NULL;
    struct infs_open_handle *handle =
        (struct infs_open_handle *)(uintptr_t)fi->fh;
    return handle->magic == INFS_HANDLE_MAGIC ? handle : NULL;
}

static const char *operation_path(const char *path,
                                  struct fuse_file_info *fi)
{
    struct infs_open_handle *handle = handle_from_info(fi);
    return handle ? handle->path : path;
}

static int path_copy(char destination[INFS_PATH_MAX + 1u],
                     const char *source)
{
    size_t length = source ? strlen(source) : 0;
    if (!source || length > INFS_PATH_MAX)
        return -ENAMETOOLONG;
    memcpy(destination, source, length + 1u);
    return 0;
}

static int add_open_handle(const char *path, const uint8_t object_id[16],
                           struct fuse_file_info *fi)
{
    if (!fi)
        return -EINVAL;
    struct infs_open_handle *handle = calloc(1, sizeof(*handle));
    if (!handle)
        return -ENOMEM;
    int rc = path_copy(handle->path, path);
    if (rc != 0) {
        free(handle);
        return rc;
    }
    handle->magic = INFS_HANDLE_MAGIC;
    memcpy(handle->object_id, object_id, 16);
    handle->next = g_handles;
    g_handles = handle;
    fi->fh = (uint64_t)(uintptr_t)handle;
    return 0;
}

static int object_has_open_handle(const uint8_t object_id[16])
{
    for (struct infs_open_handle *handle = g_handles; handle;
         handle = handle->next) {
        if (memcmp(handle->object_id, object_id, 16) == 0)
            return 1;
    }
    return 0;
}

static void update_object_handles(const uint8_t object_id[16],
                                  const char *path, int orphaned)
{
    for (struct infs_open_handle *handle = g_handles; handle;
         handle = handle->next) {
        if (memcmp(handle->object_id, object_id, 16) == 0) {
            (void)path_copy(handle->path, path);
            handle->orphaned = orphaned;
        }
    }
}

static void update_path_prefix(const char *oldpath, const char *newpath)
{
    const size_t old_length = strlen(oldpath);
    const size_t new_length = strlen(newpath);
    for (struct infs_open_handle *handle = g_handles; handle;
         handle = handle->next) {
        if (handle->orphaned || strncmp(handle->path, oldpath, old_length) != 0 ||
            (handle->path[old_length] != '\0' &&
             handle->path[old_length] != '/'))
            continue;
        const size_t suffix_length = strlen(handle->path + old_length);
        memmove(handle->path + new_length, handle->path + old_length,
                suffix_length + 1u);
        memcpy(handle->path, newpath, new_length);
    }
}

static int path_prefix_fits(const char *oldpath, const char *newpath)
{
    const size_t old_length = strlen(oldpath);
    const size_t new_length = strlen(newpath);
    for (struct infs_open_handle *handle = g_handles; handle;
         handle = handle->next) {
        if (handle->orphaned || strncmp(handle->path, oldpath, old_length) != 0 ||
            (handle->path[old_length] != '\0' &&
             handle->path[old_length] != '/'))
            continue;
        if (new_length + strlen(handle->path + old_length) > INFS_PATH_MAX)
            return 0;
    }
    return 1;
}

static int internal_orphan_name(const char *name)
{
    return name && strncmp(name, INFS_ORPHAN_PREFIX,
                           sizeof(INFS_ORPHAN_PREFIX) - 1u) == 0;
}

static int internal_orphan_directory(const char *path)
{
    struct infs_attributes attributes;
    if (infs_get_attributes(&g_volume, path, &attributes) != INFS_STATUS_OK)
        return 0;
    const uint64_t required = INFS_ATTR_HIDDEN | INFS_ATTR_SYSTEM;
    return attributes.object_type == INFS_OBJECT_DIRECTORY &&
        (attributes.portable_flags & required) == required;
}

static int neg_status(infs_status status)
{
    return -infs_status_to_errno(status);
}


static int linux_meta_path(const uint8_t object_id[16],
                 char path[INFS_PATH_MAX + 1u])
{
    char object_text[37];
    infs_uuid_to_string(object_id, object_text);
    int length = snprintf(path, INFS_PATH_MAX + 1u, "%s/%s",
                INFS_LINUX_META_DIRECTORY, object_text);
    return length < 0 || length > (int)INFS_PATH_MAX ? -ENAMETOOLONG : 0;
}

static int linux_meta_directory_is_internal(void)
{
    struct infs_attributes attributes;
    if (infs_get_attributes(&g_volume, INFS_LINUX_META_DIRECTORY,
                  &attributes) != INFS_STATUS_OK)
        return 0;
    const uint64_t required = INFS_ATTR_HIDDEN | INFS_ATTR_SYSTEM;
    return attributes.object_type == INFS_OBJECT_DIRECTORY &&
        (attributes.portable_flags & required) == required;
}

static infs_status ensure_linux_meta_directory(void)
{
    struct infs_lookup existing;
    infs_status status = infs_lookup_path(
        &g_volume, INFS_LINUX_META_DIRECTORY, &existing);
    if (status == INFS_STATUS_OK)
        return linux_meta_directory_is_internal() ? INFS_STATUS_OK :
  INFS_STATUS_ALREADY_EXISTS;
    if (status != INFS_STATUS_NOT_FOUND)
        return status;

    const struct infs_create_options options = {
        .portable_flags = INFS_ATTR_HIDDEN | INFS_ATTR_SYSTEM,
        .posix_permissions = 0700,
        .posix_uid = (uint32_t)getuid(),
        .posix_gid = (uint32_t)getgid(),
    };
    return infs_mkdir(&g_volume, INFS_LINUX_META_DIRECTORY, &options);
}

static void linux_meta_init_header(struct infs_linux_meta_header *header)
{
    memset(header, 0, sizeof(*header));
    memcpy(header->magic, INFS_LINUX_META_MAGIC, sizeof(header->magic));
    header->version = infs_cpu_to_le32(INFS_LINUX_META_VERSION);
}

static int linux_meta_load(const uint8_t object_id[16], uint8_t **blob_out,
                 size_t *size_out)
{
    if (!blob_out || !size_out)
        return -EINVAL;
    *blob_out = NULL;
    *size_out = 0;

    char path[INFS_PATH_MAX + 1u];
    int rc = linux_meta_path(object_id, path);
    if (rc != 0)
        return rc;

    struct infs_attributes attributes;
    infs_status status = infs_get_attributes(&g_volume, path, &attributes);
    if (status == INFS_STATUS_NOT_FOUND) {
        uint8_t *blob = calloc(1, sizeof(struct infs_linux_meta_header));
        if (!blob)
  return -ENOMEM;
        linux_meta_init_header((struct infs_linux_meta_header *)blob);
        *blob_out = blob;
        *size_out = sizeof(struct infs_linux_meta_header);
        return 0;
    }
    if (status != INFS_STATUS_OK)
        return neg_status(status);
    if (attributes.object_type != INFS_OBJECT_FILE ||
        attributes.logical_size < sizeof(struct infs_linux_meta_header) ||
        attributes.logical_size > INFS_LINUX_META_MAX)
        return -EIO;

    size_t size = (size_t)attributes.logical_size;
    uint8_t *blob = malloc(size);
    if (!blob)
        return -ENOMEM;
    int64_t n = infs_read_file(&g_volume, path, blob, size, 0);
    if (n < 0) {
        free(blob);
        return neg_status((infs_status)n);
    }
    if ((size_t)n != size) {
        free(blob);
        return -EIO;
    }

    struct infs_linux_meta_header *header =
        (struct infs_linux_meta_header *)blob;
    if (memcmp(header->magic, INFS_LINUX_META_MAGIC, sizeof(header->magic)) != 0 ||
        infs_le32_to_cpu(header->version) != INFS_LINUX_META_VERSION ||
        infs_le32_to_cpu(header->xattr_bytes) !=
  size - sizeof(struct infs_linux_meta_header)) {
        free(blob);
        return -EIO;
    }

    size_t offset = sizeof(*header);
    while (offset < size) {
        if (size - offset < sizeof(struct infs_linux_xattr_record)) {
  free(blob);
  return -EIO;
        }
        const struct infs_linux_xattr_record *record =
  (const struct infs_linux_xattr_record *)(blob + offset);
        size_t name_length = infs_le16_to_cpu(record->name_length);
        size_t value_length = infs_le32_to_cpu(record->value_length);
        size_t record_size = sizeof(*record) + name_length + value_length;
        if (name_length == 0 || record_size > size - offset) {
  free(blob);
  return -EIO;
        }
        offset += record_size;
    }
    if (offset != size) {
        free(blob);
        return -EIO;
    }

    *blob_out = blob;
    *size_out = size;
    return 0;
}

static int linux_meta_store(const uint8_t object_id[16], const uint8_t *blob,
                  size_t size)
{
    if (!blob || size < sizeof(struct infs_linux_meta_header) ||
        size > INFS_LINUX_META_MAX)
        return -EINVAL;
    const struct infs_linux_meta_header *header =
        (const struct infs_linux_meta_header *)blob;
    const int empty = infs_le32_to_cpu(header->special_mode) == 0 &&
        infs_le32_to_cpu(header->xattr_bytes) == 0;

    char path[INFS_PATH_MAX + 1u];
    int rc = linux_meta_path(object_id, path);
    if (rc != 0)
        return rc;

    struct infs_lookup existing;
    infs_status status = infs_lookup_path(&g_volume, path, &existing);
    if (empty) {
        if (status == INFS_STATUS_NOT_FOUND)
  return 0;
        if (status != INFS_STATUS_OK)
  return neg_status(status);
        status = infs_unlink(&g_volume, path);
        if (status != INFS_STATUS_OK)
  return neg_status(status);
        if (g_volume.tx_active) {
  status = infs_volume_sync(&g_volume);
  if (status != INFS_STATUS_OK)
      return neg_status(status);
        }
        return 0;
    }

    status = ensure_linux_meta_directory();
    if (status != INFS_STATUS_OK)
        return neg_status(status);
    status = infs_lookup_path(&g_volume, path, &existing);
    if (status == INFS_STATUS_NOT_FOUND) {
        const struct infs_create_options options = {
  .portable_flags = INFS_ATTR_HIDDEN | INFS_ATTR_SYSTEM,
  .posix_permissions = 0600,
  .posix_uid = (uint32_t)getuid(),
  .posix_gid = (uint32_t)getgid(),
        };
        status = infs_create_file(&g_volume, path, &options);
    }
    if (status != INFS_STATUS_OK)
        return neg_status(status);

    status = infs_truncate_file(&g_volume, path, 0);
    if (status != INFS_STATUS_OK)
        return neg_status(status);
    int64_t n = infs_write_file_buffered(&g_volume, path, blob, size, 0);
    if (n < 0)
        return neg_status((infs_status)n);
    if ((size_t)n != size)
        return -EIO;
    status = infs_volume_sync(&g_volume);
    return status == INFS_STATUS_OK ? 0 : neg_status(status);
}

static int linux_meta_remove(const uint8_t object_id[16])
{
    char path[INFS_PATH_MAX + 1u];
    int rc = linux_meta_path(object_id, path);
    if (rc != 0)
        return rc;

    /* Probe first. Calling infs_unlink() for a metadata path that does
     * not exist would abort an already-active deferred transaction.
     * That could silently roll back the user's successful unlink while
     * the FUSE callback still returned success. */
    struct infs_lookup existing;
    infs_status status = infs_lookup_path(&g_volume, path, &existing);
    if (status == INFS_STATUS_NOT_FOUND)
        return 0;
    if (status != INFS_STATUS_OK)
        return neg_status(status);

    status = infs_unlink(&g_volume, path);
    if (status != INFS_STATUS_OK)
        return neg_status(status);
    if (g_volume.tx_active) {
        status = infs_volume_sync(&g_volume);
        if (status != INFS_STATUS_OK)
            return neg_status(status);
    }
    return 0;
}

static int linux_meta_find_xattr(const uint8_t *blob, size_t size,
                       const char *name, size_t *offset_out,
                       size_t *record_size_out,
                       size_t *value_offset_out,
                       size_t *value_length_out)
{
    size_t wanted = strlen(name);
    size_t offset = sizeof(struct infs_linux_meta_header);
    while (offset < size) {
        const struct infs_linux_xattr_record *record =
  (const struct infs_linux_xattr_record *)(blob + offset);
        size_t name_length = infs_le16_to_cpu(record->name_length);
        size_t value_length = infs_le32_to_cpu(record->value_length);
        size_t record_size = sizeof(*record) + name_length + value_length;
        if (name_length == wanted &&
  memcmp(blob + offset + sizeof(*record), name, wanted) == 0) {
  if (offset_out)
      *offset_out = offset;
  if (record_size_out)
      *record_size_out = record_size;
  if (value_offset_out)
      *value_offset_out = offset + sizeof(*record) + name_length;
  if (value_length_out)
      *value_length_out = value_length;
  return 1;
        }
        offset += record_size;
    }
    return 0;
}

static int linux_meta_get_special(const uint8_t object_id[16], mode_t *mode,
                        dev_t *rdev)
{
    uint8_t *blob = NULL;
    size_t size = 0;
    int rc = linux_meta_load(object_id, &blob, &size);
    if (rc != 0)
        return rc;
    (void)size;
    const struct infs_linux_meta_header *header =
        (const struct infs_linux_meta_header *)blob;
    uint32_t stored_mode = infs_le32_to_cpu(header->special_mode);
    uint64_t stored_rdev = infs_le64_to_cpu(header->special_rdev);
    free(blob);
    if (mode)
        *mode = (mode_t)stored_mode;
    if (rdev)
        *rdev = (dev_t)stored_rdev;
    return 0;
}

static int linux_meta_set_special(const uint8_t object_id[16], mode_t mode,
                        dev_t rdev)
{
    uint8_t *blob = NULL;
    size_t size = 0;
    int rc = linux_meta_load(object_id, &blob, &size);
    if (rc != 0)
        return rc;
    struct infs_linux_meta_header *header =
        (struct infs_linux_meta_header *)blob;
    header->special_mode = infs_cpu_to_le32((uint32_t)(mode & S_IFMT));
    header->special_rdev = infs_cpu_to_le64((uint64_t)rdev);
    rc = linux_meta_store(object_id, blob, size);
    free(blob);
    return rc;
}

static int infs_setxattr_cb(const char *path, const char *name,
                  const char *value, size_t value_size, int flags)
{
    if (!name || name[0] == '\0' || strlen(name) > UINT16_MAX ||
        value_size > UINT32_MAX)
        return -EINVAL;
    struct infs_attributes attributes;
    infs_status status = infs_get_attributes(&g_volume, path, &attributes);
    if (status != INFS_STATUS_OK)
        return neg_status(status);

    uint8_t *blob = NULL;
    size_t size = 0;
    int rc = linux_meta_load(attributes.object_id, &blob, &size);
    if (rc != 0)
        return rc;

    size_t old_offset = 0, old_size = 0;
    int exists = linux_meta_find_xattr(blob, size, name, &old_offset,
                             &old_size, NULL, NULL);
    if ((flags & XATTR_CREATE) && exists) {
        free(blob);
        return -EEXIST;
    }
    if ((flags & XATTR_REPLACE) && !exists) {
        free(blob);
        return -ENODATA;
    }

    size_t name_length = strlen(name);
    size_t new_record_size = sizeof(struct infs_linux_xattr_record) +
        name_length + value_size;
    size_t new_size = size - (exists ? old_size : 0u) + new_record_size;
    if (new_size > INFS_LINUX_META_MAX) {
        free(blob);
        return -E2BIG;
    }
    uint8_t *updated = calloc(1, new_size);
    if (!updated) {
        free(blob);
        return -ENOMEM;
    }

    size_t write_offset = 0;
    if (exists) {
        memcpy(updated, blob, old_offset);
        write_offset = old_offset;
        memcpy(updated + write_offset, blob + old_offset + old_size,
     size - old_offset - old_size);
        write_offset += size - old_offset - old_size;
    } else {
        memcpy(updated, blob, size);
        write_offset = size;
    }

    struct infs_linux_xattr_record record = {
        .name_length = infs_cpu_to_le16((uint16_t)name_length),
        .value_length = infs_cpu_to_le32((uint32_t)value_size),
    };
    memcpy(updated + write_offset, &record, sizeof(record));
    memcpy(updated + write_offset + sizeof(record), name, name_length);
    if (value_size != 0)
        memcpy(updated + write_offset + sizeof(record) + name_length,
     value, value_size);

    struct infs_linux_meta_header *header =
        (struct infs_linux_meta_header *)updated;
    header->xattr_bytes = infs_cpu_to_le32(
        (uint32_t)(new_size - sizeof(*header)));
    rc = linux_meta_store(attributes.object_id, updated, new_size);
    free(updated);
    free(blob);
    return rc;
}

static int infs_getxattr_cb(const char *path, const char *name, char *value,
                  size_t size)
{
    struct infs_attributes attributes;
    infs_status status = infs_get_attributes(&g_volume, path, &attributes);
    if (status != INFS_STATUS_OK)
        return neg_status(status);
    uint8_t *blob = NULL;
    size_t blob_size = 0;
    int rc = linux_meta_load(attributes.object_id, &blob, &blob_size);
    if (rc != 0)
        return rc;
    size_t value_offset = 0, value_length = 0;
    if (!linux_meta_find_xattr(blob, blob_size, name, NULL, NULL,
                    &value_offset, &value_length)) {
        free(blob);
        return -ENODATA;
    }
    if (size == 0) {
        free(blob);
        return value_length > INT_MAX ? -EOVERFLOW : (int)value_length;
    }
    if (size < value_length) {
        free(blob);
        return -ERANGE;
    }
    memcpy(value, blob + value_offset, value_length);
    free(blob);
    return value_length > INT_MAX ? -EOVERFLOW : (int)value_length;
}

static int infs_listxattr_cb(const char *path, char *list, size_t size)
{
    struct infs_attributes attributes;
    infs_status status = infs_get_attributes(&g_volume, path, &attributes);
    if (status != INFS_STATUS_OK)
        return neg_status(status);
    uint8_t *blob = NULL;
    size_t blob_size = 0;
    int rc = linux_meta_load(attributes.object_id, &blob, &blob_size);
    if (rc != 0)
        return rc;

    size_t needed = 0;
    size_t offset = sizeof(struct infs_linux_meta_header);
    while (offset < blob_size) {
        const struct infs_linux_xattr_record *record =
  (const struct infs_linux_xattr_record *)(blob + offset);
        size_t name_length = infs_le16_to_cpu(record->name_length);
        size_t value_length = infs_le32_to_cpu(record->value_length);
        needed += name_length + 1u;
        offset += sizeof(*record) + name_length + value_length;
    }
    if (size == 0) {
        free(blob);
        return needed > INT_MAX ? -EOVERFLOW : (int)needed;
    }
    if (size < needed) {
        free(blob);
        return -ERANGE;
    }
    offset = sizeof(struct infs_linux_meta_header);
    size_t out = 0;
    while (offset < blob_size) {
        const struct infs_linux_xattr_record *record =
  (const struct infs_linux_xattr_record *)(blob + offset);
        size_t name_length = infs_le16_to_cpu(record->name_length);
        size_t value_length = infs_le32_to_cpu(record->value_length);
        memcpy(list + out, blob + offset + sizeof(*record), name_length);
        list[out + name_length] = '\0';
        out += name_length + 1u;
        offset += sizeof(*record) + name_length + value_length;
    }
    free(blob);
    return needed > INT_MAX ? -EOVERFLOW : (int)needed;
}

static int infs_removexattr_cb(const char *path, const char *name)
{
    struct infs_attributes attributes;
    infs_status status = infs_get_attributes(&g_volume, path, &attributes);
    if (status != INFS_STATUS_OK)
        return neg_status(status);
    uint8_t *blob = NULL;
    size_t size = 0;
    int rc = linux_meta_load(attributes.object_id, &blob, &size);
    if (rc != 0)
        return rc;
    size_t old_offset = 0, old_size = 0;
    if (!linux_meta_find_xattr(blob, size, name, &old_offset, &old_size,
                    NULL, NULL)) {
        free(blob);
        return -ENODATA;
    }
    memmove(blob + old_offset, blob + old_offset + old_size,
  size - old_offset - old_size);
    size -= old_size;
    struct infs_linux_meta_header *header =
        (struct infs_linux_meta_header *)blob;
    header->xattr_bytes = infs_cpu_to_le32(
        (uint32_t)(size - sizeof(*header)));
    rc = linux_meta_store(attributes.object_id, blob, size);
    free(blob);
    return rc;
}

static int infs_mknod_cb(const char *path, mode_t mode, dev_t rdev)
{
    if (!S_ISREG(mode) && !S_ISFIFO(mode) && !S_ISSOCK(mode) &&
        !S_ISCHR(mode) && !S_ISBLK(mode))
        return -EINVAL;
    struct fuse_context *ctx = fuse_get_context();
    const struct infs_create_options options = {
        .posix_permissions = (uint32_t)(mode & 07777),
        .posix_uid = (uint32_t)ctx->uid,
        .posix_gid = (uint32_t)ctx->gid,
    };
    infs_status status = infs_create_file(&g_volume, path, &options);
    if (status != INFS_STATUS_OK)
        return neg_status(status);
    if (S_ISREG(mode))
        return 0;
    struct infs_attributes attributes;
    status = infs_get_attributes(&g_volume, path, &attributes);
    if (status != INFS_STATUS_OK) {
        (void)infs_unlink(&g_volume, path);
        return neg_status(status);
    }
    int rc = linux_meta_set_special(attributes.object_id, mode, rdev);
    if (rc != 0)
        (void)infs_unlink(&g_volume, path);
    return rc;
}

static infs_status ensure_orphan_directory(void)
{
    if (g_orphan_directory[0] != '\0')
        return INFS_STATUS_OK;

    const struct infs_create_options options = {
        .portable_flags = INFS_ATTR_HIDDEN | INFS_ATTR_SYSTEM,
        .posix_permissions = 0700,
        .posix_uid = (uint32_t)getuid(),
        .posix_gid = (uint32_t)getgid(),
    };
    for (unsigned attempt = 0; attempt < 1024u; ++attempt) {
        unsigned sequence = ++g_orphan_sequence;
        int length = snprintf(
            g_orphan_directory, sizeof(g_orphan_directory),
            "/%s%ld-%u", INFS_ORPHAN_PREFIX, (long)getpid(), sequence);
        if (length < 0 || (size_t)length >= sizeof(g_orphan_directory)) {
            g_orphan_directory[0] = '\0';
            return INFS_STATUS_NAME_TOO_LONG;
        }
        struct infs_lookup existing;
        infs_status status = infs_lookup_path(
            &g_volume, g_orphan_directory, &existing);
        if (status == INFS_STATUS_NOT_FOUND) {
            status = infs_mkdir(&g_volume, g_orphan_directory, &options);
            if (status == INFS_STATUS_OK)
                return status;
            g_orphan_directory[0] = '\0';
            return status;
        }
        if (status != INFS_STATUS_OK) {
            g_orphan_directory[0] = '\0';
            return status;
        }
    }
    g_orphan_directory[0] = '\0';
    return INFS_STATUS_ALREADY_EXISTS;
}

static infs_status move_open_file_to_orphan(
    const char *path, const uint8_t object_id[16],
    char orphan_path[INFS_PATH_MAX + 1u])
{
    infs_status status = ensure_orphan_directory();
    if (status != INFS_STATUS_OK)
        return status;
    char object_text[37];
    infs_uuid_to_string(object_id, object_text);
    int length = snprintf(orphan_path, INFS_PATH_MAX + 1u, "%s/%s",
                          g_orphan_directory, object_text);
    if (length < 0 || length > (int)INFS_PATH_MAX)
        return INFS_STATUS_NAME_TOO_LONG;
    return infs_rename(&g_volume, path, orphan_path);
}

static infs_status clean_internal_orphan_directory(const char *path)
{
    struct infs_attributes attributes;
    infs_status status = infs_get_attributes(&g_volume, path, &attributes);
    if (status == INFS_STATUS_NOT_FOUND)
        return INFS_STATUS_OK;
    if (status != INFS_STATUS_OK)
        return status;
    if (!internal_orphan_directory(path))
        return INFS_STATUS_OK;

    struct infs_dir_item *items = NULL;
    size_t count = 0;
    status = infs_list_dir(&g_volume, path, &items, &count);
    if (status != INFS_STATUS_OK)
        return status;
    for (size_t i = 0; i < count; ++i) {
        if (items[i].type != INFS_OBJECT_FILE) {
            status = INFS_STATUS_NOT_EMPTY;
            break;
        }
        char child[INFS_PATH_MAX + 1u];
        int length = snprintf(child, sizeof(child), "%s/%s",
                              path, items[i].name);
        if (length < 0 || (size_t)length >= sizeof(child)) {
            status = INFS_STATUS_NAME_TOO_LONG;
            break;
        }
        status = infs_unlink(&g_volume, child);
        if (status != INFS_STATUS_OK)
            break;
    }
    infs_free_dir_items(items);
    if (status != INFS_STATUS_OK)
        return status;
    return infs_rmdir(&g_volume, path);
}

static infs_status clean_stale_orphan_directories(void)
{
    struct infs_dir_item *items = NULL;
    size_t count = 0;
    infs_status status = infs_list_dir(&g_volume, "/", &items, &count);
    if (status != INFS_STATUS_OK)
        return status;
    for (size_t i = 0; i < count; ++i) {
        if (items[i].type != INFS_OBJECT_DIRECTORY ||
            !internal_orphan_name(items[i].name))
            continue;
        char path[INFS_PATH_MAX + 1u];
        int length = snprintf(path, sizeof(path), "/%s", items[i].name);
        if (length < 0 || (size_t)length >= sizeof(path)) {
            status = INFS_STATUS_NAME_TOO_LONG;
            break;
        }
        status = clean_internal_orphan_directory(path);
        if (status != INFS_STATUS_OK)
            break;
    }
    infs_free_dir_items(items);
    return status;
}

static void apply_mount_options(const char *options, int *read_only)
{
    const char *p = options;
    while (p && *p) {
        const char *end = strchr(p, ',');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        if (length == 2u && memcmp(p, "ro", 2) == 0)
            *read_only = 1;
        else if (length == 2u && memcmp(p, "rw", 2) == 0)
            *read_only = 0;
        if (!end)
            break;
        p = end + 1;
    }
}

static int fuse_requested_read_only(int argc, char **argv)
{
    int read_only = 0;
    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "-r") == 0 ||
            strcmp(argv[i], "--read-only") == 0) {
            read_only = 1;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            apply_mount_options(argv[++i], &read_only);
        } else if (strncmp(argv[i], "-o", 2) == 0 && argv[i][2] != '\0') {
            apply_mount_options(argv[i] + 2, &read_only);
        }
    }
    return read_only;
}

static int uint64_to_off_t(uint64_t value, off_t *out)
{
    if (!out)
        return -EINVAL;
    off_t converted = (off_t)value;
    if (converted < 0 || (uint64_t)converted != value)
        return -EOVERFLOW;
    *out = converted;
    return 0;
}

static void ns_to_timespec(int64_t ns, struct timespec *out)
{
    int64_t seconds = ns / INFS_NS_PER_SECOND;
    int64_t nanoseconds = ns % INFS_NS_PER_SECOND;
    if (nanoseconds < 0) {
        --seconds;
        nanoseconds += INFS_NS_PER_SECOND;
    }
    out->tv_sec = (time_t)seconds;
    out->tv_nsec = (long)nanoseconds;
}

static int timespec_to_ns(const struct timespec *value, int64_t *out)
{
    if (!value || !out || value->tv_nsec < 0 ||
        value->tv_nsec >= INFS_NS_PER_SECOND)
        return -EINVAL;

    int64_t seconds = (int64_t)value->tv_sec;
    if ((time_t)seconds != value->tv_sec)
        return -EOVERFLOW;
    if (seconds > INT64_MAX / INFS_NS_PER_SECOND ||
        seconds < INT64_MIN / INFS_NS_PER_SECOND)
        return -EOVERFLOW;

    int64_t base = seconds * INFS_NS_PER_SECOND;
    if (base > INT64_MAX - (int64_t)value->tv_nsec)
        return -EOVERFLOW;
    *out = base + (int64_t)value->tv_nsec;
    return 0;
}

static int attributes_to_stat(const struct infs_attributes *attributes,
                              struct stat *st)
{
    if (!attributes || !st)
        return -EINVAL;
    off_t logical_size = 0;
    int rc = uint64_to_off_t(attributes->logical_size, &logical_size);
    if (rc != 0)
        return rc;

    memset(st, 0, sizeof(*st));
    mode_t type_mode = attributes->object_type == INFS_OBJECT_DIRECTORY ?
        S_IFDIR : attributes->object_type == INFS_OBJECT_SYMLINK ?
        S_IFLNK : S_IFREG;
    dev_t special_rdev = 0;
    if (attributes->object_type == INFS_OBJECT_FILE) {
        mode_t special_mode = 0;
        if (linux_meta_get_special(attributes->object_id, &special_mode,
                                   &special_rdev) == 0 && special_mode != 0)
            type_mode = special_mode;
    }
    st->st_mode = type_mode | attributes->posix_permissions;
    st->st_rdev = special_rdev;
    st->st_uid = (uid_t)attributes->posix_uid;
    st->st_gid = (gid_t)attributes->posix_gid;
    st->st_nlink = (nlink_t)attributes->link_count;
    st->st_size = logical_size;
    st->st_blksize = INFS_BLOCK_SIZE;
    st->st_blocks = (blkcnt_t)(attributes->allocated_size / 512u);
    uint64_t inode = infs_crc64_ecma(attributes->object_id, 16);
    st->st_ino = (ino_t)(inode ? inode : 1u);

    ns_to_timespec(attributes->access_time_ns, &st->st_atim);
    ns_to_timespec(attributes->modification_time_ns, &st->st_mtim);
    ns_to_timespec(attributes->change_time_ns, &st->st_ctim);
    return 0;
}

static int infs_getattr_cb(const char *path, struct stat *st,
                           struct fuse_file_info *fi)
{
    struct infs_attributes attributes;
    infs_status status = infs_get_attributes(
        &g_volume, operation_path(path, fi), &attributes);
    if (status != INFS_STATUS_OK)
        return neg_status(status);
    return attributes_to_stat(&attributes, st);
}

static int infs_readdir_cb(const char *path, void *buf, fuse_fill_dir_t filler,
                           off_t off, struct fuse_file_info *fi,
                           enum fuse_readdir_flags flags)
{
    (void)off;
    (void)fi;
    (void)flags;
    if (filler(buf, ".", NULL, 0, 0) != 0 ||
        filler(buf, "..", NULL, 0, 0) != 0)
        return 0;

    struct infs_dir_item *items = NULL;
    size_t count = 0;
    infs_status status = infs_list_dir(&g_volume, path, &items, &count);
    if (status != INFS_STATUS_OK)
        return neg_status(status);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(path, "/") == 0 && internal_orphan_name(items[i].name)) {
            char internal_path[INFS_PATH_MAX + 1u];
            int length = snprintf(internal_path, sizeof(internal_path),
                                  "/%s", items[i].name);
            if (length > 0 && (size_t)length < sizeof(internal_path) &&
                internal_orphan_directory(internal_path))
                continue;
        }
        if (strcmp(path, "/") == 0 &&
            strcmp(items[i].name, ".infilfs-posix-meta") == 0 &&
            linux_meta_directory_is_internal())
            continue;
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_mode = items[i].type == INFS_OBJECT_DIRECTORY ? S_IFDIR :
            items[i].type == INFS_OBJECT_SYMLINK ? S_IFLNK : S_IFREG;
        if (items[i].type == INFS_OBJECT_FILE) {
            mode_t special_mode = 0;
            if (linux_meta_get_special(items[i].object_id, &special_mode, NULL) == 0 &&
                special_mode != 0)
                st.st_mode = special_mode;
        }
        if (filler(buf, items[i].name, &st, 0, 0) != 0)
            break;
    }
    infs_free_dir_items(items);
    return 0;
}

static int infs_mkdir_cb(const char *path, mode_t mode)
{
    struct fuse_context *ctx = fuse_get_context();
    const struct infs_create_options options = {
        .posix_permissions = (uint32_t)mode,
        .posix_uid = (uint32_t)ctx->uid,
        .posix_gid = (uint32_t)ctx->gid,
    };
    return neg_status(infs_mkdir(&g_volume, path, &options));
}

static int infs_symlink_cb(const char *target, const char *path)
{
    struct fuse_context *ctx = fuse_get_context();
    const struct infs_create_options options = {
        .posix_permissions = 0777,
        .posix_uid = (uint32_t)ctx->uid,
        .posix_gid = (uint32_t)ctx->gid,
    };
    return neg_status(infs_create_symlink(
        &g_volume, path, target, &options));
}

static int infs_readlink_cb(const char *path, char *buffer, size_t size)
{
    if (!buffer || size == 0)
        return -EINVAL;
    char target[INFS_SYMLINK_TARGET_MAX + 1u];
    size_t length = 0;
    infs_status status = infs_read_symlink(
        &g_volume, path, target, sizeof(target), &length);
    if (status != INFS_STATUS_OK)
        return neg_status(status);
    size_t copy = length < size - 1u ? length : size - 1u;
    memcpy(buffer, target, copy);
    buffer[copy] = '\0';
    return 0;
}

static int infs_link_cb(const char *existing_path, const char *new_path)
{
    return neg_status(infs_link_file(
        &g_volume, existing_path, new_path));
}

static int infs_create_cb(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    struct fuse_context *ctx = fuse_get_context();
    const struct infs_create_options options = {
        .posix_permissions = (uint32_t)mode,
        .posix_uid = (uint32_t)ctx->uid,
        .posix_gid = (uint32_t)ctx->gid,
    };
    infs_status status = infs_create_file(&g_volume, path, &options);
    if (status != INFS_STATUS_OK)
        return neg_status(status);
    struct infs_attributes attributes;
    status = infs_get_attributes(&g_volume, path, &attributes);
    if (status != INFS_STATUS_OK)
        return neg_status(status);
    int rc = add_open_handle(path, attributes.object_id, fi);
    if (rc != 0)
        (void)infs_unlink(&g_volume, path);
    return rc;
}

static int infs_open_cb(const char *path, struct fuse_file_info *fi)
{
    struct infs_attributes attributes;
    infs_status status = infs_get_attributes(&g_volume, path, &attributes);
    if (status != INFS_STATUS_OK)
        return neg_status(status);
    if (attributes.object_type != INFS_OBJECT_FILE)
        return -EISDIR;
    if ((fi->flags & O_TRUNC) && (fi->flags & O_ACCMODE) != O_RDONLY) {
        status = infs_truncate_file(&g_volume, path, 0);
        if (status != INFS_STATUS_OK)
            return neg_status(status);
    }
    return add_open_handle(path, attributes.object_id, fi);
}

static int infs_read_cb(const char *path, char *buf, size_t size, off_t off,
                        struct fuse_file_info *fi)
{
    if (off < 0)
        return -EINVAL;
    if (size > (size_t)INT_MAX)
        return -EOVERFLOW;
    int64_t n = infs_read_file(&g_volume, operation_path(path, fi),
                               buf, size, (uint64_t)off);
    if (n < 0)
        return neg_status((infs_status)n);
    return (int)n;
}

static int infs_write_cb(const char *path, const char *buf, size_t size, off_t off,
                         struct fuse_file_info *fi)
{
    if (off < 0)
        return -EINVAL;
    if (size > (size_t)INT_MAX)
        return -EOVERFLOW;
    if (fi && (fi->flags & O_APPEND)) {
        struct infs_attributes attributes;
        infs_status status = infs_get_attributes(
            &g_volume, operation_path(path, fi), &attributes);
        if (status != INFS_STATUS_OK)
            return neg_status(status);
        int rc = uint64_to_off_t(attributes.logical_size, &off);
        if (rc != 0)
            return rc;
    }
    int64_t n = infs_write_file_buffered(
        &g_volume, operation_path(path, fi), buf, size, (uint64_t)off);
    if (n < 0)
        return neg_status((infs_status)n);
    return (int)n;
}

static int infs_truncate_cb(const char *path, off_t size, struct fuse_file_info *fi)
{
    if (size < 0)
        return -EINVAL;
    return neg_status(infs_truncate_file(
        &g_volume, operation_path(path, fi), (uint64_t)size));
}

static int infs_fallocate_cb(const char *path, int mode, off_t offset,
                   off_t length, struct fuse_file_info *fi)
{
    if (offset < 0 || length <= 0)
        return -EINVAL;
    const char *actual_path = operation_path(path, fi);
    if (mode == (FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE))
        return neg_status(infs_punch_hole(
  &g_volume, actual_path, (uint64_t)offset, (uint64_t)length));
    if (mode != 0)
        return -EOPNOTSUPP;

    uint64_t start = (uint64_t)offset;
    uint64_t span = (uint64_t)length;
    if (start > UINT64_MAX - span)
        return -EFBIG;
    uint64_t end = start + span;
    const size_t buffer_size = 64u * 1024u;
    uint8_t *buffer = calloc(1, buffer_size);
    if (!buffer)
        return -ENOMEM;

    uint64_t position = start;
    while (position < end) {
        size_t chunk = (size_t)((end - position) < buffer_size ?
  (end - position) : buffer_size);
        memset(buffer, 0, chunk);
        int64_t read_count = infs_read_file(
  &g_volume, actual_path, buffer, chunk, position);
        if (read_count < 0) {
  free(buffer);
  return neg_status((infs_status)read_count);
        }
        int64_t written = infs_write_file_buffered(
  &g_volume, actual_path, buffer, chunk, position);
        if (written < 0) {
  free(buffer);
  return neg_status((infs_status)written);
        }
        if ((size_t)written != chunk) {
  free(buffer);
  return -EIO;
        }
        position += chunk;
    }
    free(buffer);
    infs_status status = infs_volume_sync(&g_volume);
    return status == INFS_STATUS_OK ? 0 : neg_status(status);
}

static int infs_unlink_cb(const char *path)
{
    struct infs_attributes attributes;
    infs_status status = infs_get_attributes(&g_volume, path, &attributes);
    if (status != INFS_STATUS_OK)
        return neg_status(status);
    if (!object_has_open_handle(attributes.object_id)) {
        infs_status unlink_status = infs_unlink(&g_volume, path);
        if (unlink_status != INFS_STATUS_OK)
            return neg_status(unlink_status);
        if (attributes.link_count <= 1u) {
            int rc = linux_meta_remove(attributes.object_id);
            if (rc != 0)
                return rc;
        }
        return 0;
    }

    char orphan_path[INFS_PATH_MAX + 1u];
    status = move_open_file_to_orphan(
        path, attributes.object_id, orphan_path);
    if (status != INFS_STATUS_OK)
        return neg_status(status);
    update_object_handles(attributes.object_id, orphan_path, 1);
    return 0;
}

static int infs_rmdir_cb(const char *path)
{
    struct infs_attributes attributes;
    infs_status status = infs_get_attributes(&g_volume, path, &attributes);
    if (status != INFS_STATUS_OK)
        return neg_status(status);
    status = infs_rmdir(&g_volume, path);
    if (status != INFS_STATUS_OK)
        return neg_status(status);
    return linux_meta_remove(attributes.object_id);
}

static int infs_rename_cb(const char *oldpath, const char *newpath, unsigned flags)
{
    if (flags != 0)
        return -EOPNOTSUPP;
    if (!path_prefix_fits(oldpath, newpath))
        return -ENAMETOOLONG;

    struct infs_attributes source;
    infs_status status = infs_get_attributes(&g_volume, oldpath, &source);
    if (status != INFS_STATUS_OK)
        return neg_status(status);

    struct infs_attributes destination;
    int preserved_destination = 0;
    char orphan_path[INFS_PATH_MAX + 1u] = {0};
    status = infs_get_attributes(&g_volume, newpath, &destination);
    if (status == INFS_STATUS_OK &&
        memcmp(destination.object_id, source.object_id, 16) != 0 &&
        source.object_type == INFS_OBJECT_FILE &&
        destination.object_type == INFS_OBJECT_FILE &&
        object_has_open_handle(destination.object_id)) {
        /* Start the two namespace changes in a fresh deferred transaction so
         * replacement remains one crash-atomic publication. Each rename is a
         * bounded metadata mutation far below the publication threshold. */
        if (g_volume.tx_active) {
            status = infs_volume_sync(&g_volume);
            if (status != INFS_STATUS_OK)
                return neg_status(status);
        }
        status = move_open_file_to_orphan(
            newpath, destination.object_id, orphan_path);
        if (status != INFS_STATUS_OK)
            return neg_status(status);
        update_object_handles(destination.object_id, orphan_path, 1);
        preserved_destination = 1;
    } else if (status != INFS_STATUS_OK && status != INFS_STATUS_NOT_FOUND) {
        return neg_status(status);
    }

    status = infs_rename(&g_volume, oldpath, newpath);
    if (status != INFS_STATUS_OK) {
        if (preserved_destination) {
            struct infs_lookup lookup;
            if (infs_lookup_path(&g_volume, orphan_path, &lookup) !=
                INFS_STATUS_OK) {
                update_object_handles(destination.object_id, newpath, 0);
                if (g_orphan_directory[0] != '\0' &&
                    infs_lookup_path(&g_volume, g_orphan_directory, &lookup) !=
                    INFS_STATUS_OK)
                    g_orphan_directory[0] = '\0';
            }
        }
        return neg_status(status);
    }
    update_path_prefix(oldpath, newpath);
    return 0;
}

static int infs_chmod_cb(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    return neg_status(infs_set_posix_compat(
        &g_volume, operation_path(path, fi), INFS_POSIX_SET_PERMISSIONS,
        (uint32_t)mode, 0, 0));
}

static int infs_chown_cb(const char *path, uid_t uid, gid_t gid,
                         struct fuse_file_info *fi)
{
    uint32_t mask = 0;
    if (uid != (uid_t)-1)
        mask |= INFS_POSIX_SET_UID;
    if (gid != (gid_t)-1)
        mask |= INFS_POSIX_SET_GID;
    return neg_status(infs_set_posix_compat(
        &g_volume, operation_path(path, fi), mask, 0,
        (uint32_t)uid, (uint32_t)gid));
}

static int infs_utimens_cb(const char *path, const struct timespec tv[2],
                           struct fuse_file_info *fi)
{
    if (!tv)
        return neg_status(infs_set_times(
            &g_volume, operation_path(path, fi), NULL));

    struct infs_time_update update = {
        .access_action = tv[0].tv_nsec == UTIME_OMIT ? INFS_TIME_OMIT :
                         tv[0].tv_nsec == UTIME_NOW ? INFS_TIME_NOW : INFS_TIME_SET,
        .modification_action = tv[1].tv_nsec == UTIME_OMIT ? INFS_TIME_OMIT :
                               tv[1].tv_nsec == UTIME_NOW ? INFS_TIME_NOW : INFS_TIME_SET,
    };
    if (update.access_action == INFS_TIME_SET) {
        int rc = timespec_to_ns(&tv[0], &update.access_time_ns);
        if (rc != 0)
            return rc;
    }
    if (update.modification_action == INFS_TIME_SET) {
        int rc = timespec_to_ns(&tv[1], &update.modification_time_ns);
        if (rc != 0)
            return rc;
    }
    return neg_status(infs_set_times(
        &g_volume, operation_path(path, fi), &update));
}

static int infs_statfs_cb(const char *path, struct statvfs *st)
{
    (void)path;
    memset(st, 0, sizeof(*st));
    st->f_bsize = INFS_BLOCK_SIZE;
    st->f_frsize = INFS_BLOCK_SIZE;
    st->f_blocks = infs_le64_to_cpu(g_volume.sb.total_blocks);
    st->f_bfree = infs_le64_to_cpu(g_volume.sb.free_blocks);
    st->f_bavail = st->f_bfree;
    st->f_files = st->f_blocks;
    st->f_ffree = st->f_bfree;
    st->f_favail = st->f_bavail;
    st->f_namemax = INFS_NAME_MAX;
    return 0;
}

static int sync_pending_writes(void)
{
    if (!g_volume.writable || !g_volume.tx_active)
        return 0;
    return neg_status(infs_volume_sync(&g_volume));
}

static int infs_flush_cb(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    /* FUSE flush is a close-time notification and may be called repeatedly.
     * It is not a durability request. The generic bounded publish policy keeps
     * writeback finite without turning every close into a full bitmap commit. */
    return 0;
}

static int infs_release_cb(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    struct infs_open_handle *handle = handle_from_info(fi);
    if (!handle)
        return 0;

    struct infs_open_handle **link = &g_handles;
    while (*link && *link != handle)
        link = &(*link)->next;
    if (*link == handle)
        *link = handle->next;

    const int orphaned = handle->orphaned;
    uint8_t object_id[16];
    char orphan_path[INFS_PATH_MAX + 1u];
    memcpy(object_id, handle->object_id, 16);
    (void)path_copy(orphan_path, handle->path);
    handle->magic = 0;
    free(handle);
    fi->fh = 0;

    /* POSIX keeps an unlinked file alive until its final descriptor closes.
     * The adapter retains it in a hidden, system-marked namespace directory;
     * the final release performs the real transactional unlink. */
    if (orphaned && !object_has_open_handle(object_id)) {
        infs_status status = infs_unlink(&g_volume, orphan_path);
        if (status != INFS_STATUS_OK && status != INFS_STATUS_NOT_FOUND)
            return neg_status(status);
        int meta_rc = linux_meta_remove(object_id);
        if (meta_rc != 0)
            return meta_rc;
        if (g_orphan_directory[0] != '\0') {
            status = infs_rmdir(&g_volume, g_orphan_directory);
            if (status == INFS_STATUS_OK)
                g_orphan_directory[0] = '\0';
            else if (status != INFS_STATUS_NOT_EMPTY &&
                     status != INFS_STATUS_NOT_FOUND)
                return neg_status(status);
        }
    }
    return 0;
}

static int infs_fsync_cb(const char *path, int datasync, struct fuse_file_info *fi)
{
    (void)path;
    (void)datasync;
    (void)fi;
    return sync_pending_writes();
}

static void *infs_init_cb(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    /* The adapter implements unlink/replacement lifetime itself so retained
     * objects remain transactional Format 0.8 entries instead of libfuse's
     * generic .fuse_hidden path convention. */
    cfg->hard_remove = 1;
    cfg->use_ino = 1;
    cfg->attr_timeout = 0.0;
#ifdef FUSE_CAP_HANDLE_KILLPRIV
    conn->want &= ~FUSE_CAP_HANDLE_KILLPRIV;
#endif
#ifdef FUSE_CAP_HANDLE_KILLPRIV_V2
    conn->want &= ~FUSE_CAP_HANDLE_KILLPRIV_V2;
#endif
    return NULL;
}

static const struct fuse_operations infs_ops = {
    .init = infs_init_cb,
    .getattr = infs_getattr_cb,
    .readdir = infs_readdir_cb,
    .mknod = infs_mknod_cb,
    .mkdir = infs_mkdir_cb,
    .symlink = infs_symlink_cb,
    .readlink = infs_readlink_cb,
    .link = infs_link_cb,
    .create = infs_create_cb,
    .open = infs_open_cb,
    .read = infs_read_cb,
    .write = infs_write_cb,
    .flush = infs_flush_cb,
    .release = infs_release_cb,
    .truncate = infs_truncate_cb,
    .fallocate = infs_fallocate_cb,
    .unlink = infs_unlink_cb,
    .rmdir = infs_rmdir_cb,
    .rename = infs_rename_cb,
    .chmod = infs_chmod_cb,
    .chown = infs_chown_cb,
    .utimens = infs_utimens_cb,
    .setxattr = infs_setxattr_cb,
    .getxattr = infs_getxattr_cb,
    .listxattr = infs_listxattr_cb,
    .removexattr = infs_removexattr_cb,
    .statfs = infs_statfs_cb,
    .fsync = infs_fsync_cb,
};

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <image-or-device> <mountpoint> [FUSE options]\n",
                argv[0]);
        return 2;
    }

    int read_only = fuse_requested_read_only(argc, argv);
    infs_status status = infs_posix_volume_open(&g_volume, argv[1], !read_only);
    if (status != INFS_STATUS_OK) {
        fprintf(stderr, "Cannot mount InfiltratorFS%s: %s\n",
                read_only ? " read-only" : "",
                infs_status_string(status));
        return 1;
    }

    if (!read_only) {
        status = clean_stale_orphan_directories();
        if (status != INFS_STATUS_OK) {
            fprintf(stderr, "Cannot clean stale InfiltratorFS open handles: %s\n",
                    infs_status_string(status));
            infs_volume_close(&g_volume);
            return 1;
        }
        status = infs_volume_set_deferred_publish(&g_volume, 1, 0);
        if (status != INFS_STATUS_OK) {
            fprintf(stderr, "Cannot enable InfiltratorFS deferred publication: %s\n",
                    infs_status_string(status));
            infs_volume_close(&g_volume);
            return 1;
        }
    }

    char **fuse_argv = calloc((size_t)argc + 3u, sizeof(*fuse_argv));
    if (!fuse_argv) {
        perror("calloc");
        infs_volume_close(&g_volume);
        return 1;
    }

    int j = 0;
    fuse_argv[j++] = argv[0];
    for (int i = 2; i < argc; ++i)
        fuse_argv[j++] = argv[i];
    fuse_argv[j++] = "-s"; /* The current core remains deliberately single-writer. */
    fuse_argv[j++] = "-o";
    fuse_argv[j++] = "default_permissions";

    int rc = fuse_main(j, fuse_argv, &infs_ops, NULL);
    free(fuse_argv);
    if (g_volume.writable) {
        infs_status cleanup_status = clean_stale_orphan_directories();
        if (cleanup_status != INFS_STATUS_OK && rc == 0) {
            fprintf(stderr, "Final InfiltratorFS open-handle cleanup failed: %s\n",
                    infs_status_string(cleanup_status));
            rc = 1;
        }
    }
    while (g_handles) {
        struct infs_open_handle *next = g_handles->next;
        g_handles->magic = 0;
        free(g_handles);
        g_handles = next;
    }
    if (g_volume.writable && g_volume.tx_active) {
        infs_status sync_status = infs_volume_sync(&g_volume);
        if (sync_status != INFS_STATUS_OK && rc == 0) {
            fprintf(stderr, "Final InfiltratorFS sync failed: %s\n",
                    infs_status_string(sync_status));
            rc = 1;
        }
    }
    infs_volume_close(&g_volume);
    return rc;
}
