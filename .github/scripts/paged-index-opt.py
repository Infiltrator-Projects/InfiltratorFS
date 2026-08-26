from pathlib import Path

ns_path = Path('kernel/infiltratorfs_rw_namespace.inc')
text = ns_path.read_text()

marker = '''static int infilfs_ns_rebuild_index(\n    struct infilfs_native_pending *pending,\n    struct infilfs_ns_index_change *changes, u32 change_count)\n{\n'''
if marker not in text:
    raise SystemExit('namespace index rebuild marker not found')
if 'static int infilfs_ns_update_paged_index(' not in text:
    helper = r'''/*
 * Fast path for the overwhelmingly common namespace index mutations: repoint
 * an existing object and/or append a newly created object.  A paged object
 * index does not need to be flattened and completely rewritten for those
 * operations.  Reuse the data-path updater so only touched index pages plus
 * the index head are copy-on-written.  Removes still use the full rebuild
 * below because compacting entries can shift records across page boundaries.
 */
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

'''
    text = text.replace(marker, helper + marker, 1)

old = '''    for (c = 0; c < change_count; ++c) {\n        changes[c].found = false;\n        if (changes[c].action == INFILFS_NS_ADD)\n            add_count++;\n    }\n    old_head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);\n'''
new = '''    for (c = 0; c < change_count; ++c) {\n        changes[c].found = false;\n        if (changes[c].action == INFILFS_NS_ADD)\n            add_count++;\n    }\n\n    ret = infilfs_ns_update_paged_index(pending, changes, change_count);\n    if (ret != -EOPNOTSUPP)\n        return ret;\n\n    old_head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);\n'''
if old not in text:
    raise SystemExit('namespace fast-path insertion point not found')
text = text.replace(old, new, 1)
ns_path.write_text(text)

test_path = Path('tests/native-deferred-publication-policy.sh')
test = test_path.read_text()
anchor = '''grep -Fq 'pending->pending_bytes >= pending->publish_threshold' "$ns"\n'''
extra = '''grep -Fq 'infilfs_ns_update_paged_index' "$ns"\ngrep -Fq 'infilfs_native_index_update(pending, native, change_count)' "$ns"\ngrep -Fq 'changes[c].action == INFILFS_NS_REMOVE' "$ns"\n'''
if extra not in test:
    if anchor not in test:
        raise SystemExit('policy regression-test anchor not found')
    test = test.replace(anchor, anchor + extra, 1)
    test_path.write_text(test)
