// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Linux-only metadata compatibility.
 *
 * Format 0.18 deliberately keeps portable objects independent of Linux VFS
 * details.  The final FUSE adapter stored xattrs and special-node type/rdev in
 * hidden, system-marked ordinary files below /.infilfs-posix-meta.  Reuse that
 * exact representation so native mounts preserve existing metadata without
 * changing the portable object layout.
 */

/* O(1) UUID-sidecar lookup after one bounded per-mount cache build. */
static struct infilfs_linux_meta_cache_entry *
infilfs_linux_meta_cache_lookup(struct infilfs_sb_info *sbi,
                                const u8 target_object_id[16])
{
    struct infilfs_linux_meta_cache_entry *entry;
    u32 bucket;

    if (!sbi || !sbi->linux_meta_cache)
        return NULL;
    bucket = infilfs_native_id_hash(target_object_id) &
        (INFILFS_LINUX_META_CACHE_BUCKETS - 1u);
    hlist_for_each_entry(entry, &sbi->linux_meta_cache[bucket], node) {
        if (memcmp(entry->target_object_id, target_object_id, 16) == 0)
            return entry;
    }
    return NULL;
}

static int infilfs_linux_meta_cache_prepare(struct infilfs_sb_info *sbi)
{
    u32 i;

    if (sbi->linux_meta_cache)
        return 0;
    sbi->linux_meta_cache = kvcalloc(
        INFILFS_LINUX_META_CACHE_BUCKETS,
        sizeof(*sbi->linux_meta_cache), GFP_NOFS);
    if (!sbi->linux_meta_cache)
        return -ENOMEM;
    for (i = 0; i < INFILFS_LINUX_META_CACHE_BUCKETS; ++i)
        INIT_HLIST_HEAD(&sbi->linux_meta_cache[i]);
    return 0;
}

static void infilfs_linux_meta_cache_reset(struct infilfs_sb_info *sbi)
{
    u32 i;

    if (!sbi || !sbi->linux_meta_cache)
        return;
    for (i = 0; i < INFILFS_LINUX_META_CACHE_BUCKETS; ++i) {
        struct infilfs_linux_meta_cache_entry *entry;
        struct hlist_node *tmp;

        hlist_for_each_entry_safe(entry, tmp,
                                  &sbi->linux_meta_cache[i], node) {
            hlist_del(&entry->node);
            kfree(entry);
        }
        INIT_HLIST_HEAD(&sbi->linux_meta_cache[i]);
    }
    sbi->linux_meta_cache_valid = false;
}

static void infilfs_linux_meta_cache_destroy(struct super_block *sb)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);

    if (!sbi)
        return;
    infilfs_linux_meta_cache_reset(sbi);
    kvfree(sbi->linux_meta_cache);
    sbi->linux_meta_cache = NULL;
    sbi->linux_meta_dir_checked = false;
    sbi->linux_meta_dir_cached = false;
}

static int infilfs_linux_meta_cache_insert(
    struct infilfs_sb_info *sbi, const u8 target_object_id[16],
    const u8 sidecar_object_id[16], u64 sidecar_object_block)
{
    struct infilfs_linux_meta_cache_entry *entry;
    u32 bucket;
    int ret;

    ret = infilfs_linux_meta_cache_prepare(sbi);
    if (ret)
        return ret;
    entry = infilfs_linux_meta_cache_lookup(sbi, target_object_id);
    if (entry) {
        memcpy(entry->sidecar_object_id, sidecar_object_id, 16);
        entry->sidecar_object_block = sidecar_object_block;
        return 0;
    }
    entry = kzalloc(sizeof(*entry), GFP_NOFS);
    if (!entry)
        return -ENOMEM;
    memcpy(entry->target_object_id, target_object_id, 16);
    memcpy(entry->sidecar_object_id, sidecar_object_id, 16);
    entry->sidecar_object_block = sidecar_object_block;
    bucket = infilfs_native_id_hash(target_object_id) &
        (INFILFS_LINUX_META_CACHE_BUCKETS - 1u);
    hlist_add_head(&entry->node, &sbi->linux_meta_cache[bucket]);
    return 0;
}

static void infilfs_linux_meta_cache_remove(
    struct infilfs_sb_info *sbi, const u8 target_object_id[16])
{
    struct infilfs_linux_meta_cache_entry *entry =
        infilfs_linux_meta_cache_lookup(sbi, target_object_id);

    if (!entry)
        return;
    hlist_del(&entry->node);
    kfree(entry);
}

static int infilfs_linux_meta_hex_nibble(u8 c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -EINVAL;
}

static bool infilfs_linux_meta_parse_uuid_name(
    const u8 *name, size_t length, u8 object_id[16])
{
    static const u8 hyphen_at[4] = {8, 13, 18, 23};
    size_t i;
    unsigned int nibble = 0;
    unsigned int h = 0;

    if (!name || length != 36)
        return false;
    memset(object_id, 0, 16);
    for (i = 0; i < length; ++i) {
        int value;

        if (h < ARRAY_SIZE(hyphen_at) && i == hyphen_at[h]) {
            if (name[i] != '-')
                return false;
            ++h;
            continue;
        }
        value = infilfs_linux_meta_hex_nibble(name[i]);
        if (value < 0 || nibble >= 32)
            return false;
        if ((nibble & 1u) == 0)
            object_id[nibble >> 1] = (u8)value << 4;
        else
            object_id[nibble >> 1] |= (u8)value;
        ++nibble;
    }
    return nibble == 32 && h == ARRAY_SIZE(hyphen_at);
}

struct infilfs_linux_meta_cache_build {
    struct super_block *sb;
    struct infilfs_sb_info *sbi;
};

static int infilfs_linux_meta_cache_build_visitor(
    const struct infilfs_dirent_disk *entry, const u8 *name, void *arg)
{
    struct infilfs_linux_meta_cache_build *build = arg;
    u8 target_object_id[16];
    u64 block;
    u16 type;
    int ret;

    if (le16_to_cpu(entry->object_type) != INFILFS_OBJECT_FILE ||
        !infilfs_linux_meta_parse_uuid_name(
            name, le16_to_cpu(entry->name_length), target_object_id))
        return 0;
    ret = infilfs_index_lookup(build->sb, entry->object_id, &block, &type);
    if (ret)
        return ret;
    if (type != INFILFS_OBJECT_FILE)
        return -EFSCORRUPTED;
    return infilfs_linux_meta_cache_insert(
        build->sbi, target_object_id, entry->object_id, block);
}

static int infilfs_linux_meta_cache_build(struct super_block *sb,
                                          struct inode *dir)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct infilfs_linux_meta_cache_build build = {.sb = sb, .sbi = sbi};
    int ret;

    if (sbi->linux_meta_cache_valid)
        return 0;
    ret = infilfs_linux_meta_cache_prepare(sbi);
    if (ret)
        return ret;
    infilfs_linux_meta_cache_reset(sbi);
    mutex_lock(&sbi->write_lock);
    ret = infilfs_for_each_dirent(
        dir, infilfs_linux_meta_cache_build_visitor, &build);
    mutex_unlock(&sbi->write_lock);
    if (ret < 0) {
        infilfs_linux_meta_cache_reset(sbi);
        return ret;
    }
    sbi->linux_meta_cache_valid = true;
    return 0;
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

    /*
     * The hidden Linux sidecar namespace shares the global object index with
     * ordinary files.  Hold the native writer lock while resolving its
     * directory entry and index block so checkpoint publication cannot reclaim
     * an older tree path between those reads.  Release before get_inode():
     * inode population can inspect sidecar metadata recursively.
     */
    mutex_lock(&INFILFS_SB(dir->i_sb)->write_lock);
    ret = infilfs_for_each_dirent(dir, infilfs_lookup_visitor, &search);
    if (ret < 0)
        goto unlock;
    if (!search.found) {
        ret = -ENOENT;
        goto unlock;
    }
    if (search.object_type != expected_type) {
        ret = -EFSCORRUPTED;
        goto unlock;
    }
    ret = infilfs_index_lookup(dir->i_sb, search.object_id, &block,
                               &indexed_type);
    if (ret)
        goto unlock;
    if (indexed_type != expected_type) {
        ret = -EFSCORRUPTED;
        goto unlock;
    }
    mutex_unlock(&INFILFS_SB(dir->i_sb)->write_lock);
    inode = infilfs_get_inode(dir->i_sb, block, indexed_type,
                              search.object_id);
    if (IS_ERR(inode))
        return PTR_ERR(inode);
    *inode_out = inode;
    return 0;

unlock:
    mutex_unlock(&INFILFS_SB(dir->i_sb)->write_lock);
    return ret;
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
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
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
    if (!sbi || !sb->s_root)
        return -ENOENT;
    if (sbi->linux_meta_dir_checked && !sbi->linux_meta_dir_cached && !create)
        return -ENOENT;
    if (sbi->linux_meta_dir_cached) {
        dir = infilfs_get_inode(
            sb, sbi->linux_meta_dir_object_block,
            INFILFS_OBJECT_DIRECTORY, sbi->linux_meta_dir_object_id);
        if (!IS_ERR(dir)) {
            ret = infilfs_linux_meta_validate_internal(
                dir, INFILFS_OBJECT_DIRECTORY);
            if (!ret) {
                *dir_out = dir;
                return 0;
            }
            iput(dir);
            return ret;
        }
        sbi->linux_meta_dir_cached = false;
    }

    root = d_inode(sb->s_root);
    ret = infilfs_linux_meta_find_child(
        root, INFILFS_LINUX_META_DIRECTORY,
        sizeof(INFILFS_LINUX_META_DIRECTORY) - 1u,
        INFILFS_OBJECT_DIRECTORY, &dir);
    if (ret == -ENOENT && create) {
        if (sb_rdonly(sb))
            return -EROFS;
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
    if (ret) {
        if (ret == -ENOENT) {
            sbi->linux_meta_dir_checked = true;
            sbi->linux_meta_dir_cached = false;
        }
        return ret;
    }
    ret = infilfs_linux_meta_validate_internal(dir, INFILFS_OBJECT_DIRECTORY);
    if (ret) {
        iput(dir);
        return ret;
    }
    memcpy(sbi->linux_meta_dir_object_id, INFILFS_I(dir)->object_id, 16);
    sbi->linux_meta_dir_object_block = INFILFS_I(dir)->object_block;
    sbi->linux_meta_dir_checked = true;
    sbi->linux_meta_dir_cached = true;
    *dir_out = dir;
    return 0;
}

static bool infilfs_linux_meta_directory_is_internal(struct super_block *sb)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct inode *dir = NULL;
    int ret;

    if (!sbi)
        return false;
    mutex_lock(&sbi->linux_meta_lock);
    ret = infilfs_linux_meta_get_dir(sb, false, &dir);
    if (!ret)
        iput(dir);
    mutex_unlock(&sbi->linux_meta_lock);
    return ret == 0;
}

static int infilfs_linux_meta_get_file(
    struct super_block *sb, const u8 object_id[16], bool create,
    struct inode **dir_out, struct inode **file_out, char name_out[37])
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct infilfs_linux_meta_cache_entry *cached;
    struct inode *dir = NULL;
    struct inode *file = NULL;
    struct qstr name;
    u8 id[16];
    u64 block;
    int ret;

    *dir_out = NULL;
    *file_out = NULL;
    if (!sbi)
        return -EINVAL;
    infilfs_linux_meta_uuid(object_id, name_out);
    ret = infilfs_linux_meta_get_dir(sb, create, &dir);
    if (ret == -ENOENT && !create) {
        ret = infilfs_linux_meta_cache_prepare(sbi);
        if (!ret)
            sbi->linux_meta_cache_valid = true;
        return ret ? ret : -ENOENT;
    }
    if (ret)
        return ret;

    ret = infilfs_linux_meta_cache_build(sb, dir);
    if (ret)
        goto out;
    cached = infilfs_linux_meta_cache_lookup(sbi, object_id);
    if (cached) {
        file = infilfs_get_inode(
            sb, cached->sidecar_object_block, INFILFS_OBJECT_FILE,
            cached->sidecar_object_id);
        if (IS_ERR(file)) {
            ret = PTR_ERR(file);
            file = NULL;
            goto out;
        }
        ret = infilfs_linux_meta_validate_internal(file, INFILFS_OBJECT_FILE);
        if (ret)
            goto out;
        *dir_out = dir;
        *file_out = file;
        return 0;
    }
    if (!create) {
        ret = -ENOENT;
        goto out;
    }

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
            goto out;
        }
        ret = infilfs_linux_meta_cache_insert(sbi, object_id, id, block);
        if (ret)
            goto out;
    } else if (ret == -EEXIST) {
        sbi->linux_meta_cache_valid = false;
        ret = infilfs_linux_meta_cache_build(sb, dir);
        if (ret)
            goto out;
        cached = infilfs_linux_meta_cache_lookup(sbi, object_id);
        if (!cached) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        file = infilfs_get_inode(
            sb, cached->sidecar_object_block, INFILFS_OBJECT_FILE,
            cached->sidecar_object_id);
        if (IS_ERR(file)) {
            ret = PTR_ERR(file);
            file = NULL;
            goto out;
        }
    }
    if (ret)
        goto out;
    ret = infilfs_linux_meta_validate_internal(file, INFILFS_OBJECT_FILE);
    if (ret)
        goto out;
    sbi->linux_meta_dir_object_block = INFILFS_I(dir)->object_block;
    sbi->linux_meta_dir_checked = true;
    sbi->linux_meta_dir_cached = true;
    *dir_out = dir;
    *file_out = file;
    return 0;
out:
    if (file)
        iput(file);
    if (dir)
        iput(dir);
    return ret;
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
    s64 dir_block_delta = 0;
    bool retained = false;
    int ret;

    ret = infilfs_ns_begin(dir->i_sb, &pending);
    if (ret)
        return ret;
    ret = infilfs_ns_delete_object(pending, file, &changes, &change_count,
                                   &change_capacity, &retained,
                                   &retained_block);
    if (!ret)
        ret = infilfs_ns_rebuild_directory_accounted(
            pending, dir, &qname, NULL, NULL, NULL, 0, 0,
            &new_dir_block, &dir_block_delta);
    if (!ret)
        ret = infilfs_ns_changes_append(
            &changes, &change_count, &change_capacity, dir_ii->object_id,
            INFILFS_NS_REPOINT, new_dir_block, INFILFS_OBJECT_DIRECTORY);
    if (!ret)
        ret = infilfs_ns_rebuild_index(pending, changes, change_count);
    ret = infilfs_ns_finish(pending, ret);
    if (!ret) {
        dir_ii->object_block = new_dir_block;
        infilfs_adjust_inode_blocks_after_commit(dir, dir_block_delta);
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

static int infilfs_linux_meta_remove_object_locked(struct super_block *sb,
                                                   const u8 object_id[16])
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
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
    if (!ret) {
        infilfs_linux_meta_cache_remove(sbi, object_id);
        sbi->linux_meta_dir_object_block = INFILFS_I(dir)->object_block;
    }
    iput(file);
    iput(dir);
    return ret;
}

static int infilfs_linux_meta_remove_object(struct super_block *sb,
                                            const u8 object_id[16])
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    int ret;

    if (!sbi)
        return -EINVAL;
    mutex_lock(&sbi->linux_meta_lock);
    ret = infilfs_linux_meta_remove_object_locked(sb, object_id);
    mutex_unlock(&sbi->linux_meta_lock);
    return ret;
}

static int infilfs_linux_meta_store(struct super_block *sb,
                                    const u8 object_id[16],
                                    const u8 *blob, size_t size)
{
    const struct infilfs_linux_meta_header *header;
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct inode *dir = NULL;
    struct inode *file = NULL;
    struct infilfs_inode_info *ii;
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
        return infilfs_linux_meta_remove_object_locked(sb, object_id);

    ret = infilfs_linux_meta_get_file(sb, object_id, true,
                                      &dir, &file, name);
    if (ret)
        return ret;

    if (i_size_read(file) != 0) {
        ret = infilfs_native_truncate(file, 0);
        if (ret)
            goto out;
    }
    vec.iov_base = (void *)blob;
    vec.iov_len = size;
    iov_iter_kvec(&iter, ITER_SOURCE, &vec, 1, size);
    if (size <= INFILFS_INLINE_DATA_MAX)
        written = infilfs_native_inline_write_iter(file, &position, &iter, size);
    else
        written = infilfs_native_extent_write_iter(file, &position, &iter, size);
    if (written != size) {
        ret = written < 0 ? (int)written : -EIO;
        goto out;
    }
    ii = INFILFS_I(file);
    ret = ii ? infilfs_linux_meta_cache_insert(
        sbi, object_id, ii->object_id, ii->object_block) : -EIO;
out:
    iput(file);
    iput(dir);
    return ret;
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
    struct infilfs_sb_info *sbi = INFILFS_SB(inode->i_sb);
    struct infilfs_linux_meta_header *header;
    u8 *blob = NULL;
    size_t size = 0;
    int ret;

    if (!sbi)
        return -EINVAL;
    mutex_lock(&sbi->linux_meta_lock);
    ret = infilfs_linux_meta_load(inode->i_sb, ii->object_id, &blob, &size);
    if (ret)
        goto out;
    header = (struct infilfs_linux_meta_header *)blob;
    header->special_mode = cpu_to_le32(mode & S_IFMT);
    header->special_rdev = cpu_to_le64((u64)rdev);
    ret = infilfs_linux_meta_store(inode->i_sb, ii->object_id, blob, size);
out:
    kvfree(blob);
    mutex_unlock(&sbi->linux_meta_lock);
    return ret;
}

static int infilfs_linux_xattr_get(
    const struct xattr_handler *handler, struct dentry *dentry,
    struct inode *inode, const char *name, void *buffer, size_t size)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_sb_info *sbi = INFILFS_SB(inode->i_sb);
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
    if (!sbi) {
        kfree(full);
        return -EINVAL;
    }
    mutex_lock(&sbi->linux_meta_lock);
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
    mutex_unlock(&sbi->linux_meta_lock);
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
    struct infilfs_sb_info *sbi = INFILFS_SB(inode->i_sb);
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
    if (value_size > XATTR_SIZE_MAX || value_size > U32_MAX) {
        kfree(full);
        return -E2BIG;
    }
    if (!sbi) {
        kfree(full);
        return -EINVAL;
    }
    mutex_lock(&sbi->linux_meta_lock);
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
    mutex_unlock(&sbi->linux_meta_lock);
    kfree(full);
    /*
     * trusted.infiltratorfs.quota-db is internal filesystem bookkeeping, not
     * user-visible inode metadata.  Rewriting the host inode ctime for every
     * quota database update both misstates POSIX semantics and interleaves a
     * second object rewrite with the still-deferred quota sidecar transaction.
     * Keep normal xattr ctime semantics for every externally visible xattr, but
     * do not manufacture a ctime change for this private quota record.
     */
    if (!ret &&
        !(handler && handler->prefix &&
          strcmp(handler->prefix, XATTR_TRUSTED_PREFIX) == 0 &&
          strcmp(name, "infiltratorfs.quota-db") == 0)) {
        struct iattr attr;
        int ctime_ret;

        memset(&attr, 0, sizeof(attr));
        attr.ia_valid = ATTR_CTIME;
        attr.ia_ctime = current_time(inode);
        ctime_ret = infilfs_posix_rewrite_inode(inode, &attr);
        if (!ctime_ret)
            inode_set_ctime_to_ts(inode, attr.ia_ctime);
        else
            pr_warn_ratelimited(
                "InfiltratorFS: xattr committed but ctime persistence failed for inode %lu: %d\n",
                inode->i_ino, ctime_ret);
    }
    return ret;
}

static ssize_t infilfs_linux_listxattr(struct dentry *dentry,
                                       char *list, size_t size)
{
    struct inode *inode = d_inode(dentry);
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_sb_info *sbi = INFILFS_SB(inode->i_sb);
    u8 *blob = NULL;
    size_t blob_size = 0;
    size_t offset;
    size_t needed = 0;
    int ret;

    if (!sbi)
        return -EINVAL;
    mutex_lock(&sbi->linux_meta_lock);
    ret = infilfs_linux_meta_load(inode->i_sb, ii->object_id,
                                  &blob, &blob_size);
    if (ret)
        goto out;
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
    mutex_unlock(&sbi->linux_meta_lock);
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


/*
 * Native Linux POSIX ACL adapter.
 *
 * Format 0.18 deliberately keeps Linux ACLs out of the portable object
 * layout. Store the Linux POSIX ACL xattr representation in the existing
 * hidden Linux metadata sidecar. The VFS owns permission evaluation,
 * idmapped-mount translation and ACL caching; this adapter supplies durable
 * get/set plus create/chmod integration.
 */
static const char *infilfs_posix_acl_suffix(int type)
{
    switch (type) {
    case ACL_TYPE_ACCESS:
        return "posix_acl_access";
    case ACL_TYPE_DEFAULT:
        return "posix_acl_default";
    default:
        return NULL;
    }
}

static struct posix_acl *infilfs_posix_acl_get(
    struct inode *inode, int type, bool rcu)
{
    const char *suffix = infilfs_posix_acl_suffix(type);
    void *value = NULL;
    struct posix_acl *acl;
    int size;
    int got;
    int ret;

    if (rcu)
        return ERR_PTR(-ECHILD);
    if (!suffix)
        return ERR_PTR(-EINVAL);
    if (type == ACL_TYPE_DEFAULT && !S_ISDIR(inode->i_mode))
        return NULL;

    size = infilfs_linux_xattr_get(
        &infilfs_linux_system_xattr_handler, NULL, inode,
        suffix, NULL, 0);
    if (size == -ENODATA)
        return NULL;
    if (size < 0)
        return ERR_PTR(size);
    if (!size)
        return ERR_PTR(-EFSCORRUPTED);

    value = kvmalloc((size_t)size, GFP_NOFS);
    if (!value)
        return ERR_PTR(-ENOMEM);
    got = infilfs_linux_xattr_get(
        &infilfs_linux_system_xattr_handler, NULL, inode,
        suffix, value, (size_t)size);
    if (got != size) {
        kvfree(value);
        return ERR_PTR(got < 0 ? got : -EIO);
    }

    acl = posix_acl_from_xattr(&init_user_ns, value, (size_t)size);
    kvfree(value);
    if (IS_ERR_OR_NULL(acl))
        return acl ? acl : ERR_PTR(-EFSCORRUPTED);
    ret = posix_acl_valid(&init_user_ns, acl);
    if (ret) {
        posix_acl_release(acl);
        return ERR_PTR(ret == -ENOMEM ? ret : -EFSCORRUPTED);
    }
    return acl;
}

static int infilfs_posix_acl_encode(
    const struct posix_acl *acl, void **value_out, size_t *size_out)
{
    void *value;
    size_t size;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
    value = posix_acl_to_xattr(&init_user_ns, acl, &size, GFP_NOFS);
    if (!value)
        return -ENOMEM;
#else
    int encoded;

    encoded = posix_acl_to_xattr(&init_user_ns, acl, NULL, 0);
    if (encoded < 0)
        return encoded;
    size = (size_t)encoded;
    value = kvmalloc(size, GFP_NOFS);
    if (!value)
        return -ENOMEM;
    encoded = posix_acl_to_xattr(&init_user_ns, acl, value, size);
    if (encoded < 0 || (size_t)encoded != size) {
        kvfree(value);
        return encoded < 0 ? encoded : -EIO;
    }
#endif
    *value_out = value;
    *size_out = size;
    return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_acl_store(
    struct mnt_idmap *idmap, struct dentry *dentry,
#else
static int infilfs_posix_acl_store(
    struct user_namespace *idmap, struct dentry *dentry,
#endif
    struct inode *inode, int type, const struct posix_acl *acl)
{
    const char *suffix = infilfs_posix_acl_suffix(type);
    void *value = NULL;
    size_t size = 0;
    int ret;

    if (!suffix)
        return -EINVAL;
    if (!acl) {
        ret = infilfs_linux_xattr_set(
            &infilfs_linux_system_xattr_handler, idmap, dentry,
            inode, suffix, NULL, 0, 0);
        return ret == -ENODATA ? 0 : ret;
    }
    ret = infilfs_posix_acl_encode(acl, &value, &size);
    if (ret)
        return ret;
    ret = infilfs_linux_xattr_set(
        &infilfs_linux_system_xattr_handler, idmap, dentry,
        inode, suffix, value, size, 0);
    kvfree(value);
    return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_acl_set(
    struct mnt_idmap *idmap, struct dentry *dentry,
#else
static int infilfs_posix_acl_set(
    struct user_namespace *idmap, struct dentry *dentry,
#endif
    struct posix_acl *acl, int type)
{
    struct inode *inode = d_inode(dentry);
    struct posix_acl *old_acl;
    struct posix_acl *stored_acl = acl;
    umode_t old_mode = inode->i_mode;
    umode_t mode = old_mode;
    struct iattr attr;
    int rollback_ret;
    int ret;

    if (type != ACL_TYPE_ACCESS && type != ACL_TYPE_DEFAULT)
        return -EINVAL;
    if (type == ACL_TYPE_DEFAULT && !S_ISDIR(inode->i_mode)) {
        if (acl)
            return -EACCES;
        set_cached_acl(inode, type, NULL);
        return 0;
    }
    if (acl) {
        ret = posix_acl_valid(&init_user_ns, acl);
        if (ret)
            return ret;
    }
    if (type == ACL_TYPE_ACCESS && stored_acl) {
        ret = posix_acl_update_mode(idmap, inode, &mode, &stored_acl);
        if (ret)
            return ret;
    }

    old_acl = infilfs_posix_acl_get(inode, type, false);
    if (IS_ERR(old_acl))
        return PTR_ERR(old_acl);

    ret = infilfs_posix_acl_store(idmap, dentry, inode, type, stored_acl);
    if (ret)
        goto out_old;

    if (type == ACL_TYPE_ACCESS && mode != old_mode) {
        memset(&attr, 0, sizeof(attr));
        attr.ia_valid = ATTR_MODE | ATTR_CTIME;
        attr.ia_mode = mode;
        attr.ia_ctime = current_time(inode);
        ret = infilfs_posix_rewrite_inode(inode, &attr);
        if (ret) {
            rollback_ret = infilfs_posix_acl_store(
                idmap, dentry, inode, type, old_acl);
            if (rollback_ret) {
                INFILFS_SB(inode->i_sb)->write_poisoned = true;
                pr_err("InfiltratorFS: ACL rollback failed for inode %lu: %d\n",
                       inode->i_ino, rollback_ret);
            }
            goto out_old;
        }
        inode->i_mode = (inode->i_mode & S_IFMT) | (mode & 07777);
    }
    set_cached_acl(inode, type, stored_acl);
    ret = 0;
out_old:
    posix_acl_release(old_acl);
    return ret;
}

static int infilfs_posix_acl_prepare_create(
    struct inode *dir, umode_t *mode, umode_t type,
    struct posix_acl **default_acl, struct posix_acl **access_acl)
{
    umode_t acl_mode = (*mode & 07777) | type;
    int ret;

    ret = posix_acl_create(dir, &acl_mode, default_acl, access_acl);
    if (!ret)
        *mode = (*mode & S_IFMT) | (acl_mode & 07777);
    return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_acl_finish_create(
    struct mnt_idmap *idmap, struct inode *dir,
#else
static int infilfs_posix_acl_finish_create(
    struct user_namespace *idmap, struct inode *dir,
#endif
    struct dentry *dentry, struct posix_acl *default_acl,
    struct posix_acl *access_acl, bool is_directory)
{
    struct inode *inode = d_inode(dentry);
    int cleanup;
    int ret = 0;

    if (!inode)
        return -EIO;
    if (access_acl)
        ret = infilfs_posix_acl_store(
            idmap, dentry, inode, ACL_TYPE_ACCESS, access_acl);
    if (!ret && default_acl)
        ret = infilfs_posix_acl_store(
            idmap, dentry, inode, ACL_TYPE_DEFAULT, default_acl);
    if (!ret) {
        set_cached_acl(inode, ACL_TYPE_ACCESS, access_acl);
        if (is_directory)
            set_cached_acl(inode, ACL_TYPE_DEFAULT, default_acl);
        return 0;
    }

    (void)infilfs_linux_meta_remove_object(
        inode->i_sb, INFILFS_I(inode)->object_id);
    cleanup = is_directory ?
        infilfs_ns_rmdir(dir, dentry) : infilfs_ns_unlink(dir, dentry);
    if (!cleanup)
        d_drop(dentry);
    else
        pr_err("InfiltratorFS: could not remove object after ACL create failure: %d\n",
               cleanup);
    return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_acl_create(
    struct mnt_idmap *idmap, struct inode *dir,
#else
static int infilfs_posix_acl_create(
    struct user_namespace *idmap, struct inode *dir,
#endif
    struct dentry *dentry, umode_t mode, bool excl)
{
    struct posix_acl *default_acl = NULL;
    struct posix_acl *access_acl = NULL;
    int ret;

    ret = infilfs_posix_acl_prepare_create(
        dir, &mode, S_IFREG, &default_acl, &access_acl);
    if (!ret)
        ret = infilfs_posix_create(idmap, dir, dentry, mode, excl);
    if (!ret)
        ret = infilfs_posix_acl_finish_create(
            idmap, dir, dentry, default_acl, access_acl, false);
    posix_acl_release(access_acl);
    posix_acl_release(default_acl);
    return ret;
}


#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_acl_tmpfile(
    struct mnt_idmap *idmap, struct inode *dir,
    struct file *file, umode_t mode)
#else
static int infilfs_posix_acl_tmpfile(
    struct user_namespace *idmap, struct inode *dir,
    struct file *file, umode_t mode)
#endif
{
    struct posix_acl *default_acl = NULL;
    struct posix_acl *access_acl = NULL;
    struct inode *inode = NULL;
    int ret;

    ret = infilfs_posix_acl_prepare_create(
        dir, &mode, S_IFREG, &default_acl, &access_acl);
    if (!ret)
        ret = infilfs_posix_tmpfile(idmap, dir, file, mode);
    if (!ret) {
        inode = file_inode(file);
        if (!inode)
            ret = -EIO;
        else if (access_acl)
            ret = infilfs_posix_acl_store(
                idmap, NULL, inode, ACL_TYPE_ACCESS, access_acl);
        if (!ret && inode)
            set_cached_acl(inode, ACL_TYPE_ACCESS, access_acl);
    }
    posix_acl_release(access_acl);
    posix_acl_release(default_acl);
    return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
static struct dentry *infilfs_posix_acl_mkdir(
    struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, umode_t mode)
{
    struct posix_acl *default_acl = NULL;
    struct posix_acl *access_acl = NULL;
    struct dentry *result;
    int ret;

    ret = infilfs_posix_acl_prepare_create(
        dir, &mode, S_IFDIR, &default_acl, &access_acl);
    if (ret)
        return ERR_PTR(ret);
    result = infilfs_posix_mkdir(idmap, dir, dentry, mode);
    if (IS_ERR(result)) {
        posix_acl_release(access_acl);
        posix_acl_release(default_acl);
        return result;
    }
    ret = infilfs_posix_acl_finish_create(
        idmap, dir, dentry, default_acl, access_acl, true);
    posix_acl_release(access_acl);
    posix_acl_release(default_acl);
    return ret ? ERR_PTR(ret) : result;
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_acl_mkdir(
    struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, umode_t mode)
#else
static int infilfs_posix_acl_mkdir(
    struct user_namespace *idmap, struct inode *dir,
    struct dentry *dentry, umode_t mode)
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 17, 0)
{
    struct posix_acl *default_acl = NULL;
    struct posix_acl *access_acl = NULL;
    int ret;

    ret = infilfs_posix_acl_prepare_create(
        dir, &mode, S_IFDIR, &default_acl, &access_acl);
    if (!ret)
        ret = infilfs_posix_mkdir(idmap, dir, dentry, mode);
    if (!ret)
        ret = infilfs_posix_acl_finish_create(
            idmap, dir, dentry, default_acl, access_acl, true);
    posix_acl_release(access_acl);
    posix_acl_release(default_acl);
    return ret;
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_acl_mknod(
    struct mnt_idmap *idmap, struct inode *dir,
#else
static int infilfs_posix_acl_mknod(
    struct user_namespace *idmap, struct inode *dir,
#endif
    struct dentry *dentry, umode_t mode, dev_t rdev)
{
    struct posix_acl *default_acl = NULL;
    struct posix_acl *access_acl = NULL;
    umode_t type = mode & S_IFMT;
    int ret;

    if (!type)
        type = S_IFREG;
    ret = infilfs_posix_acl_prepare_create(
        dir, &mode, type, &default_acl, &access_acl);
    if (!ret)
        ret = infilfs_posix_mknod(idmap, dir, dentry, mode, rdev);
    if (!ret)
        ret = infilfs_posix_acl_finish_create(
            idmap, dir, dentry, default_acl, access_acl, false);
    posix_acl_release(access_acl);
    posix_acl_release(default_acl);
    return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_acl_setattr(
    struct mnt_idmap *idmap, struct dentry *dentry,
#else
static int infilfs_posix_acl_setattr(
    struct user_namespace *idmap, struct dentry *dentry,
#endif
    struct iattr *attr)
{
    struct inode *inode = d_inode(dentry);
    umode_t old_mode = inode->i_mode;
    bool mode_changed = (attr->ia_valid & ATTR_MODE) != 0;
    int ret = infilfs_posix_setattr(idmap, dentry, attr);

    if (!ret && mode_changed) {
        ret = posix_acl_chmod(idmap, dentry, inode->i_mode);
        if (ret) {
            struct iattr rollback;
            int rollback_ret;

            memset(&rollback, 0, sizeof(rollback));
            rollback.ia_valid = ATTR_MODE | ATTR_CTIME;
            rollback.ia_mode = old_mode;
            rollback.ia_ctime = current_time(inode);
            rollback_ret = infilfs_posix_rewrite_inode(inode, &rollback);
            if (!rollback_ret) {
                inode->i_mode = (inode->i_mode & S_IFMT) |
                    (old_mode & 07777);
                inode_set_ctime_to_ts(inode, rollback.ia_ctime);
            } else {
                INFILFS_SB(inode->i_sb)->write_poisoned = true;
                pr_err("InfiltratorFS: chmod ACL rollback failed for inode %lu: %d\n",
                       inode->i_ino, rollback_ret);
            }
        }
    }
    return ret;
}
