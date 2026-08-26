// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Native verified-read cursor cache.
 *
 * The integrity reader validates every data block against the SHA-256 chain.
 * The checksum object that contains a logical block is found by walking that
 * chain.  A read_iter() call already carries a local cursor while it copies one
 * request, but ordinary tools such as sha256sum issue many separate read_iter()
 * calls.  Resetting the cursor to the checksum head for every syscall turns a
 * sequential read into an increasingly expensive repeated-prefix walk.
 *
 * Reuse the existing bounded per-superblock/per-file checksum cache between
 * read syscalls.  The cache is only an acceleration hint: a cursor is accepted
 * only when it belongs to this superblock/object and is not ahead of the target
 * checksum group.  Backward/random reads therefore fall back to the checksum
 * head and preserve the same validation semantics.  Writers already refresh
 * this cache with the current checksum tail, and unmount invalidates it.
 */

static void infilfs_native_read_cursor_seed(
    struct super_block *sb, const u8 owner_id[16], u64 logical,
    struct infilfs_native_read_checksum_cursor *cursor)
{
    struct infilfs_native_checksum_cache_entry cached;
    u64 target = (logical / INFILFS_NATIVE_CHECKSUMS_PER_OBJECT) *
                 INFILFS_NATIVE_CHECKSUMS_PER_OBJECT;

    memset(cursor, 0, sizeof(*cursor));
    if (!infilfs_native_checksum_cache_lookup(sb, owner_id, &cached))
        return;
    if (cached.start_logical > target)
        return;

    memcpy(cursor->object_id, cached.object_id, sizeof(cursor->object_id));
    cursor->object_block = cached.object_block;
    cursor->start_logical = cached.start_logical;
    cursor->valid = true;
}

static void infilfs_native_read_cursor_publish(
    struct super_block *sb, const u8 owner_id[16],
    const struct infilfs_native_read_checksum_cursor *cursor)
{
    if (!cursor->valid)
        return;
    infilfs_native_checksum_cache_store(sb, owner_id, cursor->object_id,
                                        cursor->object_block,
                                        cursor->start_logical);
}

static ssize_t infilfs_file_read_iter_cached(struct kiocb *iocb,
                                              struct iov_iter *to)
{
    struct file *filep = iocb->ki_filp;
    struct inode *inode = file_inode(filep);
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    const struct infilfs_object_header_disk *header;
    const struct infilfs_file_payload_disk *file;
    struct infilfs_native_read_checksum_cursor cursor = {0};
    loff_t pos = iocb->ki_pos;
    u64 file_size;
    size_t requested, done = 0;
    u8 *object = NULL, *data_block = NULL, *checksum_object = NULL;
    int ret;

    if (pos < 0)
        return -EINVAL;
    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    data_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    checksum_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!object || !data_block || !checksum_object) {
        ret = -ENOMEM;
        goto out;
    }
    ret = infilfs_read_object(inode->i_sb, ii->object_block,
                              INFILFS_OBJECT_FILE, ii->object_id, object);
    if (ret)
        goto out;
    header = (const struct infilfs_object_header_disk *)object;
    file = (const struct infilfs_file_payload_disk *)(header + 1);
    file_size = le64_to_cpu(file->attributes.logical_size);
    if (le32_to_cpu(file->data_checksum_type) != INFILFS_CHECKSUM_SHA256) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    if ((u64)pos >= file_size) {
        ret = 0;
        goto out;
    }
    requested = min_t(u64, iov_iter_count(to), file_size - (u64)pos);

    if (le32_to_cpu(file->extent_count) == 0 &&
        file_size <= INFILFS_INLINE_DATA_MAX) {
        const struct infilfs_data_checksum_disk *expected;
        const u8 *inline_bytes;
        u8 actual[32];
        size_t need, copied;

        if (!file_size) {
            ret = 0;
            goto out;
        }
        expected = (const struct infilfs_data_checksum_disk *)(file + 1);
        inline_bytes = (const u8 *)(expected + 1);
        need = sizeof(*file) + sizeof(*expected) + (size_t)file_size;
        if (need > le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        ret = infilfs_rw_inline_digest(inline_bytes, (size_t)file_size, actual);
        if (ret || memcmp(expected->bytes, actual, sizeof(actual)) != 0) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        copied = copy_to_iter(inline_bytes + pos, requested, to);
        iocb->ki_pos += copied;
        ret = copied;
        goto out;
    }

    /*
     * Carry the last validated checksum object across read syscalls.  This is
     * the critical difference from the original verified reader: sequential
     * readers resume near their current logical position instead of walking
     * the checksum chain from its head for every userspace read().
     */
    infilfs_native_read_cursor_seed(inode->i_sb, ii->object_id,
                                    (u64)pos >> INFILFS_DISK_BLOCK_SHIFT,
                                    &cursor);

    while (done < requested) {
        u64 absolute = (u64)pos + done;
        u64 logical = absolute >> INFILFS_DISK_BLOCK_SHIFT;
        size_t within = absolute & (INFILFS_DISK_BLOCK_SIZE - 1u);
        size_t chunk = min_t(size_t, INFILFS_DISK_BLOCK_SIZE - within,
                             requested - done);
        u64 physical = 0;
        u32 flags = 0;
        size_t copied;

        ret = infilfs_map_file_block(inode, object, logical, &physical, &flags);
        if (ret)
            goto partial;
        if (flags == INFILFS_EXTENT_HOLE) {
            memset(data_block, 0, INFILFS_DISK_BLOCK_SIZE);
        } else if (flags == INFILFS_EXTENT_NORMAL) {
            struct infilfs_data_checksum_disk expected, actual;

            ret = infilfs_read_block(inode->i_sb, physical, data_block);
            if (ret)
                goto partial;
            ret = infilfs_native_read_expected_digest(
                inode->i_sb, ii->object_id, file->checksum_head_id, logical,
                &cursor, checksum_object, &expected);
            if (ret)
                goto partial;
            infilfs_native_block_digest(data_block, &actual);
            if (memcmp(&expected, &actual, sizeof(expected)) != 0) {
                ret = -EFSCORRUPTED;
                goto partial;
            }
        } else {
            ret = -EFSCORRUPTED;
            goto partial;
        }
        copied = copy_to_iter(data_block + within, chunk, to);
        done += copied;
        if (copied != chunk)
            break;
    }
    iocb->ki_pos += done;
    ret = done;
    goto out;

partial:
    if (done) {
        iocb->ki_pos += done;
        ret = done;
    }
out:
    if (ii)
        infilfs_native_read_cursor_publish(inode->i_sb, ii->object_id, &cursor);
    kfree(checksum_object);
    kfree(data_block);
    kfree(object);
    return ret;
}
