// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Native scalable directory tree.
 *
 * File names are routed by SHA-256, one digest byte per 256-way branch level.
 * Leaf records retain the normal exact UTF-8 name bytes and are compared
 * byte-for-byte, so a hash never becomes namespace identity. Every modified
 * path is copy-on-written through the native pending transaction.
 */

static void infilfs_tree_dir_digest(const u8 *name, size_t len, u8 digest[32])
{
    struct infilfs_rw_sha256_ctx ctx;

    infilfs_rw_sha256_init(&ctx);
    infilfs_rw_sha256_update(&ctx, name, len);
    infilfs_rw_sha256_final(&ctx, digest);
}

static u64 infilfs_tree_dir_root(
    const struct infilfs_directory_payload_disk *payload)
{
    __le64 root_le = 0;

    memcpy(&root_le, payload + 1, sizeof(root_le));
    return le64_to_cpu(root_le);
}

static void infilfs_tree_dir_set_root(
    struct infilfs_directory_payload_disk *payload, u64 root)
{
    __le64 root_le = cpu_to_le64(root);

    memcpy(payload + 1, &root_le, sizeof(root_le));
}

static bool infilfs_tree_dir_branch_valid(
    struct super_block *sb, const u8 block[INFILFS_DISK_BLOCK_SIZE],
    const u8 owner_id[16], const __le64 **children_out, u32 *count_out)
{
    const struct infilfs_metadata_page_disk *page =
        (const struct infilfs_metadata_page_disk *)block;
    const __le64 *children;
    u32 expected;
    u32 seen = 0;
    u32 i;

    if (!infilfs_metadata_page_valid(
            sb, block, infilfs_directory_branch_page_magic, owner_id))
        return false;
    expected = le32_to_cpu(page->entry_count);
    if (expected > INFILFS_DIRECTORY_TREE_FANOUT ||
        le32_to_cpu(page->bytes_used) != INFILFS_DIRECTORY_TREE_BRANCH_BYTES)
        return false;

    children = (const __le64 *)(page + 1);
    for (i = 0; i < INFILFS_DIRECTORY_TREE_FANOUT; ++i) {
        u64 child = le64_to_cpu(children[i]);

        if (!child)
            continue;
        if (!infilfs_block_allocated(sb, child))
            return false;
        seen++;
    }
    if (seen != expected)
        return false;
    if (children_out)
        *children_out = children;
    if (count_out)
        *count_out = expected;
    return true;
}

static int infilfs_tree_dir_leaf_find(
    struct super_block *sb, const u8 block[INFILFS_DISK_BLOCK_SIZE],
    const u8 owner_id[16], const u8 *name, u16 name_len,
    struct infilfs_dir_lookup *search)
{
    const struct infilfs_metadata_page_disk *page =
        (const struct infilfs_metadata_page_disk *)block;
    const u8 *entries;
    u32 bytes;
    u32 offset = 0;

    if (!infilfs_metadata_page_valid(
            sb, block, infilfs_directory_page_magic, owner_id))
        return -EFSCORRUPTED;
    bytes = le32_to_cpu(page->bytes_used);
    entries = (const u8 *)(page + 1);
    while (offset < bytes) {
        const struct infilfs_dirent_disk *entry;
        u16 rec;
        u16 len;

        if (bytes - offset < sizeof(*entry))
            return -EFSCORRUPTED;
        entry = (const struct infilfs_dirent_disk *)(entries + offset);
        rec = le16_to_cpu(entry->record_size);
        len = le16_to_cpu(entry->name_length);
        if (rec < sizeof(*entry) || (rec & 7u) ||
            rec > bytes - offset || !len || len > INFILFS_NAME_MAX ||
            sizeof(*entry) + len > rec)
            return -EFSCORRUPTED;
        if (len == name_len &&
            !memcmp(entries + offset + sizeof(*entry), name, name_len)) {
            if (search) {
                memcpy(search->object_id, entry->object_id, 16);
                search->object_type = le16_to_cpu(entry->object_type);
                search->found = true;
            }
            return 0;
        }
        offset += rec;
    }
    return offset == bytes ? -ENOENT : -EFSCORRUPTED;
}

static int infilfs_tree_dir_lookup_name(
    struct inode *dir, const u8 *name, u16 name_len,
    struct infilfs_dir_lookup *search)
{
    struct infilfs_inode_info *ii = INFILFS_I(dir);
    struct infilfs_object_header_disk *header;
    struct infilfs_directory_payload_disk *payload;
    u8 digest[32];
    u8 *head = NULL;
    u8 *block = NULL;
    u64 node;
    u32 count;
    unsigned int depth = 0;
    int ret;

    head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!head || !block) {
        ret = -ENOMEM;
        goto out;
    }
    ret = infilfs_read_object(dir->i_sb, ii->object_block,
                              INFILFS_OBJECT_DIRECTORY, ii->object_id, head);
    if (ret)
        goto out;
    header = (struct infilfs_object_header_disk *)head;
    if (le16_to_cpu(header->object_version) != INFILFS_OBJECT_VERSION_TREE) {
        ret = -EOPNOTSUPP;
        goto out;
    }
    payload = (struct infilfs_directory_payload_disk *)(header + 1);
    count = le32_to_cpu(payload->entry_count);
    if (le32_to_cpu(payload->bytes_used) != 0 ||
        le32_to_cpu(header->payload_size) !=
            sizeof(*payload) + sizeof(__le64)) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    node = infilfs_tree_dir_root(payload);
    if (!count) {
        ret = node ? -EFSCORRUPTED : -ENOENT;
        goto out;
    }
    if (!node) {
        ret = -EFSCORRUPTED;
        goto out;
    }

    infilfs_tree_dir_digest(name, name_len, digest);
    for (;;) {
        const __le64 *children;

        if (!infilfs_block_allocated(dir->i_sb, node)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        ret = infilfs_read_allocated_block(dir->i_sb, node, block);
        if (ret)
            goto out;
        if (!memcmp(block, infilfs_directory_page_magic, 8)) {
            ret = infilfs_tree_dir_leaf_find(
                dir->i_sb, block, header->object_id,
                name, name_len, search);
            goto out;
        }
        if (depth >= INFILFS_DIRECTORY_TREE_DEPTH ||
            !infilfs_tree_dir_branch_valid(
                dir->i_sb, block, header->object_id, &children, NULL)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        node = le64_to_cpu(children[digest[depth++]]);
        if (!node) {
            ret = -ENOENT;
            goto out;
        }
    }
out:
    if (ret == -ENOENT)
        ret = 0;
    kfree(block);
    kfree(head);
    return ret;
}

static int infilfs_tree_dir_for_each_node(
    struct inode *inode, const u8 owner_id[16], u64 node, unsigned int depth,
    int (*visitor)(const struct infilfs_dirent_disk *, const u8 *, void *),
    void *arg)
{
    u8 *block;
    int ret;

    if (!node || depth > INFILFS_DIRECTORY_TREE_DEPTH ||
        !infilfs_block_allocated(inode->i_sb, node))
        return -EFSCORRUPTED;
    block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!block)
        return -ENOMEM;
    ret = infilfs_read_allocated_block(inode->i_sb, node, block);
    if (ret)
        goto out;
    if (!memcmp(block, infilfs_directory_page_magic, 8)) {
        const struct infilfs_metadata_page_disk *page =
            (const struct infilfs_metadata_page_disk *)block;

        if (!infilfs_metadata_page_valid(
                inode->i_sb, block, infilfs_directory_page_magic, owner_id)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        ret = infilfs_walk_dir_buffer(
            (const u8 *)(page + 1), le32_to_cpu(page->bytes_used),
            visitor, arg);
        goto out;
    }
    if (depth >= INFILFS_DIRECTORY_TREE_DEPTH) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    {
        const __le64 *children;
        u32 slot;

        if (!infilfs_tree_dir_branch_valid(
                inode->i_sb, block, owner_id, &children, NULL)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        for (slot = 0; slot < INFILFS_DIRECTORY_TREE_FANOUT; ++slot) {
            u64 child = le64_to_cpu(children[slot]);

            if (!child)
                continue;
            ret = infilfs_tree_dir_for_each_node(
                inode, owner_id, child, depth + 1u, visitor, arg);
            if (ret)
                goto out;
        }
    }
    ret = 0;
out:
    kfree(block);
    return ret;
}

static int infilfs_tree_dir_for_each(
    struct inode *inode,
    int (*visitor)(const struct infilfs_dirent_disk *, const u8 *, void *),
    void *arg)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_object_header_disk *header;
    struct infilfs_directory_payload_disk *payload;
    u8 *head;
    u64 root;
    u32 count;
    int ret;

    head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!head)
        return -ENOMEM;
    ret = infilfs_read_object(inode->i_sb, ii->object_block,
                              INFILFS_OBJECT_DIRECTORY, ii->object_id, head);
    if (ret)
        goto out;
    header = (struct infilfs_object_header_disk *)head;
    payload = (struct infilfs_directory_payload_disk *)(header + 1);
    if (le16_to_cpu(header->object_version) != INFILFS_OBJECT_VERSION_TREE ||
        le32_to_cpu(payload->bytes_used) != 0 ||
        le32_to_cpu(header->payload_size) !=
            sizeof(*payload) + sizeof(__le64)) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    count = le32_to_cpu(payload->entry_count);
    root = infilfs_tree_dir_root(payload);
    if (!count) {
        ret = root ? -EFSCORRUPTED : 0;
        goto out;
    }
    if (!root) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    ret = infilfs_tree_dir_for_each_node(
        inode, header->object_id, root, 0u, visitor, arg);
out:
    kfree(head);
    return ret;
}

static int infilfs_native_tree_dir_leaf_validate(
    struct infilfs_native_pending *pending,
    const u8 block[INFILFS_DISK_BLOCK_SIZE], const u8 owner_id[16],
    u8 **entries_out, u32 *bytes_out, u32 *count_out)
{
    struct infilfs_metadata_page_disk *page =
        (struct infilfs_metadata_page_disk *)block;
    u8 *entries;
    u32 bytes;
    u32 offset = 0;
    u32 seen = 0;

    if (!infilfs_metadata_page_valid(
            pending->sb, block, infilfs_directory_page_magic, owner_id))
        return -EFSCORRUPTED;
    bytes = le32_to_cpu(page->bytes_used);
    entries = (u8 *)(page + 1);
    while (offset < bytes) {
        struct infilfs_dirent_disk *entry;
        u16 rec;
        u16 len;

        if (bytes - offset < sizeof(*entry))
            return -EFSCORRUPTED;
        entry = (struct infilfs_dirent_disk *)(entries + offset);
        rec = le16_to_cpu(entry->record_size);
        len = le16_to_cpu(entry->name_length);
        if (rec < sizeof(*entry) || (rec & 7u) ||
            rec > bytes - offset || !len || len > INFILFS_NAME_MAX ||
            sizeof(*entry) + len > rec ||
            !infilfs_rw_utf8_valid(entries + offset + sizeof(*entry), len))
            return -EFSCORRUPTED;
        offset += rec;
        seen++;
    }
    if (offset != bytes || seen != le32_to_cpu(page->entry_count))
        return -EFSCORRUPTED;
    if (entries_out)
        *entries_out = entries;
    if (bytes_out)
        *bytes_out = bytes;
    if (count_out)
        *count_out = seen;
    return 0;
}

static int infilfs_native_tree_dir_build(
    struct infilfs_native_pending *pending, const u8 owner_id[16],
    const u8 *entries, size_t bytes, u32 count,
    unsigned int depth, u64 *root_out)
{
    u8 *block;
    int ret;

    if (!entries || !bytes || !count || !root_out)
        return -EINVAL;
    block = kzalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!block)
        return -ENOMEM;
    if (bytes <= INFILFS_METADATA_PAGE_DATA_SIZE) {
        struct infilfs_metadata_page_disk *page;
        u64 leaf;

        infilfs_rw_init_page(
            block, infilfs_directory_page_magic, owner_id,
            pending->tx.generation);
        page = (struct infilfs_metadata_page_disk *)block;
        memcpy(page + 1, entries, bytes);
        page->entry_count = cpu_to_le32(count);
        page->bytes_used = cpu_to_le32(bytes);
        ret = infilfs_rw_finalize_page(block);
        if (!ret)
            ret = infilfs_rw_tx_alloc(&pending->tx, 1, &leaf);
        if (!ret)
            ret = infilfs_native_stage_block(pending->sb, leaf, block);
        if (!ret)
            *root_out = leaf;
        kfree(block);
        return ret;
    }
    if (depth >= INFILFS_DIRECTORY_TREE_DEPTH) {
        kfree(block);
        return -ENOSPC;
    }

    {
        struct infilfs_metadata_page_disk *page;
        __le64 *children;
        u8 *group;
        u32 child_count = 0;
        u32 slot;
        u64 branch;

        infilfs_rw_init_page(
            block, infilfs_directory_branch_page_magic, owner_id,
            pending->tx.generation);
        page = (struct infilfs_metadata_page_disk *)block;
        children = (__le64 *)(page + 1);
        group = kvmalloc(bytes, GFP_NOFS);
        if (!group) {
            kfree(block);
            return -ENOMEM;
        }
        ret = 0;
        for (slot = 0; slot < INFILFS_DIRECTORY_TREE_FANOUT; ++slot) {
            size_t in_off = 0;
            size_t group_bytes = 0;
            u32 group_count = 0;
            u64 child = 0;

            while (in_off < bytes) {
                const struct infilfs_dirent_disk *entry;
                u16 rec;
                u16 len;
                u8 digest[32];

                if (bytes - in_off < sizeof(*entry)) {
                    ret = -EFSCORRUPTED;
                    break;
                }
                entry = (const struct infilfs_dirent_disk *)(entries + in_off);
                rec = le16_to_cpu(entry->record_size);
                len = le16_to_cpu(entry->name_length);
                if (!rec || rec > bytes - in_off ||
                    sizeof(*entry) + len > rec) {
                    ret = -EFSCORRUPTED;
                    break;
                }
                infilfs_tree_dir_digest(
                    entries + in_off + sizeof(*entry), len, digest);
                if (digest[depth] == (u8)slot) {
                    memcpy(group + group_bytes, entries + in_off, rec);
                    group_bytes += rec;
                    group_count++;
                }
                in_off += rec;
            }
            if (ret)
                break;
            if (!group_count)
                continue;
            ret = infilfs_native_tree_dir_build(
                pending, owner_id, group, group_bytes, group_count,
                depth + 1u, &child);
            if (ret)
                break;
            children[slot] = cpu_to_le64(child);
            child_count++;
        }
        kvfree(group);
        if (ret) {
            kfree(block);
            return ret;
        }
        if (!child_count) {
            kfree(block);
            return -EFSCORRUPTED;
        }
        page->entry_count = cpu_to_le32(child_count);
        page->bytes_used = cpu_to_le32(INFILFS_DIRECTORY_TREE_BRANCH_BYTES);
        ret = infilfs_rw_finalize_page(block);
        if (!ret)
            ret = infilfs_rw_tx_alloc(&pending->tx, 1, &branch);
        if (!ret)
            ret = infilfs_native_stage_block(pending->sb, branch, block);
        if (!ret)
            *root_out = branch;
    }
    kfree(block);
    return ret;
}

enum infilfs_native_tree_dir_op {
    INFILFS_TREE_DIR_ADD,
    INFILFS_TREE_DIR_REMOVE
};

static int infilfs_native_tree_dir_mutate(
    struct infilfs_native_pending *pending, const u8 owner_id[16], u64 node,
    const struct qstr *name, const u8 object_id[16], u16 type,
    const u8 digest[32], unsigned int depth,
    enum infilfs_native_tree_dir_op op, u64 *replacement_out)
{
    u8 *block;
    int ret;

    if (!node || depth > INFILFS_DIRECTORY_TREE_DEPTH ||
        !infilfs_block_allocated(pending->sb, node))
        return -EFSCORRUPTED;
    block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!block)
        return -ENOMEM;
    ret = infilfs_read_allocated_block(pending->sb, node, block);
    if (ret)
        goto out;

    if (!memcmp(block, infilfs_directory_page_magic, 8)) {
        struct infilfs_metadata_page_disk *page;
        u8 *entries = NULL;
        u32 bytes = 0, count = 0, offset = 0;
        u32 found = U32_MAX;
        u16 found_rec = 0;

        ret = infilfs_native_tree_dir_leaf_validate(
            pending, block, owner_id, &entries, &bytes, &count);
        if (ret)
            goto out;
        while (offset < bytes) {
            struct infilfs_dirent_disk *entry =
                (struct infilfs_dirent_disk *)(entries + offset);
            u16 rec = le16_to_cpu(entry->record_size);
            u16 len = le16_to_cpu(entry->name_length);

            if (len == name->len &&
                !memcmp(entries + offset + sizeof(*entry),
                        name->name, name->len)) {
                found = offset;
                found_rec = rec;
                break;
            }
            offset += rec;
        }

        if (op == INFILFS_TREE_DIR_REMOVE) {
            if (found == U32_MAX) {
                ret = -ENOENT;
                goto out;
            }
            if (count == 1u) {
                ret = infilfs_rw_tx_defer_free(&pending->tx, node, 1);
                if (!ret)
                    *replacement_out = 0;
                goto out;
            }
            memmove(entries + found, entries + found + found_rec,
                    bytes - found - found_rec);
            memset(entries + bytes - found_rec, 0, found_rec);
            page = (struct infilfs_metadata_page_disk *)block;
            page->entry_count = cpu_to_le32(count - 1u);
            page->bytes_used = cpu_to_le32(bytes - found_rec);
            page->generation = cpu_to_le64(pending->tx.generation);
            ret = infilfs_rw_finalize_page(block);
            if (!ret)
                ret = infilfs_native_store_private_or_cow(
                    pending, node, block, replacement_out);
            goto out;
        }

        if (found != U32_MAX) {
            ret = -EEXIST;
            goto out;
        }
        {
            size_t rec = ALIGN(sizeof(struct infilfs_dirent_disk) + name->len, 8);

            if (rec <= INFILFS_METADATA_PAGE_DATA_SIZE - bytes) {
                struct infilfs_dirent_disk *entry =
                    (struct infilfs_dirent_disk *)(entries + bytes);

                memset(entry, 0, rec);
                entry->record_size = cpu_to_le16((u16)rec);
                entry->name_length = cpu_to_le16((u16)name->len);
                entry->object_type = cpu_to_le16(type);
                memcpy(entry->object_id, object_id, 16);
                memcpy((u8 *)entry + sizeof(*entry), name->name, name->len);
                page = (struct infilfs_metadata_page_disk *)block;
                page->entry_count = cpu_to_le32(count + 1u);
                page->bytes_used = cpu_to_le32(bytes + rec);
                page->generation = cpu_to_le64(pending->tx.generation);
                ret = infilfs_rw_finalize_page(block);
                if (!ret)
                    ret = infilfs_native_store_private_or_cow(
                        pending, node, block, replacement_out);
                goto out;
            }

            {
                u8 *grown = kvmalloc((size_t)bytes + rec, GFP_NOFS);
                struct infilfs_dirent_disk *entry;

                if (!grown) {
                    ret = -ENOMEM;
                    goto out;
                }
                memcpy(grown, entries, bytes);
                entry = (struct infilfs_dirent_disk *)(grown + bytes);
                memset(entry, 0, rec);
                entry->record_size = cpu_to_le16((u16)rec);
                entry->name_length = cpu_to_le16((u16)name->len);
                entry->object_type = cpu_to_le16(type);
                memcpy(entry->object_id, object_id, 16);
                memcpy((u8 *)entry + sizeof(*entry), name->name, name->len);
                ret = infilfs_native_tree_dir_build(
                    pending, owner_id, grown, (size_t)bytes + rec,
                    count + 1u, depth, replacement_out);
                kvfree(grown);
                if (!ret)
                    ret = infilfs_rw_tx_defer_free(&pending->tx, node, 1);
                goto out;
            }
        }
    }

    if (depth >= INFILFS_DIRECTORY_TREE_DEPTH) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    {
        const __le64 *read_children;
        __le64 *children;
        struct infilfs_metadata_page_disk *page;
        u32 child_count;
        u32 slot = digest[depth];
        u64 child;
        u64 replacement = 0;

        if (!infilfs_tree_dir_branch_valid(
                pending->sb, block, owner_id,
                &read_children, &child_count)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        page = (struct infilfs_metadata_page_disk *)block;
        children = (__le64 *)(page + 1);
        child = le64_to_cpu(read_children[slot]);

        if (!child) {
            size_t rec;
            u8 *single;
            struct infilfs_dirent_disk *entry;

            if (op != INFILFS_TREE_DIR_ADD) {
                ret = -ENOENT;
                goto out;
            }
            rec = ALIGN(sizeof(*entry) + name->len, 8);
            single = kzalloc(rec, GFP_NOFS);
            if (!single) {
                ret = -ENOMEM;
                goto out;
            }
            entry = (struct infilfs_dirent_disk *)single;
            entry->record_size = cpu_to_le16((u16)rec);
            entry->name_length = cpu_to_le16((u16)name->len);
            entry->object_type = cpu_to_le16(type);
            memcpy(entry->object_id, object_id, 16);
            memcpy(single + sizeof(*entry), name->name, name->len);
            ret = infilfs_native_tree_dir_build(
                pending, owner_id, single, rec, 1u, depth + 1u,
                &replacement);
            kfree(single);
            if (ret)
                goto out;
            child_count++;
        } else {
            ret = infilfs_native_tree_dir_mutate(
                pending, owner_id, child, name, object_id, type,
                digest, depth + 1u, op, &replacement);
            if (ret)
                goto out;
            if (!replacement)
                child_count--;
        }

        if (!child_count) {
            ret = infilfs_rw_tx_defer_free(&pending->tx, node, 1);
            if (!ret)
                *replacement_out = 0;
            goto out;
        }
        children[slot] = cpu_to_le64(replacement);
        page->entry_count = cpu_to_le32(child_count);
        page->generation = cpu_to_le64(pending->tx.generation);
        ret = infilfs_rw_finalize_page(block);
        if (!ret)
            ret = infilfs_native_store_private_or_cow(
                pending, node, block, replacement_out);
    }
out:
    kfree(block);
    return ret;
}

static bool infilfs_tree_dir_same_name(
    const struct qstr *a, const struct qstr *b)
{
    return a && b && a->len == b->len && !memcmp(a->name, b->name, a->len);
}

static int __maybe_unused infilfs_native_tree_directory_update(
    struct infilfs_native_pending *pending, struct inode *dir,
    const struct qstr *remove_a, const struct qstr *remove_b,
    const struct qstr *add_name, const u8 add_id[16], u16 add_type,
    int link_delta, u64 *new_dir_block_out)
{
    struct infilfs_inode_info *ii = INFILFS_I(dir);
    struct infilfs_object_header_disk *header;
    struct infilfs_directory_payload_disk *payload;
    u8 *head;
    u64 root;
    u32 count;
    int ret = 0;

    head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!head)
        return -ENOMEM;
    ret = infilfs_read_object(
        pending->sb, ii->object_block, INFILFS_OBJECT_DIRECTORY,
        ii->object_id, head);
    if (ret)
        goto out;
    header = (struct infilfs_object_header_disk *)head;
    if (le16_to_cpu(header->object_version) != INFILFS_OBJECT_VERSION_TREE) {
        ret = -EOPNOTSUPP;
        goto out;
    }
    payload = (struct infilfs_directory_payload_disk *)(header + 1);
    if (le32_to_cpu(payload->bytes_used) != 0 ||
        le32_to_cpu(header->payload_size) !=
            sizeof(*payload) + sizeof(__le64)) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    count = le32_to_cpu(payload->entry_count);
    root = infilfs_tree_dir_root(payload);
    if ((count == 0) != (root == 0)) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    if (add_name &&
        (!add_name->len || add_name->len > INFILFS_NAME_MAX ||
         !infilfs_rw_utf8_valid(add_name->name, add_name->len) ||
         (add_name->len == 1 && add_name->name[0] == '.') ||
         (add_name->len == 2 && add_name->name[0] == '.' &&
          add_name->name[1] == '.'))) {
        ret = -EINVAL;
        goto out;
    }

#define INFILFS_TREE_REMOVE_ONE(qname) do {                                  \
        if (qname) {                                                          \
            u8 digest[32];                                                     \
            u64 replacement = 0;                                              \
            if (!count || !root) {                                            \
                ret = -ENOENT;                                                 \
                goto out;                                                      \
            }                                                                  \
            infilfs_tree_dir_digest((qname)->name, (qname)->len, digest);      \
            ret = infilfs_native_tree_dir_mutate(                              \
                pending, header->object_id, root, qname, NULL, 0, digest,      \
                0u, INFILFS_TREE_DIR_REMOVE, &replacement);                    \
            if (ret)                                                           \
                goto out;                                                      \
            root = replacement;                                                \
            count--;                                                           \
        }                                                                      \
    } while (0)

    INFILFS_TREE_REMOVE_ONE(remove_a);
    if (remove_b && !infilfs_tree_dir_same_name(remove_a, remove_b))
        INFILFS_TREE_REMOVE_ONE(remove_b);
#undef INFILFS_TREE_REMOVE_ONE

    if (add_name) {
        u8 digest[32];
        u64 replacement = 0;

        if (count == U32_MAX) {
            ret = -EOVERFLOW;
            goto out;
        }
        infilfs_tree_dir_digest(add_name->name, add_name->len, digest);
        if (!root) {
            size_t rec = ALIGN(
                sizeof(struct infilfs_dirent_disk) + add_name->len, 8);
            u8 *single = kzalloc(rec, GFP_NOFS);
            struct infilfs_dirent_disk *entry;

            if (!single) {
                ret = -ENOMEM;
                goto out;
            }
            entry = (struct infilfs_dirent_disk *)single;
            entry->record_size = cpu_to_le16((u16)rec);
            entry->name_length = cpu_to_le16((u16)add_name->len);
            entry->object_type = cpu_to_le16(add_type);
            memcpy(entry->object_id, add_id, 16);
            memcpy(single + sizeof(*entry), add_name->name, add_name->len);
            ret = infilfs_native_tree_dir_build(
                pending, header->object_id, single, rec, 1u, 0u,
                &replacement);
            kfree(single);
        } else {
            ret = infilfs_native_tree_dir_mutate(
                pending, header->object_id, root, add_name, add_id, add_type,
                digest, 0u, INFILFS_TREE_DIR_ADD, &replacement);
        }
        if (ret)
            goto out;
        root = replacement;
        count++;
    }

    if ((count == 0) != (root == 0)) {
        ret = -EFSCORRUPTED;
        goto out;
    }
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
    payload->entry_count = cpu_to_le32(count);
    payload->bytes_used = 0;
    infilfs_tree_dir_set_root(payload, root);
    payload->attributes.modification_time_ns =
        cpu_to_le64(ktime_get_real_ns());
    payload->attributes.change_time_ns =
        payload->attributes.modification_time_ns;
    header->generation = cpu_to_le64(pending->tx.generation);
    header->payload_size = cpu_to_le32(
        sizeof(*payload) + sizeof(__le64));
    ret = infilfs_rw_finalize_object(head);
    if (!ret) {
        u64 new_head_block;

        ret = infilfs_native_store_private_or_cow(
            pending, ii->object_block, head, &new_head_block);
        if (!ret) {
            if (!memcmp(ii->object_id,
                        pending->tx.sbi->disk.root_object_id, 16))
                pending->tx.next_sb.root_object_block =
                    cpu_to_le64(new_head_block);
            infilfs_native_directory_locator_invalidate(pending);
            *new_dir_block_out = new_head_block;
        }
    }
out:
    kfree(head);
    return ret;
}
