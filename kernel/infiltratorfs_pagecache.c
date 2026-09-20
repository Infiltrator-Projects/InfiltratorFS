// SPDX-License-Identifier: GPL-3.0-or-later
#include <linux/bvec.h>

#include "infiltratorfs_internal.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0) && IS_ENABLED(CONFIG_IOMAP)
#include <linux/iomap.h>
#define INFILFS_HAVE_VERIFIED_IOMAP_READ 1
#else
#define INFILFS_HAVE_VERIFIED_IOMAP_READ 0
#endif
/*
 * Native Linux page-cache integration.
 *
 * Ordinary read(2)/write(2), mmap and readahead share the page cache. Dirty
 * folios are coalesced into bounded contiguous batches and handed to the
 * verified CoW writer; fsync/sync/unmount remain publication/durability
 * boundaries. This keeps codec/integrity work out of the application syscall.
 */

/*
 * Dirty folios have not reached the physical allocator yet. Account their
 * uncompressed CoW demand exactly once until successful writeback or discard.
 * This is a conservative statfs estimate, not an allocation reservation: the
 * verified writer continues to own physical admission and compression savings.
 *
 * dirty_folio may run without the folio lock (under a page-table lock), so the
 * private marker and counter share a short superblock spinlock. The only nested
 * locks are those taken by filemap_dirty_folio; never acquire this lock while
 * holding mapping->i_pages or inode->i_lock.
 */
static bool infilfs_dirty_folio(struct address_space *mapping,
                              struct folio *folio)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(mapping->host->i_sb);
    unsigned long flags;
    bool changed;

    spin_lock_irqsave(&sbi->pagecache_accounting_lock, flags);
    /*
     * PG_private is intentionally left to the page-cache framework. Modern
     * iomap uses folio->private for its per-folio state, so InfiltratorFS
     * tracks only its independent pending-CoW accounting bit in PG_private_2.
     */
    if (!folio_test_private_2(folio)) {
        folio_set_private_2(folio);
        atomic64_add(folio_size(folio) >> INFILFS_DISK_BLOCK_SHIFT,
                     &sbi->pagecache_pending_blocks);
    }
    changed = filemap_dirty_folio(mapping, folio);
    spin_unlock_irqrestore(&sbi->pagecache_accounting_lock, flags);
    return changed;
}

static void infilfs_pagecache_unaccount(struct folio *folio, bool discard)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(folio->mapping->host->i_sb);
    unsigned long flags;

    spin_lock_irqsave(&sbi->pagecache_accounting_lock, flags);
    /* A concurrent mmap dirtying must retain the next writeback's demand. */
    if (folio_test_private_2(folio) &&
        (discard || !folio_test_dirty(folio))) {
        atomic64_sub(folio_size(folio) >> INFILFS_DISK_BLOCK_SHIFT,
                     &sbi->pagecache_pending_blocks);
        folio_clear_private_2(folio);
    }
    spin_unlock_irqrestore(&sbi->pagecache_accounting_lock, flags);
}

static void infilfs_invalidate_folio(struct folio *folio, size_t offset,
                                   size_t length)
{
    if (!offset && length == folio_size(folio))
        infilfs_pagecache_unaccount(folio, true);
#if INFILFS_HAVE_VERIFIED_IOMAP_READ
    iomap_invalidate_folio(folio, offset, length);
#endif
}

static bool infilfs_release_folio(struct folio *folio, gfp_t gfp)
{
    (void)gfp;
    if (folio_test_dirty(folio) || folio_test_writeback(folio))
        return false;
    infilfs_pagecache_unaccount(folio, false);
#if INFILFS_HAVE_VERIFIED_IOMAP_READ
    if (!iomap_release_folio(folio, gfp))
        return false;
#endif
    return !folio_test_private_2(folio);
}

struct infilfs_pagecache_write_ctx {
    struct infilfs_quota_reservation quota;
    loff_t old_size;
};

struct infilfs_writeback_cluster {
    struct bio_vec *bvecs;
    struct folio **folios;
    unsigned int count;
    unsigned int capacity;
    unsigned int bvec_count;
    unsigned int bvec_capacity;
    loff_t position;
    size_t bytes;
};

struct infilfs_readahead_cluster {
    struct bio_vec *bvecs;
    struct folio **folios;
    unsigned int count;
    unsigned int capacity;
    unsigned int bvec_count;
    unsigned int bvec_capacity;
    loff_t position;
    size_t bytes;
};

#if INFILFS_HAVE_VERIFIED_IOMAP_READ
struct infilfs_iomap_read_range {
    struct folio *folio;
    size_t offset;
    size_t length;
};

struct infilfs_iomap_read_batch {
    struct inode *inode;
    struct bio_vec *bvecs;
    struct infilfs_iomap_read_range *ranges;
    unsigned int bvec_count;
    unsigned int bvec_capacity;
    unsigned int range_count;
    unsigned int range_capacity;
    loff_t position;
    size_t bytes;
    int error;
};
#endif

static int infilfs_pagecache_append_bvecs(
    struct bio_vec *bvecs, unsigned int capacity, unsigned int *count,
    struct folio *folio, size_t bytes)
{
    unsigned int needed;
    size_t offset = 0;

    if (!bvecs || !count || !folio || bytes > folio_size(folio))
        return -EINVAL;
    if (!bytes)
        return 0;

    needed = DIV_ROUND_UP(bytes, PAGE_SIZE);
    if (*count > capacity || needed > capacity - *count)
        return -EOVERFLOW;

    while (offset < bytes) {
        size_t chunk = min_t(size_t, PAGE_SIZE, bytes - offset);
        struct bio_vec *bvec = &bvecs[*count];

        bvec->bv_page = folio_page(folio, offset >> PAGE_SHIFT);
        bvec->bv_offset = 0;
        bvec->bv_len = chunk;
        (*count)++;
        offset += chunk;
    }
    return 0;
}

#if INFILFS_HAVE_VERIFIED_IOMAP_READ
static int infilfs_pagecache_append_bvec_range(
    struct bio_vec *bvecs, unsigned int capacity, unsigned int *count,
    struct folio *folio, size_t offset, size_t bytes)
{
    unsigned int needed;
    size_t end;

    if (!bvecs || !count || !folio || offset > folio_size(folio) ||
        bytes > folio_size(folio) - offset)
        return -EINVAL;
    if (!bytes)
        return 0;

    end = offset + bytes;
    needed = DIV_ROUND_UP((offset & (PAGE_SIZE - 1u)) + bytes, PAGE_SIZE);
    if (*count > capacity || needed > capacity - *count)
        return -EOVERFLOW;

    while (offset < end) {
        size_t page_offset = offset & (PAGE_SIZE - 1u);
        size_t chunk = min_t(size_t, PAGE_SIZE - page_offset, end - offset);
        struct bio_vec *bvec = &bvecs[*count];

        bvec->bv_page = folio_page(folio, offset >> PAGE_SHIFT);
        bvec->bv_offset = page_offset;
        bvec->bv_len = chunk;
        (*count)++;
        offset += chunk;
    }
    return 0;
}
#endif

static void infilfs_pagecache_zero_folio(
    struct folio *folio, size_t offset, size_t bytes)
{
    while (bytes) {
        size_t page_offset = offset & (PAGE_SIZE - 1u);
        size_t chunk = min_t(size_t, PAGE_SIZE - page_offset, bytes);
        struct page *page = folio_page(folio, offset >> PAGE_SHIFT);
        void *address = kmap_local_page(page);

        memset((u8 *)address + page_offset, 0, chunk);
        kunmap_local(address);
        offset += chunk;
        bytes -= chunk;
    }
}

static void infilfs_readahead_zero_tail(
    struct infilfs_readahead_cluster *cluster, size_t wanted)
{
    unsigned int i;
    size_t keep = wanted;

    for (i = 0; i < cluster->count; ++i) {
        struct folio *folio = cluster->folios[i];
        size_t bytes = folio_size(folio);

        if (keep >= bytes) {
            keep -= bytes;
            continue;
        }
        infilfs_pagecache_zero_folio(folio, keep, bytes - keep);
        keep = 0;
    }
}

static int infilfs_pagecache_fill_folio(struct folio *folio)
{
    struct inode *inode = folio->mapping->host;
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct bio_vec inline_bvec;
    struct bio_vec *bvecs = &inline_bvec;
    struct iov_iter iter;
    loff_t position = folio_pos(folio);
    u64 persisted = ii ? READ_ONCE(ii->persisted_size) : 0;
    size_t bytes = folio_size(folio);
    size_t wanted = 0;
    unsigned int bvec_count = 0;
    unsigned int bvec_capacity;
    ssize_t got = 0;
    int ret = 0;

    if (position >= 0 && (u64)position < persisted)
        wanted = min_t(u64, bytes, persisted - (u64)position);
    bvec_capacity = wanted ? DIV_ROUND_UP(wanted, PAGE_SIZE) : 0;

    if (bvec_capacity > 1u) {
        bvecs = kvmalloc_array(
            bvec_capacity, sizeof(*bvecs), GFP_NOFS);
        if (!bvecs)
            return -ENOMEM;
    }

    if (wanted) {
        ret = infilfs_pagecache_append_bvecs(
            bvecs, bvec_capacity, &bvec_count, folio, wanted);
        if (ret)
            goto out;
        iov_iter_bvec(&iter, ITER_DEST, bvecs, bvec_count, wanted);
        got = infilfs_native_read_iter_cached(inode, &position, &iter);
        if (got != wanted) {
            ret = got < 0 ? (int)got : -EIO;
            goto out;
        }
    }

    if (wanted < bytes)
        infilfs_pagecache_zero_folio(folio, wanted, bytes - wanted);
    flush_dcache_folio(folio);
    folio_mark_uptodate(folio);

out:
    if (bvecs != &inline_bvec)
        kvfree(bvecs);
    return ret;
}

static int infilfs_read_folio_legacy(struct file *file, struct folio *folio)
{
    int ret;

    (void)file;
    ret = infilfs_pagecache_fill_folio(folio);
    if (ret)
        mapping_set_error(folio->mapping, ret);
    folio_unlock(folio);
    return ret;
}

/*
 * The verified native reader amortises object lookup, extent-page validation,
 * checksum-object decoding and compressed-extent expansion across one
 * read_iter call. Feeding it one 4 KiB folio at a time throws that work away
 * between pages and is especially expensive for compressed 256 KiB clusters.
 *
 * Readahead already hands us a contiguous run of locked folios. Collapse that
 * run into the same 1 MiB bounded unit used by native writeback, perform one
 * fully verified native read, then populate the page cache. Integrity semantics
 * are unchanged: every data block is still SHA-256 checked before any folio is
 * marked uptodate.
 */
static int infilfs_readahead_cluster_submit(
    struct infilfs_readahead_cluster *cluster)
{
    struct folio *first;
    struct inode *inode;
    struct infilfs_inode_info *ii;
    struct iov_iter iter;
    loff_t position;
    u64 persisted;
    size_t wanted = 0;
    ssize_t got = 0;
    unsigned int i;
    int ret = 0;

    if (!cluster->count)
        return 0;

    first = cluster->folios[0];
    inode = first->mapping->host;
    ii = INFILFS_I(inode);
    persisted = ii ? READ_ONCE(ii->persisted_size) : 0;
    position = cluster->position;

    if (position >= 0 && (u64)position < persisted)
        wanted = min_t(u64, cluster->bytes, persisted - (u64)position);
    if (wanted) {
        iov_iter_bvec(
            &iter, ITER_DEST, cluster->bvecs,
            cluster->bvec_count, wanted);
        got = infilfs_native_read_iter_cached(inode, &position, &iter);
        if (got != wanted)
            ret = got < 0 ? (int)got : -EIO;
    }
    if (!ret && wanted < cluster->bytes)
        infilfs_readahead_zero_tail(cluster, wanted);

    for (i = 0; i < cluster->count; ++i) {
        struct folio *folio = cluster->folios[i];

        if (!ret) {
            flush_dcache_folio(folio);
            folio_mark_uptodate(folio);
        } else {
            mapping_set_error(folio->mapping, ret);
        }
        folio_unlock(folio);
    }

    cluster->count = 0;
    cluster->bvec_count = 0;
    cluster->bytes = 0;
    cluster->position = 0;
    return ret;
}

static void infilfs_readahead_legacy(struct readahead_control *rac)
{
    struct infilfs_readahead_cluster cluster = {0};
    const size_t batch_bytes = INFILFS_NATIVE_WRITEBACK_BATCH_BYTES;
    unsigned int max_segments =
        DIV_ROUND_UP(batch_bytes, PAGE_SIZE) + 1u;
    struct folio *folio;
    int ret = 0;

    cluster.bvecs = kvmalloc_array(
        max_segments, sizeof(*cluster.bvecs), GFP_NOFS);
    cluster.folios = kvmalloc_array(
        max_segments, sizeof(*cluster.folios), GFP_NOFS);
    cluster.capacity = max_segments;
    cluster.bvec_capacity = max_segments;
    if (!cluster.bvecs || !cluster.folios)
        goto fallback;

    while ((folio = readahead_folio(rac)) != NULL) {
        loff_t position = folio_pos(folio);
        size_t bytes = folio_size(folio);
        unsigned int segments = DIV_ROUND_UP(bytes, PAGE_SIZE);

        if (bytes > batch_bytes) {
            ret = infilfs_readahead_cluster_submit(&cluster);
            if (ret) {
                infilfs_read_folio_legacy(rac->file, folio);
                goto fallback_remaining;
            }
            infilfs_read_folio_legacy(rac->file, folio);
            continue;
        }

        if (cluster.count &&
            (position != cluster.position + cluster.bytes ||
             bytes > batch_bytes - cluster.bytes ||
             cluster.count >= cluster.capacity ||
             segments > cluster.bvec_capacity - cluster.bvec_count)) {
            ret = infilfs_readahead_cluster_submit(&cluster);
            if (ret) {
                infilfs_read_folio_legacy(rac->file, folio);
                goto fallback_remaining;
            }
        }

        if (!cluster.count)
            cluster.position = position;
        ret = infilfs_pagecache_append_bvecs(
            cluster.bvecs, cluster.bvec_capacity,
            &cluster.bvec_count, folio, bytes);
        if (ret) {
            infilfs_read_folio_legacy(rac->file, folio);
            goto fallback_remaining;
        }
        cluster.folios[cluster.count++] = folio;
        cluster.bytes += bytes;

        if (cluster.bytes == batch_bytes) {
            ret = infilfs_readahead_cluster_submit(&cluster);
            if (ret)
                goto fallback_remaining;
        }
    }
    (void)infilfs_readahead_cluster_submit(&cluster);
    goto out;

fallback:
    kvfree(cluster.folios);
    kvfree(cluster.bvecs);
    while ((folio = readahead_folio(rac)) != NULL)
        infilfs_read_folio_legacy(rac->file, folio);
    return;

fallback_remaining:
    while ((folio = readahead_folio(rac)) != NULL)
        infilfs_read_folio_legacy(rac->file, folio);
out:
    kvfree(cluster.folios);
    kvfree(cluster.bvecs);
}

#if INFILFS_HAVE_VERIFIED_IOMAP_READ
/*
 * Linux 7.0 iomap can delegate the actual read transport to the filesystem.
 * InfiltratorFS uses that interface only for page-cache state management:
 * compression, sparse extents and SHA-256 verification remain in the native
 * verified reader. This mirrors the iomap/FUSE model where IOMAP_MAPPED is a
 * logical transport mapping rather than a promise that generic bio I/O can
 * address the data directly.
 */
static int infilfs_iomap_read_begin(
    struct inode *inode, loff_t pos, loff_t length, unsigned int flags,
    struct iomap *iomap, struct iomap *srcmap)
{
    (void)inode;
    (void)srcmap;

    if (flags || pos < 0 || length <= 0)
        return -EOPNOTSUPP;

    iomap->type = IOMAP_MAPPED;
    iomap->offset = pos;
    iomap->length = length;
    iomap->addr = IOMAP_NULL_ADDR;
    return 0;
}

static const struct iomap_ops infilfs_iomap_read_ops = {
    .iomap_begin = infilfs_iomap_read_begin,
};

static int infilfs_iomap_read_range_sync(
    const struct iomap_iter *iter, struct folio *folio, size_t length)
{
    struct bio_vec inline_bvec;
    struct bio_vec *bvecs = &inline_bvec;
    struct iov_iter to;
    loff_t position = iter->pos;
    size_t offset = offset_in_folio(folio, position);
    unsigned int bvec_count = 0;
    unsigned int bvec_capacity;
    ssize_t got;
    int ret;

    if (!length || offset > folio_size(folio) ||
        length > folio_size(folio) - offset)
        return -EIO;

    bvec_capacity =
        DIV_ROUND_UP((offset & (PAGE_SIZE - 1u)) + length, PAGE_SIZE);
    if (bvec_capacity > 1u) {
        bvecs = kvmalloc_array(
            bvec_capacity, sizeof(*bvecs), GFP_NOFS);
        if (!bvecs)
            return -ENOMEM;
    }

    ret = infilfs_pagecache_append_bvec_range(
        bvecs, bvec_capacity, &bvec_count, folio, offset, length);
    if (ret)
        goto out;

    iov_iter_bvec(&to, ITER_DEST, bvecs, bvec_count, length);
    got = infilfs_native_read_iter_cached(
        iter->inode, &position, &to);
    if (got != length)
        ret = got < 0 ? (int)got : -EIO;
    else
        flush_dcache_folio(folio);

out:
    if (ret)
        mapping_set_error(folio->mapping, ret);
    if (bvecs != &inline_bvec)
        kvfree(bvecs);
    return ret;
}

static int infilfs_iomap_read_batch_submit(
    struct infilfs_iomap_read_batch *batch)
{
    struct iov_iter to;
    loff_t position;
    ssize_t got;
    unsigned int i;
    int ret = 0;

    if (!batch || !batch->range_count)
        return 0;

    position = batch->position;
    iov_iter_bvec(
        &to, ITER_DEST, batch->bvecs, batch->bvec_count, batch->bytes);
    got = infilfs_native_read_iter_cached(
        batch->inode, &position, &to);
    if (got != batch->bytes)
        ret = got < 0 ? (int)got : -EIO;
    if (ret)
        mapping_set_error(batch->inode->i_mapping, ret);

    for (i = 0; i < batch->range_count; ++i) {
        struct infilfs_iomap_read_range *range = &batch->ranges[i];

        if (!ret)
            flush_dcache_folio(range->folio);
        iomap_finish_folio_read(
            range->folio, range->offset, range->length, ret);
        folio_put(range->folio);
    }

    batch->bvec_count = 0;
    batch->range_count = 0;
    batch->bytes = 0;
    batch->position = 0;
    if (ret)
        batch->error = ret;
    return ret;
}

static int infilfs_iomap_read_folio_range(
    const struct iomap_iter *iter, struct iomap_read_folio_ctx *ctx,
    size_t length)
{
    struct folio *folio = ctx->cur_folio;
    struct infilfs_iomap_read_batch *batch = ctx->read_ctx;
    size_t offset = offset_in_folio(folio, iter->pos);
    unsigned int needed;
    int ret;

    if (!batch) {
        ret = infilfs_iomap_read_range_sync(iter, folio, length);
        if (!ret)
            iomap_finish_folio_read(folio, offset, length, 0);
        return ret;
    }

    if (batch->error)
        return batch->error;
    if (!length || offset > folio_size(folio) ||
        length > folio_size(folio) - offset)
        return -EIO;

    needed =
        DIV_ROUND_UP((offset & (PAGE_SIZE - 1u)) + length, PAGE_SIZE);
    if (needed > batch->bvec_capacity || !batch->range_capacity)
        return -EOVERFLOW;

    if (batch->range_count &&
        (iter->pos != batch->position + batch->bytes ||
         length > INFILFS_NATIVE_WRITEBACK_BATCH_BYTES - batch->bytes ||
         needed > batch->bvec_capacity - batch->bvec_count ||
         batch->range_count >= batch->range_capacity)) {
        ret = infilfs_iomap_read_batch_submit(batch);
        if (ret)
            return ret;
    }

    if (!batch->range_count) {
        batch->inode = iter->inode;
        batch->position = iter->pos;
    }

    ret = infilfs_pagecache_append_bvec_range(
        batch->bvecs, batch->bvec_capacity, &batch->bvec_count,
        folio, offset, length);
    if (ret)
        return ret;

    folio_get(folio);
    batch->ranges[batch->range_count].folio = folio;
    batch->ranges[batch->range_count].offset = offset;
    batch->ranges[batch->range_count].length = length;
    batch->range_count++;
    batch->bytes += length;
    return 0;
}

static void infilfs_iomap_submit_read(struct iomap_read_folio_ctx *ctx)
{
    struct infilfs_iomap_read_batch *batch = ctx->read_ctx;

    if (batch && batch->range_count)
        (void)infilfs_iomap_read_batch_submit(batch);
}

static const struct iomap_read_ops infilfs_iomap_verified_read_ops = {
    .read_folio_range = infilfs_iomap_read_folio_range,
    .submit_read = infilfs_iomap_submit_read,
};

static int infilfs_read_folio(struct file *file, struct folio *folio)
{
    struct iomap_read_folio_ctx ctx = {
        .ops = &infilfs_iomap_verified_read_ops,
        .cur_folio = folio,
    };

    (void)file;
    iomap_read_folio(&infilfs_iomap_read_ops, &ctx, NULL);
    return 0;
}

static void infilfs_readahead(struct readahead_control *rac)
{
    struct infilfs_iomap_read_batch batch = {0};
    struct iomap_read_folio_ctx ctx = {
        .ops = &infilfs_iomap_verified_read_ops,
        .rac = rac,
        .read_ctx = &batch,
    };
    unsigned int capacity =
        DIV_ROUND_UP(INFILFS_NATIVE_WRITEBACK_BATCH_BYTES, PAGE_SIZE) + 1u;

    batch.bvecs = kvmalloc_array(
        capacity, sizeof(*batch.bvecs), GFP_NOFS);
    batch.ranges = kvmalloc_array(
        capacity, sizeof(*batch.ranges), GFP_NOFS);
    batch.bvec_capacity = capacity;
    batch.range_capacity = capacity;
    if (!batch.bvecs || !batch.ranges) {
        kvfree(batch.ranges);
        kvfree(batch.bvecs);
        infilfs_readahead_legacy(rac);
        return;
    }

    iomap_readahead(&infilfs_iomap_read_ops, &ctx, NULL);
    if (batch.range_count)
        (void)infilfs_iomap_read_batch_submit(&batch);
    kvfree(batch.ranges);
    kvfree(batch.bvecs);
}
#else
static int infilfs_read_folio(struct file *file, struct folio *folio)
{
    return infilfs_read_folio_legacy(file, folio);
}

static void infilfs_readahead(struct readahead_control *rac)
{
    infilfs_readahead_legacy(rac);
}
#endif

static int infilfs_pagecache_prepare_folio(struct address_space *mapping,
                                 loff_t pos, unsigned int len,
                                 struct folio **folio_out)
{
    struct folio *folio;
    int ret = 0;

    folio = __filemap_get_folio(mapping, pos >> PAGE_SHIFT, FGP_WRITEBEGIN,
                      mapping_gfp_mask(mapping));
    if (IS_ERR(folio))
        return PTR_ERR(folio);
    if (!folio_test_uptodate(folio)) {
        if (pos == folio_pos(folio) && len >= folio_size(folio))
            folio_mark_uptodate(folio);
        else
            ret = infilfs_pagecache_fill_folio(folio);
        if (ret) {
            folio_unlock(folio);
            folio_put(folio);
            return ret;
        }
    }
    *folio_out = folio;
    return 0;
}

static int infilfs_pagecache_write_begin_common(
    struct address_space *mapping, loff_t pos, unsigned int len,
    struct folio **folio_out, void **fsdata)
{
    struct inode *inode = mapping->host;
    struct infilfs_pagecache_write_ctx *ctx = NULL;
    loff_t old_size = i_size_read(inode);
    u64 growth = 0;
    int ret;

    *fsdata = NULL;
    if (len && pos >= 0 && (u64)len <= (u64)LLONG_MAX - (u64)pos &&
        pos + len > old_size)
        growth = (u64)(pos + len - old_size);
    if (growth) {
        ctx = kzalloc(sizeof(*ctx), GFP_NOFS);
        if (!ctx)
            return -ENOMEM;
        ctx->old_size = old_size;
        ret = infilfs_quota_reserve_inode(inode, growth, 0, &ctx->quota);
        if (ret) {
            kfree(ctx);
            return ret;
        }
    }

    ret = infilfs_pagecache_prepare_folio(mapping, pos, len, folio_out);
    if (ret) {
        if (ctx) {
            infilfs_quota_reservation_abort(&ctx->quota);
            kfree(ctx);
        }
        return ret;
    }
    *fsdata = ctx;
    return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
static int infilfs_write_begin(const struct kiocb *iocb,
                     struct address_space *mapping, loff_t pos,
                     unsigned int len, struct folio **foliop,
                     void **fsdata)
{
    (void)iocb;
    return infilfs_pagecache_write_begin_common(
        mapping, pos, len, foliop, fsdata);
}

static int infilfs_write_end(const struct kiocb *iocb,
                   struct address_space *mapping, loff_t pos,
                   unsigned int len, unsigned int copied,
                   struct folio *folio, void *fsdata)
#else
static int infilfs_write_begin(struct file *file,
                     struct address_space *mapping, loff_t pos,
                     unsigned int len, struct page **pagep,
                     void **fsdata)
{
    struct folio *folio;
    int ret;

    (void)file;
    ret = infilfs_pagecache_write_begin_common(
        mapping, pos, len, &folio, fsdata);
    if (!ret)
        *pagep = &folio->page;
    return ret;
}

static int infilfs_write_end(struct file *file,
                   struct address_space *mapping, loff_t pos,
                   unsigned int len, unsigned int copied,
                   struct page *page, void *fsdata)
#endif
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
    struct inode *inode = mapping->host;

    (void)iocb;
#else
    struct folio *folio = page_folio(page);
    struct inode *inode = mapping->host;

    (void)file;
#endif
    struct infilfs_pagecache_write_ctx *ctx = fsdata;
    u64 actual_growth = 0;

    (void)len;
    if (copied) {
        loff_t end = pos + copied;

        if (end > i_size_read(inode))
            i_size_write(inode, end);
        folio_mark_dirty(folio);
        if (ctx && end > ctx->old_size)
            actual_growth = (u64)(end - ctx->old_size);
    }
    if (ctx) {
        if (copied)
            infilfs_quota_reservation_finish(
                &ctx->quota, actual_growth, 0);
        else
            infilfs_quota_reservation_abort(&ctx->quota);
        kfree(ctx);
    }
    folio_unlock(folio);
    folio_put(folio);
    return copied;
}

static int infilfs_writeback_folio(struct folio *folio,
                         struct writeback_control *wbc)
{
    struct address_space *mapping = folio->mapping;
    struct inode *inode;
    struct bio_vec inline_bvec;
    struct bio_vec *bvecs = &inline_bvec;
    struct iov_iter iter;
    loff_t position;
    loff_t write_position;
    loff_t file_size;
    size_t length;
    unsigned int bvec_count = 0;
    unsigned int bvec_capacity;
    ssize_t written;
    int ret = 0;

    if (!mapping)
        return 0;
    inode = mapping->host;
    folio_lock(folio);
    if (folio->mapping != mapping || !folio_test_dirty(folio))
        goto unlock;
    folio_wait_writeback(folio);

    position = folio_pos(folio);
    file_size = i_size_read(inode);
    if (position >= file_size) {
        if (!folio_clear_dirty_for_io(folio))
            goto unlock;
        folio_start_writeback(folio);
        infilfs_pagecache_unaccount(folio, true);
        folio_end_writeback(folio);
        goto unlock;
    }

    length = min_t(loff_t, folio_size(folio), file_size - position);
    bvec_capacity = DIV_ROUND_UP(length, PAGE_SIZE);
    if (bvec_capacity > 1u) {
        bvecs = kvmalloc_array(
            bvec_capacity, sizeof(*bvecs), GFP_NOFS);
        if (!bvecs) {
            ret = -ENOMEM;
            mapping_set_error(mapping, ret);
            goto unlock;
        }
    }
    ret = infilfs_pagecache_append_bvecs(
        bvecs, bvec_capacity, &bvec_count, folio, length);
    if (ret) {
        mapping_set_error(mapping, ret);
        goto out_free;
    }

    if (!folio_clear_dirty_for_io(folio)) {
        ret = 0;
        goto out_free;
    }

    folio_start_writeback(folio);
    iov_iter_bvec(
        &iter, ITER_SOURCE, bvecs, bvec_count, length);
    write_position = position;
    written = infilfs_native_writeback_iter(
        inode, &write_position, &iter, length);
    if (written != length)
        ret = written < 0 ? (int)written : -EIO;
    if (ret) {
        mapping_set_error(mapping, ret);
        folio_redirty_for_writepage(wbc, folio);
    } else {
        infilfs_pagecache_unaccount(folio, false);
        if (wbc->sync_mode == WB_SYNC_NONE)
            wbc->nr_to_write -=
                (long)(folio_size(folio) >> PAGE_SHIFT);
    }
    folio_end_writeback(folio);
#if INFILFS_HAVE_VERIFIED_IOMAP_READ
    if (!ret)
        (void)iomap_release_folio(folio, GFP_NOFS);
#endif

out_free:
    if (bvecs != &inline_bvec)
        kvfree(bvecs);
unlock:
    folio_unlock(folio);
    return ret;
}

static int infilfs_writeback_cluster_submit(
    struct address_space *mapping, struct writeback_control *wbc,
    struct infilfs_writeback_cluster *cluster)
{
    struct inode *inode = mapping->host;
    struct iov_iter iter;
    loff_t position;
    ssize_t written;
    int ret = 0;
    unsigned int i;

    if (!cluster->count)
        return 0;

    iov_iter_bvec(
        &iter, ITER_SOURCE, cluster->bvecs,
        cluster->bvec_count, cluster->bytes);
    position = cluster->position;
    written = infilfs_native_writeback_iter(
        inode, &position, &iter, cluster->bytes);
    if (written != cluster->bytes)
        ret = written < 0 ? (int)written : -EIO;
    if (ret)
        mapping_set_error(mapping, ret);

    for (i = 0; i < cluster->count; ++i) {
        struct folio *folio = cluster->folios[i];

        if (ret)
            folio_redirty_for_writepage(wbc, folio);
        else
            infilfs_pagecache_unaccount(folio, false);
        folio_end_writeback(folio);
#if INFILFS_HAVE_VERIFIED_IOMAP_READ
        if (!ret)
            (void)iomap_release_folio(folio, GFP_NOFS);
#endif
        folio_unlock(folio);
        folio_put(folio);
    }
    cluster->count = 0;
    cluster->bvec_count = 0;
    cluster->bytes = 0;
    cluster->position = 0;
    return ret;
}

static int infilfs_writepages(struct address_space *mapping,
                    struct writeback_control *wbc)
{
    struct infilfs_writeback_cluster cluster = {0};
    struct folio_batch fbatch;
    pgoff_t index = wbc->range_start >> PAGE_SHIFT;
    pgoff_t end = wbc->range_end == LLONG_MAX ? (pgoff_t)-1 :
        wbc->range_end >> PAGE_SHIFT;
    size_t batch_bytes = infilfs_native_writeback_batch_bytes();
    unsigned int max_segments =
        DIV_ROUND_UP(batch_bytes, PAGE_SIZE) + 1u;
    int ret = 0;

    cluster.bvecs = kvmalloc_array(
        max_segments, sizeof(*cluster.bvecs), GFP_NOFS);
    cluster.folios = kvmalloc_array(
        max_segments, sizeof(*cluster.folios), GFP_NOFS);
    cluster.capacity = max_segments;
    cluster.bvec_capacity = max_segments;
    if (!cluster.bvecs || !cluster.folios) {
        ret = -ENOMEM;
        goto out;
    }

    folio_batch_init(&fbatch);
    while (index <= end && filemap_get_folios_tag(
             mapping, &index, end, PAGECACHE_TAG_DIRTY, &fbatch)) {
        unsigned int i;

        for (i = 0; i < folio_batch_count(&fbatch); ++i) {
            struct folio *folio = fbatch.folios[i];
            loff_t position = folio_pos(folio);
            loff_t file_size = i_size_read(mapping->host);
            size_t length = position < file_size ?
                min_t(loff_t, folio_size(folio), file_size - position) : 0;
            unsigned int segments =
                length ? DIV_ROUND_UP(length, PAGE_SIZE) : 0;

            if (folio_size(folio) > batch_bytes) {
                ret = infilfs_writeback_cluster_submit(mapping, wbc, &cluster);
                if (!ret)
                    ret = infilfs_writeback_folio(folio, wbc);
                if (ret)
                    break;
                continue;
            }

            if (cluster.count &&
                (position != cluster.position + cluster.bytes ||
                 cluster.bytes + length > batch_bytes ||
                 segments > cluster.bvec_capacity - cluster.bvec_count)) {
                ret = infilfs_writeback_cluster_submit(mapping, wbc, &cluster);
                if (ret)
                    break;
            }

            folio_lock(folio);
            if (folio->mapping != mapping || !folio_test_dirty(folio)) {
                folio_unlock(folio);
                continue;
            }
            folio_wait_writeback(folio);
            if (!folio_clear_dirty_for_io(folio)) {
                folio_unlock(folio);
                continue;
            }

            position = folio_pos(folio);
            file_size = i_size_read(mapping->host);
            if (position >= file_size) {
                folio_start_writeback(folio);
                infilfs_pagecache_unaccount(folio, true);
                folio_end_writeback(folio);
                folio_unlock(folio);
                continue;
            }

            length = min_t(loff_t, folio_size(folio), file_size - position);
            segments = DIV_ROUND_UP(length, PAGE_SIZE);
            if (!cluster.count)
                cluster.position = position;
            if (position != cluster.position + cluster.bytes ||
                cluster.bytes + length > batch_bytes ||
                cluster.count >= cluster.capacity ||
                segments > cluster.bvec_capacity - cluster.bvec_count) {
                folio_redirty_for_writepage(wbc, folio);
                folio_unlock(folio);
                ret = infilfs_writeback_cluster_submit(mapping, wbc, &cluster);
                if (ret)
                    break;
                continue;
            }

            ret = infilfs_pagecache_append_bvecs(
                cluster.bvecs, cluster.bvec_capacity,
                &cluster.bvec_count, folio, length);
            if (ret) {
                folio_redirty_for_writepage(wbc, folio);
                folio_unlock(folio);
                break;
            }

            folio_start_writeback(folio);
            folio_get(folio);
            cluster.folios[cluster.count++] = folio;
            cluster.bytes += length;
            if (wbc->sync_mode == WB_SYNC_NONE)
                wbc->nr_to_write -=
                    (long)(folio_size(folio) >> PAGE_SHIFT);

            if (cluster.bytes == batch_bytes ||
                (wbc->sync_mode == WB_SYNC_NONE &&
                 wbc->nr_to_write <= 0)) {
                ret = infilfs_writeback_cluster_submit(mapping, wbc, &cluster);
                if (ret || (wbc->sync_mode == WB_SYNC_NONE &&
                            wbc->nr_to_write <= 0))
                    break;
            }
        }
        folio_batch_release(&fbatch);
        cond_resched();
        if (ret || (wbc->sync_mode == WB_SYNC_NONE &&
                    wbc->nr_to_write <= 0))
            break;
    }

    if (!ret)
        ret = infilfs_writeback_cluster_submit(mapping, wbc, &cluster);
    else if (cluster.count)
        (void)infilfs_writeback_cluster_submit(mapping, wbc, &cluster);

out:
    if (ret)
        mapping_set_error(mapping, ret);
    kvfree(cluster.folios);
    kvfree(cluster.bvecs);
    return ret;
}

const struct address_space_operations infilfs_aops = {
    .read_folio = infilfs_read_folio,
    .readahead = infilfs_readahead,
    .writepages = infilfs_writepages,
    .write_begin = infilfs_write_begin,
    .write_end = infilfs_write_end,
    .dirty_folio = infilfs_dirty_folio,
    .invalidate_folio = infilfs_invalidate_folio,
    .release_folio = infilfs_release_folio,
};