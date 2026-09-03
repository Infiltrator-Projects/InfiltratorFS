// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Transactional native namespace operations.
 *
 * Namespace mutations join the mount's deferred native transaction and take an
 * operation savepoint, so each syscall is atomic and rollback-safe without
 * forcing a device-wide durability flush.  Publication is shared with the
 * data path: explicit fsync/syncfs, idle timeout, bounded transaction size,
 * or unmount.  Directory/index metadata is still rebuilt copy-on-write.
 */

struct infilfs_ns_index_change {
    u8 object_id[16];
    u64 object_block;
    u16 object_type;
    u8 action;
    bool found;
};

#define INFILFS_NS_REPOINT 1u
#define INFILFS_NS_ADD     2u
#define INFILFS_NS_REMOVE  3u

static const struct infilfs_attributes_disk *
infilfs_ns_attributes(const u8 object[INFILFS_DISK_BLOCK_SIZE])
{
    const struct infilfs_object_header_disk *header =
        (const struct infilfs_object_header_disk *)object;

    switch (le16_to_cpu(header->object_type)) {
    case INFILFS_OBJECT_DIRECTORY:
        return &((const struct infilfs_directory_payload_disk *)(header + 1))->attributes;
    case INFILFS_OBJECT_FILE:
        return &((const struct infilfs_file_payload_disk *)(header + 1))->attributes;
    case INFILFS_OBJECT_SYMLINK:
        return &((const struct infilfs_symlink_payload_disk *)(header + 1))->attributes;
    default:
        return NULL;
    }
}

static struct infilfs_attributes_disk *
infilfs_ns_attributes_mut(u8 object[INFILFS_DISK_BLOCK_SIZE])
{
    return (struct infilfs_attributes_disk *)infilfs_ns_attributes(object);
}

static int infilfs_ns_validate_name(const struct qstr *name)
{
    if (!name || !name->len || name->len > INFILFS_NAME_MAX ||
        !infilfs_rw_utf8_valid(name->name, name->len) ||
        (name->len == 1 && name->name[0] == '.') ||
        (name->len == 2 && name->name[0] == '.' && name->name[1] == '.'))
        return -EINVAL;
    return 0;
}

static int infilfs_ns_begin(struct super_block *sb,
                            struct infilfs_native_pending **pending_out)
{
    struct infilfs_native_pending *pending;
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    int ret;

    if (!sbi || !sbi->rw_enabled || sb_rdonly(sb))
        return -EROFS;
    pending = infilfs_native_pending_get(sb, true);
    if (!pending)
        return -ENOMEM;
    cancel_delayed_work_sync(&pending->idle_work);
    mutex_lock(&sbi->write_lock);
    ret = infilfs_native_pending_begin_locked(pending);
    if (!ret)
        ret = infilfs_native_operation_snapshot(pending);
    if (ret) {
        mutex_unlock(&sbi->write_lock);
        return ret;
    }
    *pending_out = pending;
    return 0;
}

/*
 * Namespace syscalls are transaction boundaries, not implicit fsyncs.
 * Keep each operation atomic through the existing operation snapshot,
 * but publish a burst of successful mutations together.  Explicit
 * fsync/syncfs, the five-second idle timer, unmount, or the bounded
 * metadata charge below remains a durability boundary.
 *
 * The synthetic 64 KiB charge bounds metadata-only bursts even when a
 * transaction-private directory/index block can be reused without a
 * corresponding free-block delta.  This prevents an unbounded pending
 * transaction while avoiding repeated full checkpoint/bitmap publication
 * during ordinary desktop namespace traffic.
 */
#define INFILFS_NATIVE_METADATA_PUBLISH_CHARGE (64ULL * 1024ULL)

static int infilfs_ns_finish(struct infilfs_native_pending *pending, int ret)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(pending->sb);
    int rollback_ret = 0;

    if (ret) {
        rollback_ret = infilfs_native_operation_rollback(pending);
        mutex_unlock(&sbi->write_lock);
        return rollback_ret ? rollback_ret : ret;
    }

    infilfs_native_operation_commit(pending);
    infilfs_native_checksum_cache_invalidate_sb(pending->sb);
    infilfs_native_writer_tail_invalidate(pending);
    pending->pending_bytes += INFILFS_NATIVE_METADATA_PUBLISH_CHARGE;

    if (infilfs_native_pending_should_publish(pending)) {
        ret = infilfs_native_pending_commit_locked(pending);
    } else if (pending->active && !pending->commit_failed) {
        mod_delayed_work(system_wq, &pending->idle_work,
                         INFILFS_NATIVE_IDLE_DELAY);
    }
    mutex_unlock(&sbi->write_lock);
    return ret;
}

static int infilfs_ns_index_snapshot(
    struct super_block *sb, struct infilfs_index_entry_disk **entries_out,
    u32 *count_out)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct infilfs_object_header_disk *header;
    struct infilfs_index_payload_disk *payload;
    struct infilfs_index_entry_disk *entries = NULL;
    u8 *head = NULL;
    u8 *page_block = NULL;
    u32 count;
    u16 version;
    int ret;

    head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!head)
        return -ENOMEM;
    ret = infilfs_read_object(sb, le64_to_cpu(sbi->disk.object_index_block),
                              INFILFS_OBJECT_INDEX, NULL, head);
    if (ret)
        goto out;
    header = (struct infilfs_object_header_disk *)head;
    payload = (struct infilfs_index_payload_disk *)(header + 1);
    count = le32_to_cpu(payload->entry_count);
    entries = kvmalloc_array(count ? count : 1u, sizeof(*entries), GFP_NOFS);
    if (!entries) {
        ret = -ENOMEM;
        goto out;
    }
    version = le16_to_cpu(header->object_version);
    if (version == INFILFS_OBJECT_VERSION_TREE) {
        kvfree(entries);
        entries = NULL;
        ret = infilfs_index_tree_snapshot(
            sb, head, &entries, &count);
        if (ret)
            goto out;
    } else if (version == INFILFS_OBJECT_VERSION_CLASSIC) {
        size_t need = sizeof(*payload) + (size_t)count * sizeof(*entries);
        if (le32_to_cpu(payload->reserved) != 0 ||
            need > le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        if (count)
            memcpy(entries, payload + 1, (size_t)count * sizeof(*entries));
    } else if (version == INFILFS_OBJECT_VERSION_PAGED) {
        const __le64 *pages = (const __le64 *)(payload + 1);
        u32 page_count = le32_to_cpu(payload->reserved);
        u32 copied = 0;
        u32 p;

        if ((count && !page_count) || page_count > INFILFS_INDEX_PAGE_POINTERS) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!page_block) {
            ret = -ENOMEM;
            goto out;
        }
        for (p = 0; p < page_count; ++p) {
            struct infilfs_metadata_page_disk *page;
            u32 n;
            ret = infilfs_read_allocated_block(
                sb, le64_to_cpu(pages[p]), page_block);
            if (ret)
                goto out;
            if (!infilfs_metadata_page_valid(sb, page_block,
                                             infilfs_index_page_magic,
                                             header->object_id)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            page = (struct infilfs_metadata_page_disk *)page_block;
            n = le32_to_cpu(page->entry_count);
            if (n > INFILFS_INDEX_ENTRIES_PER_PAGE || copied > count ||
                n > count - copied ||
                le32_to_cpu(page->bytes_used) != n * sizeof(*entries)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            memcpy(entries + copied, page + 1, (size_t)n * sizeof(*entries));
            copied += n;
        }
        if (copied != count) {
            ret = -EFSCORRUPTED;
            goto out;
        }
    } else {
        ret = -EFSCORRUPTED;
        goto out;
    }
    *entries_out = entries;
    *count_out = count;
    entries = NULL;
    ret = 0;
out:
    kvfree(entries);
    kfree(page_block);
    kfree(head);
    return ret;
}

static int infilfs_ns_change_find(struct infilfs_ns_index_change *changes,
                                  u32 change_count, const u8 id[16])
{
    u32 i;
    for (i = 0; i < change_count; ++i)
        if (memcmp(changes[i].object_id, id, 16) == 0)
            return (int)i;
    return -1;
}

/*
 * Fast path for the overwhelmingly common namespace index mutations: repoint
 * an existing object and/or append a newly created object.  A paged object
 * index does not need to be flattened and completely rewritten for those
 * operations.  Reuse the data-path updater so only touched index pages plus
 * the index head are copy-on-written.  Removes still use the full rebuild
 * below because compacting entries can shift records across page boundaries.
 */
static int infilfs_ns_update_tree_index(
    struct infilfs_native_pending *pending,
    struct infilfs_ns_index_change *changes, u32 change_count)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(pending->sb);
    struct infilfs_native_index_change *native = NULL;
    struct infilfs_object_header_disk *header;
    u8 *head = NULL;
    u32 c;
    int ret;

    if (!change_count)
        return 0;
    head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!head)
        return -ENOMEM;
    ret = infilfs_read_object(
        pending->sb, le64_to_cpu(sbi->disk.object_index_block),
        INFILFS_OBJECT_INDEX, NULL, head);
    if (ret)
        goto out;
    header = (struct infilfs_object_header_disk *)head;
    if (le16_to_cpu(header->object_version) != INFILFS_OBJECT_VERSION_TREE) {
        ret = -EOPNOTSUPP;
        goto out;
    }

    native = kcalloc(change_count, sizeof(*native), GFP_NOFS);
    if (!native) {
        ret = -ENOMEM;
        goto out;
    }
    for (c = 0; c < change_count; ++c) {
        if (changes[c].action != INFILFS_NS_REPOINT &&
            changes[c].action != INFILFS_NS_ADD &&
            changes[c].action != INFILFS_NS_REMOVE) {
            ret = -EINVAL;
            goto out;
        }
        memcpy(native[c].object_id, changes[c].object_id, 16);
        native[c].object_block = changes[c].object_block;
        native[c].object_type = changes[c].object_type;
        native[c].add = changes[c].action == INFILFS_NS_ADD;
        native[c].remove = changes[c].action == INFILFS_NS_REMOVE;
    }

    ret = infilfs_native_index_update(pending, native, change_count);
    if (!ret) {
        sbi->disk = pending->tx.next_sb;
        for (c = 0; c < change_count; ++c)
            changes[c].found = true;
    }
out:
    kfree(native);
    kfree(head);
    return ret;
}

static int infilfs_ns_update_paged_index(
    struct infilfs_native_pending *pending,
    struct infilfs_ns_index_change *changes, u32 change_count)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(pending->sb);
    struct infilfs_native_index_change *native = NULL;
    struct infilfs_object_header_disk *header;
    u8 *head = NULL;
    u32 c;
    int ret;

    if (!change_count)
        return 0;
    for (c = 0; c < change_count; ++c) {
        if (changes[c].action == INFILFS_NS_REMOVE)
            return -EOPNOTSUPP;
        if (changes[c].action != INFILFS_NS_REPOINT &&
            changes[c].action != INFILFS_NS_ADD)
            return -EINVAL;
    }

    head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!head)
        return -ENOMEM;
    ret = infilfs_read_object(
        pending->sb, le64_to_cpu(sbi->disk.object_index_block),
        INFILFS_OBJECT_INDEX, NULL, head);
    if (ret)
        goto out;
    header = (struct infilfs_object_header_disk *)head;
    if (le16_to_cpu(header->object_version) != INFILFS_OBJECT_VERSION_PAGED) {
        ret = -EOPNOTSUPP;
        goto out;
    }

    native = kcalloc(change_count, sizeof(*native), GFP_NOFS);
    if (!native) {
        ret = -ENOMEM;
        goto out;
    }
    for (c = 0; c < change_count; ++c) {
        memcpy(native[c].object_id, changes[c].object_id, 16);
        native[c].object_block = changes[c].object_block;
        native[c].object_type = changes[c].object_type;
        native[c].add = changes[c].action == INFILFS_NS_ADD;
    }

    ret = infilfs_native_index_update(pending, native, change_count);
    if (!ret) {
        sbi->disk = pending->tx.next_sb;
        for (c = 0; c < change_count; ++c)
            changes[c].found = true;
    }
out:
    kfree(native);
    kfree(head);
    return ret;
}

static int infilfs_ns_rebuild_index(
    struct infilfs_native_pending *pending,
    struct infilfs_ns_index_change *changes, u32 change_count)
{
    struct infilfs_rw_tx *tx = &pending->tx;
    struct infilfs_sb_info *sbi = INFILFS_SB(pending->sb);
    struct infilfs_index_entry_disk *old_entries = NULL;
    struct infilfs_index_entry_disk *new_entries = NULL;
    u8 *old_head = NULL;
    u8 *new_head = NULL;
    u8 *page_block = NULL;
    struct infilfs_object_header_disk *header;
    struct infilfs_index_payload_disk *payload;
    u32 old_count = 0, new_count = 0, add_count = 0;
    u32 i, c;
    bool paged;
    u64 new_head_block;
    int ret;

    for (c = 0; c < change_count; ++c) {
        changes[c].found = false;
        if (changes[c].action == INFILFS_NS_ADD)
            add_count++;
    }

    ret = infilfs_ns_update_tree_index(pending, changes, change_count);
    if (ret != -EOPNOTSUPP)
        return ret;

    ret = infilfs_ns_update_paged_index(pending, changes, change_count);
    if (ret != -EOPNOTSUPP)
        return ret;

    /*
     * The fallback rebuild can compact or reorder index entries, so cached
     * page/slot positions are no longer authoritative.
     */
    infilfs_native_index_locator_invalidate(pending);

    old_head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    new_head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!old_head || !new_head || !page_block) {
        ret = -ENOMEM;
        goto out;
    }
    ret = infilfs_rw_collect_index(tx, old_head, &old_entries, &old_count);
    if (ret)
        goto out;
    new_entries = kvmalloc_array((size_t)old_count + add_count + 1u,
                                 sizeof(*new_entries), GFP_NOFS);
    if (!new_entries) {
        ret = -ENOMEM;
        goto out;
    }
    for (i = 0; i < old_count; ++i) {
        int idx = infilfs_ns_change_find(changes, change_count,
                                         old_entries[i].object_id);
        if (idx >= 0) {
            struct infilfs_ns_index_change *change = &changes[idx];
            if (change->action == INFILFS_NS_ADD) {
                ret = -EEXIST;
                goto out;
            }
            change->found = true;
            if (change->action == INFILFS_NS_REMOVE)
                continue;
            new_entries[new_count] = old_entries[i];
            new_entries[new_count].object_block = cpu_to_le64(change->object_block);
            new_entries[new_count].object_type = cpu_to_le16(change->object_type);
            new_count++;
            continue;
        }
        new_entries[new_count++] = old_entries[i];
    }
    for (c = 0; c < change_count; ++c) {
        if (changes[c].action == INFILFS_NS_ADD) {
            memset(&new_entries[new_count], 0, sizeof(new_entries[new_count]));
            memcpy(new_entries[new_count].object_id, changes[c].object_id, 16);
            new_entries[new_count].object_block = cpu_to_le64(changes[c].object_block);
            new_entries[new_count].object_type = cpu_to_le16(changes[c].object_type);
            changes[c].found = true;
            new_count++;
        } else if (!changes[c].found) {
            ret = -ENOENT;
            goto out;
        }
    }

    header = (struct infilfs_object_header_disk *)old_head;
    paged = le16_to_cpu(header->object_version) == INFILFS_OBJECT_VERSION_PAGED ||
        (le64_to_cpu(tx->next_sb.incompat_flags) & INFILFS_INCOMPAT_PAGED_METADATA);
    memset(new_head, 0, INFILFS_DISK_BLOCK_SIZE);
    memcpy(new_head, old_head, sizeof(*header) + sizeof(*payload));
    header = (struct infilfs_object_header_disk *)new_head;
    payload = (struct infilfs_index_payload_disk *)(header + 1);
    header->generation = cpu_to_le64(tx->generation);
    payload->entry_count = cpu_to_le32(new_count);

    if (paged) {
        __le64 *pointers = (__le64 *)(payload + 1);
        u32 page_count = DIV_ROUND_UP(new_count, INFILFS_INDEX_ENTRIES_PER_PAGE);
        u32 copied = 0;
        u32 p;
        if (page_count > INFILFS_INDEX_PAGE_POINTERS) {
            ret = -ENOSPC;
            goto out;
        }
        header->object_version = cpu_to_le16(INFILFS_OBJECT_VERSION_PAGED);
        payload->reserved = cpu_to_le32(page_count);
        header->payload_size = cpu_to_le32(sizeof(*payload) +
            (size_t)page_count * sizeof(__le64));
        for (p = 0; p < page_count; ++p) {
            struct infilfs_metadata_page_disk *page;
            u64 page_no;
            u32 n = min_t(u32, INFILFS_INDEX_ENTRIES_PER_PAGE,
                          new_count - copied);
            ret = infilfs_rw_tx_alloc(tx, 1, &page_no);
            if (ret)
                goto out;
            infilfs_rw_init_page(page_block, infilfs_index_page_magic,
                                 header->object_id, tx->generation);
            page = (struct infilfs_metadata_page_disk *)page_block;
            page->entry_count = cpu_to_le32(n);
            page->bytes_used = cpu_to_le32(n * sizeof(*new_entries));
            memcpy(page + 1, new_entries + copied, (size_t)n * sizeof(*new_entries));
            ret = infilfs_rw_finalize_page(page_block);
            if (ret)
                goto out;
            ret = infilfs_native_stage_block(pending->sb, page_no, page_block);
            if (ret)
                goto out;
            pointers[p] = cpu_to_le64(page_no);
            copied += n;
        }
    } else {
        size_t payload_size = sizeof(*payload) +
            (size_t)new_count * sizeof(*new_entries);
        if (payload_size > INFILFS_DISK_BLOCK_SIZE - sizeof(*header)) {
            ret = -ENOSPC;
            goto out;
        }
        header->object_version = cpu_to_le16(INFILFS_OBJECT_VERSION_CLASSIC);
        payload->reserved = 0;
        header->payload_size = cpu_to_le32(payload_size);
        if (new_count)
            memcpy(payload + 1, new_entries,
                   (size_t)new_count * sizeof(*new_entries));
    }
    ret = infilfs_rw_finalize_object(new_head);
    if (ret)
        goto out;
    ret = infilfs_rw_tx_alloc(tx, 1, &new_head_block);
    if (ret)
        goto out;
    ret = infilfs_native_stage_block(pending->sb, new_head_block, new_head);
    if (ret)
        goto out;
    ret = infilfs_rw_tx_defer_free(tx,
        le64_to_cpu(sbi->disk.object_index_block), 1);
    if (ret)
        goto out;
    tx->next_sb.object_index_block = cpu_to_le64(new_head_block);
    sbi->disk = tx->next_sb;
    ret = 0;
out:
    kvfree(new_entries);
    kvfree(old_entries);
    kfree(page_block);
    kfree(new_head);
    kfree(old_head);
    return ret;
}

static bool infilfs_ns_name_matches(const u8 *record, size_t record_len,
                                    const char *name, size_t name_len)
{
    const struct infilfs_dirent_disk *entry =
        (const struct infilfs_dirent_disk *)record;
    u16 n = le16_to_cpu(entry->name_length);
    return n == name_len && sizeof(*entry) + n <= record_len &&
        memcmp(record + sizeof(*entry), name, n) == 0;
}

/*
 * Complete volatile exact-name locator for the currently hot directory.
 *
 * The first append validates every paged record and builds the hash table.
 * Pure additions keep it coherent; later appends need only the final metadata
 * page.  Removes/renames invalidate it before their compacting rebuild.
 */
static int infilfs_ns_directory_locator_build(
    struct infilfs_native_pending *pending,
    const struct infilfs_object_header_disk *header,
    const struct infilfs_directory_payload_disk *payload)
{
    const __le64 *pages = (const __le64 *)(payload + 1);
    u32 page_count = le32_to_cpu(payload->bytes_used);
    u32 directory_entries = le32_to_cpu(payload->entry_count);
    u8 *page_block = NULL;
    u32 total_entries = 0;
    u32 p;
    int ret;

    infilfs_native_directory_locator_invalidate(pending);
    ret = infilfs_native_directory_locator_ensure(
        pending, directory_entries ? directory_entries : 1u);
    if (ret)
        return ret;
    memset(pending->directory_locators, 0,
           (size_t)pending->directory_locator_capacity *
               sizeof(*pending->directory_locators));
    memcpy(pending->directory_locator_owner_id, header->object_id, 16);

    page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!page_block) {
        ret = -ENOMEM;
        goto out;
    }

    for (p = 0; p < page_count; ++p) {
        struct infilfs_metadata_page_disk *page;
        u32 used;
        u32 expected_entries;
        u32 seen = 0;
        size_t offset = 0;

        ret = infilfs_read_allocated_block(
            pending->sb, le64_to_cpu(pages[p]), page_block);
        if (ret)
            goto out;
        if (!infilfs_metadata_page_valid(
                pending->sb, page_block, infilfs_directory_page_magic,
                header->object_id)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        page = (struct infilfs_metadata_page_disk *)page_block;
        used = le32_to_cpu(page->bytes_used);
        expected_entries = le32_to_cpu(page->entry_count);
        if (used > INFILFS_METADATA_PAGE_DATA_SIZE) {
            ret = -EFSCORRUPTED;
            goto out;
        }

        while (offset < used) {
            struct infilfs_dirent_disk *entry;
            u16 rec;
            u16 name_len;
            const char *entry_name;

            if (used - offset < sizeof(*entry)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            entry = (struct infilfs_dirent_disk *)
                (page_block + sizeof(*page) + offset);
            rec = le16_to_cpu(entry->record_size);
            name_len = le16_to_cpu(entry->name_length);
            if (rec < sizeof(*entry) || rec > used - offset || (rec & 7u) ||
                !name_len || name_len > INFILFS_NAME_MAX ||
                sizeof(*entry) + name_len > rec) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            entry_name = (const char *)entry + sizeof(*entry);
            ret = infilfs_native_directory_locator_insert(
                pending, header->object_id, entry_name, name_len);
            if (ret == -EEXIST)
                ret = -EFSCORRUPTED;
            if (ret)
                goto out;
            offset += rec;
            seen++;
        }
        if (offset != used || seen != expected_entries ||
            total_entries > U32_MAX - seen) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        total_entries += seen;
    }

    if (total_entries != directory_entries) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    pending->directory_locator_valid = true;
    ret = 0;
out:
    kfree(page_block);
    if (ret)
        infilfs_native_directory_locator_invalidate(pending);
    return ret;
}

/*
 * Fast path for pure additions to an already-paged directory.  Directory
 * creation traffic is append-heavy; rebuilding every existing metadata page
 * for each new name turns directory growth into quadratic write amplification.
 * Scan existing pages to preserve duplicate-name and corruption checks, then
 * copy-on-write only the last (or one new) page plus the directory head.
 */
static int infilfs_ns_append_paged_directory(
    struct infilfs_native_pending *pending, struct inode *dir,
    const struct qstr *add_name, const u8 add_id[16], u16 add_type,
    int link_delta, u64 *new_dir_block_out)
{
    struct infilfs_rw_tx *tx = &pending->tx;
    struct infilfs_inode_info *ii = INFILFS_I(dir);
    struct infilfs_object_header_disk *header;
    struct infilfs_directory_payload_disk *payload;
    struct infilfs_metadata_page_disk *page = NULL;
    __le64 *pointers;
    u8 *head = NULL;
    u8 *page_block = NULL;
    size_t add_rec;
    u32 page_count;
    u32 directory_entries;
    u32 last_entries = 0;
    u32 last_used = 0;
    u64 last_page_no = 0;
    u64 new_page_no;
    u64 replacement;
    u64 new_head_block;
    int ret;

    ret = infilfs_ns_validate_name(add_name);
    if (ret)
        return ret;
    add_rec = ALIGN(sizeof(struct infilfs_dirent_disk) + add_name->len, 8);
    if (add_rec > INFILFS_METADATA_PAGE_DATA_SIZE)
        return -ENAMETOOLONG;

    head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!head || !page_block) {
        ret = -ENOMEM;
        goto out;
    }
    ret = infilfs_read_object(pending->sb, ii->object_block,
                              INFILFS_OBJECT_DIRECTORY, ii->object_id, head);
    if (ret)
        goto out;
    header = (struct infilfs_object_header_disk *)head;
    if (le16_to_cpu(header->object_version) != INFILFS_OBJECT_VERSION_PAGED) {
        ret = -EOPNOTSUPP;
        goto out;
    }
    payload = (struct infilfs_directory_payload_disk *)(header + 1);
    page_count = le32_to_cpu(payload->bytes_used);
    directory_entries = le32_to_cpu(payload->entry_count);
    if (page_count > INFILFS_DIRECTORY_PAGE_POINTERS ||
        (directory_entries && !page_count) ||
        le32_to_cpu(header->payload_size) != sizeof(*payload) +
            (size_t)page_count * sizeof(__le64)) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    pointers = (__le64 *)(payload + 1);

    /*
     * Build an exact name set once.  A hot directory then performs O(1)
     * duplicate checks and touches only its final page for each pure append.
     */
    if (!infilfs_native_directory_locator_matches(
            pending, header->object_id, directory_entries)) {
        ret = infilfs_ns_directory_locator_build(pending, header, payload);
        if (ret)
            goto out;
    }
    if (infilfs_native_directory_locator_lookup(
            pending, header->object_id, add_name->name, add_name->len)) {
        ret = -EEXIST;
        goto out;
    }

    if (page_count) {
        last_page_no = le64_to_cpu(pointers[page_count - 1u]);
        ret = infilfs_read_allocated_block(
            pending->sb, last_page_no, page_block);
        if (ret)
            goto out;
        if (!infilfs_metadata_page_valid(
                pending->sb, page_block, infilfs_directory_page_magic,
                header->object_id)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        page = (struct infilfs_metadata_page_disk *)page_block;
        last_entries = le32_to_cpu(page->entry_count);
        last_used = le32_to_cpu(page->bytes_used);
        if (last_used > INFILFS_METADATA_PAGE_DATA_SIZE ||
            last_entries > directory_entries) {
            ret = -EFSCORRUPTED;
            goto out;
        }
    }

    if (directory_entries == U32_MAX) {
        ret = -EOVERFLOW;
        goto out;
    }

    if (page_count && last_used <= INFILFS_METADATA_PAGE_DATA_SIZE - add_rec) {
        struct infilfs_dirent_disk *entry =
            (struct infilfs_dirent_disk *)(page_block + sizeof(*page) + last_used);

        if (last_entries == U32_MAX) {
            ret = -EOVERFLOW;
            goto out;
        }
        memset(entry, 0, add_rec);
        entry->record_size = cpu_to_le16((u16)add_rec);
        entry->name_length = cpu_to_le16((u16)add_name->len);
        entry->object_type = cpu_to_le16(add_type);
        memcpy(entry->object_id, add_id, 16);
        memcpy((u8 *)entry + sizeof(*entry), add_name->name, add_name->len);
        page->entry_count = cpu_to_le32(last_entries + 1u);
        page->bytes_used = cpu_to_le32(last_used + add_rec);
        page->generation = cpu_to_le64(tx->generation);
        ret = infilfs_rw_finalize_page(page_block);
        if (ret)
            goto out;
        ret = infilfs_native_store_private_or_cow(
            pending, last_page_no, page_block, &replacement);
        if (ret)
            goto out;
        pointers[page_count - 1u] = cpu_to_le64(replacement);
    } else {
        struct infilfs_dirent_disk *entry;

        if (page_count >= INFILFS_DIRECTORY_PAGE_POINTERS) {
            ret = -ENOSPC;
            goto out;
        }
        infilfs_rw_init_page(page_block, infilfs_directory_page_magic,
                             header->object_id, tx->generation);
        page = (struct infilfs_metadata_page_disk *)page_block;
        entry = (struct infilfs_dirent_disk *)(page + 1);
        memset(entry, 0, add_rec);
        entry->record_size = cpu_to_le16((u16)add_rec);
        entry->name_length = cpu_to_le16((u16)add_name->len);
        entry->object_type = cpu_to_le16(add_type);
        memcpy(entry->object_id, add_id, 16);
        memcpy((u8 *)entry + sizeof(*entry), add_name->name, add_name->len);
        page->entry_count = cpu_to_le32(1);
        page->bytes_used = cpu_to_le32(add_rec);
        ret = infilfs_rw_finalize_page(page_block);
        if (ret)
            goto out;
        ret = infilfs_rw_tx_alloc(tx, 1, &new_page_no);
        if (ret)
            goto out;
        ret = infilfs_native_stage_block(pending->sb, new_page_no, page_block);
        if (ret)
            goto out;
        pointers[page_count++] = cpu_to_le64(new_page_no);
    }

    payload->entry_count = cpu_to_le32(directory_entries + 1u);
    payload->bytes_used = cpu_to_le32(page_count);
    payload->attributes.modification_time_ns = cpu_to_le64(ktime_get_real_ns());
    payload->attributes.change_time_ns = payload->attributes.modification_time_ns;
    if (link_delta) {
        u64 links = le64_to_cpu(payload->attributes.link_count);
        if ((link_delta < 0 && links < (u64)(-link_delta)) ||
            (link_delta > 0 && links > U64_MAX - (u64)link_delta)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        payload->attributes.link_count = cpu_to_le64(
            link_delta < 0 ? links - (u64)(-link_delta) :
                             links + (u64)link_delta);
    }
    header->generation = cpu_to_le64(tx->generation);
    header->payload_size = cpu_to_le32(sizeof(*payload) +
        (size_t)page_count * sizeof(__le64));
    ret = infilfs_rw_finalize_object(head);
    if (ret)
        goto out;
    ret = infilfs_native_store_private_or_cow(
        pending, ii->object_block, head, &new_head_block);
    if (ret)
        goto out;
    if (memcmp(ii->object_id, tx->sbi->disk.root_object_id, 16) == 0)
        tx->next_sb.root_object_block = cpu_to_le64(new_head_block);
    ret = infilfs_native_directory_locator_insert(
        pending, header->object_id, add_name->name, add_name->len);
    if (ret == -EEXIST)
        ret = -EFSCORRUPTED;
    if (ret)
        goto out;
    pending->directory_locator_valid = true;
    *new_dir_block_out = new_head_block;
    ret = 0;
out:
    kfree(page_block);
    kfree(head);
    return ret;
}

static int infilfs_ns_rebuild_directory(
    struct infilfs_native_pending *pending, struct inode *dir,
    const struct qstr *remove_a, const struct qstr *remove_b,
    const struct qstr *add_name, const u8 add_id[16], u16 add_type,
    int link_delta, u64 *new_dir_block_out)
{
    struct infilfs_rw_tx *tx = &pending->tx;
    struct infilfs_inode_info *ii = INFILFS_I(dir);
    u8 *old_head = NULL, *new_head = NULL, *page_block = NULL;
    u8 *all = NULL, *out_bytes = NULL;
    size_t bytes = 0, out_len = 0, offset = 0, add_rec = 0;
    u32 count = 0, out_count = 0;
    bool found_a = !remove_a, found_b = !remove_b;
    bool paged;
    struct infilfs_object_header_disk *header;
    struct infilfs_directory_payload_disk *payload;
    u64 new_head_block;
    int ret;

    ret = infilfs_native_tree_directory_update(
        pending, dir, remove_a, remove_b, add_name, add_id, add_type,
        link_delta, new_dir_block_out);
    if (ret != -EOPNOTSUPP)
        return ret;

    if (!remove_a && !remove_b && add_name) {
        ret = infilfs_ns_append_paged_directory(
            pending, dir, add_name, add_id, add_type, link_delta,
            new_dir_block_out);
        if (ret != -EOPNOTSUPP)
            return ret;
    }

    /* Removes, renames and classic rebuilds invalidate the exact-name view. */
    infilfs_native_directory_locator_invalidate(pending);

    old_head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    new_head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!old_head || !new_head || !page_block) {
        ret = -ENOMEM;
        goto out;
    }
    ret = infilfs_rw_collect_directory(tx, dir, old_head, &all, &bytes, &count);
    if (ret)
        goto out;
    if (add_name) {
        ret = infilfs_ns_validate_name(add_name);
        if (ret)
            goto out;
        add_rec = ALIGN(sizeof(struct infilfs_dirent_disk) + add_name->len, 8);
    }
    out_bytes = kvmalloc(bytes + add_rec + 1u, GFP_NOFS);
    if (!out_bytes) {
        ret = -ENOMEM;
        goto out;
    }
    while (offset < bytes) {
        struct infilfs_dirent_disk *entry;
        u16 rec;
        bool remove = false;

        if (bytes - offset < sizeof(*entry)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        entry = (struct infilfs_dirent_disk *)(all + offset);
        rec = le16_to_cpu(entry->record_size);
        if (rec < sizeof(*entry) || rec > bytes - offset || (rec & 7u)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        if (remove_a && infilfs_ns_name_matches(all + offset, rec,
                                                remove_a->name, remove_a->len)) {
            found_a = true;
            remove = true;
        }
        if (remove_b && infilfs_ns_name_matches(all + offset, rec,
                                                remove_b->name, remove_b->len)) {
            found_b = true;
            remove = true;
        }
        if (!remove && add_name &&
            infilfs_ns_name_matches(all + offset, rec,
                                    add_name->name, add_name->len)) {
            ret = -EEXIST;
            goto out;
        }
        if (!remove) {
            memcpy(out_bytes + out_len, all + offset, rec);
            out_len += rec;
            out_count++;
        }
        offset += rec;
    }
    if (offset != bytes || !found_a || !found_b) {
        ret = !found_a || !found_b ? -ENOENT : -EFSCORRUPTED;
        goto out;
    }
    if (add_name) {
        struct infilfs_dirent_disk *entry =
            (struct infilfs_dirent_disk *)(out_bytes + out_len);
        memset(entry, 0, add_rec);
        entry->record_size = cpu_to_le16((u16)add_rec);
        entry->name_length = cpu_to_le16((u16)add_name->len);
        entry->object_type = cpu_to_le16(add_type);
        memcpy(entry->object_id, add_id, 16);
        memcpy((u8 *)entry + sizeof(*entry), add_name->name, add_name->len);
        out_len += add_rec;
        out_count++;
    }

    header = (struct infilfs_object_header_disk *)old_head;
    paged = le16_to_cpu(header->object_version) == INFILFS_OBJECT_VERSION_PAGED ||
        (le64_to_cpu(tx->next_sb.incompat_flags) & INFILFS_INCOMPAT_PAGED_METADATA);
    memset(new_head, 0, INFILFS_DISK_BLOCK_SIZE);
    memcpy(new_head, old_head, sizeof(*header) + sizeof(*payload));
    header = (struct infilfs_object_header_disk *)new_head;
    payload = (struct infilfs_directory_payload_disk *)(header + 1);
    header->generation = cpu_to_le64(tx->generation);
    payload->entry_count = cpu_to_le32(out_count);
    payload->attributes.modification_time_ns = cpu_to_le64(ktime_get_real_ns());
    payload->attributes.change_time_ns = payload->attributes.modification_time_ns;
    if (link_delta) {
        u64 links = le64_to_cpu(payload->attributes.link_count);
        if ((link_delta < 0 && links < (u64)(-link_delta)) ||
            (link_delta > 0 && links > U64_MAX - (u64)link_delta)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        payload->attributes.link_count = cpu_to_le64(
            link_delta < 0 ? links - (u64)(-link_delta) :
                             links + (u64)link_delta);
    }

    if (paged) {
        __le64 *pointers = (__le64 *)(payload + 1);
        size_t pos = 0;
        u32 page_count = 0;
        header->object_version = cpu_to_le16(INFILFS_OBJECT_VERSION_PAGED);
        while (pos < out_len) {
            struct infilfs_metadata_page_disk *page;
            size_t used = 0;
            u32 page_entries = 0;
            u64 page_no;
            if (page_count >= INFILFS_DIRECTORY_PAGE_POINTERS) {
                ret = -ENOSPC;
                goto out;
            }
            infilfs_rw_init_page(page_block, infilfs_directory_page_magic,
                                 header->object_id, tx->generation);
            while (pos < out_len) {
                struct infilfs_dirent_disk *entry =
                    (struct infilfs_dirent_disk *)(out_bytes + pos);
                u16 rec = le16_to_cpu(entry->record_size);
                if (!rec || rec > out_len - pos ||
                    rec > INFILFS_METADATA_PAGE_DATA_SIZE) {
                    ret = -EFSCORRUPTED;
                    goto out;
                }
                if (used && used + rec > INFILFS_METADATA_PAGE_DATA_SIZE)
                    break;
                memcpy(page_block + sizeof(*page) + used, out_bytes + pos, rec);
                used += rec;
                pos += rec;
                page_entries++;
            }
            page = (struct infilfs_metadata_page_disk *)page_block;
            page->entry_count = cpu_to_le32(page_entries);
            page->bytes_used = cpu_to_le32(used);
            ret = infilfs_rw_finalize_page(page_block);
            if (ret)
                goto out;
            ret = infilfs_rw_tx_alloc(tx, 1, &page_no);
            if (ret)
                goto out;
            ret = infilfs_native_stage_block(pending->sb, page_no, page_block);
            if (ret)
                goto out;
            pointers[page_count++] = cpu_to_le64(page_no);
        }
        payload->bytes_used = cpu_to_le32(page_count);
        header->payload_size = cpu_to_le32(sizeof(*payload) +
            (size_t)page_count * sizeof(__le64));
    } else {
        if (sizeof(*payload) + out_len >
            INFILFS_DISK_BLOCK_SIZE - sizeof(*header)) {
            ret = -ENOSPC;
            goto out;
        }
        header->object_version = cpu_to_le16(INFILFS_OBJECT_VERSION_CLASSIC);
        payload->bytes_used = cpu_to_le32(out_len);
        header->payload_size = cpu_to_le32(sizeof(*payload) + out_len);
        if (out_len)
            memcpy(payload + 1, out_bytes, out_len);
    }
    ret = infilfs_rw_finalize_object(new_head);
    if (ret)
        goto out;
    ret = infilfs_rw_tx_alloc(tx, 1, &new_head_block);
    if (ret)
        goto out;
    ret = infilfs_native_stage_block(pending->sb, new_head_block, new_head);
    if (ret)
        goto out;
    ret = infilfs_rw_tx_defer_free(tx, ii->object_block, 1);
    if (ret)
        goto out;
    if (memcmp(ii->object_id, tx->sbi->disk.root_object_id, 16) == 0)
        tx->next_sb.root_object_block = cpu_to_le64(new_head_block);
    *new_dir_block_out = new_head_block;
    ret = 0;
out:
    kvfree(out_bytes);
    kvfree(all);
    kfree(page_block);
    kfree(new_head);
    kfree(old_head);
    return ret;
}

static int infilfs_ns_cow_object(struct infilfs_native_pending *pending,
                                 struct inode *inode,
                                 const u8 new_parent_id[16],
                                 s64 link_delta, bool clear_parent,
                                 u64 *new_block_out)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    u8 *object;
    struct infilfs_object_header_disk *header;
    struct infilfs_attributes_disk *attributes;
    u64 links;
    u64 new_block;
    int ret;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;
    ret = infilfs_read_object(inode->i_sb, ii->object_block,
                              ii->object_type, ii->object_id, object);
    if (ret)
        goto out;
    header = (struct infilfs_object_header_disk *)object;
    attributes = infilfs_ns_attributes_mut(object);
    if (!attributes) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    links = le64_to_cpu(attributes->link_count);
    if ((link_delta < 0 && links < (u64)(-link_delta)) ||
        (link_delta > 0 && links > U64_MAX - (u64)link_delta)) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    if (link_delta)
        attributes->link_count = cpu_to_le64(
            link_delta < 0 ? links - (u64)(-link_delta) :
                             links + (u64)link_delta);
    if (clear_parent)
        memset(header->parent_id, 0, sizeof(header->parent_id));
    else if (new_parent_id)
        memcpy(header->parent_id, new_parent_id, 16);
    attributes->change_time_ns = cpu_to_le64(ktime_get_real_ns());
    header->generation = cpu_to_le64(pending->tx.generation);
    ret = infilfs_rw_finalize_object(object);
    if (ret)
        goto out;
    ret = infilfs_rw_tx_alloc(&pending->tx, 1, &new_block);
    if (ret)
        goto out;
    ret = infilfs_native_stage_block(pending->sb, new_block, object);
    if (ret)
        goto out;
    ret = infilfs_rw_tx_defer_free(&pending->tx, ii->object_block, 1);
    if (!ret)
        *new_block_out = new_block;
out:
    kfree(object);
    return ret;
}

static int infilfs_ns_read_file_extents(
    struct super_block *sb, u64 object_block, const u8 object_id[16],
    struct infilfs_extent_disk **extents_out, u32 *count_out)
{
    u8 *object = NULL, *page_block = NULL;
    struct infilfs_object_header_disk *header;
    struct infilfs_file_payload_disk *file;
    struct infilfs_extent_disk *extents = NULL;
    u32 count, copied = 0;
    int ret;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;
    ret = infilfs_read_object(sb, object_block, INFILFS_OBJECT_FILE,
                              object_id, object);
    if (ret)
        goto out;
    header = (struct infilfs_object_header_disk *)object;
    file = (struct infilfs_file_payload_disk *)(header + 1);
    count = le32_to_cpu(file->extent_count);
    if (!count) {
        *extents_out = NULL;
        *count_out = 0;
        ret = 0;
        goto out;
    }
    extents = kvmalloc_array(count, sizeof(*extents), GFP_NOFS);
    if (!extents) {
        ret = -ENOMEM;
        goto out;
    }
    if (le16_to_cpu(header->object_version) == INFILFS_OBJECT_VERSION_CLASSIC) {
        size_t need = sizeof(*file) + (size_t)count * sizeof(*extents);
        if (need != le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        memcpy(extents, file + 1, (size_t)count * sizeof(*extents));
        copied = count;
    } else if (le16_to_cpu(header->object_version) == INFILFS_OBJECT_VERSION_PAGED) {
        struct infilfs_extent_head_disk *head =
            (struct infilfs_extent_head_disk *)(file + 1);
        __le64 *pages = (__le64 *)(head + 1);
        u32 page_count = le32_to_cpu(head->page_count);
        u32 p;
        if (!page_count || page_count > INFILFS_EXTENT_PAGE_POINTERS ||
            le32_to_cpu(head->reserved) != 0) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!page_block) {
            ret = -ENOMEM;
            goto out;
        }
        for (p = 0; p < page_count; ++p) {
            struct infilfs_metadata_page_disk *page;
            u32 n;
            ret = infilfs_read_allocated_block(
                sb, le64_to_cpu(pages[p]), page_block);
            if (ret)
                goto out;
            if (!infilfs_metadata_page_valid(sb, page_block,
                                             infilfs_extent_page_magic,
                                             object_id)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            page = (struct infilfs_metadata_page_disk *)page_block;
            n = le32_to_cpu(page->entry_count);
            if (!n || n > INFILFS_EXTENTS_PER_PAGE || copied > count ||
                n > count - copied ||
                le32_to_cpu(page->bytes_used) != n * sizeof(*extents)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            memcpy(extents + copied, page + 1, (size_t)n * sizeof(*extents));
            copied += n;
        }
    } else {
        ret = -EFSCORRUPTED;
        goto out;
    }
    if (copied != count) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    *extents_out = extents;
    *count_out = count;
    extents = NULL;
    ret = 0;
out:
    kvfree(extents);
    kfree(page_block);
    kfree(object);
    return ret;
}

static int infilfs_ns_other_reference_cover(
    struct super_block *sb, const u8 owner_id[16], u64 cursor, u64 end,
    u64 *cover_end, u64 *next_start)
{
    struct infilfs_index_entry_disk *entries = NULL;
    u32 count = 0, i;
    int ret;

    *cover_end = cursor;
    *next_start = end;
    ret = infilfs_ns_index_snapshot(sb, &entries, &count);
    if (ret)
        return ret;
    for (i = 0; i < count; ++i) {
        struct infilfs_extent_disk *extents = NULL;
        u32 extent_count = 0, j;
        if (le16_to_cpu(entries[i].object_type) != INFILFS_OBJECT_FILE ||
            memcmp(entries[i].object_id, owner_id, 16) == 0)
            continue;
        ret = infilfs_ns_read_file_extents(
            sb, le64_to_cpu(entries[i].object_block), entries[i].object_id,
            &extents, &extent_count);
        if (ret) {
            kvfree(entries);
            return ret;
        }
        for (j = 0; j < extent_count; ++j) {
            u64 physical, extent_end, blocks;
            u32 flags = le32_to_cpu(extents[j].flags);
            u32 logical_blocks = le32_to_cpu(extents[j].block_count);

            if (infilfs_extent_kind(flags) != INFILFS_EXTENT_NORMAL)
                continue;
            physical = le64_to_cpu(extents[j].physical_block);
            blocks = infilfs_extent_physical_blocks(logical_blocks, flags);
            extent_end = physical + blocks;
            if (extent_end <= cursor || physical >= end)
                continue;
            if (physical <= cursor) {
                u64 candidate = min_t(u64, extent_end, end);
                if (candidate > *cover_end)
                    *cover_end = candidate;
            } else if (physical < *next_start) {
                *next_start = physical;
            }
        }
        kvfree(extents);
    }
    kvfree(entries);
    return 0;
}

static int infilfs_ns_free_unshared_run(
    struct infilfs_native_pending *pending, const u8 owner_id[16],
    u64 start, u64 count)
{
    u64 cursor, end;
    int ret;

    if (!count)
        return 0;
    if (start > U64_MAX - count)
        return -EOVERFLOW;
    cursor = start;
    end = start + count;
    while (cursor < end) {
        u64 cover_end, next_start;
        ret = infilfs_ns_other_reference_cover(
            pending->sb, owner_id, cursor, end, &cover_end, &next_start);
        if (ret)
            return ret;
        if (cover_end > cursor) {
            cursor = cover_end;
            continue;
        }
        if (next_start <= cursor || next_start > end)
            return -EFSCORRUPTED;
        ret = infilfs_rw_tx_defer_free(&pending->tx,
                                       cursor, next_start - cursor);
        if (ret)
            return ret;
        cursor = next_start;
    }
    return 0;
}

static int infilfs_ns_changes_append(
    struct infilfs_ns_index_change **changes, u32 *count, u32 *capacity,
    const u8 id[16], u8 action, u64 block, u16 type)
{
    struct infilfs_ns_index_change *grown;
    u32 new_capacity;

    if (infilfs_ns_change_find(*changes, *count, id) >= 0)
        return -EEXIST;
    if (*count == *capacity) {
        new_capacity = *capacity ? *capacity * 2u : 16u;
        grown = kvmalloc_array(new_capacity, sizeof(**changes), GFP_NOFS);
        if (!grown)
            return -ENOMEM;
        if (*count)
            memcpy(grown, *changes, (size_t)*count * sizeof(**changes));
        kvfree(*changes);
        *changes = grown;
        *capacity = new_capacity;
    }
    memset(&(*changes)[*count], 0, sizeof(**changes));
    memcpy((*changes)[*count].object_id, id, 16);
    (*changes)[*count].action = action;
    (*changes)[*count].object_block = block;
    (*changes)[*count].object_type = type;
    (*count)++;
    return 0;
}

static int infilfs_ns_collect_checksum_removals(
    struct infilfs_native_pending *pending,
    const struct infilfs_file_payload_disk *file, const u8 owner_id[16],
    struct infilfs_ns_index_change **changes, u32 *change_count,
    u32 *change_capacity)
{
    u8 current[16];
    unsigned int guard;
    int ret;

    memcpy(current, file->checksum_head_id, 16);
    for (guard = 0; memchr_inv(current, 0, 16) && guard < 1048576u; ++guard) {
        u64 block;
        u16 type;
        u8 *object;
        struct infilfs_native_checksum_payload_disk *payload;
        u8 next[16];

        ret = infilfs_index_lookup(pending->sb, current, &block, &type);
        if (ret)
            return ret;
        if (type != INFILFS_OBJECT_CHECKSUM)
            return -EFSCORRUPTED;
        object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!object)
            return -ENOMEM;
        ret = infilfs_native_checksum_decode(pending->sb, owner_id, current,
                                             block, object, &payload);
        if (!ret)
            memcpy(next, payload->next_object_id, 16);
        kfree(object);
        if (ret)
            return ret;
        ret = infilfs_ns_changes_append(changes, change_count, change_capacity,
                                        current, INFILFS_NS_REMOVE, 0,
                                        INFILFS_OBJECT_CHECKSUM);
        if (ret)
            return ret;
        ret = infilfs_rw_tx_defer_free(&pending->tx, block, 1);
        if (ret)
            return ret;
        memcpy(current, next, 16);
    }
    return memchr_inv(current, 0, 16) ? -ELOOP : 0;
}

static int infilfs_ns_delete_file_resources(
    struct infilfs_native_pending *pending, struct inode *inode,
    struct infilfs_ns_index_change **changes, u32 *change_count,
    u32 *change_capacity)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    u8 *object;
    struct infilfs_object_header_disk *header;
    struct infilfs_file_payload_disk *file;
    struct infilfs_extent_disk *extents = NULL;
    u32 extent_count = 0, i;
    u64 old_blocks = 0;
    bool was_inline = false;
    int ret;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;
    ret = infilfs_native_collect_extents(pending, inode, object, &extents,
                                         &extent_count, &old_blocks,
                                         &was_inline);
    if (ret)
        goto out;
    (void)old_blocks;
    (void)was_inline;
    header = (struct infilfs_object_header_disk *)object;
    file = (struct infilfs_file_payload_disk *)(header + 1);
    ret = infilfs_ns_collect_checksum_removals(
        pending, file, ii->object_id, changes, change_count, change_capacity);
    if (ret)
        goto out;
    for (i = 0; i < extent_count; ++i) {
        u32 flags = le32_to_cpu(extents[i].flags);
        u32 logical_blocks = le32_to_cpu(extents[i].block_count);

        if (infilfs_extent_kind(flags) != INFILFS_EXTENT_NORMAL)
            continue;
        ret = infilfs_ns_free_unshared_run(
            pending, ii->object_id,
            le64_to_cpu(extents[i].physical_block),
            infilfs_extent_physical_blocks(logical_blocks, flags));
        if (ret)
            goto out;
    }

    if (le16_to_cpu(header->object_version) ==
        INFILFS_OBJECT_VERSION_PAGED) {
        struct infilfs_extent_head_disk *head =
            (struct infilfs_extent_head_disk *)(file + 1);
        __le64 *pages = (__le64 *)(head + 1);
        u32 page_count = le32_to_cpu(head->page_count);

        if (!page_count || page_count > INFILFS_EXTENT_PAGE_POINTERS ||
            le32_to_cpu(head->reserved) != 0 ||
            sizeof(*file) + sizeof(*head) +
                (size_t)page_count * sizeof(__le64) !=
                    le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        for (i = 0; i < page_count; ++i) {
            ret = infilfs_rw_tx_defer_free(
                &pending->tx, le64_to_cpu(pages[i]), 1);
            if (ret)
                goto out;
        }
    }

    ret = infilfs_rw_tx_defer_free(&pending->tx, ii->object_block, 1);
    if (ret)
        goto out;
    ret = infilfs_ns_changes_append(changes, change_count, change_capacity,
                                    ii->object_id, INFILFS_NS_REMOVE, 0,
                                    INFILFS_OBJECT_FILE);
out:
    kvfree(extents);
    kfree(object);
    return ret;
}

static int infilfs_ns_defer_directory_pages(
    struct infilfs_native_pending *pending, struct inode *inode)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    u8 *object;
    struct infilfs_object_header_disk *header;
    struct infilfs_directory_payload_disk *payload;
    int ret = 0;
    u32 p;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;
    ret = infilfs_read_object(inode->i_sb, ii->object_block,
                              INFILFS_OBJECT_DIRECTORY, ii->object_id, object);
    if (ret)
        goto out;
    header = (struct infilfs_object_header_disk *)object;
    payload = (struct infilfs_directory_payload_disk *)(header + 1);
    if (le32_to_cpu(payload->entry_count) != 0) {
        ret = -ENOTEMPTY;
        goto out;
    }
    if (le16_to_cpu(header->object_version) == INFILFS_OBJECT_VERSION_PAGED) {
        __le64 *pages = (__le64 *)(payload + 1);
        u32 page_count = le32_to_cpu(payload->bytes_used);
        if (page_count > INFILFS_DIRECTORY_PAGE_POINTERS) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        for (p = 0; p < page_count; ++p) {
            ret = infilfs_rw_tx_defer_free(&pending->tx,
                                           le64_to_cpu(pages[p]), 1);
            if (ret)
                goto out;
        }
    }
    ret = infilfs_rw_tx_defer_free(&pending->tx, ii->object_block, 1);
out:
    kfree(object);
    return ret;
}

static int infilfs_ns_delete_object(
    struct infilfs_native_pending *pending, struct inode *inode,
    struct infilfs_ns_index_change **changes, u32 *change_count,
    u32 *change_capacity, bool *retained_out, u64 *retained_block_out)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    int ret;

    *retained_out = false;
    *retained_block_out = 0;
    if (ii->object_type == INFILFS_OBJECT_FILE) {
        u8 *object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        struct infilfs_file_payload_disk *file;
        u64 links;
        if (!object)
            return -ENOMEM;
        ret = infilfs_read_object(inode->i_sb, ii->object_block,
                                  INFILFS_OBJECT_FILE, ii->object_id, object);
        if (ret) {
            kfree(object);
            return ret;
        }
        file = (struct infilfs_file_payload_disk *)
            ((struct infilfs_object_header_disk *)object + 1);
        links = le64_to_cpu(file->attributes.link_count);
        kfree(object);
        if (!links)
            return -EFSCORRUPTED;
        /*
         * Keep the object indexed even when this is its final directory link.
         * Linux may still have open struct file references after unlink (or
         * after rename replaces a destination).  Persisting link_count == 0
         * here lets those handles continue to address stable CoW data.  The
         * final object/checksum/extent reclamation is performed from
         * ->evict_inode after the VFS releases the last reference.
         */
        if (links >= 1u) {
            ret = infilfs_ns_cow_object(pending, inode, NULL, -1, true,
                                        retained_block_out);
            if (ret)
                return ret;
            *retained_out = true;
            return infilfs_ns_changes_append(
                changes, change_count, change_capacity, ii->object_id,
                INFILFS_NS_REPOINT, *retained_block_out, INFILFS_OBJECT_FILE);
        }
    }
    if (ii->object_type == INFILFS_OBJECT_DIRECTORY) {
        ret = infilfs_ns_defer_directory_pages(pending, inode);
        if (ret)
            return ret;
    } else if (ii->object_type == INFILFS_OBJECT_SYMLINK) {
        ret = infilfs_rw_tx_defer_free(&pending->tx, ii->object_block, 1);
        if (ret)
            return ret;
    } else {
        return -EOPNOTSUPP;
    }
    return infilfs_ns_changes_append(
        changes, change_count, change_capacity, ii->object_id,
        INFILFS_NS_REMOVE, 0, ii->object_type);
}

/* Reclaim an unlinked regular file only after its final VFS reference dies. */
static int infilfs_ns_evict_unlinked_file(struct inode *inode)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_native_pending *pending;
    struct infilfs_ns_index_change *changes = NULL;
    u32 change_count = 0;
    u32 change_capacity = 0;
    int ret;

    if (!ii || ii->object_type != INFILFS_OBJECT_FILE || inode->i_nlink != 0 ||
        sb_rdonly(inode->i_sb))
        return 0;

    ret = infilfs_ns_begin(inode->i_sb, &pending);
    if (ret)
        return ret;
    ret = infilfs_ns_delete_file_resources(
        pending, inode, &changes, &change_count, &change_capacity);
    if (!ret)
        ret = infilfs_ns_rebuild_index(pending, changes, change_count);
    ret = infilfs_ns_finish(pending, ret);
    kvfree(changes);
    return ret;
}

static int infilfs_ns_create_symlink_object(
    struct infilfs_native_pending *pending, struct inode *parent,
    const char *target, u8 object_id[16], u64 *object_block_out)
{
    struct infilfs_inode_info *parent_ii = INFILFS_I(parent);
    struct infilfs_object_header_disk *header;
    struct infilfs_symlink_payload_disk *payload;
    size_t length;
    u8 *object;
    u64 block;
    int ret;

    if (!target)
        return -EINVAL;
    length = strnlen(target, INFILFS_DISK_BLOCK_SIZE);
    if (!length)
        return -EINVAL;
    if (length > INFILFS_DISK_BLOCK_SIZE - sizeof(*header) - sizeof(*payload))
        return -ENAMETOOLONG;
    if (!infilfs_rw_utf8_valid(target, length))
        return -EINVAL;
    ret = infilfs_rw_generate_id(parent->i_sb, object_id);
    if (ret)
        return ret;
    object = kzalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;
    header = (struct infilfs_object_header_disk *)object;
    memcpy(header->magic, infilfs_object_magic, 8);
    header->object_type = cpu_to_le16(INFILFS_OBJECT_SYMLINK);
    header->object_version = cpu_to_le16(INFILFS_OBJECT_VERSION_CLASSIC);
    header->header_size = cpu_to_le32(sizeof(*header));
    header->generation = cpu_to_le64(pending->tx.generation);
    memcpy(header->object_id, object_id, 16);
    memcpy(header->parent_id, parent_ii->object_id, 16);
    header->checksum_type = cpu_to_le32(INFILFS_CHECKSUM_CRC64_ECMA);
    payload = (struct infilfs_symlink_payload_disk *)(header + 1);
    infilfs_rw_fill_common_attributes(&payload->attributes, &payload->posix,
                                      0777, 1);
    payload->attributes.logical_size = cpu_to_le64(length);
    payload->target_length = cpu_to_le32((u32)length);
    memcpy(payload + 1, target, length);
    header->payload_size = cpu_to_le32(sizeof(*payload) + length);
    ret = infilfs_rw_finalize_object(object);
    if (ret)
        goto out;
    ret = infilfs_rw_tx_alloc(&pending->tx, 1, &block);
    if (ret)
        goto out;
    ret = infilfs_native_stage_block(pending->sb, block, object);
    if (!ret)
        *object_block_out = block;
out:
    kfree(object);
    return ret;
}

static int infilfs_ns_is_ancestor(struct super_block *sb,
                                  const u8 ancestor[16], const u8 start[16])
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    u8 current[16];
    unsigned int depth;

    memcpy(current, start, 16);
    for (depth = 0; depth < 1024u; ++depth) {
        u64 block;
        u16 type;
        u8 *object;
        struct infilfs_object_header_disk *header;
        int ret;
        if (memcmp(current, ancestor, 16) == 0)
            return 1;
        if (memcmp(current, sbi->disk.root_object_id, 16) == 0)
            return 0;
        ret = infilfs_index_lookup(sb, current, &block, &type);
        if (ret)
            return ret;
        if (type != INFILFS_OBJECT_DIRECTORY)
            return -EFSCORRUPTED;
        object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!object)
            return -ENOMEM;
        ret = infilfs_read_object(sb, block, type, current, object);
        if (ret) {
            kfree(object);
            return ret;
        }
        header = (struct infilfs_object_header_disk *)object;
        if (!memchr_inv(header->parent_id, 0, 16)) {
            kfree(object);
            return 0;
        }
        memcpy(current, header->parent_id, 16);
        kfree(object);
    }
    return -ELOOP;
}

static int infilfs_ns_unlink(struct inode *dir, struct dentry *dentry)
{
    struct inode *inode = d_inode(dentry);
    struct infilfs_inode_info *dir_ii = INFILFS_I(dir);
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_native_pending *pending;
    struct infilfs_quota_reservation released_quota = {0};
    struct infilfs_ns_index_change *changes = NULL;
    u32 change_count = 0, change_capacity = 0;
    u64 new_dir_block = 0, retained_block = 0;
    u64 released_bytes = 0;
    bool retained = false;
    bool quota_locked = false;
    int ret;

    if (!inode || !ii || ii->object_type == INFILFS_OBJECT_DIRECTORY)
        return -EISDIR;
    if (ii->object_type == INFILFS_OBJECT_SYMLINK) {
        mutex_lock(&INFILFS_SB(dir->i_sb)->quota_lock);
        quota_locked = true;
        ret = infilfs_quota_capture_inode_locked(inode, &released_quota);
        if (ret) {
            mutex_unlock(&INFILFS_SB(dir->i_sb)->quota_lock);
            return ret;
        }
        released_bytes = (u64)i_size_read(inode);
    }
    ret = infilfs_ns_begin(dir->i_sb, &pending);
    if (ret) {
        if (quota_locked)
            mutex_unlock(&INFILFS_SB(dir->i_sb)->quota_lock);
        return ret;
    }
    ret = infilfs_ns_delete_object(pending, inode, &changes, &change_count,
                                   &change_capacity, &retained, &retained_block);
    if (!ret)
        ret = infilfs_ns_rebuild_directory(pending, dir, &dentry->d_name, NULL,
                                           NULL, NULL, 0, 0, &new_dir_block);
    if (!ret)
        ret = infilfs_ns_changes_append(&changes, &change_count, &change_capacity,
                                        dir_ii->object_id, INFILFS_NS_REPOINT,
                                        new_dir_block, INFILFS_OBJECT_DIRECTORY);
    if (!ret)
        ret = infilfs_ns_rebuild_index(pending, changes, change_count);
    ret = infilfs_ns_finish(pending, ret);
    if (quota_locked) {
        if (!ret && released_quota.active)
            infilfs_quota_adjust_locked(
                &released_quota, released_bytes, 1);
        released_quota.active = false;
        if (!ret &&
            infilfs_quota_remove_project_root_locked(
                dir->i_sb, ii->object_id)) {
            int quota_policy_ret =
                infilfs_quota_persist_locked(dir->i_sb);

            if (quota_policy_ret) {
                INFILFS_SB(dir->i_sb)->write_poisoned = true;
                pr_err("InfiltratorFS: removed project root could not be persisted after rmdir: %d\n",
                       quota_policy_ret);
            }
        }
        mutex_unlock(&INFILFS_SB(dir->i_sb)->quota_lock);
        quota_locked = false;
    }
    if (!ret) {
        dir_ii->object_block = new_dir_block;
        infilfs_refresh_inode_blocks_after_commit(dir);
        if (retained) {
            ii->object_block = retained_block;
            drop_nlink(inode);
        } else {
            clear_nlink(inode);
        }
    }
    kvfree(changes);
    return ret;
}

static int infilfs_ns_rmdir(struct inode *dir, struct dentry *dentry)
{
    struct inode *inode = d_inode(dentry);
    struct infilfs_inode_info *dir_ii = INFILFS_I(dir);
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_native_pending *pending;
    struct infilfs_quota_reservation released_quota = {0};
    struct infilfs_ns_index_change *changes = NULL;
    u32 change_count = 0, change_capacity = 0;
    u64 new_dir_block = 0, retained_block = 0;
    u64 released_bytes = 0;
    bool retained = false;
    bool quota_locked = false;
    int ret;

    if (!inode || !ii || ii->object_type != INFILFS_OBJECT_DIRECTORY)
        return -ENOTDIR;

    mutex_lock(&INFILFS_SB(dir->i_sb)->quota_lock);
    quota_locked = true;
    ret = infilfs_quota_capture_inode_locked(inode, &released_quota);
    if (ret) {
        mutex_unlock(&INFILFS_SB(dir->i_sb)->quota_lock);
        return ret;
    }
    released_bytes = (u64)i_size_read(inode);

    ret = infilfs_ns_begin(dir->i_sb, &pending);
    if (ret) {
        mutex_unlock(&INFILFS_SB(dir->i_sb)->quota_lock);
        return ret;
    }
    ret = infilfs_ns_delete_object(pending, inode, &changes, &change_count,
                                   &change_capacity, &retained, &retained_block);
    if (!ret)
        ret = infilfs_ns_rebuild_directory(pending, dir, &dentry->d_name, NULL,
                                           NULL, NULL, 0, -1, &new_dir_block);
    if (!ret)
        ret = infilfs_ns_changes_append(&changes, &change_count, &change_capacity,
                                        dir_ii->object_id, INFILFS_NS_REPOINT,
                                        new_dir_block, INFILFS_OBJECT_DIRECTORY);
    if (!ret)
        ret = infilfs_ns_rebuild_index(pending, changes, change_count);
    ret = infilfs_ns_finish(pending, ret);
    if (quota_locked) {
        if (!ret && released_quota.active)
            infilfs_quota_adjust_locked(
                &released_quota, released_bytes, 1);
        released_quota.active = false;
        mutex_unlock(&INFILFS_SB(dir->i_sb)->quota_lock);
        quota_locked = false;
    }
    if (!ret) {
        dir_ii->object_block = new_dir_block;
        infilfs_refresh_inode_blocks_after_commit(dir);
        drop_nlink(dir);
        clear_nlink(inode);
    }
    kvfree(changes);
    return ret;
}

static int infilfs_ns_link(struct dentry *old_dentry, struct inode *dir,
                           struct dentry *dentry)
{
    struct inode *inode = d_inode(old_dentry);
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_inode_info *dir_ii = INFILFS_I(dir);
    struct infilfs_native_pending *pending;
    struct infilfs_ns_index_change changes[2];
    u64 new_file_block = 0, new_dir_block = 0;
    int ret;

    if (!inode || !ii || ii->object_type != INFILFS_OBJECT_FILE)
        return -EPERM;
    if (!(le64_to_cpu(INFILFS_SB(dir->i_sb)->disk.incompat_flags) &
          INFILFS_INCOMPAT_HARD_LINKS))
        return -EOPNOTSUPP;
    ret = infilfs_ns_validate_name(&dentry->d_name);
    if (ret)
        return ret;
    ret = infilfs_quota_hardlink_check(inode, dir);
    if (ret)
        return ret;
    ret = infilfs_ns_begin(dir->i_sb, &pending);
    if (ret)
        return ret;
    ret = infilfs_ns_cow_object(pending, inode, NULL, 1, true, &new_file_block);
    if (!ret)
        ret = infilfs_ns_rebuild_directory(pending, dir, NULL, NULL,
                                           &dentry->d_name, ii->object_id,
                                           INFILFS_OBJECT_FILE, 0,
                                           &new_dir_block);
    if (!ret) {
        memset(changes, 0, sizeof(changes));
        memcpy(changes[0].object_id, ii->object_id, 16);
        changes[0].action = INFILFS_NS_REPOINT;
        changes[0].object_block = new_file_block;
        changes[0].object_type = INFILFS_OBJECT_FILE;
        memcpy(changes[1].object_id, dir_ii->object_id, 16);
        changes[1].action = INFILFS_NS_REPOINT;
        changes[1].object_block = new_dir_block;
        changes[1].object_type = INFILFS_OBJECT_DIRECTORY;
        ret = infilfs_ns_rebuild_index(pending, changes, ARRAY_SIZE(changes));
    }
    ret = infilfs_ns_finish(pending, ret);
    if (!ret) {
        ii->object_block = new_file_block;
        dir_ii->object_block = new_dir_block;
        infilfs_refresh_inode_blocks_after_commit(dir);
        inc_nlink(inode);
        ihold(inode);
        d_instantiate(dentry, inode);
    }
    return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_ns_symlink(struct mnt_idmap *idmap, struct inode *dir,
                              struct dentry *dentry, const char *target)
#else
static int infilfs_ns_symlink(struct user_namespace *idmap, struct inode *dir,
                              struct dentry *dentry, const char *target)
#endif
{
    struct infilfs_inode_info *dir_ii = INFILFS_I(dir);
    struct infilfs_native_pending *pending;
    struct infilfs_ns_index_change changes[2];
    struct inode *inode;
    u8 object_id[16];
    u64 object_block = 0, new_dir_block = 0;
    int ret;

    (void)idmap;
    if (!(le64_to_cpu(INFILFS_SB(dir->i_sb)->disk.incompat_flags) &
          INFILFS_INCOMPAT_SYMBOLIC_LINKS))
        return -EOPNOTSUPP;
    ret = infilfs_ns_validate_name(&dentry->d_name);
    if (ret)
        return ret;
    ret = infilfs_ns_begin(dir->i_sb, &pending);
    if (ret)
        return ret;
    ret = infilfs_ns_create_symlink_object(pending, dir, target,
                                           object_id, &object_block);
    if (!ret)
        ret = infilfs_ns_rebuild_directory(pending, dir, NULL, NULL,
                                           &dentry->d_name, object_id,
                                           INFILFS_OBJECT_SYMLINK, 0,
                                           &new_dir_block);
    if (!ret) {
        memset(changes, 0, sizeof(changes));
        memcpy(changes[0].object_id, dir_ii->object_id, 16);
        changes[0].action = INFILFS_NS_REPOINT;
        changes[0].object_block = new_dir_block;
        changes[0].object_type = INFILFS_OBJECT_DIRECTORY;
        memcpy(changes[1].object_id, object_id, 16);
        changes[1].action = INFILFS_NS_ADD;
        changes[1].object_block = object_block;
        changes[1].object_type = INFILFS_OBJECT_SYMLINK;
        ret = infilfs_ns_rebuild_index(pending, changes, ARRAY_SIZE(changes));
    }
    ret = infilfs_ns_finish(pending, ret);
    if (ret)
        return ret;
    dir_ii->object_block = new_dir_block;
    infilfs_refresh_inode_blocks_after_commit(dir);
    inode = infilfs_get_inode(dir->i_sb, object_block,
                              INFILFS_OBJECT_SYMLINK, object_id);
    if (IS_ERR(inode))
        return PTR_ERR(inode);
    d_instantiate(dentry, inode);
    return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_ns_rename(struct mnt_idmap *idmap,
                             struct inode *old_dir, struct dentry *old_dentry,
                             struct inode *new_dir, struct dentry *new_dentry,
                             unsigned int flags)
#else
static int infilfs_ns_rename(struct user_namespace *idmap,
                             struct inode *old_dir, struct dentry *old_dentry,
                             struct inode *new_dir, struct dentry *new_dentry,
                             unsigned int flags)
#endif
{
    struct inode *source = d_inode(old_dentry);
    struct inode *destination = d_inode(new_dentry);
    struct infilfs_inode_info *source_ii = INFILFS_I(source);
    struct infilfs_inode_info *old_dir_ii = INFILFS_I(old_dir);
    struct infilfs_inode_info *new_dir_ii = INFILFS_I(new_dir);
    struct infilfs_inode_info *destination_ii = destination ? INFILFS_I(destination) : NULL;
    struct infilfs_native_pending *pending;
    struct infilfs_quota_reservation new_project_quota = {0};
    struct infilfs_quota_reservation old_project_quota = {0};
    struct infilfs_quota_reservation destination_quota = {0};
    struct infilfs_ns_index_change *changes = NULL;
    u32 change_count = 0, change_capacity = 0;
    u64 old_dir_block = 0, new_dir_block = 0, source_block = 0;
    u64 retained_destination_block = 0;
    u64 destination_bytes = 0;
    bool retained_destination = false;
    bool destination_sync_delete = false;
    bool same_parent;
    bool quota_locked = false;
    int old_delta = 0, new_delta = 0;
    int ret;

    (void)idmap;
    if (!source || !source_ii)
        return -ENOENT;
    if (flags & ~(RENAME_NOREPLACE))
        return -EINVAL;
    if ((flags & RENAME_NOREPLACE) && destination)
        return -EEXIST;
    if (destination == source)
        return 0;
    ret = infilfs_ns_validate_name(&old_dentry->d_name);
    if (!ret)
        ret = infilfs_ns_validate_name(&new_dentry->d_name);
    if (ret)
        return ret;
    if (destination) {
        if (source_ii->object_type == INFILFS_OBJECT_DIRECTORY &&
            destination_ii->object_type != INFILFS_OBJECT_DIRECTORY)
            return -ENOTDIR;
        if (source_ii->object_type != INFILFS_OBJECT_DIRECTORY &&
            destination_ii->object_type == INFILFS_OBJECT_DIRECTORY)
            return -EISDIR;
    }
    same_parent = old_dir == new_dir;
    destination_sync_delete = destination &&
        destination_ii->object_type != INFILFS_OBJECT_FILE;
    if (source_ii->object_type == INFILFS_OBJECT_DIRECTORY && !same_parent) {
        ret = infilfs_ns_is_ancestor(old_dir->i_sb, source_ii->object_id,
                                     new_dir_ii->object_id);
        if (ret < 0)
            return ret;
        if (ret)
            return -EINVAL;
    }
    if (!same_parent && source_ii->object_type == INFILFS_OBJECT_FILE &&
        source->i_nlink > 1) {
        ret = infilfs_quota_hardlink_check(source, new_dir);
        if (ret)
            return ret;
    }

    if (!same_parent || destination_sync_delete) {
        mutex_lock(&INFILFS_SB(old_dir->i_sb)->quota_lock);
        quota_locked = true;
    }
    if (destination_sync_delete) {
        ret = infilfs_quota_capture_inode_locked(
            destination, &destination_quota);
        if (ret) {
            mutex_unlock(&INFILFS_SB(old_dir->i_sb)->quota_lock);
            return ret;
        }
        destination_bytes = (u64)i_size_read(destination);
    }
    ret = infilfs_ns_begin(old_dir->i_sb, &pending);
    if (ret) {
        destination_quota.active = false;
        if (quota_locked)
            mutex_unlock(&INFILFS_SB(old_dir->i_sb)->quota_lock);
        return ret;
    }
    if (!same_parent) {
        ret = infilfs_quota_prepare_reparent_locked(
            source, new_dir, &new_project_quota, &old_project_quota);
        if (ret)
            goto finish2;
    }
    if (destination) {
        ret = infilfs_ns_delete_object(pending, destination, &changes,
                                       &change_count, &change_capacity,
                                       &retained_destination,
                                       &retained_destination_block);
        if (ret)
            goto finish;
    }
    ret = infilfs_ns_cow_object(
        pending, source,
        (!same_parent && !(source_ii->object_type == INFILFS_OBJECT_FILE &&
          source->i_nlink > 1)) ? new_dir_ii->object_id : NULL,
        0, false, &source_block);
finish:
    ;
    if (!ret)
        ret = infilfs_ns_changes_append(&changes, &change_count, &change_capacity,
                                        source_ii->object_id, INFILFS_NS_REPOINT,
                                        source_block, source_ii->object_type);
    if (ret)
        goto finish2;

    if (same_parent) {
        int delta = destination &&
            destination_ii->object_type == INFILFS_OBJECT_DIRECTORY ? -1 : 0;
        ret = infilfs_ns_rebuild_directory(
            pending, old_dir, &old_dentry->d_name,
            destination ? &new_dentry->d_name : NULL,
            &new_dentry->d_name, source_ii->object_id,
            source_ii->object_type, delta, &old_dir_block);
        if (!ret) {
            new_dir_block = old_dir_block;
            ret = infilfs_ns_changes_append(
                &changes, &change_count, &change_capacity,
                old_dir_ii->object_id, INFILFS_NS_REPOINT,
                old_dir_block, INFILFS_OBJECT_DIRECTORY);
        }
        old_delta = delta;
        new_delta = 0;
    } else {
        old_delta = source_ii->object_type == INFILFS_OBJECT_DIRECTORY ? -1 : 0;
        new_delta = (source_ii->object_type == INFILFS_OBJECT_DIRECTORY ? 1 : 0) -
            (destination && destination_ii->object_type == INFILFS_OBJECT_DIRECTORY ? 1 : 0);
        ret = infilfs_ns_rebuild_directory(
            pending, old_dir, &old_dentry->d_name, NULL,
            NULL, NULL, 0, old_delta, &old_dir_block);
        if (!ret)
            ret = infilfs_ns_rebuild_directory(
                pending, new_dir,
                destination ? &new_dentry->d_name : NULL, NULL,
                &new_dentry->d_name, source_ii->object_id,
                source_ii->object_type, new_delta, &new_dir_block);
        if (!ret)
            ret = infilfs_ns_changes_append(
                &changes, &change_count, &change_capacity,
                old_dir_ii->object_id, INFILFS_NS_REPOINT,
                old_dir_block, INFILFS_OBJECT_DIRECTORY);
        if (!ret)
            ret = infilfs_ns_changes_append(
                &changes, &change_count, &change_capacity,
                new_dir_ii->object_id, INFILFS_NS_REPOINT,
                new_dir_block, INFILFS_OBJECT_DIRECTORY);
    }
    if (!ret)
        ret = infilfs_ns_rebuild_index(pending, changes, change_count);
finish2:
    ret = infilfs_ns_finish(pending, ret);
    if (quota_locked) {
        if (ret) {
            if (new_project_quota.active)
                infilfs_quota_adjust_locked(
                    &new_project_quota,
                    new_project_quota.bytes,
                    new_project_quota.objects);
            new_project_quota.active = false;
            old_project_quota.active = false;
            destination_quota.active = false;
        } else {
            /*
             * Destination-project usage was reserved before mutation. Keeping
             * that reservation commits the source transfer; the old project
             * and any synchronously removed non-file destination are debited
             * only after the namespace transaction is known to have committed.
             */
            new_project_quota.active = false;
            if (old_project_quota.active)
                infilfs_quota_adjust_locked(
                    &old_project_quota,
                    old_project_quota.bytes,
                    old_project_quota.objects);
            old_project_quota.active = false;
            if (destination_quota.active)
                infilfs_quota_adjust_locked(
                    &destination_quota, destination_bytes, 1);
            destination_quota.active = false;

            if (destination_sync_delete &&
                infilfs_quota_remove_project_root_locked(
                    old_dir->i_sb, destination_ii->object_id)) {
                int quota_policy_ret =
                    infilfs_quota_persist_locked(old_dir->i_sb);

                if (quota_policy_ret) {
                    INFILFS_SB(old_dir->i_sb)->write_poisoned = true;
                    pr_err("InfiltratorFS: replaced project root could not be persisted after rename: %d\n",
                           quota_policy_ret);
                }
            }
        }
        mutex_unlock(&INFILFS_SB(old_dir->i_sb)->quota_lock);
        quota_locked = false;
    }
    if (!ret) {
        source_ii->object_block = source_block;
        old_dir_ii->object_block = old_dir_block;
        infilfs_refresh_inode_blocks_after_commit(old_dir);
        if (!same_parent)
            new_dir_ii->object_block = new_dir_block;
        if (!same_parent)
            infilfs_refresh_inode_blocks_after_commit(new_dir);
        if (destination) {
            if (retained_destination) {
                destination_ii->object_block = retained_destination_block;
                drop_nlink(destination);
            } else {
                clear_nlink(destination);
            }
        }
        while (old_delta < 0) {
            drop_nlink(old_dir);
            old_delta++;
        }
        while (old_delta > 0) {
            inc_nlink(old_dir);
            old_delta--;
        }
        while (new_delta < 0) {
            drop_nlink(new_dir);
            new_delta++;
        }
        while (new_delta > 0) {
            inc_nlink(new_dir);
            new_delta--;
        }
    }
    kvfree(changes);
    return ret;
}
