// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Linux-only metadata compatibility.
 *
 * Format 0.17 deliberately keeps portable objects independent of Linux VFS
 * details.  The final FUSE adapter stored xattrs and special-node type/rdev in
 * hidden, system-marked ordinary files below /.infilfs-posix-meta.  Reuse that
 * exact representation so native mounts preserve existing metadata without
 * changing the portable object layout.
 */

#define INFILFS_LINUX_META_MAGIC "INPSXM01"
#define INFILFS_LINUX_META_VERSION 1u
#define INFILFS_LINUX_META_MAX (1024u * 1024u)

struct infilfs_linux_meta_header {
    u8 magic[8];
    __le32 version;
    __le32 special_mode;
    __le64 special_rdev;
    __le32 xattr_bytes;
    __le32 reserved;
} __packed;

struct infilfs_linux_xattr_record {
    __le16 name_length;
    __le16 reserved;
    __le32 value_length;
} __packed;

static_assert(sizeof(struct infilfs_linux_meta_header) == 32);
static_assert(sizeof(struct infilfs_linux_xattr_record) == 8);

static void infilfs_linux_meta_uuid(const u8 id[16], char out[37])
{
    scnprintf(out, 37,
              "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
              "%02x%02x%02x%02x%02x%02x",
              id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
              id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
}

static void infilfs_linux_meta_init(struct infilfs_linux_meta_header *header)
{
    memset(header, 0, sizeof(*header));
    memcpy(header->magic, INFILFS_LINUX_META_MAGIC, sizeof(header->magic));
    header->version = cpu_to_le32(INFILFS_LINUX_META_VERSION);
}

static int infilfs_linux_meta_find_child(
    struct inode *dir, const char *name, size_t name_len,
    u16 expected_type, struct inode **inode_out)
{
    struct infilfs_dir_lookup search = {
        .name = name,
        .name_len = name_len,
    };
    struct inode *inode;
    u64 block;
    u16 indexed_type;
    int ret;

    *inode_out = NULL;
    ret = infilfs_for_each_dirent(dir, infilfs_lookup_visitor, &search);
    if (ret < 0)
        return ret;
    if (!search.found)
        return -ENOENT;
    if (search.object_type != expected_type)
        return -EFSCORRUPTED;
    ret = infilfs_index_lookup(dir->i_sb, search.object_id, &block,
                               &indexed_type);
    if (ret)
        return ret;
    if (indexed_type != expected_type)
        return -EFSCORRUPTED;
    inode = infilfs_get_inode(dir->i_sb, block, indexed_type,
                              search.object_id);
    if (IS_ERR(inode))
        return PTR_ERR(inode);
    *inode_out = inode;
    return 0;
}

static int infilfs_linux_meta_validate_internal(struct inode *inode,
                                                u16 expected_type)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    const struct infilfs_attributes_disk *attributes;
    u8 *object;
    u64 flags;
    int ret;

    if (!ii || ii->object_type != expected_type)
        return -EFSCORRUPTED;
    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;
    ret = infilfs_read_object(inode->i_sb, ii->object_block, expected_type,
                              ii->object_id, object);
    if (ret)
        goto out;
    attributes = infilfs_ns_attributes(object);
    if (!attributes) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    flags = le64_to_cpu(attributes->portable_flags);
    ret = (flags & (INFILFS_ATTR_HIDDEN | INFILFS_ATTR_SYSTEM)) ==
        (INFILFS_ATTR_HIDDEN | INFILFS_ATTR_SYSTEM) ? 0 : -EEXIST;
out:
    kfree(object);
    return ret;
}

static int infilfs_linux_meta_get_dir(struct super_block *sb, bool create,
                                      struct inode **dir_out)
{
    struct inode *root;
    struct inode *dir = NULL;
    struct qstr name = {
        .name = INFILFS_LINUX_META_DIRECTORY,
        .len = sizeof(INFILFS_LINUX_META_DIRECTORY) - 1u,
    };
    u8 id[16];
    u64 block;
    int ret;

    *dir_out = NULL;
    if (!sb->s_root)
        return -ENOENT;
    root = d_inode(sb->s_root);
    ret = infilfs_linux_meta_find_child(
        root, INFILFS_LINUX_META_DIRECTORY,
        sizeof(INFILFS_LINUX_META_DIRECTORY) - 1u,
        INFILFS_OBJECT_DIRECTORY, &dir);
    if (ret == -ENOENT && create) {
        if (sb_rdonly(sb))
            return -EROFS;
        ret = infilfs_native_pending_flush_sb(sb);
        if (ret)
            return ret;
        ret = infilfs_posix_create_native_named_child(
            root, &name, 0700, INFILFS_OBJECT_DIRECTORY,
            INFILFS_ATTR_HIDDEN | INFILFS_ATTR_SYSTEM, id, &block);
        if (!ret) {
            inc_nlink(root);
            dir = infilfs_get_inode(sb, block, INFILFS_OBJECT_DIRECTORY, id);
            if (IS_ERR(dir))
                return PTR_ERR(dir);
        } else if (ret == -EEXIST) {
            ret = infilfs_linux_meta_find_child(
                root, INFILFS_LINUX_META_DIRECTORY,
                sizeof(INFILFS_LINUX_META_DIRECTORY) - 1u,
                INFILFS_OBJECT_DIRECTORY, &dir);
        }
    }
    if (ret)
        return ret;
    ret = infilfs_linux_meta_validate_internal(
        dir, INFILFS_OBJECT_DIRECTORY);
    if (ret) {
        iput(dir);
        return ret;
    }
    *dir_out = dir;
    return 0;
}

static bool infilfs_linux_meta_directory_is_internal(struct super_block *sb)
{
    struct inode *dir = NULL;
    int ret = infilfs_linux_meta_get_dir(sb, false, &dir);

    if (!ret)
        iput(dir);
    return ret == 0;
}

static int infilfs_linux_meta_get_file(
    struct super_block *sb, const u8 object_id[16], bool create,
    struct inode **dir_out, struct inode **file_out, char name_out[37])
{
    struct inode *dir = NULL;
    struct inode *file = NULL;
    struct qstr name;
    u8 id[16];
    u64 block;
    int ret;

    *dir_out = NULL;
    *file_out = NULL;
    infilfs_linux_meta_uuid(object_id, name_out);
    ret = infilfs_linux_meta_get_dir(sb, create, &dir);
    if (ret)
        return ret;
    ret = infilfs_linux_meta_find_child(dir, name_out, 36,
                                        INFILFS_OBJECT_FILE, &file);
    if (ret == -ENOENT && create) {
        ret = infilfs_native_pending_flush_sb(sb);
        if (ret)
            goto out;
        name.name = name_out;
        name.len = 36;
        name.hash = 0;
        ret = infilfs_posix_create_native_named_child(
            dir, &name, 0600, INFILFS_OBJECT_FILE,
            INFILFS_ATTR_HIDDEN | INFILFS_ATTR_SYSTEM, id, &block);
        if (!ret) {
            file = infilfs_get_inode(sb, block, INFILFS_OBJECT_FILE, id);
            if (IS_ERR(file)) {
                ret = PTR_ERR(file);
                file = NULL;
            }
        } else if (ret == -EEXIST) {
            ret = infilfs_linux_meta_find_child(
                dir, name_out, 36, INFILFS_OBJECT_FILE, &file);
        }
    }
    if (ret)
        goto out;
    ret = infilfs_linux_meta_validate_internal(file, INFILFS_OBJECT_FILE);
    if (ret)
        goto out;
    *dir_out = dir;
    *file_out = file;
    return 0;
out:
    if (file)
        iput(file);
    iput(dir);
    return ret;
}

static int infilfs_linux_meta_validate_blob(const u8 *blob, size_t size)
{
    const struct infilfs_linux_meta_header *header;
    size_t offset;

    if (size < sizeof(*header) || size > INFILFS_LINUX_META_MAX)
        return -EFSCORRUPTED;
    header = (const struct infilfs_linux_meta_header *)blob;
    if (memcmp(header->magic, INFILFS_LINUX_META_MAGIC,
               sizeof(header->magic)) != 0 ||
        le32_to_cpu(header->version) != INFILFS_LINUX_META_VERSION ||
        le32_to_cpu(header->reserved) != 0 ||
        le32_to_cpu(header->xattr_bytes) != size - sizeof(*header))
        return -EFSCORRUPTED;

    offset = sizeof(*header);
    while (offset < size) {
        const struct infilfs_linux_xattr_record *record;
        size_t name_length;
        size_t value_length;
        size_t record_size;

        if (size - offset < sizeof(*record))
            return -EFSCORRUPTED;
        record = (const struct infilfs_linux_xattr_record *)(blob + offset);
        name_length = le16_to_cpu(record->name_length);
        value_length = le32_to_cpu(record->value_length);
        if (le16_to_cpu(record->reserved) != 0 || !name_length ||
            name_length > size - offset - sizeof(*record))
            return -EFSCORRUPTED;
        if (value_length >
            size - offset - sizeof(*record) - name_length)
            return -EFSCORRUPTED;
        record_size = sizeof(*record) + name_length + value_length;
        if (record_size > size - offset)
            return -EFSCORRUPTED;
        offset += record_size;
    }
    return offset == size ? 0 : -EFSCORRUPTED;
}

static int infilfs_linux_meta_load(struct super_block *sb,
                                   const u8 object_id[16],
                                   u8 **blob_out, size_t *size_out)
{
    struct inode *dir = NULL;
    struct inode *file = NULL;
    struct infilfs_linux_meta_header *header;
    struct kvec vec;
    struct iov_iter iter;
    char name[37];
    loff_t position = 0;
    ssize_t got;
    size_t size;
    u8 *blob;
    int ret;

    *blob_out = NULL;
    *size_out = 0;
    ret = infilfs_linux_meta_get_file(sb, object_id, false,
                                      &dir, &file, name);
    if (ret != 0 && ret != -ENOENT)
        pr_err_ratelimited("InfiltratorFS: Linux metadata lookup failed for %*phN: %d\n",
                           16, object_id, ret);
    if (ret == -ENOENT) {
        blob = kvzalloc(sizeof(*header), GFP_NOFS);
        if (!blob)
            return -ENOMEM;
        infilfs_linux_meta_init((struct infilfs_linux_meta_header *)blob);
        *blob_out = blob;
        *size_out = sizeof(*header);
        return 0;
    }
    if (ret)
        return ret;

    size = i_size_read(file);
    if (size < sizeof(*header) || size > INFILFS_LINUX_META_MAX) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    blob = kvmalloc(size, GFP_NOFS);
    if (!blob) {
        ret = -ENOMEM;
        goto out;
    }
    vec.iov_base = blob;
    vec.iov_len = size;
    iov_iter_kvec(&iter, ITER_DEST, &vec, 1, size);
    got = infilfs_native_read_iter_cached(file, &position, &iter);
    if (got != size) {
        ret = got < 0 ? (int)got : -EIO;
        pr_err_ratelimited("InfiltratorFS: Linux metadata data read failed for %*phN: got=%zd size=%zu ret=%d\n",
                           16, object_id, got, size, ret);
        kvfree(blob);
        goto out;
    }
    ret = infilfs_linux_meta_validate_blob(blob, size);
    if (ret) {
        pr_err_ratelimited("InfiltratorFS: Linux metadata blob validation failed for %*phN: %d\n",
                           16, object_id, ret);
        kvfree(blob);
        goto out;
    }
    *blob_out = blob;
    *size_out = size;
out:
    iput(file);
    iput(dir);
    return ret;
}

static int infilfs_linux_meta_delete_file(struct inode *dir,
                                          struct inode *file,
                                          const char *name)
{
    struct infilfs_inode_info *dir_ii = INFILFS_I(dir);
    struct infilfs_inode_info *file_ii = INFILFS_I(file);
    struct infilfs_native_pending *pending;
    struct infilfs_ns_index_change *changes = NULL;
    struct qstr qname = {
        .name = name,
        .len = 36,
    };
    u32 change_count = 0;
    u32 change_capacity = 0;
    u64 new_dir_block = 0;
    u64 retained_block = 0;
    bool retained = false;
    int ret;

    ret = infilfs_native_pending_flush_sb(dir->i_sb);
    if (ret)
        return ret;
    ret = infilfs_ns_begin(dir->i_sb, &pending);
    if (ret)
        return ret;
    ret = infilfs_ns_delete_object(pending, file, &changes, &change_count,
                                   &change_capacity, &retained,
                                   &retained_block);
    if (!ret)
        ret = infilfs_ns_rebuild_directory(pending, dir, &qname, NULL, NULL,
                                           NULL, 0, 0, &new_dir_block);
    if (!ret)
        ret = infilfs_ns_changes_append(
            &changes, &change_count, &change_capacity, dir_ii->object_id,
            INFILFS_NS_REPOINT, new_dir_block, INFILFS_OBJECT_DIRECTORY);
    if (!ret)
        ret = infilfs_ns_rebuild_index(pending, changes, change_count);
    ret = infilfs_ns_finish(pending, ret);
    if (!ret) {
        dir_ii->object_block = new_dir_block;
        infilfs_refresh_inode_blocks_after_commit(dir);
        if (retained) {
            file_ii->object_block = retained_block;
            drop_nlink(file);
        } else {
            clear_nlink(file);
        }
    }
    kvfree(changes);
    return ret;
}

static int infilfs_linux_meta_remove_object(struct super_block *sb,
                                            const u8 object_id[16])
{
    struct inode *dir = NULL;
    struct inode *file = NULL;
    char name[37];
    int ret;

    ret = infilfs_linux_meta_get_file(sb, object_id, false,
                                      &dir, &file, name);
    if (ret == -ENOENT)
        return 0;
    if (ret)
        return ret;
    ret = infilfs_linux_meta_delete_file(dir, file, name);
    iput(file);
    iput(dir);
    return ret;
}

static int infilfs_linux_meta_store(struct super_block *sb,
                                    const u8 object_id[16],
                                    const u8 *blob, size_t size)
{
    const struct infilfs_linux_meta_header *header;
    struct inode *dir = NULL;
    struct inode *file = NULL;
    struct kvec vec;
    struct iov_iter iter;
    char name[37];
    loff_t position = 0;
    ssize_t written;
    bool empty;
    int ret;

    ret = infilfs_linux_meta_validate_blob(blob, size);
    if (ret)
        return ret;
    header = (const struct infilfs_linux_meta_header *)blob;
    empty = le32_to_cpu(header->special_mode) == 0 &&
        le32_to_cpu(header->xattr_bytes) == 0;
    if (empty)
        return infilfs_linux_meta_remove_object(sb, object_id);

    ret = infilfs_linux_meta_get_file(sb, object_id, true,
                                      &dir, &file, name);
    if (ret)
        return ret;
    ret = infilfs_native_truncate(file, 0);
    if (ret)
        goto out;
    vec.iov_base = (void *)blob;
    vec.iov_len = size;
    iov_iter_kvec(&iter, ITER_SOURCE, &vec, 1, size);
    written = infilfs_native_extent_write_iter(file, &position, &iter, size);
    if (written != size) {
        ret = written < 0 ? (int)written : -EIO;
        goto out;
    }
    ret = infilfs_native_pending_flush_sb(sb);
out:
    iput(file);
    iput(dir);
    return ret;
}

static int infilfs_linux_meta_find_xattr(
    const u8 *blob, size_t size, const char *name, size_t *offset_out,
    size_t *record_size_out, size_t *value_offset_out,
    size_t *value_length_out)
{
    size_t wanted = strlen(name);
    size_t offset = sizeof(struct infilfs_linux_meta_header);

    while (offset < size) {
        const struct infilfs_linux_xattr_record *record =
            (const struct infilfs_linux_xattr_record *)(blob + offset);
        size_t name_length = le16_to_cpu(record->name_length);
        size_t value_length = le32_to_cpu(record->value_length);
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

static int infilfs_linux_meta_get_special(struct super_block *sb,
                                          const u8 object_id[16],
                                          umode_t *mode, dev_t *rdev)
{
    struct infilfs_linux_meta_header *header;
    umode_t stored_mode;
    u64 stored_rdev;
    u8 *blob = NULL;
    size_t size = 0;
    int ret;

    ret = infilfs_linux_meta_load(sb, object_id, &blob, &size);
    if (ret)
        return ret;
    header = (struct infilfs_linux_meta_header *)blob;
    stored_mode = (umode_t)le32_to_cpu(header->special_mode);
    stored_rdev = le64_to_cpu(header->special_rdev);
    if (stored_mode && !S_ISFIFO(stored_mode) && !S_ISSOCK(stored_mode) &&
        !S_ISCHR(stored_mode) && !S_ISBLK(stored_mode)) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    if ((!stored_mode && stored_rdev) || (u64)(dev_t)stored_rdev != stored_rdev) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    if (mode)
        *mode = stored_mode;
    if (rdev)
        *rdev = (dev_t)stored_rdev;
    ret = 0;
out:
    kvfree(blob);
    return ret;
}

static int infilfs_linux_meta_set_special(struct inode *inode,
                                          umode_t mode, dev_t rdev)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_linux_meta_header *header;
    u8 *blob = NULL;
    size_t size = 0;
    int ret;

    ret = infilfs_linux_meta_load(inode->i_sb, ii->object_id, &blob, &size);
    if (ret)
        return ret;
    header = (struct infilfs_linux_meta_header *)blob;
    header->special_mode = cpu_to_le32(mode & S_IFMT);
    header->special_rdev = cpu_to_le64((u64)rdev);
    ret = infilfs_linux_meta_store(inode->i_sb, ii->object_id, blob, size);
    kvfree(blob);
    return ret;
}

static int infilfs_linux_xattr_name(const struct xattr_handler *handler,
                                    const char *name, char **full_out)
{
    size_t prefix;
    size_t suffix;
    char *full;

    *full_out = NULL;
    if (!handler || !handler->prefix || !name)
        return -EINVAL;
    prefix = strlen(handler->prefix);
    suffix = strnlen(name, U16_MAX);
    if (!suffix || suffix >= U16_MAX ||
        prefix >= U16_MAX || suffix > U16_MAX - prefix)
        return -EINVAL;
    full = kmalloc(prefix + suffix + 1u, GFP_NOFS);
    if (!full)
        return -ENOMEM;
    memcpy(full, handler->prefix, prefix);
    memcpy(full + prefix, name, suffix + 1u);
    *full_out = full;
    return 0;
}

static int infilfs_linux_xattr_get(
    const struct xattr_handler *handler, struct dentry *dentry,
    struct inode *inode, const char *name, void *buffer, size_t size)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    char *full;
    u8 *blob = NULL;
    size_t blob_size = 0;
    size_t value_offset = 0;
    size_t value_length = 0;
    int ret;

    (void)dentry;
    ret = infilfs_linux_xattr_name(handler, name, &full);
    if (ret)
        return ret;
    ret = infilfs_linux_meta_load(inode->i_sb, ii->object_id,
                                  &blob, &blob_size);
    if (ret)
        goto out;
    if (!infilfs_linux_meta_find_xattr(
            blob, blob_size, full, NULL, NULL,
            &value_offset, &value_length)) {
        ret = -ENODATA;
        goto out;
    }
    if (!buffer) {
        ret = (int)value_length;
        goto out;
    }
    if (size < value_length) {
        ret = -ERANGE;
        goto out;
    }
    memcpy(buffer, blob + value_offset, value_length);
    ret = (int)value_length;
out:
    kvfree(blob);
    kfree(full);
    return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_linux_xattr_set(
    const struct xattr_handler *handler, struct mnt_idmap *idmap,
#else
static int infilfs_linux_xattr_set(
    const struct xattr_handler *handler, struct user_namespace *idmap,
#endif
    struct dentry *dentry, struct inode *inode, const char *name,
    const void *value, size_t value_size, int flags)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_linux_meta_header *header;
    struct infilfs_linux_xattr_record record = {0};
    char *full;
    u8 *blob = NULL;
    u8 *updated = NULL;
    size_t size = 0;
    size_t old_offset = 0;
    size_t old_size = 0;
    size_t name_length;
    size_t new_record_size;
    size_t new_size;
    size_t write_offset;
    bool exists;
    int ret;

    (void)idmap;
    (void)dentry;
    ret = infilfs_linux_xattr_name(handler, name, &full);
    if (ret)
        return ret;
    if (value_size > U32_MAX) {
        ret = -E2BIG;
        goto out;
    }
    ret = infilfs_linux_meta_load(inode->i_sb, ii->object_id, &blob, &size);
    if (ret)
        goto out;
    exists = infilfs_linux_meta_find_xattr(
        blob, size, full, &old_offset, &old_size, NULL, NULL);

    if (!value) {
        if (!exists) {
            ret = -ENODATA;
            goto out;
        }
        memmove(blob + old_offset, blob + old_offset + old_size,
                size - old_offset - old_size);
        size -= old_size;
        header = (struct infilfs_linux_meta_header *)blob;
        header->xattr_bytes = cpu_to_le32(size - sizeof(*header));
        ret = infilfs_linux_meta_store(inode->i_sb, ii->object_id,
                                       blob, size);
        goto out;
    }
    if ((flags & XATTR_CREATE) && exists) {
        ret = -EEXIST;
        goto out;
    }
    if ((flags & XATTR_REPLACE) && !exists) {
        ret = -ENODATA;
        goto out;
    }

    name_length = strlen(full);
    new_record_size = sizeof(record) + name_length + value_size;
    if (new_record_size > INFILFS_LINUX_META_MAX ||
        size - (exists ? old_size : 0u) >
            INFILFS_LINUX_META_MAX - new_record_size) {
        ret = -E2BIG;
        goto out;
    }
    new_size = size - (exists ? old_size : 0u) + new_record_size;
    updated = kvmalloc(new_size, GFP_NOFS);
    if (!updated) {
        ret = -ENOMEM;
        goto out;
    }
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
    record.name_length = cpu_to_le16((u16)name_length);
    record.value_length = cpu_to_le32((u32)value_size);
    memcpy(updated + write_offset, &record, sizeof(record));
    memcpy(updated + write_offset + sizeof(record), full, name_length);
    memcpy(updated + write_offset + sizeof(record) + name_length,
           value, value_size);
    header = (struct infilfs_linux_meta_header *)updated;
    header->xattr_bytes = cpu_to_le32(new_size - sizeof(*header));
    ret = infilfs_linux_meta_store(inode->i_sb, ii->object_id,
                                   updated, new_size);
out:
    kvfree(updated);
    kvfree(blob);
    kfree(full);
    return ret;
}

static ssize_t infilfs_linux_listxattr(struct dentry *dentry,
                                       char *list, size_t size)
{
    struct inode *inode = d_inode(dentry);
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    u8 *blob = NULL;
    size_t blob_size = 0;
    size_t offset;
    size_t needed = 0;
    int ret;

    ret = infilfs_linux_meta_load(inode->i_sb, ii->object_id,
                                  &blob, &blob_size);
    if (ret)
        return ret;
    offset = sizeof(struct infilfs_linux_meta_header);
    while (offset < blob_size) {
        const struct infilfs_linux_xattr_record *record =
            (const struct infilfs_linux_xattr_record *)(blob + offset);
        size_t name_length = le16_to_cpu(record->name_length);
        size_t value_length = le32_to_cpu(record->value_length);

        if (name_length < XATTR_TRUSTED_PREFIX_LEN ||
            memcmp(blob + offset + sizeof(*record), XATTR_TRUSTED_PREFIX,
                   XATTR_TRUSTED_PREFIX_LEN) != 0 ||
            capable(CAP_SYS_ADMIN))
            needed += name_length + 1u;
        offset += sizeof(*record) + name_length + value_length;
    }
    if (!list) {
        ret = needed;
        goto out;
    }
    if (size < needed) {
        ret = -ERANGE;
        goto out;
    }
    offset = sizeof(struct infilfs_linux_meta_header);
    needed = 0;
    while (offset < blob_size) {
        const struct infilfs_linux_xattr_record *record =
            (const struct infilfs_linux_xattr_record *)(blob + offset);
        size_t name_length = le16_to_cpu(record->name_length);
        size_t value_length = le32_to_cpu(record->value_length);

        if (name_length < XATTR_TRUSTED_PREFIX_LEN ||
            memcmp(blob + offset + sizeof(*record), XATTR_TRUSTED_PREFIX,
                   XATTR_TRUSTED_PREFIX_LEN) != 0 ||
            capable(CAP_SYS_ADMIN)) {
            memcpy(list + needed, blob + offset + sizeof(*record), name_length);
            list[needed + name_length] = '\0';
            needed += name_length + 1u;
        }
        offset += sizeof(*record) + name_length + value_length;
    }
    ret = needed;
out:
    kvfree(blob);
    return ret;
}

static const struct xattr_handler infilfs_linux_user_xattr_handler = {
    .prefix = XATTR_USER_PREFIX,
    .get = infilfs_linux_xattr_get,
    .set = infilfs_linux_xattr_set,
};

static const struct xattr_handler infilfs_linux_trusted_xattr_handler = {
    .prefix = XATTR_TRUSTED_PREFIX,
    .get = infilfs_linux_xattr_get,
    .set = infilfs_linux_xattr_set,
};

static const struct xattr_handler infilfs_linux_security_xattr_handler = {
    .prefix = XATTR_SECURITY_PREFIX,
    .get = infilfs_linux_xattr_get,
    .set = infilfs_linux_xattr_set,
};

static const struct xattr_handler infilfs_linux_system_xattr_handler = {
    .prefix = XATTR_SYSTEM_PREFIX,
    .get = infilfs_linux_xattr_get,
    .set = infilfs_linux_xattr_set,
};

static const struct xattr_handler * const infilfs_xattr_handlers[] = {
    &infilfs_linux_user_xattr_handler,
    &infilfs_linux_trusted_xattr_handler,
    &infilfs_linux_security_xattr_handler,
    &infilfs_linux_system_xattr_handler,
    NULL,
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_mknod(struct mnt_idmap *idmap, struct inode *dir,
                               struct dentry *dentry, umode_t mode, dev_t rdev)
#else
static int infilfs_posix_mknod(struct user_namespace *idmap, struct inode *dir,
                               struct dentry *dentry, umode_t mode, dev_t rdev)
#endif
{
    struct inode *inode;
    umode_t type = mode & S_IFMT;
    int cleanup;
    int ret;

    if (!S_ISREG(mode) && !S_ISFIFO(mode) && !S_ISSOCK(mode) &&
        !S_ISCHR(mode) && !S_ISBLK(mode))
        return -EINVAL;
    ret = infilfs_posix_create(idmap, dir, dentry, mode, false);
    if (ret || S_ISREG(mode))
        return ret;
    inode = d_inode(dentry);
    ret = infilfs_linux_meta_set_special(inode, type, rdev);
    if (ret) {
        cleanup = infilfs_ns_unlink(dir, dentry);
        if (!cleanup)
            d_drop(dentry);
        return ret;
    }
    init_special_inode(inode, type | (inode->i_mode & 07777), rdev);
    inode->i_op = &infilfs_file_inode_operations;
    return 0;
}
