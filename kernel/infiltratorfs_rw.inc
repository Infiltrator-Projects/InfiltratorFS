// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Native Linux RW composition.
 *
 * Keep the already-qualified inline transaction engine intact as the legacy
 * implementation, then layer the extent-backed/deferred native data path,
 * transactional namespace mutations and the POSIX metadata bridge over the
 * VFS entry points that need them.
 */

#include <linux/statfs.h>

#define infilfs_rw_tx_begin infilfs_rw_tx_begin_legacy
#define infilfs_file_write_iter infilfs_file_write_iter_legacy
#define infilfs_file_fsync infilfs_file_fsync_legacy
#define infilfs_rw_create infilfs_rw_create_legacy
#define infilfs_rw_mkdir infilfs_rw_mkdir_legacy
#define infilfs_rw_setattr infilfs_rw_setattr_legacy
#define infilfs_rw_mount_destroy infilfs_rw_mount_destroy_legacy
#include "infiltratorfs_rw_legacy.inc"
#undef infilfs_rw_tx_begin
#undef infilfs_file_write_iter
#undef infilfs_file_fsync
#undef infilfs_rw_create
#undef infilfs_rw_mkdir
#undef infilfs_rw_setattr
#undef infilfs_rw_mount_destroy

/*
 * A deferred native transaction must be able to read back the transaction-
 * private metadata it has just staged.  The generic read validators reject
 * object/page generations newer than the live superblock generation, so make
 * the working generation visible to this mount as soon as the deferred
 * transaction begins.  No checkpoint is published here: the on-disk committed
 * generation remains unchanged until infilfs_rw_tx_commit().
 */
static int infilfs_rw_tx_begin(struct super_block *sb,
                               struct infilfs_rw_tx *tx)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    int ret = infilfs_rw_tx_begin_legacy(sb, tx);

    if (ret)
        return ret;
    tx->next_sb.generation = cpu_to_le64(tx->generation);
    sbi->disk.generation = cpu_to_le64(tx->generation);
    return 0;
}

/*
 * The data layer already wraps create/mkdir/setattr to flush a deferred data
 * transaction before entering the original synchronous metadata path.  Keep
 * those qualified wrappers available under private names so the final POSIX
 * layer can add ownership and general setattr semantics without duplicating
 * the data-path ordering rules.
 */
#define infilfs_rw_create infilfs_rw_create_data
#define infilfs_rw_mkdir infilfs_rw_mkdir_data
#define infilfs_rw_setattr infilfs_rw_setattr_data
#include "infiltratorfs_rw_data.inc"
#undef infilfs_rw_create
#undef infilfs_rw_mkdir
#undef infilfs_rw_setattr

/*
 * The verified extent reader in the data layer uses a checksum cursor while a
 * single read_iter() call is active.  Ordinary userspace readers issue many
 * separate calls, so compose the bounded cross-call cursor cache here and make
 * it the final VFS read callback.
 */
#ifdef infilfs_file_read_iter
#undef infilfs_file_read_iter
#endif
#include "infiltratorfs_rw_read_cache.inc"
#define infilfs_file_read_iter infilfs_file_read_iter_cached

/*
 * New native objects must carry the credentials of the task that created them.
 * The original bring-up code deliberately used uid/gid zero while write support
 * was root-only.  That made every file created by an ordinary desktop user
 * become root-owned once native mounts were exposed through Nautilus.
 */
static int infilfs_posix_uid_to_disk(kuid_t uid, u32 *value)
{
    uid_t raw = from_kuid(&init_user_ns, uid);

    if (raw == (uid_t)-1)
        return -EOVERFLOW;
    *value = (u32)raw;
    return 0;
}

static int infilfs_posix_gid_to_disk(kgid_t gid, u32 *value)
{
    gid_t raw = from_kgid(&init_user_ns, gid);

    if (raw == (gid_t)-1)
        return -EOVERFLOW;
    *value = (u32)raw;
    return 0;
}

static void infilfs_posix_current_ids(kuid_t *uid, kgid_t *gid)
{
    const struct cred *cred = get_current_cred();

    *uid = cred->fsuid;
    *gid = cred->fsgid;
    put_cred(cred);
}

static void infilfs_posix_fill_common_attributes(
    struct infilfs_attributes_disk *attributes,
    struct infilfs_posix_compat_disk *posix,
    umode_t mode, u64 links)
{
    u64 now = ktime_get_real_ns();
    kuid_t kernel_uid;
    kgid_t kernel_gid;
    u32 uid = 0;
    u32 gid = 0;

    /*
     * The disk format stores init-user-namespace numeric IDs.  The format
     * header intentionally removes Linux's current task macro, so obtain a
     * stable credential reference through the exported credential API rather
     * than depending on current_fsuid/current_fsgid macro expansion here.
     */
    infilfs_posix_current_ids(&kernel_uid, &kernel_gid);
    (void)infilfs_posix_uid_to_disk(kernel_uid, &uid);
    (void)infilfs_posix_gid_to_disk(kernel_gid, &gid);

    memset(attributes, 0, sizeof(*attributes));
    memset(posix, 0, sizeof(*posix));
    attributes->link_count = cpu_to_le64(links);
    attributes->birth_time_ns = cpu_to_le64(now);
    attributes->access_time_ns = cpu_to_le64(now);
    attributes->modification_time_ns = cpu_to_le64(now);
    attributes->change_time_ns = cpu_to_le64(now);
    posix->permissions = cpu_to_le32(mode & 07777);
    posix->uid = cpu_to_le32(uid);
    posix->gid = cpu_to_le32(gid);
}

/*
 * Namespace symlink creation is compiled after the legacy layer and can use
 * the credential-aware initializer directly.  Regular-file and directory
 * creation still pass through the qualified legacy transaction and are fixed
 * atomically by the POSIX wrapper below immediately after creation.
 */
#define infilfs_rw_fill_common_attributes infilfs_posix_fill_common_attributes
#include "infiltratorfs_rw_namespace.inc"
#undef infilfs_rw_fill_common_attributes

/*
 * A crash can occur after the final directory name is removed but before the
 * last open VFS reference reaches ->evict_inode.  Such files are deliberately
 * retained in the object index with link_count zero.  Reclaim that persistent
 * orphan list before a writable mount is exposed.
 */
static int infilfs_native_recover_unlinked_files(struct super_block *sb)
{
    struct infilfs_index_entry_disk *entries = NULL;
    u32 count = 0;
    u32 i;
    int ret;

    if (sb_rdonly(sb))
        return 0;
    ret = infilfs_ns_index_snapshot(sb, &entries, &count);
    if (ret)
        return ret;
    for (i = 0; i < count; ++i) {
        struct infilfs_object_header_disk *header;
        struct infilfs_file_payload_disk *file;
        struct inode *inode;
        u8 *object;

        if (le16_to_cpu(entries[i].object_type) != INFILFS_OBJECT_FILE)
            continue;
        object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!object) {
            ret = -ENOMEM;
            break;
        }
        ret = infilfs_read_object(sb, le64_to_cpu(entries[i].object_block),
                                  INFILFS_OBJECT_FILE, entries[i].object_id,
                                  object);
        if (ret) {
            kfree(object);
            break;
        }
        header = (struct infilfs_object_header_disk *)object;
        file = (struct infilfs_file_payload_disk *)(header + 1);
        if (le64_to_cpu(file->attributes.link_count) != 0) {
            kfree(object);
            continue;
        }
        kfree(object);

        inode = infilfs_get_inode(sb, le64_to_cpu(entries[i].object_block),
                                  INFILFS_OBJECT_FILE,
                                  entries[i].object_id);
        if (IS_ERR(inode)) {
            ret = PTR_ERR(inode);
            break;
        }
        clear_nlink(inode);
        ret = infilfs_ns_evict_unlinked_file(inode);
        if (!ret)
            set_nlink(inode, 1); /* prevent a second reclaim during iput */
        iput(inode);
        if (ret)
            break;
    }
    kvfree(entries);
    return ret;
}

static u64 infilfs_native_extent_allocated_blocks(
    const struct infilfs_extent_disk *extents, u32 count)
{
    u64 allocated = 0;
    u32 i;

    for (i = 0; i < count; ++i)
        if (le32_to_cpu(extents[i].flags) == INFILFS_EXTENT_NORMAL)
            allocated += le32_to_cpu(extents[i].block_count);
    return allocated;
}

static int infilfs_native_append_holes(
    struct infilfs_extent_disk *extents, u32 capacity, u32 *count,
    u64 start, u64 blocks)
{
    while (blocks) {
        u32 chunk = blocks > U32_MAX ? U32_MAX : (u32)blocks;
        int ret = infilfs_native_extent_append(
            extents, capacity, count, start, 0, chunk, INFILFS_EXTENT_HOLE);

        if (ret)
            return ret;
        start += chunk;
        blocks -= chunk;
    }
    return 0;
}

static int infilfs_native_copy_truncated_extents(
    struct infilfs_native_pending *pending, struct inode *inode,
    const struct infilfs_extent_disk *old_extents, u32 old_count,
    u64 old_blocks, u64 target_blocks,
    struct infilfs_extent_disk **new_out, u32 *new_count_out)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_extent_disk *out;
    u64 extra64 = target_blocks > old_blocks ?
        DIV_ROUND_UP_ULL(target_blocks - old_blocks, U32_MAX) : 0;
    u32 extra;
    u32 capacity;
    u32 count = 0;
    u32 i;
    int ret;

    if (extra64 > U32_MAX || extra64 > U32_MAX - old_count - 2u)
        return -EFBIG;
    extra = (u32)extra64;
    capacity = old_count + extra + 2u;
    out = kvmalloc_array(capacity, sizeof(*out), GFP_NOFS);
    if (!out)
        return -ENOMEM;

    for (i = 0; i < old_count; ++i) {
        u64 logical = le64_to_cpu(old_extents[i].logical_block);
        u64 physical = le64_to_cpu(old_extents[i].physical_block);
        u64 extent_end = logical + le32_to_cpu(old_extents[i].block_count);
        u32 flags = le32_to_cpu(old_extents[i].flags);
        u64 keep_end = min_t(u64, extent_end, target_blocks);

        if (logical < target_blocks) {
            ret = infilfs_native_extent_append(
                out, capacity, &count, logical,
                flags == INFILFS_EXTENT_NORMAL ? physical : 0,
                (u32)(keep_end - logical), flags);
            if (ret)
                goto fail;
        }
        if (flags == INFILFS_EXTENT_NORMAL && extent_end > target_blocks) {
            u64 removed_start = max_t(u64, logical, target_blocks);

            ret = infilfs_ns_free_unshared_run(
                pending, ii->object_id, physical + removed_start - logical,
                extent_end - removed_start);
            if (ret)
                goto fail;
        }
    }
    if (target_blocks > old_blocks) {
        ret = infilfs_native_append_holes(out, capacity, &count, old_blocks,
                                          target_blocks - old_blocks);
        if (ret)
            goto fail;
    }
    *new_out = out;
    *new_count_out = count;
    return 0;
fail:
    kvfree(out);
    return ret;
}

static int infilfs_native_verify_data_block(
    struct inode *inode, const struct infilfs_file_payload_disk *file,
    u64 logical, const u8 data[INFILFS_DISK_BLOCK_SIZE])
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_native_read_checksum_cursor cursor = {0};
    struct infilfs_data_checksum_disk expected;
    struct infilfs_data_checksum_disk actual;
    u8 *checksum_object;
    int ret;

    checksum_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!checksum_object)
        return -ENOMEM;
    ret = infilfs_native_read_expected_digest(
        inode->i_sb, ii->object_id, file->checksum_head_id, logical,
        &cursor, checksum_object, &expected);
    kfree(checksum_object);
    if (ret)
        return ret;
    infilfs_native_block_digest(data, &actual);
    return memcmp(actual.bytes, expected.bytes, sizeof(actual.bytes)) == 0 ?
        0 : -EIO;
}

static int infilfs_native_translate_index_changes(
    struct infilfs_ns_index_change **changes, u32 *count, u32 *capacity,
    const struct infilfs_native_index_change *native, u32 native_count)
{
    u32 i;

    for (i = 0; i < native_count; ++i) {
        int ret = infilfs_ns_changes_append(
            changes, count, capacity, native[i].object_id,
            native[i].add ? INFILFS_NS_ADD : INFILFS_NS_REPOINT,
            native[i].object_block, native[i].object_type);

        if (ret)
            return ret;
    }
    return 0;
}

/* Native extent-aware truncate, including sparse growth and partial-tail CoW. */
static int infilfs_native_truncate(struct inode *inode, u64 new_size)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_native_pending *pending = NULL;
    struct infilfs_ns_index_change *changes = NULL;
    struct infilfs_native_index_change checksum_changes[4] = {0};
    struct infilfs_extent_disk *old_extents = NULL;
    struct infilfs_extent_disk *new_extents = NULL;
    struct infilfs_file_payload_disk working_file;
    struct infilfs_object_header_disk *old_header;
    struct infilfs_file_payload_disk *old_file;
    u8 *old_object = NULL;
    u8 *new_object = NULL;
    u8 *data = NULL;
    u32 change_count = 0, change_capacity = 0;
    u32 checksum_change_count = 0;
    u32 old_extent_count = 0, new_extent_count = 0;
    u64 old_blocks = 0;
    u64 target_blocks;
    u64 new_file_block = 0;
    bool was_inline = false;
    bool file_repoint = false;
    int ret;

    if (new_size == (u64)i_size_read(inode))
        return 0;
    if (new_size > MAX_LFS_FILESIZE)
        return -EFBIG;

    ret = infilfs_ns_begin(inode->i_sb, &pending);
    if (ret)
        return ret;
    old_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    new_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!old_object || !new_object) {
        ret = -ENOMEM;
        goto finish;
    }
    ret = infilfs_native_collect_extents(
        pending, inode, old_object, &old_extents, &old_extent_count,
        &old_blocks, &was_inline);
    if (ret)
        goto finish;
    old_header = (struct infilfs_object_header_disk *)old_object;
    old_file = (struct infilfs_file_payload_disk *)(old_header + 1);
    memcpy(&working_file, old_file, sizeof(working_file));
    target_blocks = DIV_ROUND_UP_ULL(new_size, INFILFS_DISK_BLOCK_SIZE);

    /* Promote an inline file only when growth crosses the inline boundary. */
    if (was_inline && new_size <= INFILFS_INLINE_DATA_MAX) {
        u8 inline_data[INFILFS_INLINE_DATA_MAX] = {0};
        size_t old_size = min_t(u64,
            le64_to_cpu(old_file->attributes.logical_size),
            INFILFS_INLINE_DATA_MAX);
        size_t loaded_size = 0;
        struct infilfs_data_checksum_disk *digest;

        ret = infilfs_rw_load_inline(inode, inline_data, &loaded_size);
        if (ret)
            goto finish;
        if (loaded_size != old_size) {
            ret = -EFSCORRUPTED;
            goto finish;
        }
        memset(new_object, 0, INFILFS_DISK_BLOCK_SIZE);
        memcpy(new_object, old_object, sizeof(*old_header) + sizeof(*old_file));
        old_header = (struct infilfs_object_header_disk *)new_object;
        old_file = (struct infilfs_file_payload_disk *)(old_header + 1);
        old_header->generation = cpu_to_le64(pending->tx.generation);
        old_header->object_version = cpu_to_le16(INFILFS_OBJECT_VERSION_CLASSIC);
        old_file->attributes.logical_size = cpu_to_le64(new_size);
        old_file->attributes.modification_time_ns = cpu_to_le64(ktime_get_real_ns());
        old_file->attributes.change_time_ns =
            old_file->attributes.modification_time_ns;
        old_file->extent_count = 0;
        memset(old_file->checksum_head_id, 0, sizeof(old_file->checksum_head_id));
        if (new_size) {
            digest = (struct infilfs_data_checksum_disk *)(old_file + 1);
            ret = infilfs_rw_inline_digest(inline_data, (size_t)new_size,
                                           digest->bytes);
            if (ret)
                goto finish;
            memcpy(digest + 1, inline_data, (size_t)new_size);
            old_header->payload_size = cpu_to_le32(
                sizeof(*old_file) + sizeof(*digest) + (size_t)new_size);
        } else {
            old_header->payload_size = cpu_to_le32(sizeof(*old_file));
        }
        ret = infilfs_rw_finalize_object(new_object);
        if (ret)
            goto finish;
        ret = infilfs_native_store_private_or_cow(
            pending, ii->object_block, new_object, &new_file_block);
        if (ret)
            goto finish;
        ret = infilfs_ns_changes_append(
            &changes, &change_count, &change_capacity, ii->object_id,
            INFILFS_NS_REPOINT, new_file_block, INFILFS_OBJECT_FILE);
        if (!ret)
            ret = infilfs_ns_rebuild_index(pending, changes, change_count);
        goto finish;
    }

    ret = infilfs_native_copy_truncated_extents(
        pending, inode, old_extents, old_extent_count, old_blocks,
        target_blocks, &new_extents, &new_extent_count);
    if (ret)
        goto finish;

    /* Promote preserved inline bytes into the first extent block. */
    if (was_inline && le64_to_cpu(old_file->attributes.logical_size)) {
        u64 data_block;
        struct infilfs_data_checksum_disk digest;
        u8 tail_id[16];
        u64 tail_block, tail_start;
        size_t loaded_size = 0;

        data = kzalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!data) {
            ret = -ENOMEM;
            goto finish;
        }
        ret = infilfs_rw_load_inline(inode, data, &loaded_size);
        if (ret)
            goto finish;
        if (loaded_size != le64_to_cpu(old_file->attributes.logical_size)) {
            ret = -EFSCORRUPTED;
            goto finish;
        }
        ret = infilfs_native_alloc_data_exact(pending, 1, 1, &data_block);
        if (!ret)
            ret = infilfs_native_stage_block(inode->i_sb, data_block, data);
        if (!ret)
            ret = infilfs_native_replace_extent_range(
                new_extents, new_extent_count, target_blocks, 0, 1,
                data_block, INFILFS_EXTENT_NORMAL,
                &old_extents, &old_extent_count);
        if (ret)
            goto finish;
        kvfree(new_extents);
        new_extents = old_extents;
        new_extent_count = old_extent_count;
        old_extents = NULL;
        infilfs_native_block_digest(data, &digest);
        ret = infilfs_native_checksum_set_range(
            pending, &working_file, ii->object_id, 0, 1, &digest,
            checksum_changes, ARRAY_SIZE(checksum_changes),
            &checksum_change_count, tail_id, &tail_block, &tail_start);
        if (ret)
            goto finish;
    }

    if (new_size < le64_to_cpu(old_file->attributes.logical_size) &&
        target_blocks && (new_size & (INFILFS_DISK_BLOCK_SIZE - 1u))) {
        u64 logical = target_blocks - 1u;
        u64 physical = 0;
        u64 replacement;
        u32 flags = INFILFS_EXTENT_HOLE;

        ret = infilfs_native_map_extent(new_extents, new_extent_count, logical,
                                        &physical, &flags);
        if (ret)
            goto finish;
        if (flags == INFILFS_EXTENT_NORMAL) {
            struct infilfs_data_checksum_disk digest;
            struct infilfs_extent_disk *replaced = NULL;
            u32 replaced_count = 0;
            u8 tail_id[16];
            u64 tail_block, tail_start;

            if (!data) {
                data = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
                if (!data) {
                    ret = -ENOMEM;
                    goto finish;
                }
            }
            ret = infilfs_read_block(inode->i_sb, physical, data);
            if (!ret)
                ret = infilfs_native_verify_data_block(inode, old_file,
                                                       logical, data);
            if (ret)
                goto finish;
            memset(data + (new_size & (INFILFS_DISK_BLOCK_SIZE - 1u)), 0,
                   INFILFS_DISK_BLOCK_SIZE -
                       (new_size & (INFILFS_DISK_BLOCK_SIZE - 1u)));
            ret = infilfs_native_alloc_data_exact(pending, 1, 1, &replacement);
            if (!ret)
                ret = infilfs_native_stage_block(inode->i_sb, replacement, data);
            if (!ret)
                ret = infilfs_native_replace_extent_range(
                    new_extents, new_extent_count, target_blocks, logical, 1,
                    replacement, INFILFS_EXTENT_NORMAL,
                    &replaced, &replaced_count);
            if (!ret)
                ret = infilfs_ns_free_unshared_run(
                    pending, ii->object_id, physical, 1);
            if (ret) {
                kvfree(replaced);
                goto finish;
            }
            kvfree(new_extents);
            new_extents = replaced;
            new_extent_count = replaced_count;
            infilfs_native_block_digest(data, &digest);
            ret = infilfs_native_checksum_set_range(
                pending, &working_file, ii->object_id, logical, 1, &digest,
                checksum_changes, ARRAY_SIZE(checksum_changes),
                &checksum_change_count, tail_id, &tail_block, &tail_start);
            if (ret)
                goto finish;
        }
    }

    if (!infilfs_native_extent_allocated_blocks(new_extents,
                                                 new_extent_count) &&
        memchr_inv(working_file.checksum_head_id, 0,
                   sizeof(working_file.checksum_head_id))) {
        ret = infilfs_ns_collect_checksum_removals(
            pending, &working_file, ii->object_id, &changes, &change_count,
            &change_capacity);
        if (ret)
            goto finish;
        memset(working_file.checksum_head_id, 0,
               sizeof(working_file.checksum_head_id));
    } else {
        ret = infilfs_native_translate_index_changes(
            &changes, &change_count, &change_capacity, checksum_changes,
            checksum_change_count);
        if (ret)
            goto finish;
    }

    ret = infilfs_native_build_file_object(
        pending, inode, old_object, &working_file, new_extents,
        new_extent_count, new_size, new_object, &new_file_block,
        &file_repoint);
    if (ret)
        goto finish;
    if (file_repoint)
        ret = infilfs_ns_changes_append(
            &changes, &change_count, &change_capacity, ii->object_id,
            INFILFS_NS_REPOINT, new_file_block, INFILFS_OBJECT_FILE);
    if (!ret)
        ret = infilfs_ns_rebuild_index(pending, changes, change_count);

finish:
    ret = infilfs_ns_finish(pending, ret);
    if (!ret) {
        ii->object_block = new_file_block;
        i_size_write(inode, new_size);
        inode->i_blocks = infilfs_native_extent_allocated_blocks(
            new_extents, new_extent_count) *
            (INFILFS_DISK_BLOCK_SIZE >> 9);
        infilfs_native_checksum_cache_invalidate_sb(inode->i_sb);
    }
    kfree(data);
    kfree(new_object);
    kfree(old_object);
    kvfree(new_extents);
    kvfree(old_extents);
    kvfree(changes);
    return ret;
}

static int infilfs_native_punch_partial_locked(
    struct infilfs_native_pending *pending, struct inode *inode,
    struct infilfs_file_payload_disk *working_file,
    struct infilfs_extent_disk **extents_inout, u32 *extent_count_inout,
    u64 logical, size_t within, size_t length)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_native_index_change checksum_changes[4] = {0};
    struct infilfs_extent_disk *replaced = NULL;
    struct infilfs_data_checksum_disk digest;
    u8 *data;
    u8 tail_id[16];
    u64 physical = 0;
    u64 replacement = 0;
    u64 tail_block = 0, tail_start = 0;
    u32 flags = INFILFS_EXTENT_HOLE;
    u32 replaced_count = 0;
    u32 checksum_change_count = 0;
    int ret;

    if (!length)
        return 0;
    ret = infilfs_native_map_extent(*extents_inout, *extent_count_inout,
                                    logical, &physical, &flags);
    if (ret || flags == INFILFS_EXTENT_HOLE)
        return ret;
    data = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!data)
        return -ENOMEM;
    ret = infilfs_read_block(inode->i_sb, physical, data);
    if (!ret)
        ret = infilfs_native_verify_data_block(inode, working_file,
                                               logical, data);
    if (ret)
        goto out;
    memset(data + within, 0, length);
    ret = infilfs_native_alloc_data_exact(pending, 1, 1, &replacement);
    if (!ret)
        ret = infilfs_native_stage_block(inode->i_sb, replacement, data);
    if (!ret)
        ret = infilfs_native_replace_extent_range(
            *extents_inout, *extent_count_inout, i_size_read(inode) ?
                DIV_ROUND_UP_ULL(i_size_read(inode),
                                 INFILFS_DISK_BLOCK_SIZE) : 0,
            logical, 1, replacement, INFILFS_EXTENT_NORMAL,
            &replaced, &replaced_count);
    if (!ret)
        ret = infilfs_ns_free_unshared_run(
            pending, ii->object_id, physical, 1);
    if (ret)
        goto out;

    infilfs_native_block_digest(data, &digest);
    ret = infilfs_native_checksum_set_range(
        pending, working_file, ii->object_id, logical, 1, &digest,
        checksum_changes, ARRAY_SIZE(checksum_changes),
        &checksum_change_count, tail_id, &tail_block, &tail_start);
    if (!ret)
        ret = infilfs_native_index_update(
            pending, checksum_changes, checksum_change_count);
    if (ret)
        goto out;
    kvfree(*extents_inout);
    *extents_inout = replaced;
    *extent_count_inout = replaced_count;
    replaced = NULL;
out:
    kvfree(replaced);
    kfree(data);
    return ret;
}

static int infilfs_native_punch_hole(struct inode *inode,
                                     u64 offset, u64 length)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_native_pending *pending = NULL;
    struct infilfs_ns_index_change *changes = NULL;
    struct infilfs_extent_disk *old_extents = NULL;
    struct infilfs_extent_disk *extents = NULL;
    struct infilfs_object_header_disk *header;
    struct infilfs_file_payload_disk *file;
    struct infilfs_file_payload_disk working_file;
    u8 *old_object = NULL;
    u8 *new_object = NULL;
    u32 change_count = 0, change_capacity = 0;
    u32 old_extent_count = 0, extent_count = 0;
    u64 old_blocks = 0;
    u64 file_size = i_size_read(inode);
    u64 end;
    u64 pos;
    u64 new_file_block = 0;
    bool was_inline = false;
    bool file_repoint = false;
    int ret;

    if (!length || offset >= file_size)
        return 0;
    end = length > file_size - offset ? file_size : offset + length;
    ret = infilfs_ns_begin(inode->i_sb, &pending);
    if (ret)
        return ret;
    old_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    new_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!old_object || !new_object) {
        ret = -ENOMEM;
        goto finish;
    }
    ret = infilfs_native_collect_extents(
        pending, inode, old_object, &old_extents, &old_extent_count,
        &old_blocks, &was_inline);
    if (ret)
        goto finish;
    header = (struct infilfs_object_header_disk *)old_object;
    file = (struct infilfs_file_payload_disk *)(header + 1);
    memcpy(&working_file, file, sizeof(working_file));

    if (was_inline) {
        struct infilfs_data_checksum_disk *digest;
        u8 inline_data[INFILFS_INLINE_DATA_MAX] = {0};
        size_t loaded_size = 0;

        ret = infilfs_rw_load_inline(inode, inline_data, &loaded_size);
        if (ret)
            goto finish;
        if (loaded_size != file_size) {
            ret = -EFSCORRUPTED;
            goto finish;
        }
        memset(inline_data + offset, 0, (size_t)(end - offset));
        memset(new_object, 0, INFILFS_DISK_BLOCK_SIZE);
        memcpy(new_object, old_object, sizeof(*header) + sizeof(*file));
        header = (struct infilfs_object_header_disk *)new_object;
        file = (struct infilfs_file_payload_disk *)(header + 1);
        header->generation = cpu_to_le64(pending->tx.generation);
        file->attributes.modification_time_ns = cpu_to_le64(ktime_get_real_ns());
        file->attributes.change_time_ns = file->attributes.modification_time_ns;
        digest = (struct infilfs_data_checksum_disk *)(file + 1);
        ret = infilfs_rw_inline_digest(inline_data, (size_t)file_size,
                                       digest->bytes);
        if (ret)
            goto finish;
        memcpy(digest + 1, inline_data, (size_t)file_size);
        header->payload_size = cpu_to_le32(
            sizeof(*file) + sizeof(*digest) + (size_t)file_size);
        ret = infilfs_rw_finalize_object(new_object);
        if (!ret)
            ret = infilfs_native_store_private_or_cow(
                pending, ii->object_block, new_object, &new_file_block);
        if (!ret)
            ret = infilfs_ns_changes_append(
                &changes, &change_count, &change_capacity, ii->object_id,
                INFILFS_NS_REPOINT, new_file_block, INFILFS_OBJECT_FILE);
        if (!ret)
            ret = infilfs_ns_rebuild_index(pending, changes, change_count);
        goto finish;
    }

    extents = kvmalloc_array(old_extent_count ? old_extent_count : 1u,
                             sizeof(*extents), GFP_NOFS);
    if (!extents) {
        ret = -ENOMEM;
        goto finish;
    }
    if (old_extent_count)
        memcpy(extents, old_extents,
               (size_t)old_extent_count * sizeof(*extents));
    extent_count = old_extent_count;
    pos = offset;
    if (pos & (INFILFS_DISK_BLOCK_SIZE - 1u)) {
        size_t within = pos & (INFILFS_DISK_BLOCK_SIZE - 1u);
        size_t chunk = min_t(u64, INFILFS_DISK_BLOCK_SIZE - within,
                             end - pos);

        ret = infilfs_native_punch_partial_locked(
            pending, inode, &working_file, &extents, &extent_count,
            pos >> INFILFS_DISK_BLOCK_SHIFT, within, chunk);
        if (ret)
            goto finish;
        pos += chunk;
    }
    while (end - pos >= INFILFS_DISK_BLOCK_SIZE) {
        u64 remaining = (end - pos) >> INFILFS_DISK_BLOCK_SHIFT;
        u32 chunk = remaining > U32_MAX ? U32_MAX : (u32)remaining;
        struct infilfs_extent_disk *replaced = NULL;
        u32 replaced_count = 0;
        u64 logical = pos >> INFILFS_DISK_BLOCK_SHIFT;

        ret = infilfs_native_replace_extent_range(
            extents, extent_count, old_blocks, logical, chunk, 0,
            INFILFS_EXTENT_HOLE, &replaced, &replaced_count);
        if (!ret)
            ret = infilfs_native_release_replaced_extents(
                pending, ii->object_id, extents, extent_count,
                logical, chunk);
        if (ret) {
            kvfree(replaced);
            goto finish;
        }
        kvfree(extents);
        extents = replaced;
        extent_count = replaced_count;
        pos += (u64)chunk << INFILFS_DISK_BLOCK_SHIFT;
    }
    if (pos < end) {
        ret = infilfs_native_punch_partial_locked(
            pending, inode, &working_file, &extents, &extent_count,
            pos >> INFILFS_DISK_BLOCK_SHIFT, 0, (size_t)(end - pos));
        if (ret)
            goto finish;
    }

    if (!infilfs_native_extent_allocated_blocks(extents, extent_count) &&
        memchr_inv(working_file.checksum_head_id, 0,
                   sizeof(working_file.checksum_head_id))) {
        ret = infilfs_ns_collect_checksum_removals(
            pending, &working_file, ii->object_id, &changes, &change_count,
            &change_capacity);
        if (ret)
            goto finish;
        memset(working_file.checksum_head_id, 0,
               sizeof(working_file.checksum_head_id));
    }
    ret = infilfs_native_build_file_object(
        pending, inode, old_object, &working_file, extents, extent_count,
        file_size, new_object, &new_file_block, &file_repoint);
    if (ret)
        goto finish;
    if (file_repoint)
        ret = infilfs_ns_changes_append(
            &changes, &change_count, &change_capacity, ii->object_id,
            INFILFS_NS_REPOINT, new_file_block, INFILFS_OBJECT_FILE);
    if (!ret)
        ret = infilfs_ns_rebuild_index(pending, changes, change_count);

finish:
    ret = infilfs_ns_finish(pending, ret);
    if (!ret) {
        ii->object_block = new_file_block;
        inode->i_blocks = infilfs_native_extent_allocated_blocks(
            extents, extent_count) * (INFILFS_DISK_BLOCK_SIZE >> 9);
        infilfs_native_checksum_cache_invalidate_sb(inode->i_sb);
    }
    kvfree(changes);
    kvfree(extents);
    kvfree(old_extents);
    kfree(new_object);
    kfree(old_object);
    return ret;
}

static long infilfs_file_fallocate(struct file *file, int mode,
                                   loff_t offset, loff_t length)
{
    const int punch = FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE;
    struct inode *inode = file_inode(file);
    u8 *buffer;
    u64 position;
    u64 end;
    int ret = 0;

    if (offset < 0 || length <= 0)
        return -EINVAL;
    if ((u64)offset > U64_MAX - (u64)length)
        return -EFBIG;
    if (mode == punch)
        return infilfs_native_punch_hole(inode, (u64)offset, (u64)length);
    if (mode != 0)
        return -EOPNOTSUPP;

    end = (u64)offset + (u64)length;
    buffer = kvzalloc(64u * 1024u, GFP_NOFS);
    if (!buffer)
        return -ENOMEM;
    position = (u64)offset;
    while (position < end) {
        struct kiocb iocb;
        struct iov_iter iter;
        struct kvec vec;
        size_t chunk = min_t(u64, 64u * 1024u, end - position);
        ssize_t got;
        ssize_t written;

        memset(buffer, 0, chunk);
        vec.iov_base = buffer;
        vec.iov_len = chunk;
        init_sync_kiocb(&iocb, file);
        iocb.ki_pos = position;
        iov_iter_kvec(&iter, ITER_DEST, &vec, 1, chunk);
        got = infilfs_file_read_iter_cached(&iocb, &iter);
        if (got < 0) {
            ret = (int)got;
            break;
        }

        vec.iov_base = buffer;
        vec.iov_len = chunk;
        init_sync_kiocb(&iocb, file);
        iocb.ki_pos = position;
        iov_iter_kvec(&iter, ITER_SOURCE, &vec, 1, chunk);
        written = infilfs_file_write_iter(&iocb, &iter);
        if (written < 0) {
            ret = (int)written;
            break;
        }
        if (written != chunk) {
            ret = -EIO;
            break;
        }
        position += chunk;
    }
    kvfree(buffer);
    if (!ret)
        ret = infilfs_native_pending_flush_sb(inode->i_sb);
    return ret;
}

static int infilfs_posix_object_fields(
    u8 object[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_attributes_disk **attributes_out,
    struct infilfs_posix_compat_disk **posix_out)
{
    struct infilfs_object_header_disk *header =
        (struct infilfs_object_header_disk *)object;

    switch (le16_to_cpu(header->object_type)) {
    case INFILFS_OBJECT_DIRECTORY: {
        struct infilfs_directory_payload_disk *payload =
            (struct infilfs_directory_payload_disk *)(header + 1);
        *attributes_out = &payload->attributes;
        *posix_out = &payload->posix;
        return 0;
    }
    case INFILFS_OBJECT_FILE: {
        struct infilfs_file_payload_disk *payload =
            (struct infilfs_file_payload_disk *)(header + 1);
        *attributes_out = &payload->attributes;
        *posix_out = &payload->posix;
        return 0;
    }
    case INFILFS_OBJECT_SYMLINK: {
        struct infilfs_symlink_payload_disk *payload =
            (struct infilfs_symlink_payload_disk *)(header + 1);
        *attributes_out = &payload->attributes;
        *posix_out = &payload->posix;
        return 0;
    }
    default:
        return -EOPNOTSUPP;
    }
}

static __le64 infilfs_posix_encode_time(struct timespec64 value)
{
    return cpu_to_le64((u64)timespec64_to_ns(&value));
}

/*
 * Copy-on-write one inode's POSIX metadata and repoint its object-index entry
 * in the same transaction.  This is the common persistence primitive for
 * chmod(2), chown(2), utimensat(2)/touch(1), and creator ownership repair.
 */
static int infilfs_posix_rewrite_inode(struct inode *inode,
                                       const struct iattr *attr)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_native_pending *pending = NULL;
    struct infilfs_object_header_disk *header;
    struct infilfs_attributes_disk *attributes;
    struct infilfs_posix_compat_disk *posix;
    struct infilfs_ns_index_change change;
    u8 *object = NULL;
    u64 old_block;
    u64 new_block = 0;
    u32 value;
    bool metadata_changed = false;
    int ret;

    if (!ii)
        return -EIO;
    if (!(attr->ia_valid & (ATTR_MODE | ATTR_UID | ATTR_GID |
                            ATTR_ATIME | ATTR_MTIME | ATTR_CTIME)))
        return 0;

    ret = infilfs_ns_begin(inode->i_sb, &pending);
    if (ret)
        return ret;

    old_block = ii->object_block;
    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object) {
        ret = -ENOMEM;
        goto finish;
    }
    ret = infilfs_read_object(inode->i_sb, old_block, ii->object_type,
                              ii->object_id, object);
    if (ret)
        goto finish;
    header = (struct infilfs_object_header_disk *)object;
    ret = infilfs_posix_object_fields(object, &attributes, &posix);
    if (ret)
        goto finish;

    if (attr->ia_valid & ATTR_MODE) {
        posix->permissions = cpu_to_le32(attr->ia_mode & 07777);
        metadata_changed = true;
    }
    if (attr->ia_valid & ATTR_UID) {
        ret = infilfs_posix_uid_to_disk(attr->ia_uid, &value);
        if (ret)
            goto finish;
        posix->uid = cpu_to_le32(value);
        metadata_changed = true;
    }
    if (attr->ia_valid & ATTR_GID) {
        ret = infilfs_posix_gid_to_disk(attr->ia_gid, &value);
        if (ret)
            goto finish;
        posix->gid = cpu_to_le32(value);
        metadata_changed = true;
    }
    if (attr->ia_valid & ATTR_ATIME) {
        attributes->access_time_ns = infilfs_posix_encode_time(attr->ia_atime);
        metadata_changed = true;
    }
    if (attr->ia_valid & ATTR_MTIME) {
        attributes->modification_time_ns =
            infilfs_posix_encode_time(attr->ia_mtime);
        metadata_changed = true;
    }
    if (attr->ia_valid & ATTR_CTIME) {
        attributes->change_time_ns = infilfs_posix_encode_time(attr->ia_ctime);
    } else if (metadata_changed) {
        attributes->change_time_ns = cpu_to_le64(ktime_get_real_ns());
    }

    header->generation = cpu_to_le64(pending->tx.generation);
    ret = infilfs_rw_finalize_object(object);
    if (ret)
        goto finish;
    ret = infilfs_rw_tx_alloc(&pending->tx, 1, &new_block);
    if (ret)
        goto finish;
    ret = infilfs_native_stage_block(inode->i_sb, new_block, object);
    if (ret)
        goto finish;
    ret = infilfs_rw_tx_defer_free(&pending->tx, old_block, 1);
    if (ret)
        goto finish;

    memset(&change, 0, sizeof(change));
    memcpy(change.object_id, ii->object_id, sizeof(change.object_id));
    change.object_block = new_block;
    change.object_type = ii->object_type;
    change.action = INFILFS_NS_REPOINT;

    if (memcmp(ii->object_id, pending->tx.sbi->disk.root_object_id, 16) == 0)
        pending->tx.next_sb.root_object_block = cpu_to_le64(new_block);

    ret = infilfs_ns_rebuild_index(pending, &change, 1);

finish:
    kfree(object);
    ret = infilfs_ns_finish(pending, ret);
    if (!ret)
        ii->object_block = new_block;
    return ret;
}

/*
 * Finish the ownership semantics that inode_init_owner() would normally apply
 * during VFS creation.  The existing qualified create transaction has already
 * preserved the requested umask-filtered mode; we add the creator uid and the
 * correct fsgid/setgid-directory inheritance and persist them before returning
 * success to the VFS.
 */
static int infilfs_posix_fix_created(struct inode *parent,
                                     struct dentry *dentry,
                                     bool is_directory)
{
    struct inode *inode = d_inode(dentry);
    struct iattr attr;
    int ret;
    kuid_t creator_uid;
    kgid_t creator_gid;

    if (!inode)
        return -EIO;

    memset(&attr, 0, sizeof(attr));
    infilfs_posix_current_ids(&creator_uid, &creator_gid);
    attr.ia_valid = ATTR_UID | ATTR_GID | ATTR_CTIME;
    attr.ia_uid = creator_uid;
    attr.ia_gid = (parent->i_mode & S_ISGID) ? parent->i_gid : creator_gid;
    attr.ia_ctime = current_time(inode);

    if (is_directory && (parent->i_mode & S_ISGID)) {
        attr.ia_valid |= ATTR_MODE;
        attr.ia_mode = (inode->i_mode & 07777) | S_ISGID;
    }

    ret = infilfs_posix_rewrite_inode(inode, &attr);
    if (ret)
        return ret;

    inode->i_uid = attr.ia_uid;
    inode->i_gid = attr.ia_gid;
    if (attr.ia_valid & ATTR_MODE)
        inode->i_mode = (inode->i_mode & S_IFMT) | (attr.ia_mode & 07777);
    return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_create(struct mnt_idmap *idmap, struct inode *dir,
                                struct dentry *dentry, umode_t mode, bool excl)
#else
static int infilfs_posix_create(struct user_namespace *idmap,
                                struct inode *dir, struct dentry *dentry,
                                umode_t mode, bool excl)
#endif
{
    int ret = infilfs_rw_create_data(idmap, dir, dentry, mode, excl);

    if (ret)
        return ret;
    return infilfs_posix_fix_created(dir, dentry, false);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
static struct dentry *infilfs_posix_mkdir(struct mnt_idmap *idmap,
                                          struct inode *dir,
                                          struct dentry *dentry,
                                          umode_t mode)
{
    struct dentry *result = infilfs_rw_mkdir_data(idmap, dir, dentry, mode);
    struct dentry *created;
    int ret;

    if (IS_ERR(result))
        return result;
    created = result ? result : dentry;
    ret = infilfs_posix_fix_created(dir, created, true);
    if (ret)
        return ERR_PTR(ret);
    return result;
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_mkdir(struct mnt_idmap *idmap, struct inode *dir,
                               struct dentry *dentry, umode_t mode)
{
    int ret = infilfs_rw_mkdir_data(idmap, dir, dentry, mode);

    if (ret)
        return ret;
    return infilfs_posix_fix_created(dir, dentry, true);
}
#else
static int infilfs_posix_mkdir(struct user_namespace *idmap,
                               struct inode *dir, struct dentry *dentry,
                               umode_t mode)
{
    int ret = infilfs_rw_mkdir_data(idmap, dir, dentry, mode);

    if (ret)
        return ret;
    return infilfs_posix_fix_created(dir, dentry, true);
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_symlink(struct mnt_idmap *idmap, struct inode *dir,
                                 struct dentry *dentry, const char *target)
#else
static int infilfs_posix_symlink(struct user_namespace *idmap,
                                 struct inode *dir, struct dentry *dentry,
                                 const char *target)
#endif
{
    int ret = infilfs_ns_symlink(idmap, dir, dentry, target);

    if (ret)
        return ret;
    return infilfs_posix_fix_created(dir, dentry, false);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_setattr(struct mnt_idmap *idmap,
                                 struct dentry *dentry,
                                 struct iattr *attr)
#else
static int infilfs_posix_setattr(struct user_namespace *idmap,
                                 struct dentry *dentry,
                                 struct iattr *attr)
#endif
{
    struct inode *inode = d_inode(dentry);
    struct iattr metadata;
    unsigned int disk_fields;
    int ret;

    ret = setattr_prepare(idmap, dentry, attr);
    if (ret)
        return ret;
    metadata = *attr;

    /*
     * Size changes use the extent-aware native transaction.  Metadata fields
     * that accompany ATTR_SIZE are persisted immediately afterwards.
     */
    if (attr->ia_valid & ATTR_SIZE) {
        if (!S_ISREG(inode->i_mode))
            return -EISDIR;
        if (attr->ia_size < 0)
            return -EINVAL;
        ret = infilfs_native_truncate(inode, (u64)attr->ia_size);
        if (ret)
            return ret;
        metadata.ia_valid &= ~ATTR_SIZE;
    }

    disk_fields = metadata.ia_valid &
        (ATTR_MODE | ATTR_UID | ATTR_GID | ATTR_ATIME | ATTR_MTIME | ATTR_CTIME);
    if (!disk_fields)
        return 0;

    ret = infilfs_posix_rewrite_inode(inode, &metadata);
    if (ret)
        return ret;

    /*
     * Keep Linux's in-memory inode authoritative for permission checks between
     * metadata transactions.  getattr still reports the persisted timestamps
     * from disk, so both views agree after this copy.
     */
    setattr_copy(idmap, inode, &metadata);
    return 0;
}

/*
 * Native VFS metadata bridge.
 *
 * InfiltratorFS stores all four timestamps, including birth time, in its
 * portable on-disk attributes.  Report those authoritative values through
 * ->getattr so desktop file managers see creation time as well as POSIX times.
 */
static const struct infilfs_attributes_disk *
infilfs_vfs_attributes(const u8 object[INFILFS_DISK_BLOCK_SIZE])
{
    const struct infilfs_object_header_disk *header =
        (const struct infilfs_object_header_disk *)object;

    switch (le16_to_cpu(header->object_type)) {
    case INFILFS_OBJECT_DIRECTORY:
        return &((const struct infilfs_directory_payload_disk *)(header + 1))
                    ->attributes;
    case INFILFS_OBJECT_FILE:
        return &((const struct infilfs_file_payload_disk *)(header + 1))
                    ->attributes;
    case INFILFS_OBJECT_SYMLINK:
        return &((const struct infilfs_symlink_payload_disk *)(header + 1))
                    ->attributes;
    default:
        return NULL;
    }
}

static struct timespec64 infilfs_vfs_decode_time(__le64 encoded)
{
    return ns_to_timespec64((s64)le64_to_cpu(encoded));
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_getattr(struct mnt_idmap *idmap, const struct path *path,
                           struct kstat *stat, u32 request_mask,
                           unsigned int query_flags)
#else
static int infilfs_getattr(struct user_namespace *idmap,
                           const struct path *path, struct kstat *stat,
                           u32 request_mask, unsigned int query_flags)
#endif
{
    struct inode *inode = d_inode(path->dentry);
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    const struct infilfs_attributes_disk *attributes;
    u8 *object;
    int ret;

    (void)query_flags;
    if (!ii)
        return -EIO;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!object)
        return -ENOMEM;
    ret = infilfs_read_object(inode->i_sb, ii->object_block,
                              ii->object_type, ii->object_id, object);
    if (ret)
        goto out;
    attributes = infilfs_vfs_attributes(object);
    if (!attributes) {
        ret = -EOPNOTSUPP;
        goto out;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    generic_fillattr(idmap, request_mask, inode, stat);
#else
    generic_fillattr(idmap, inode, stat);
#endif
    stat->atime = infilfs_vfs_decode_time(attributes->access_time_ns);
    stat->mtime = infilfs_vfs_decode_time(attributes->modification_time_ns);
    stat->ctime = infilfs_vfs_decode_time(attributes->change_time_ns);
    stat->btime = infilfs_vfs_decode_time(attributes->birth_time_ns);
    stat->result_mask |= STATX_ATIME | STATX_MTIME | STATX_CTIME | STATX_BTIME;
    ret = 0;
out:
    kfree(object);
    return ret;
}

/*
 * Report real filesystem capacity to statfs(2).  simple_statfs() intentionally
 * leaves block accounting at zero, which made desktop file managers lose the
 * used/free-space pie chart after the switch from FUSE to the native VFS.
 */
static int infilfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
    struct super_block *sb = dentry->d_sb;
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    u64 total;
    u64 free_blocks;

    if (!sbi)
        return -EIO;
    total = le64_to_cpu(sbi->disk.total_blocks);
    free_blocks = le64_to_cpu(sbi->disk.free_blocks);
    if (!total || free_blocks > total)
        return -EFSCORRUPTED;

    memset(buf, 0, sizeof(*buf));
    buf->f_type = INFILTRATORFS_MAGIC;
    buf->f_bsize = INFILFS_DISK_BLOCK_SIZE;
    buf->f_frsize = INFILFS_DISK_BLOCK_SIZE;
    buf->f_blocks = total;
    buf->f_bfree = free_blocks;
    buf->f_bavail = free_blocks;
    /*
     * Object count has no fixed inode table; these are conservative upper
     * bounds used only for generic VFS reporting.
     */
    buf->f_files = total;
    buf->f_ffree = free_blocks;
    buf->f_namelen = INFILFS_NAME_MAX;
    return 0;
}

/*
 * infiltratorfs.c declares the operation tables immediately after including
 * this composition file.  Supply the final native callbacks here while keeping
 * that table compact.
 */
#define simple_statfs infilfs_statfs
#define infilfs_rw_create infilfs_posix_create
#define infilfs_rw_mkdir infilfs_posix_mkdir, \
    .unlink = infilfs_ns_unlink, \
    .rmdir = infilfs_ns_rmdir, \
    .link = infilfs_ns_link, \
    .symlink = infilfs_posix_symlink, \
    .rename = infilfs_ns_rename, \
    .setattr = infilfs_posix_setattr, \
    .getattr = infilfs_getattr
#define infilfs_rw_setattr infilfs_posix_setattr, .getattr = infilfs_getattr
#define infilfs_get_link infilfs_get_link, \
    .setattr = infilfs_posix_setattr, \
    .getattr = infilfs_getattr
