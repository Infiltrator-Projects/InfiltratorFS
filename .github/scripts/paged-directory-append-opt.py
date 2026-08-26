from pathlib import Path

ns_path = Path('kernel/infiltratorfs_rw_namespace.inc')
text = ns_path.read_text()

marker = '''static int infilfs_ns_rebuild_directory(\n    struct infilfs_native_pending *pending, struct inode *dir,\n    const struct qstr *remove_a, const struct qstr *remove_b,\n    const struct qstr *add_name, const u8 add_id[16], u16 add_type,\n    int link_delta, u64 *new_dir_block_out)\n{\n'''
if marker not in text:
    raise SystemExit('directory rebuild marker not found')
if 'static int infilfs_ns_append_paged_directory(' not in text:
    helper = r'''/*
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
    u32 total_entries = 0;
    u32 last_entries = 0;
    u32 last_used = 0;
    u64 last_page_no = 0;
    u64 new_page_no;
    u64 replacement;
    u64 new_head_block;
    u32 p;
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
     * We still scan names because VFS negative dentries are not a persistent
     * uniqueness index.  The fast path removes the expensive flatten/copy and
     * rewrite of every directory page, not the correctness check itself.
     */
    for (p = 0; p < page_count; ++p) {
        u64 page_no = le64_to_cpu(pointers[p]);
        u32 used;
        u32 expected_entries;
        u32 seen = 0;
        size_t offset = 0;

        ret = infilfs_read_block(pending->sb, page_no, page_block);
        if (ret)
            goto out;
        if (!infilfs_metadata_page_valid(pending->sb, page_block,
                                         infilfs_directory_page_magic,
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

            if (used - offset < sizeof(*entry)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            entry = (struct infilfs_dirent_disk *)
                (page_block + sizeof(*page) + offset);
            rec = le16_to_cpu(entry->record_size);
            name_len = le16_to_cpu(entry->name_length);
            if (rec < sizeof(*entry) || rec > used - offset || (rec & 7u) ||
                sizeof(*entry) + name_len > rec) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            if (infilfs_ns_name_matches((const u8 *)entry, rec,
                                        add_name->name, add_name->len)) {
                ret = -EEXIST;
                goto out;
            }
            offset += rec;
            seen++;
        }
        if (offset != used || seen != expected_entries ||
            total_entries > U32_MAX - seen) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        total_entries += seen;
        if (p + 1u == page_count) {
            last_page_no = page_no;
            last_entries = expected_entries;
            last_used = used;
        }
    }
    if (total_entries != directory_entries) {
        ret = -EFSCORRUPTED;
        goto out;
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
    *new_dir_block_out = new_head_block;
    ret = 0;
out:
    kfree(page_block);
    kfree(head);
    return ret;
}

'''
    text = text.replace(marker, helper + marker, 1)

old = marker + '''    struct infilfs_rw_tx *tx = &pending->tx;\n'''
new = marker + '''    struct infilfs_rw_tx *tx = &pending->tx;\n'''
# Insert the fast-path call after local declarations instead of before them, so
# the source remains compatible with kernel C dialects that require declarations
# before statements.  The declaration tail below is stable in this function.
decl_tail = '''    u64 new_head_block;\n    int ret;\n\n    old_head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);\n'''
fast = '''    u64 new_head_block;\n    int ret;\n\n    if (!remove_a && !remove_b && add_name) {\n        ret = infilfs_ns_append_paged_directory(\n            pending, dir, add_name, add_id, add_type, link_delta,\n            new_dir_block_out);\n        if (ret != -EOPNOTSUPP)\n            return ret;\n    }\n\n    old_head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);\n'''
if decl_tail not in text:
    raise SystemExit('directory fast-path insertion point not found')
text = text.replace(decl_tail, fast, 1)
ns_path.write_text(text)

test_path = Path('tests/native-deferred-publication-policy.sh')
test = test_path.read_text()
anchor = '''grep -Fq 'changes[c].action == INFILFS_NS_REMOVE' "$ns"\n'''
extra = '''grep -Fq 'infilfs_ns_append_paged_directory' "$ns"\ngrep -Fq 'if (!remove_a && !remove_b && add_name)' "$ns"\ngrep -Fq 'pending, last_page_no, page_block, &replacement' "$ns"\ngrep -Fq 'infilfs_rw_collect_directory(tx, dir, old_head' "$ns"\n'''
if extra not in test:
    if anchor not in test:
        raise SystemExit('policy test anchor not found')
    test = test.replace(anchor, anchor + extra, 1)
    test_path.write_text(test)
