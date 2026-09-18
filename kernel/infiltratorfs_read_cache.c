// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"
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

#define INFILFS_NATIVE_READAHEAD_BLOCKS 256u
#define INFILFS_NATIVE_READ_HASH_MIN_BLOCKS 16u

struct infilfs_native_read_hash_work {
    struct work_struct work;
    struct completion done;
    const u8 *data;
    struct infilfs_data_checksum_disk *digests;
    u32 first;
    u32 count;
};

static void infilfs_native_read_hash_workfn(struct work_struct *work)
{
    struct infilfs_native_read_hash_work *item = container_of(
        work, struct infilfs_native_read_hash_work, work);
    u32 i;

    infilfs_cpu_work_enter();
    for (i = 0; i < item->count; ++i)
        infilfs_native_block_digest(
            item->data +
                (size_t)(item->first + i) * INFILFS_DISK_BLOCK_SIZE,
            &item->digests[item->first + i]);
    infilfs_cpu_work_exit();
    complete(&item->done);
}

static void infilfs_native_digest_run(
    const u8 *data, u32 blocks,
    struct infilfs_data_checksum_disk *digests)
{
    struct infilfs_native_read_hash_work *work;
    u32 workers;
    u32 first = 0;
    u32 i;

    if (blocks < INFILFS_NATIVE_READ_HASH_MIN_BLOCKS ||
        infilfs_cpu_budget() <= 1u) {
        for (i = 0; i < blocks; ++i)
            infilfs_native_block_digest(
                data + (size_t)i * INFILFS_DISK_BLOCK_SIZE,
                &digests[i]);
        return;
    }

    workers = min_t(u32, infilfs_cpu_budget(), blocks);
    work = kcalloc(workers, sizeof(*work), GFP_NOFS);
    if (!work) {
        for (i = 0; i < blocks; ++i)
            infilfs_native_block_digest(
                data + (size_t)i * INFILFS_DISK_BLOCK_SIZE,
                &digests[i]);
        return;
    }

    for (i = 0; i < workers; ++i) {
        u32 remaining = blocks - first;
        u32 slots = workers - i;
        u32 count = DIV_ROUND_UP(remaining, slots);

        INIT_WORK(&work[i].work, infilfs_native_read_hash_workfn);
        init_completion(&work[i].done);
        work[i].data = data;
        work[i].digests = digests;
        work[i].first = first;
        work[i].count = count;
        if (!infilfs_queue_cpu_work(&work[i].work)) {
            u32 j;

            for (j = 0; j < count; ++j)
                infilfs_native_block_digest(
                    data + (size_t)(first + j) * INFILFS_DISK_BLOCK_SIZE,
                    &digests[first + j]);
            complete(&work[i].done);
        }
        first += count;
    }
    for (i = 0; i < workers; ++i)
        wait_for_completion(&work[i].done);
    kfree(work);
}

static void infilfs_native_readahead_extent(
    struct super_block *sb, u64 physical, u64 logical,
    u64 extent_logical, u32 extent_blocks, u64 *next_logical)
{
    struct infilfs_sb_info *sbi;
    u64 extent_end;
    u64 blocks;
    u64 i;

    if (!sb || !next_logical)
        return;
    sbi = INFILFS_SB(sb);
    if (!sbi || logical < *next_logical ||
        physical >= infilfs_volume_blocks(sbi) ||
        extent_logical > U64_MAX - extent_blocks)
        return;
    extent_end = extent_logical + extent_blocks;
    if (logical >= extent_end)
        return;
    blocks = min_t(u64, extent_end - logical,
                   INFILFS_NATIVE_READAHEAD_BLOCKS);
    blocks = min_t(u64, blocks,
                   infilfs_volume_blocks(sbi) - physical);

    /*
     * sb_bread() waits for one 4 KiB buffer at a time.  Seed a bounded run of
     * contiguous extent blocks before waiting for the first one so the block
     * layer can merge and queue sequential reads.  Checksum verification is
     * unchanged and still occurs before any byte reaches userspace.
     */
    for (i = 0; i < blocks; ++i)
        sb_breadahead(sb, (sector_t)(physical + i));
    *next_logical = logical + blocks;
}

static int infilfs_native_map_file_block_cached(
    struct inode *inode, const u8 *object, u64 logical,
    u8 extent_page[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_native_read_extent_cursor *cursor,
    u64 *physical_out, u32 *flags_out,
    u64 *extent_logical_out, u32 *extent_blocks_out)
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
        return infilfs_map_file_block_detail(
            inode, object, logical, physical_out, flags_out,
            extent_logical_out, extent_blocks_out);

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
        if (!infilfs_extent_flags_valid(
                blocks, le64_to_cpu(ext[mid].physical_block), flags))
            return -EFSCORRUPTED;
        *flags_out = flags;
        if (infilfs_extent_kind(flags) == INFILFS_EXTENT_HOLE)
            *physical_out = 0;
        else if (infilfs_extent_is_compressed(flags))
            *physical_out = le64_to_cpu(ext[mid].physical_block);
        else
            *physical_out =
                le64_to_cpu(ext[mid].physical_block) + (logical - start);
        if (extent_logical_out)
            *extent_logical_out = start;
        if (extent_blocks_out)
            *extent_blocks_out = blocks;
        return 0;
    }
    return -EFSCORRUPTED;
}

/*
 * infilfs_native_read_expected_digest() authenticates and decodes the checksum
 * metadata object that contains a logical block.  A normal read_iter() then
 * consumes several neighbouring 4 KiB blocks from that same checksum object.
 * Re-reading and re-validating the identical metadata block for every data
 * block is pure duplicate work and was enough to make large verified reads hit
 * the qualification timeout.
 *
 * Keep the already-authenticated checksum object resident for the remainder of
 * this read_iter() call.  Every data block is still SHA-256 hashed and compared
 * with its stored digest; the optimisation removes only repeated metadata I/O
 * and validation for a checksum group that has already been authenticated.
 */
static int infilfs_native_read_expected_digest_cached(
    struct super_block *sb, const u8 owner_id[16], const u8 head_id[16],
    u64 logical, struct infilfs_native_read_checksum_cursor *cursor,
    u8 object[INFILFS_DISK_BLOCK_SIZE],
    u64 *loaded_start, u32 *loaded_count, bool *loaded_valid,
    struct infilfs_data_checksum_disk *expected)
{
    const struct infilfs_object_header_disk *header;
    const struct infilfs_native_checksum_payload_disk *payload;
    const struct infilfs_data_checksum_disk *values;
    u64 target = (logical / INFILFS_NATIVE_CHECKSUMS_PER_OBJECT) *
                 INFILFS_NATIVE_CHECKSUMS_PER_OBJECT;
    u64 offset;
    u32 count;
    int ret;

    if (*loaded_valid && *loaded_start == target) {
        if (logical < target)
            return -EFSCORRUPTED;
        offset = logical - target;
        if (offset >= *loaded_count)
            return -EFSCORRUPTED;
        header = (const struct infilfs_object_header_disk *)object;
        payload = (const struct infilfs_native_checksum_payload_disk *)(header + 1);
        values = (const struct infilfs_data_checksum_disk *)(payload + 1);
        *expected = values[offset];
        return 0;
    }

    ret = infilfs_native_read_expected_digest(
        sb, owner_id, head_id, logical, cursor, object, expected);
    if (ret) {
        *loaded_valid = false;
        return ret;
    }

    header = (const struct infilfs_object_header_disk *)object;
    payload = (const struct infilfs_native_checksum_payload_disk *)(header + 1);
    count = le32_to_cpu(payload->checksum_count);
    if (!cursor->valid || cursor->start_logical != target ||
        le64_to_cpu(payload->start_logical_block) != target || !count ||
        count > INFILFS_NATIVE_CHECKSUMS_PER_OBJECT) {
        *loaded_valid = false;
        return -EFSCORRUPTED;
    }

    *loaded_start = target;
    *loaded_count = count;
    *loaded_valid = true;
    return 0;
}

ssize_t infilfs_native_read_iter_cached(struct inode *inode,
                                                loff_t *position,
                                                struct iov_iter *to)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(inode->i_sb);
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    const struct infilfs_object_header_disk *header;
    const struct infilfs_file_payload_disk *file;
    struct infilfs_native_read_checksum_cursor cursor = {0};
    struct infilfs_native_read_extent_cursor extent_cursor = {0};
    loff_t pos = *position;
    u64 file_size;
    size_t requested, done = 0;
    u8 *object = NULL, *data_block = NULL, *checksum_object = NULL;
    u8 *extent_page = NULL, *compressed_plain = NULL;
    u8 *run_data = NULL;
    struct infilfs_data_checksum_disk *run_expected = NULL;
    struct infilfs_data_checksum_disk *run_actual = NULL;
    size_t compressed_capacity = 0;
    u64 compressed_physical = 0, compressed_logical = 0;
    u32 compressed_blocks = 0, compressed_flags = 0;
    bool compressed_valid = false;
    u64 checksum_loaded_start = 0;
    u32 checksum_loaded_count = 0;
    bool checksum_loaded_valid = false;
    u64 readahead_next_logical = 0;
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

    /*
     * Deferred native writers temporarily expose transaction-private object
     * index and checksum topology through sbi->disk while holding write_lock.
     * A verified reader must not combine an inode/file object from one topology
     * with checksum/index pages from another. Lookup and readdir already use
     * this same lock for multi-block topology walks; do the same for one read
     * syscall. The lock is released before returning to userspace, so ordinary
     * sequential readers yield between syscalls rather than monopolising the
     * writer lock for the lifetime of the open file.
     */
    down_read(&sbi->write_lock);
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
        const struct infilfs_data_checksum_disk *expected_inline;
        const u8 *inline_bytes;
        u8 actual_inline[32];
        size_t need, copied;

        if (!file_size) {
            ret = 0;
            goto out;
        }
        expected_inline =
            (const struct infilfs_data_checksum_disk *)(file + 1);
        inline_bytes = (const u8 *)(expected_inline + 1);
        need = sizeof(*file) + sizeof(*expected_inline) + (size_t)file_size;
        if (need > le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        ret = infilfs_rw_inline_digest(
            inline_bytes, (size_t)file_size, actual_inline);
        if (ret || memcmp(expected_inline->bytes, actual_inline,
                          sizeof(actual_inline)) != 0) {
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
        u64 extent_logical = 0;
        u32 extent_blocks = 0;
        u32 flags = 0;
        size_t copied;

        ret = infilfs_native_map_file_block_cached(
            inode, object, logical, extent_page, &extent_cursor,
            &physical, &flags, &extent_logical, &extent_blocks);
        if (ret)
            goto partial;
        if (infilfs_extent_kind(flags) == INFILFS_EXTENT_HOLE) {
            memset(data_block, 0, INFILFS_DISK_BLOCK_SIZE);
        } else if (infilfs_extent_kind(flags) == INFILFS_EXTENT_NORMAL) {
            struct infilfs_data_checksum_disk expected, actual;
            u64 extent_end = extent_logical + extent_blocks;
            u64 available_blocks = extent_end > logical ?
                extent_end - logical : 0;
            u32 run_blocks = 0;

            if (within == 0 &&
                requested - done >= 2u * INFILFS_DISK_BLOCK_SIZE &&
                available_blocks >= 2u) {
                run_blocks = min_t(
                    u64, available_blocks,
                    (requested - done) >> INFILFS_DISK_BLOCK_SHIFT);
                run_blocks = min_t(
                    u32, run_blocks, INFILFS_NATIVE_READAHEAD_BLOCKS);
            }

            if (infilfs_extent_is_compressed(flags)) {
                size_t plain_bytes =
                    (size_t)extent_blocks * INFILFS_DISK_BLOCK_SIZE;

                if (!compressed_valid ||
                    compressed_physical != physical ||
                    compressed_logical != extent_logical ||
                    compressed_blocks != extent_blocks ||
                    compressed_flags != flags) {
                    if (compressed_capacity < plain_bytes) {
                        kvfree(compressed_plain);
                        compressed_plain = kvmalloc(plain_bytes, GFP_NOFS);
                        if (!compressed_plain) {
                            compressed_capacity = 0;
                            ret = -ENOMEM;
                            goto partial;
                        }
                        compressed_capacity = plain_bytes;
                    }

                    ret = infilfs_read_compressed_extent(
                        inode, physical, extent_blocks, flags,
                        compressed_plain, compressed_capacity);
                    if (ret)
                        goto partial;

                    compressed_physical = physical;
                    compressed_logical = extent_logical;
                    compressed_blocks = extent_blocks;
                    compressed_flags = flags;
                    compressed_valid = true;
                }

                if (run_blocks >= 2u) {
                    const u8 *run_source =
                        compressed_plain +
                        (size_t)(logical - extent_logical) *
                            INFILFS_DISK_BLOCK_SIZE;
                    u32 j;

                    if (!run_expected)
                        run_expected = kvmalloc_array(
                            INFILFS_NATIVE_READAHEAD_BLOCKS,
                            sizeof(*run_expected), GFP_NOFS);
                    if (!run_actual)
                        run_actual = kvmalloc_array(
                            INFILFS_NATIVE_READAHEAD_BLOCKS,
                            sizeof(*run_actual), GFP_NOFS);
                    if (!run_expected || !run_actual) {
                        ret = -ENOMEM;
                        goto partial;
                    }

                    for (j = 0; j < run_blocks; ++j) {
                        ret = infilfs_native_read_expected_digest_cached(
                            inode->i_sb, ii->object_id,
                            file->checksum_head_id, logical + j,
                            &cursor, checksum_object,
                            &checksum_loaded_start,
                            &checksum_loaded_count,
                            &checksum_loaded_valid,
                            &run_expected[j]);
                        if (ret)
                            goto partial;
                    }
                    infilfs_native_digest_run(
                        run_source, run_blocks, run_actual);
                    for (j = 0; j < run_blocks; ++j) {
                        if (memcmp(&run_expected[j], &run_actual[j],
                                   sizeof(run_expected[j])) != 0) {
                            ret = -EFSCORRUPTED;
                            goto partial;
                        }
                    }
                    {
                        size_t run_bytes =
                            (size_t)run_blocks *
                            INFILFS_DISK_BLOCK_SIZE;
                        size_t run_copied =
                            copy_to_iter(run_source, run_bytes, to);

                        done += run_copied;
                        if (run_copied != run_bytes)
                            break;
                    }
                    continue;
                }

                memcpy(data_block,
                       compressed_plain +
                           (size_t)(logical - extent_logical) *
                               INFILFS_DISK_BLOCK_SIZE,
                       INFILFS_DISK_BLOCK_SIZE);
            } else {
                infilfs_native_readahead_extent(
                    inode->i_sb, physical, logical, extent_logical,
                    extent_blocks, &readahead_next_logical);

                if (run_blocks >= 2u) {
                    u32 j;

                    if (!run_data)
                        run_data = kvmalloc(
                            (size_t)INFILFS_NATIVE_READAHEAD_BLOCKS *
                            INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
                    if (!run_expected)
                        run_expected = kvmalloc_array(
                            INFILFS_NATIVE_READAHEAD_BLOCKS,
                            sizeof(*run_expected), GFP_NOFS);
                    if (!run_actual)
                        run_actual = kvmalloc_array(
                            INFILFS_NATIVE_READAHEAD_BLOCKS,
                            sizeof(*run_actual), GFP_NOFS);
                    if (!run_data || !run_expected || !run_actual) {
                        ret = -ENOMEM;
                        goto partial;
                    }

                    ret = infilfs_read_allocated_blocks(
                        inode->i_sb, physical, run_blocks, run_data);
                    if (ret)
                        goto partial;
                    for (j = 0; j < run_blocks; ++j) {
                        ret = infilfs_native_read_expected_digest_cached(
                            inode->i_sb, ii->object_id,
                            file->checksum_head_id, logical + j,
                            &cursor, checksum_object,
                            &checksum_loaded_start,
                            &checksum_loaded_count,
                            &checksum_loaded_valid,
                            &run_expected[j]);
                        if (ret)
                            goto partial;
                    }
                    infilfs_native_digest_run(
                        run_data, run_blocks, run_actual);
                    for (j = 0; j < run_blocks; ++j) {
                        if (memcmp(&run_expected[j], &run_actual[j],
                                   sizeof(run_expected[j])) != 0) {
                            ret = -EFSCORRUPTED;
                            goto partial;
                        }
                    }
                    {
                        size_t run_bytes =
                            (size_t)run_blocks *
                            INFILFS_DISK_BLOCK_SIZE;
                        size_t run_copied =
                            copy_to_iter(run_data, run_bytes, to);

                        done += run_copied;
                        if (run_copied != run_bytes)
                            break;
                    }
                    continue;
                }

                ret = infilfs_read_allocated_block(
                    inode->i_sb, physical, data_block);
            }
            if (ret)
                goto partial;
            ret = infilfs_native_read_expected_digest_cached(
                inode->i_sb, ii->object_id, file->checksum_head_id, logical,
                &cursor, checksum_object, &checksum_loaded_start,
                &checksum_loaded_count, &checksum_loaded_valid, &expected);
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
    if (object && data_block && checksum_object && extent_page) {
        if (ii)
            infilfs_native_read_cursor_publish(
                inode->i_sb, ii->object_id, &cursor);
        up_read(&sbi->write_lock);
    }
    kvfree(run_actual);
    kvfree(run_expected);
    kvfree(run_data);
    kvfree(compressed_plain);
    kfree(extent_page);
    kfree(checksum_object);
    kfree(data_block);
    kfree(object);
    return ret;
}

ssize_t infilfs_file_read_iter_cached(struct kiocb *iocb,
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