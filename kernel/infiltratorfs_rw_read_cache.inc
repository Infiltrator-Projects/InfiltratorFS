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


struct infilfs_native_read_extent_cursor {
    u32 page_index;
    u64 first_logical;
    u64 last_logical;
    bool valid;
};

static int infilfs_native_map_file_block_cached(
    struct inode *inode, const u8 *object, u64 logical,
    u8 extent_page[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_native_read_extent_cursor *cursor,
    u64 *physical_out, u32 *flags_out)
{
    const struct infilfs_object_header_disk *header =
        (const struct infilfs_object_header_disk *)object;
    const struct infilfs_file_payload_disk *file =
        (const struct infilfs_file_payload_disk *)(header + 1);
    const struct infilfs_extent_head_disk *head;
    const __le64 *pages;
    const struct infilfs_metadata_page_disk *page;
    const struct infilfs_extent_disk *ext;
    u16 version = le16_to_cpu(header->object_version);
    u32 page_count;
    u32 count;
    u32 lo;
    u32 hi;
    int ret;

    if (version != INFILFS_OBJECT_VERSION_PAGED)
        return infilfs_map_file_block(
            inode, object, logical, physical_out, flags_out);

    head = (const struct infilfs_extent_head_disk *)(file + 1);
    pages = (const __le64 *)(head + 1);
    page_count = le32_to_cpu(head->page_count);
    if (!page_count || page_count > INFILFS_EXTENT_PAGE_POINTERS ||
        le32_to_cpu(head->reserved) != 0 ||
        sizeof(*file) + sizeof(*head) +
            (size_t)page_count * sizeof(*pages) !=
                le32_to_cpu(header->payload_size))
        return -EFSCORRUPTED;

    /*
     * Sequential reads normally stay inside one extent page for many data
     * blocks.  Keep that already-authenticated page resident for this
     * read_iter() call instead of rereading it for every 4 KiB block.
     * Random/backward access falls through to the binary page search.
     */
    if (!cursor->valid ||
        logical < cursor->first_logical ||
        logical >= cursor->last_logical) {
        lo = 0;
        hi = page_count;
        cursor->valid = false;

        while (lo < hi) {
            u32 mid = lo + (hi - lo) / 2u;
            u64 first;
            u64 last;

            ret = infilfs_read_allocated_block(
                inode->i_sb, le64_to_cpu(pages[mid]), extent_page);
            if (ret)
                return ret;
            if (!infilfs_metadata_page_valid(
                    inode->i_sb, extent_page, infilfs_extent_page_magic,
                    header->object_id))
                return -EFSCORRUPTED;

            page = (const struct infilfs_metadata_page_disk *)extent_page;
            count = le32_to_cpu(page->entry_count);
            if (!count || count > INFILFS_EXTENTS_PER_PAGE ||
                le32_to_cpu(page->bytes_used) !=
                    count * sizeof(struct infilfs_extent_disk))
                return -EFSCORRUPTED;
            ext = (const struct infilfs_extent_disk *)(page + 1);
            first = le64_to_cpu(ext[0].logical_block);
            last = le64_to_cpu(ext[count - 1u].logical_block) +
                le32_to_cpu(ext[count - 1u].block_count);
            if (last <= first)
                return -EFSCORRUPTED;

            if (logical < first) {
                hi = mid;
            } else if (logical >= last) {
                lo = mid + 1u;
            } else {
                cursor->page_index = mid;
                cursor->first_logical = first;
                cursor->last_logical = last;
                cursor->valid = true;
                break;
            }
        }
        if (!cursor->valid)
            return -EFSCORRUPTED;
    }

    page = (const struct infilfs_metadata_page_disk *)extent_page;
    count = le32_to_cpu(page->entry_count);
    ext = (const struct infilfs_extent_disk *)(page + 1);
    lo = 0;
    hi = count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        u64 start = le64_to_cpu(ext[mid].logical_block);
        u32 blocks = le32_to_cpu(ext[mid].block_count);
        u32 flags = le32_to_cpu(ext[mid].flags);
        u64 end;

        if (!blocks || start > U64_MAX - blocks)
            return -EFSCORRUPTED;
        end = start + blocks;
        if (logical < start) {
            hi = mid;
            continue;
        }
        if (logical >= end) {
            lo = mid + 1u;
            continue;
        }
        if (flags != INFILFS_EXTENT_NORMAL &&
            flags != INFILFS_EXTENT_HOLE)
            return -EFSCORRUPTED;
        *flags_out = flags;
        *physical_out = flags == INFILFS_EXTENT_HOLE ? 0 :
            le64_to_cpu(ext[mid].physical_block) + (logical - start);
        return 0;
    }
    return -EFSCORRUPTED;
}

static ssize_t infilfs_native_read_iter_cached(struct inode *inode,
                                                loff_t *position,
                                                struct iov_iter *to)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    const struct infilfs_object_header_disk *header;
    const struct infilfs_file_payload_disk *file;
    struct infilfs_native_read_checksum_cursor cursor = {0};
    struct infilfs_native_read_extent_cursor extent_cursor = {0};
    loff_t pos = *position;
    u64 file_size;
    size_t requested, done = 0;
    u8 *object = NULL, *data_block = NULL, *checksum_object = NULL;
    u8 *extent_page = NULL;
    int ret;

    if (pos < 0)
        return -EINVAL;
    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    data_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    checksum_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    extent_page = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!object || !data_block || !checksum_object || !extent_page) {
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
        *position += copied;
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

        ret = infilfs_native_map_file_block_cached(
            inode, object, logical, extent_page, &extent_cursor,
            &physical, &flags);
        if (ret)
            goto partial;
        if (flags == INFILFS_EXTENT_HOLE) {
            memset(data_block, 0, INFILFS_DISK_BLOCK_SIZE);
        } else if (flags == INFILFS_EXTENT_NORMAL) {
            struct infilfs_data_checksum_disk expected, actual;

            ret = infilfs_read_allocated_block(
                inode->i_sb, physical, data_block);
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
    *position += done;
    ret = done;
    goto out;

partial:
    if (done) {
        *position += done;
        ret = done;
    }
out:
    if (ii)
        infilfs_native_read_cursor_publish(inode->i_sb, ii->object_id, &cursor);
    kfree(extent_page);
    kfree(checksum_object);
    kfree(data_block);
    kfree(object);
    return ret;
}

static ssize_t infilfs_file_read_iter_cached(struct kiocb *iocb,
                                              struct iov_iter *to)
{
    struct file *file = iocb->ki_filp;
    size_t count = iov_iter_count(to);
    int ret;

    if (count) {
        loff_t end;

        if (iocb->ki_pos < 0)
            return -EINVAL;
        end = count - 1u > LLONG_MAX - iocb->ki_pos ?
            LLONG_MAX : iocb->ki_pos + count - 1u;
        ret = file_write_and_wait_range(file, iocb->ki_pos, end);
        if (ret)
            return ret;
    }
    return infilfs_native_read_iter_cached(file_inode(file),
                                            &iocb->ki_pos, to);
}
