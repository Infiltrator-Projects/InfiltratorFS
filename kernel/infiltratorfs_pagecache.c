// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"
/*
 * Native Linux page-cache integration.
 *
 * Ordinary read(2)/write(2), mmap and readahead share the page cache. Dirty
 * folios are coalesced into bounded contiguous batches and handed to the
 * verified CoW writer; fsync/sync/unmount remain publication/durability
 * boundaries. This keeps codec/integrity work out of the application syscall.
 */

struct infilfs_pagecache_write_ctx {
    struct infilfs_quota_reservation quota;
    loff_t old_size;
};

struct infilfs_writeback_cluster {
    u8 *buffer;
    struct folio **folios;
    size_t *lengths;
    unsigned int count;
    unsigned int capacity;
    loff_t position;
    size_t bytes;
};

static int infilfs_pagecache_fill_folio(struct folio *folio)
{
    struct inode *inode = folio->mapping->host;
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    loff_t start = folio_pos(folio);
    u64 persisted = ii ? READ_ONCE(ii->persisted_size) : 0;
    size_t wanted = 0;
    size_t offset;
    int ret = 0;

    if (start >= 0 && (u64)start < persisted)
        wanted = min_t(u64, folio_size(folio), persisted - (u64)start);
    for (offset = 0; offset < folio_size(folio); offset += PAGE_SIZE) {
        struct page *page = folio_page(folio, offset >> PAGE_SHIFT);
        struct kvec vec;
        struct iov_iter iter;
        loff_t position = start + offset;
        size_t chunk = offset < wanted ?
  min_t(size_t, PAGE_SIZE, wanted - offset) : 0;
        void *address = kmap_local_page(page);
        ssize_t got = 0;

        if (chunk) {
  vec.iov_base = address;
  vec.iov_len = chunk;
  iov_iter_kvec(&iter, ITER_DEST, &vec, 1, chunk);
  got = infilfs_native_read_iter_cached(inode, &position, &iter);
        }
        if (got != chunk) {
  ret = got < 0 ? (int)got : -EIO;
  kunmap_local(address);
  break;
        }
        if (chunk < PAGE_SIZE)
  memset(address + chunk, 0, PAGE_SIZE - chunk);
        kunmap_local(address);
    }
    if (!ret) {
        flush_dcache_folio(folio);
        folio_mark_uptodate(folio);
    }
    return ret;
}

static int infilfs_read_folio(struct file *file, struct folio *folio)
{
    int ret;

    (void)file;
    ret = infilfs_pagecache_fill_folio(folio);
    if (ret)
        mapping_set_error(folio->mapping, ret);
    folio_unlock(folio);
    return ret;
}

static void infilfs_readahead(struct readahead_control *rac)
{
    struct folio *folio;

    while ((folio = readahead_folio(rac)) != NULL)
        infilfs_read_folio(rac->file, folio);
}

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
    loff_t position;
    loff_t file_size;
    size_t length;
    size_t done = 0;
    int ret = 0;

    if (!mapping)
        return 0;
    inode = mapping->host;
    folio_lock(folio);
    if (folio->mapping != mapping || !folio_test_dirty(folio))
        goto unlock;
    folio_wait_writeback(folio);
    if (!folio_clear_dirty_for_io(folio))
        goto unlock;

    position = folio_pos(folio);
    file_size = i_size_read(inode);
    if (position >= file_size) {
        folio_start_writeback(folio);
        folio_end_writeback(folio);
        goto unlock;
    }
    length = min_t(loff_t, folio_size(folio), file_size - position);
    folio_start_writeback(folio);
    while (done < length) {
        struct page *page = folio_page(folio, done >> PAGE_SHIFT);
        struct kvec vec;
        struct iov_iter iter;
        loff_t write_position = position + done;
        size_t chunk = min_t(size_t, PAGE_SIZE, length - done);
        void *address = kmap_local_page(page);
        ssize_t written;

        vec.iov_base = address;
        vec.iov_len = chunk;
        iov_iter_kvec(&iter, ITER_SOURCE, &vec, 1, chunk);
        written = infilfs_native_writeback_iter(
  inode, &write_position, &iter, chunk);
        kunmap_local(address);
        if (written != chunk) {
  ret = written < 0 ? (int)written : -EIO;
  break;
        }
        done += chunk;
    }
    if (ret) {
        mapping_set_error(mapping, ret);
        folio_redirty_for_writepage(wbc, folio);
    }
    folio_end_writeback(folio);
unlock:
    folio_unlock(folio);
    return ret;
}

static int infilfs_writeback_cluster_submit(
    struct address_space *mapping, struct writeback_control *wbc,
    struct infilfs_writeback_cluster *cluster)
{
    struct inode *inode = mapping->host;
    struct kvec vec;
    struct iov_iter iter;
    loff_t position;
    ssize_t written;
    int ret = 0;
    unsigned int i;

    if (!cluster->count)
        return 0;
    vec.iov_base = cluster->buffer;
    vec.iov_len = cluster->bytes;
    iov_iter_kvec(&iter, ITER_SOURCE, &vec, 1, cluster->bytes);
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
        folio_end_writeback(folio);
        folio_unlock(folio);
        folio_put(folio);
    }
    cluster->count = 0;
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
    unsigned int max_folios =
        DIV_ROUND_UP(INFILFS_NATIVE_WRITEBACK_BATCH_BYTES, PAGE_SIZE) + 1u;
    int ret = 0;

    cluster.buffer = kvmalloc(INFILFS_NATIVE_WRITEBACK_BATCH_BYTES, GFP_NOFS);
    cluster.folios = kvmalloc_array(max_folios, sizeof(*cluster.folios), GFP_NOFS);
    cluster.lengths = kvmalloc_array(max_folios, sizeof(*cluster.lengths), GFP_NOFS);
    cluster.capacity = max_folios;
    if (!cluster.buffer || !cluster.folios || !cluster.lengths) {
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
  size_t copied = 0;

  if (folio_size(folio) > INFILFS_NATIVE_WRITEBACK_BATCH_BYTES) {
      ret = infilfs_writeback_cluster_submit(mapping, wbc, &cluster);
      if (!ret)
          ret = infilfs_writeback_folio(folio, wbc);
      if (ret)
          break;
      continue;
  }

  if (cluster.count &&
      (position != cluster.position + cluster.bytes ||
       cluster.bytes + length > INFILFS_NATIVE_WRITEBACK_BATCH_BYTES)) {
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
      folio_end_writeback(folio);
      folio_unlock(folio);
      continue;
  }
  length = min_t(loff_t, folio_size(folio), file_size - position);
  if (!cluster.count)
      cluster.position = position;
  if (position != cluster.position + cluster.bytes ||
      cluster.bytes + length > INFILFS_NATIVE_WRITEBACK_BATCH_BYTES ||
      cluster.count >= cluster.capacity) {
      folio_redirty_for_writepage(wbc, folio);
      folio_unlock(folio);
      ret = infilfs_writeback_cluster_submit(mapping, wbc, &cluster);
      if (ret)
          break;
      continue;
  }

  folio_start_writeback(folio);
  while (copied < length) {
      struct page *page = folio_page(folio, copied >> PAGE_SHIFT);
      size_t chunk = min_t(size_t, PAGE_SIZE, length - copied);
      void *address = kmap_local_page(page);

      memcpy(cluster.buffer + cluster.bytes + copied,
             address, chunk);
      kunmap_local(address);
      copied += chunk;
  }
  folio_get(folio);
  cluster.folios[cluster.count] = folio;
  cluster.lengths[cluster.count] = length;
  cluster.count++;
  cluster.bytes += length;

  if (cluster.bytes == INFILFS_NATIVE_WRITEBACK_BATCH_BYTES ||
      (wbc->sync_mode == WB_SYNC_NONE && --wbc->nr_to_write <= 0)) {
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
    kvfree(cluster.lengths);
    kvfree(cluster.folios);
    kvfree(cluster.buffer);
    return ret;
}

const struct address_space_operations infilfs_aops = {
    .read_folio = infilfs_read_folio,
    .readahead = infilfs_readahead,
    .writepages = infilfs_writepages,
    .write_begin = infilfs_write_begin,
    .write_end = infilfs_write_end,
    .dirty_folio = filemap_dirty_folio,
};
