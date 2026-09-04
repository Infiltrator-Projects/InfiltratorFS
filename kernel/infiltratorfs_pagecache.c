// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"
/*
 * Native Linux page-cache integration.
 *
 * Reads retain InfiltratorFS checksum verification.  Dirty folios are written
 * through the same copy-on-write extent transaction used by write(2), then
 * published by the normal fsync/sync/unmount durability paths.
 */

static int infilfs_pagecache_fill_folio(struct folio *folio)
{
    struct inode *inode = folio->mapping->host;
    loff_t start = folio_pos(folio);
    loff_t file_size = i_size_read(inode);
    size_t wanted = 0;
    size_t offset;
    int ret = 0;

    if (start < file_size)
        wanted = min_t(loff_t, folio_size(folio), file_size - start);
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
                                           pgoff_t index,
                                           struct folio **folio_out)
{
    struct folio *folio;
    int ret = 0;

    folio = __filemap_get_folio(mapping, index, FGP_WRITEBEGIN,
                                mapping_gfp_mask(mapping));
    if (IS_ERR(folio))
        return PTR_ERR(folio);
    if (!folio_test_uptodate(folio)) {
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

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
static int infilfs_write_begin(const struct kiocb *iocb,
                               struct address_space *mapping, loff_t pos,
                               unsigned int len, struct folio **foliop,
                               void **fsdata)
{
    (void)iocb;
    (void)len;
    (void)fsdata;
    return infilfs_pagecache_prepare_folio(mapping, pos >> PAGE_SHIFT,
                                            foliop);
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
    (void)len;
    (void)fsdata;
    ret = infilfs_pagecache_prepare_folio(mapping, pos >> PAGE_SHIFT,
                                           &folio);
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
    (void)len;
    (void)fsdata;
    if (copied) {
        loff_t end = pos + copied;

        if (end > i_size_read(inode))
            i_size_write(inode, end);
        folio_mark_dirty(folio);
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
        written = infilfs_native_extent_write_iter(
            inode, &write_position, &iter, chunk);
        kunmap_local(address);
        if (written != chunk) {
            ret = written < 0 ? (int)written : -EIO;
            break;
        }
        /*
         * The extent writer advances i_size to the size currently published
         * on disk.  Buffered writes may already have extended the authoritative
         * page-cache size beyond this folio, so retain that VFS-visible size
         * while the remaining dirty folios are written back.
         */
        if (i_size_read(inode) < file_size)
            i_size_write(inode, file_size);
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

static int infilfs_writepages(struct address_space *mapping,
                              struct writeback_control *wbc)
{
    struct folio_batch fbatch;
    pgoff_t index = wbc->range_start >> PAGE_SHIFT;
    pgoff_t end = wbc->range_end == LLONG_MAX ? (pgoff_t)-1 :
        wbc->range_end >> PAGE_SHIFT;
    int ret = 0;

    folio_batch_init(&fbatch);
    while (index <= end && filemap_get_folios_tag(
               mapping, &index, end, PAGECACHE_TAG_DIRTY, &fbatch)) {
        unsigned int i;

        for (i = 0; i < folio_batch_count(&fbatch); ++i) {
            int one = infilfs_writeback_folio(fbatch.folios[i], wbc);

            if (one && !ret)
                ret = one;
            if (one || (wbc->sync_mode == WB_SYNC_NONE &&
                        --wbc->nr_to_write <= 0))
                break;
        }
        folio_batch_release(&fbatch);
        cond_resched();
        if (ret || (wbc->sync_mode == WB_SYNC_NONE &&
                    wbc->nr_to_write <= 0))
            break;
    }
    if (!ret)
        ret = infilfs_native_pending_flush_sb(mapping->host->i_sb);
    if (ret)
        mapping_set_error(mapping, ret);
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
