// SPDX-License-Identifier: GPL-3.0-or-later
static int ownership_claim(uint8_t *owners, uint64_t total, uint64_t block)
{
    if (block >= total || bitmap_get(owners, block))
        return INFS_STATUS_CORRUPT;
    bitmap_set(owners, block, 1);
    return INFS_STATUS_OK;
}

static int ownership_claim_data(uint8_t *owners, uint8_t *data_owners,
                                uint64_t total, uint64_t block,
                                int allow_shared)
{
    if (block >= total)
        return INFS_STATUS_CORRUPT;
    if (bitmap_get(owners, block)) {
        if (!allow_shared || !bitmap_get(data_owners, block))
            return INFS_STATUS_CORRUPT;
        return INFS_STATUS_OK;
    }
    bitmap_set(owners, block, 1);
    bitmap_set(data_owners, block, 1);
    return INFS_STATUS_OK;
}

static int claim_index_tree_node(
    struct infs_volume *vol, uint8_t *owners, uint64_t total,
    const uint8_t owner_id[16], uint64_t block_no, unsigned depth)
{
    if (depth > 16u)
        return INFS_STATUS_CORRUPT;
    int status = ownership_claim(owners, total, block_no);
    if (status != INFS_STATUS_OK)
        return status;

    uint8_t block[INFS_BLOCK_SIZE];
    status = read_block(vol, block_no, block);
    if (status != INFS_STATUS_OK)
        return status;

    if (memcmp(block, INFS_INDEX_PAGE_MAGIC, 8) == 0) {
        struct infs_index_entry_disk *entries = NULL;
        uint32_t count = 0;
        return index_page_validate(vol, block, owner_id, &entries, &count);
    }
    if (memcmp(block, INFS_INDEX_BRANCH_PAGE_MAGIC, 8) != 0 ||
        !infs_validate_metadata_page(
            block, INFS_INDEX_BRANCH_PAGE_MAGIC, owner_id) ||
        !metadata_page_generation_valid(vol, block))
        return INFS_STATUS_CORRUPT;

    const struct infs_metadata_page_disk *page =
        (const struct infs_metadata_page_disk *)block;
    if (infs_le32_to_cpu(page->bytes_used) != INFS_INDEX_TREE_BRANCH_BYTES ||
        infs_le32_to_cpu(page->entry_count) > INFS_INDEX_TREE_FANOUT ||
        depth >= 16u)
        return INFS_STATUS_CORRUPT;

    const uint64_t *children = (const uint64_t *)(page + 1);
    uint32_t seen = 0;
    for (uint32_t i = 0; i < INFS_INDEX_TREE_FANOUT; ++i) {
        uint64_t child = infs_le64_to_cpu(children[i]);
        if (!child)
            continue;
        ++seen;
        if (child >= total || !bitmap_get(vol->bitmap, child))
            return INFS_STATUS_CORRUPT;
        status = claim_index_tree_node(
            vol, owners, total, owner_id, child, depth + 1u);
        if (status != INFS_STATUS_OK)
            return status;
    }
    return seen == infs_le32_to_cpu(page->entry_count) ?
        INFS_STATUS_OK : INFS_STATUS_CORRUPT;
}

static int claim_index_metadata_pages(struct infs_volume *vol,
                                      uint8_t *owners, uint64_t total)
{
    uint8_t index_block[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    infs_status status = index_load(vol, index_block, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)index_block;
    if (infs_le16_to_cpu(header->object_version) ==
        INFS_OBJECT_VERSION_TREE) {
        if (infs_le32_to_cpu(payload->reserved) != 0 ||
            infs_le32_to_cpu(header->payload_size) !=
                sizeof(*payload) + sizeof(uint64_t))
            return INFS_STATUS_CORRUPT;
        uint64_t root_le = 0;
        memcpy(&root_le, payload + 1, sizeof(root_le));
        uint64_t root = infs_le64_to_cpu(root_le);
        if (!root || root >= total || !bitmap_get(vol->bitmap, root))
            return INFS_STATUS_CORRUPT;
        return claim_index_tree_node(
            vol, owners, total, header->object_id, root, 0u);
    }
    if (infs_le16_to_cpu(header->object_version) !=
        INFS_OBJECT_VERSION_PAGED)
        return INFS_STATUS_OK;

    uint32_t page_count = infs_le32_to_cpu(payload->reserved);
    uint64_t *pages = index_page_pointers(payload);
    for (uint32_t i = 0; i < page_count; ++i) {
        uint64_t page_block = infs_le64_to_cpu(pages[i]);
        status = ownership_claim(owners, total, page_block);
        if (status != INFS_STATUS_OK)
            return status;
    }
    return INFS_STATUS_OK;
}

static int claim_directory_tree_node(
    struct infs_volume *vol, uint8_t *owners, uint64_t total,
    const uint8_t owner_id[16], uint64_t block_no, unsigned depth)
{
    if (!block_no || depth > INFS_DIRECTORY_TREE_DEPTH)
        return INFS_STATUS_CORRUPT;
    int status = ownership_claim(owners, total, block_no);
    if (status != INFS_STATUS_OK)
        return status;
    uint8_t block[INFS_BLOCK_SIZE];
    status = read_block(vol, block_no, block);
    if (status != INFS_STATUS_OK)
        return status;
    if (memcmp(block, INFS_DIRECTORY_PAGE_MAGIC, 8) == 0)
        return directory_page_validate(vol, block, owner_id, NULL, NULL, NULL);
    if (depth >= INFS_DIRECTORY_TREE_DEPTH ||
        memcmp(block, INFS_DIRECTORY_BRANCH_PAGE_MAGIC, 8) != 0 ||
        !infs_validate_metadata_page(
            block, INFS_DIRECTORY_BRANCH_PAGE_MAGIC, owner_id) ||
        !metadata_page_generation_valid(vol, block))
        return INFS_STATUS_CORRUPT;
    const struct infs_metadata_page_disk *page =
        (const struct infs_metadata_page_disk *)block;
    if (infs_le32_to_cpu(page->bytes_used) !=
            INFS_DIRECTORY_TREE_BRANCH_BYTES ||
        infs_le32_to_cpu(page->entry_count) > INFS_DIRECTORY_TREE_FANOUT)
        return INFS_STATUS_CORRUPT;
    const uint64_t *children = (const uint64_t *)(page + 1);
    uint32_t seen = 0;
    for (uint32_t i = 0; i < INFS_DIRECTORY_TREE_FANOUT; ++i) {
        uint64_t child = infs_le64_to_cpu(children[i]);
        if (!child)
            continue;
        ++seen;
        if (child >= total || !bitmap_get(vol->bitmap, child))
            return INFS_STATUS_CORRUPT;
        status = claim_directory_tree_node(
            vol, owners, total, owner_id, child, depth + 1u);
        if (status != INFS_STATUS_OK)
            return status;
    }
    return seen == infs_le32_to_cpu(page->entry_count) ?
        INFS_STATUS_OK : INFS_STATUS_CORRUPT;
}

static int claim_directory_metadata_pages(struct infs_volume *vol,
                                          uint8_t *owners, uint64_t total,
                                          uint64_t object_block)
{
    uint8_t object[INFS_BLOCK_SIZE];
    infs_status status = read_object(vol, object_block, object);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_directory_payload_disk *payload;
    status = dir_validate(object, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)object;
    if (infs_le16_to_cpu(header->object_version) ==
        INFS_OBJECT_VERSION_TREE) {
        uint64_t root_le = 0;
        memcpy(&root_le, payload + 1, sizeof(root_le));
        uint64_t root = infs_le64_to_cpu(root_le);
        if (infs_le32_to_cpu(payload->entry_count) == 0)
            return root == 0 ? INFS_STATUS_OK : INFS_STATUS_CORRUPT;
        if (!root || root >= total || !bitmap_get(vol->bitmap, root))
            return INFS_STATUS_CORRUPT;
        return claim_directory_tree_node(
            vol, owners, total, header->object_id, root, 0u);
    }
    if (infs_le16_to_cpu(header->object_version) !=
        INFS_OBJECT_VERSION_PAGED)
        return INFS_STATUS_OK;

    uint32_t page_count = infs_le32_to_cpu(payload->bytes_used);
    uint64_t *pages = directory_page_pointers(payload);
    for (uint32_t i = 0; i < page_count; ++i) {
        uint64_t page_block = infs_le64_to_cpu(pages[i]);
        status = ownership_claim(owners, total, page_block);
        if (status != INFS_STATUS_OK)
            return status;
    }
    return INFS_STATUS_OK;
}

static int claim_file_extent_metadata_pages(struct infs_volume *vol,
                                            uint8_t *owners, uint64_t total,
                                            uint64_t object_block)
{
    uint8_t object[INFS_BLOCK_SIZE];
    infs_status status = read_object(vol, object_block, object);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_file_payload_disk *file;
    status = file_validate_volume(vol, object, &file, NULL);
    if (status != INFS_STATUS_OK)
        return status;
    if (!file_has_paged_extents(object))
        return INFS_STATUS_OK;
    struct infs_extent_head_disk *head = file_extent_head(file);
    uint32_t pages = infs_le32_to_cpu(head->page_count);
    uint64_t *ptr = file_extent_page_pointers(file);
    for (uint32_t i = 0; i < pages; ++i) {
        status = ownership_claim(owners, total, infs_le64_to_cpu(ptr[i]));
        if (status != INFS_STATUS_OK)
            return status;
    }
    return INFS_STATUS_OK;
}

static int build_live_ownership_bitmap_progress(
    struct infs_volume *vol, const struct infs_index_entry_disk *entries,
    uint32_t count, uint8_t *owners,
    struct scrub_metadata_progress_context *progress)
{
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    uint8_t *data_owners = calloc(1, vol->bitmap_bytes);
    if (!owners || !data_owners) {
        free(data_owners);
        return INFS_STATUS_NO_MEMORY;
    }
    int allow_shared =
        (infs_le64_to_cpu(vol->sb.incompat_flags) &
         INFS_INCOMPAT_SHARED_EXTENTS) != 0;

    int status = INFS_STATUS_OK;
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        status = ownership_claim(
            owners, total, infs_le64_to_cpu(vol->sb.checkpoint_block[i]));
        if (status != INFS_STATUS_OK)
            goto out;
    }

    if (vol->snapshot_view) {
        for (uint64_t i = 0; i < vol->snapshot_bitmap_block_count; ++i) {
            status = ownership_claim(
                owners, total, vol->snapshot_bitmap_start_block + i);
            if (status != INFS_STATUS_OK)
                goto out;
        }
    } else {
        struct infs_allocation_layout map = {0};
        status = allocation_map_load(vol, &vol->sb, NULL, NULL, &map);
        if (status != INFS_STATUS_OK)
            goto out;
        for (size_t i = 0; i < map.leaf_count; ++i) {
            status = ownership_claim(owners, total, map.leaf_blocks[i]);
            if (status != INFS_STATUS_OK)
                break;
        }
        for (size_t i = 0; status == INFS_STATUS_OK &&
             i < map.branch_count; ++i)
            status = ownership_claim(owners, total, map.branch_blocks[i]);
        allocation_map_layout_destroy(&map);
        if (status != INFS_STATUS_OK)
            goto out;
    }

    status = ownership_claim(
        owners, total, infs_le64_to_cpu(vol->sb.object_index_block));
    if (status != INFS_STATUS_OK)
        goto out;
    status = claim_index_metadata_pages(vol, owners, total);
    if (status != INFS_STATUS_OK)
        goto out;

    scrub_metadata_progress_begin(
        progress, vol, INFS_SCRUB_METADATA_OWNERSHIP_OBJECTS, count);

    for (uint32_t i = 0; i < count; ++i) {
        uint64_t object_block = infs_le64_to_cpu(entries[i].object_block);
        status = ownership_claim(owners, total, object_block);
        if (status != INFS_STATUS_OK)
            goto out;

        uint16_t object_type = infs_le16_to_cpu(entries[i].object_type);
        if (object_type == INFS_OBJECT_DIRECTORY) {
            status = claim_directory_metadata_pages(vol, owners, total,
                                                    object_block);
            if (status != INFS_STATUS_OK)
                goto out;
            scrub_metadata_progress_update(progress, (uint64_t)i + 1u);
            continue;
        }
        if (object_type != INFS_OBJECT_FILE) {
            scrub_metadata_progress_update(progress, (uint64_t)i + 1u);
            continue;
        }
        status = claim_file_extent_metadata_pages(vol, owners, total,
                                                  object_block);
        if (status != INFS_STATUS_OK)
            goto out;

        uint8_t object[INFS_BLOCK_SIZE];
        status = read_object(vol, object_block, object);
        if (status != INFS_STATUS_OK)
            goto out;

        struct infs_file_payload_disk *file;
        struct infs_extent_disk *ext;
        status = file_validate_volume(vol, object, &file, &ext);
        if (status != INFS_STATUS_OK)
            goto out;

        struct infs_extent_disk *all = NULL;
        uint32_t extent_count = 0;
        int owned = 0;
        status = file_extent_snapshot(vol, object, file, ext,
                                      &all, &extent_count, &owned);
        if (status != INFS_STATUS_OK)
            goto out;

        uint64_t file_physical_total = 0;
        for (uint32_t j = 0; j < extent_count; ++j) {
            uint32_t flags = infs_le32_to_cpu(all[j].flags);
            if (extent_kind(flags) != INFS_EXTENT_NORMAL)
                continue;
            uint32_t logical_blocks = infs_le32_to_cpu(all[j].block_count);
            uint64_t blocks = extent_physical_blocks(logical_blocks, flags);
            if (UINT64_MAX - file_physical_total < blocks) {
                if (owned) free(all);
                status = INFS_STATUS_OVERFLOW;
                goto out;
            }
            file_physical_total += blocks;
        }
        int detailed_blocks = progress && file_physical_total >= 1024u;
        if (detailed_blocks)
            scrub_metadata_progress_begin(
                progress, vol, INFS_SCRUB_METADATA_OWNERSHIP_DATA_BLOCKS,
                file_physical_total);
        uint64_t file_blocks_done = 0;

        for (uint32_t j = 0; j < extent_count; ++j) {
            uint32_t flags = infs_le32_to_cpu(all[j].flags);
            uint32_t logical_blocks = infs_le32_to_cpu(all[j].block_count);
            if (extent_kind(flags) != INFS_EXTENT_NORMAL)
                continue;
            uint64_t physical = infs_le64_to_cpu(all[j].physical_block);
            uint64_t blocks = extent_physical_blocks(logical_blocks, flags);
            for (uint64_t k = 0; k < blocks; ++k) {
                status = ownership_claim_data(
                    owners, data_owners, total, physical + k, allow_shared);
                if (status != INFS_STATUS_OK) {
                    if (owned) free(all);
                    goto out;
                }
                ++file_blocks_done;
                if (detailed_blocks)
                    scrub_metadata_progress_update(progress, file_blocks_done);
            }
        }
        if (owned) free(all);
        if (detailed_blocks)
            scrub_metadata_progress_begin(
                progress, vol, INFS_SCRUB_METADATA_OWNERSHIP_OBJECTS, count);
        scrub_metadata_progress_update(progress, (uint64_t)i + 1u);
    }

out:
    free(data_owners);
    return status;
}

static int build_live_ownership_bitmap(
    struct infs_volume *vol, const struct infs_index_entry_disk *entries,
    uint32_t count, uint8_t *owners)
{
    return build_live_ownership_bitmap_progress(
        vol, entries, count, owners, NULL);
}

static infs_status snapshot_read_bitmap_progress(
    struct infs_volume *vol, const struct infs_snapshot_record_disk *record,
    uint8_t **bitmap_out, struct scrub_metadata_progress_context *progress,
    uint64_t generation)
{
    if (!progress)
        return snapshot_read_bitmap(vol, record, bitmap_out);
    if (!bitmap_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    *bitmap_out = NULL;
    uint8_t *bitmap = malloc(vol->bitmap_bytes);
    if (!bitmap)
        return INFS_STATUS_NO_MEMORY;
    uint64_t start = infs_le64_to_cpu(record->bitmap_start_block);
    infs_status status = infs_storage_read(
        &vol->storage, start * INFS_BLOCK_SIZE, bitmap, vol->bitmap_bytes);
    if (status != INFS_STATUS_OK) {
        free(bitmap);
        return status;
    }

    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    scrub_metadata_progress_begin_generation(
        progress, generation, INFS_SCRUB_METADATA_SNAPSHOT_BITMAP, total);
    uint64_t free_count = 0;
    for (uint64_t b = 0; b < total; ++b) {
        if (!bitmap_get(bitmap, b))
            ++free_count;
        scrub_metadata_progress_update(progress, b + 1u);
    }
    if (free_count != infs_le64_to_cpu(record->free_blocks)) {
        free(bitmap);
        return INFS_STATUS_CORRUPT;
    }
    for (uint64_t b = total; b < (uint64_t)vol->bitmap_bytes * 8u; ++b) {
        if (!bitmap_get(bitmap, b)) {
            free(bitmap);
            return INFS_STATUS_CORRUPT;
        }
    }
    uint64_t blocks = infs_le64_to_cpu(record->bitmap_block_count);
    for (uint64_t i = 0; i < blocks; ++i) {
        if (!bitmap_get(vol->bitmap, start + i) ||
            !bitmap_get(bitmap, start + i)) {
            free(bitmap);
            return INFS_STATUS_CORRUPT;
        }
    }
    *bitmap_out = bitmap;
    return INFS_STATUS_OK;
}

static infs_status snapshot_union_bitmap_progress(
    struct infs_volume *vol, uint8_t **union_out,
    struct scrub_metadata_progress_context *progress)
{
    if (!progress)
        return snapshot_union_bitmap(vol, union_out);
    if (!vol || !union_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    *union_out = calloc(1, vol->bitmap_bytes);
    if (!*union_out)
        return INFS_STATUS_NO_MEMORY;
    if (!snapshots_enabled(vol))
        return INFS_STATUS_OK;

    uint8_t catalog[INFS_BLOCK_SIZE];
    struct infs_snapshot_catalog_payload_disk *payload;
    infs_status status = snapshot_catalog_load(vol, catalog, &payload, NULL);
    if (status == INFS_STATUS_NOT_FOUND)
        return INFS_STATUS_OK;
    if (status != INFS_STATUS_OK) {
        free(*union_out);
        *union_out = NULL;
        return status;
    }

    uint32_t count = infs_le32_to_cpu(payload->snapshot_count);
    struct infs_snapshot_record_disk *records =
        (struct infs_snapshot_record_disk *)(payload + 1);
    scrub_metadata_progress_begin(
        progress, vol, INFS_SCRUB_METADATA_SNAPSHOT_UNION, count);

    for (uint32_t i = 0; i < count; ++i) {
        uint8_t *bitmap = NULL;
        status = snapshot_read_bitmap_progress(
            vol, &records[i], &bitmap, progress,
            infs_le64_to_cpu(records[i].generation));
        if (status != INFS_STATUS_OK) {
            free(*union_out);
            *union_out = NULL;
            return status;
        }

        scrub_metadata_progress_begin(
            progress, vol, INFS_SCRUB_METADATA_SNAPSHOT_UNION, count);
        for (size_t j = 0; j < vol->bitmap_bytes; ++j)
            (*union_out)[j] |= bitmap[j];
        free(bitmap);
        scrub_metadata_progress_update(progress, (uint64_t)i + 1u);
    }
    return INFS_STATUS_OK;
}

static int validate_bitmap_ownership_progress(
    struct infs_volume *vol, const struct infs_index_entry_disk *entries,
    uint32_t count, struct scrub_metadata_progress_context *progress)
{
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    uint8_t *owners = calloc(1, vol->bitmap_bytes);
    if (!owners)
        return INFS_STATUS_NO_MEMORY;
    int status = build_live_ownership_bitmap_progress(
        vol, entries, count, owners, progress);
    if (status != INFS_STATUS_OK) {
        free(owners);
        return status;
    }

    uint8_t *snapshot_union = NULL;
    status = snapshot_union_bitmap_progress(vol, &snapshot_union, progress);
    if (status != INFS_STATUS_OK) {
        free(owners);
        return status;
    }
    for (size_t i = 0; i < vol->bitmap_bytes; ++i)
        owners[i] |= snapshot_union[i];
    free(snapshot_union);

    scrub_metadata_progress_begin(
        progress, vol, INFS_SCRUB_METADATA_OWNERSHIP_BITMAP, total);
    for (uint64_t b = 0; b < total; ++b) {
        if (bitmap_get(owners, b) != bitmap_get(vol->bitmap, b)) {
            status = INFS_STATUS_CORRUPT;
            break;
        }
        scrub_metadata_progress_update(progress, b + 1u);
    }
    free(owners);
    return status;
}

static int validate_bitmap_ownership(struct infs_volume *vol,
                                     const struct infs_index_entry_disk *entries,
                                     uint32_t count)
{
    return validate_bitmap_ownership_progress(vol, entries, count, NULL);
}

static int validate_integrity_metadata_progress(
    struct infs_volume *vol, struct scrub_metadata_progress_context *progress,
    int validate_snapshots)
{
    struct infs_index_entry_disk *entries = NULL;
    uint32_t count = 0;
    int status = index_snapshot(vol, &entries, &count);
    if (status != INFS_STATUS_OK)
        return status;

    status = validate_bitmap_ownership_progress(vol, entries, count, progress);
    if (status != INFS_STATUS_OK)
        goto out;

    scrub_metadata_progress_begin(
        progress, vol, INFS_SCRUB_METADATA_INTEGRITY_OBJECTS, count);

    for (uint32_t i = 0; i < count; ++i) {
        uint16_t type = infs_le16_to_cpu(entries[i].object_type);
        if (type == INFS_OBJECT_CHECKSUM) {
            uint8_t block[INFS_BLOCK_SIZE];
            status = read_object(
                vol, infs_le64_to_cpu(entries[i].object_block), block);
            if (status != INFS_STATUS_OK)
                goto out;
            struct infs_object_header_disk *hdr =
                (struct infs_object_header_disk *)block;
            struct infs_checksum_payload_disk *cp;
            if (checksum_validate(block, NULL, &cp, NULL) != 0 ||
                memcmp(hdr->parent_id, cp->owner_object_id, 16) != 0 ||
                (infs_le64_to_cpu(cp->start_logical_block) %
                 INFS_CHECKSUMS_PER_OBJECT) != 0) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            struct infs_lookup owner;
            status = index_find(vol, cp->owner_object_id, &owner);
            if (status != INFS_STATUS_OK || owner.type != INFS_OBJECT_FILE) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            if (!id_is_zero(cp->next_object_id)) {
                struct infs_lookup next;
                uint8_t next_block[INFS_BLOCK_SIZE];
                struct infs_checksum_payload_disk *next_cp;
                status = index_find(vol, cp->next_object_id, &next);
                if (status != INFS_STATUS_OK ||
                    next.type != INFS_OBJECT_CHECKSUM) {
                    status = INFS_STATUS_CORRUPT;
                    goto out;
                }
                status = read_object(vol, next.block, next_block);
                if (status != INFS_STATUS_OK)
                    goto out;
                if (checksum_validate(next_block, cp->owner_object_id,
                                      &next_cp, NULL) != 0 ||
                    infs_le64_to_cpu(next_cp->start_logical_block) <=
                        infs_le64_to_cpu(cp->start_logical_block)) {
                    status = INFS_STATUS_CORRUPT;
                    goto out;
                }
            }
        } else if (type == INFS_OBJECT_FILE) {
            uint8_t object[INFS_BLOCK_SIZE];
            status = read_object(
                vol, infs_le64_to_cpu(entries[i].object_block), object);
            if (status != INFS_STATUS_OK)
                goto out;
            struct infs_object_header_disk *hdr =
                (struct infs_object_header_disk *)object;
            struct infs_file_payload_disk *file;
            struct infs_extent_disk *ext;
            status = file_validate_volume(vol, object, &file, &ext);
            if (status != INFS_STATUS_OK)
                goto out;
            uint64_t allocated = 0;
            status = file_allocated_blocks_volume(
                vol, object, file, ext, &allocated);
            if (status != INFS_STATUS_OK)
                goto out;
            if (allocated && id_is_zero(file->checksum_head_id)) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            struct infs_extent_disk *all = NULL;
            uint32_t extent_count = 0;
            int owned = 0;
            status = file_extent_snapshot(vol, object, file, ext,
                                          &all, &extent_count, &owned);
            if (status != INFS_STATUS_OK)
                goto out;
            for (uint32_t j = 0; j < extent_count; ++j) {
                if (extent_kind(infs_le32_to_cpu(all[j].flags)) !=
                    INFS_EXTENT_NORMAL)
                    continue;
                uint64_t logical = infs_le64_to_cpu(all[j].logical_block);
                uint64_t remaining = infs_le32_to_cpu(all[j].block_count);
                while (remaining) {
                    uint64_t chunk = INFS_CHECKSUMS_PER_OBJECT -
                        (logical % INFS_CHECKSUMS_PER_OBJECT);
                    if (chunk > remaining)
                        chunk = remaining;
                    struct infs_data_checksum_disk digest;
                    status = checksum_get(vol, file, hdr->object_id,
                                          logical + chunk - 1u, &digest);
                    if (status != INFS_STATUS_OK)
                        goto out;
                    logical += chunk;
                    remaining -= chunk;
                }
            }
            if (owned) free(all);
        } else if (type == INFS_OBJECT_DIRECTORY) {
            uint8_t object[INFS_BLOCK_SIZE];
            status = read_object(
                vol, infs_le64_to_cpu(entries[i].object_block), object);
            if (status != INFS_STATUS_OK)
                goto out;
            struct infs_directory_payload_disk *dir;
            status = dir_validate(object, &dir);
            if (status != INFS_STATUS_OK)
                goto out;
            struct infs_object_header_disk *header =
                (struct infs_object_header_disk *)object;
            if (infs_le16_to_cpu(header->object_version) ==
                    INFS_OBJECT_VERSION_TREE ||
                infs_le16_to_cpu(header->object_version) ==
                    INFS_OBJECT_VERSION_PAGED) {
                uint8_t *snapshot = NULL;
                size_t bytes = 0;
                uint32_t entries_count = 0;
                status = infs_le16_to_cpu(header->object_version) ==
                        INFS_OBJECT_VERSION_TREE ?
                    tree_dir_snapshot(vol, object, &snapshot, &bytes,
                                      &entries_count) :
                    paged_dir_snapshot(vol, object, &snapshot, &bytes,
                                       &entries_count);
                free(snapshot);
                if (status != INFS_STATUS_OK ||
                    entries_count != infs_le32_to_cpu(dir->entry_count)) {
                    if (status == INFS_STATUS_OK)
                        status = INFS_STATUS_CORRUPT;
                    goto out;
                }
                (void)bytes;
            }
        } else if (type == INFS_OBJECT_SYMLINK) {
            uint8_t object[INFS_BLOCK_SIZE];
            status = read_object(
                vol, infs_le64_to_cpu(entries[i].object_block), object);
            if (status != INFS_STATUS_OK)
                goto out;
            status = symlink_validate(vol, object, NULL, NULL);
            if (status != INFS_STATUS_OK)
                goto out;
        } else if (type == INFS_OBJECT_SNAPSHOT_CATALOG) {
            if (!snapshots_enabled(vol) ||
                memcmp(entries[i].object_id, snapshot_catalog_id, 16) != 0) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            if (validate_snapshots) {
                status = snapshot_validate_catalog_graphs(vol);
                if (status != INFS_STATUS_OK)
                    goto out;
            }
        }
        scrub_metadata_progress_update(progress, (uint64_t)i + 1u);
    }
    status = INFS_STATUS_OK;

out:
    free(entries);
    return status;
}

static int validate_integrity_metadata(struct infs_volume *vol)
{
    return validate_integrity_metadata_progress(vol, NULL, 1);
}
