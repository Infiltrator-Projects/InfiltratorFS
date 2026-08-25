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

static int build_live_ownership_bitmap(
    struct infs_volume *vol, const struct infs_index_entry_disk *entries,
    uint32_t count, uint8_t *owners)
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

    uint64_t bitmap_start = infs_le64_to_cpu(vol->sb.bitmap_start_block);
    uint64_t bitmap_blocks = infs_le64_to_cpu(vol->sb.bitmap_block_count);
    for (uint64_t i = 0; i < bitmap_blocks; ++i) {
        status = ownership_claim(owners, total, bitmap_start + i);
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
            continue;
        }
        if (object_type != INFS_OBJECT_FILE)
            continue;
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
        for (uint32_t j = 0; j < extent_count; ++j) {
            if (infs_le32_to_cpu(all[j].flags) != INFS_EXTENT_NORMAL)
                continue;
            uint64_t physical = infs_le64_to_cpu(all[j].physical_block);
            uint32_t blocks = infs_le32_to_cpu(all[j].block_count);
            for (uint32_t k = 0; k < blocks; ++k) {
                status = ownership_claim_data(
                    owners, data_owners, total, physical + k, allow_shared);
                if (status != INFS_STATUS_OK) {
                    if (owned) free(all);
                    goto out;
                }
            }
        }
        if (owned) free(all);
    }

out:
    free(data_owners);
    return status;
}

static int validate_bitmap_ownership(struct infs_volume *vol,
                                     const struct infs_index_entry_disk *entries,
                                     uint32_t count)
{
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    uint8_t *owners = calloc(1, vol->bitmap_bytes);
    if (!owners)
        return INFS_STATUS_NO_MEMORY;
    int status = build_live_ownership_bitmap(vol, entries, count, owners);
    if (status != INFS_STATUS_OK) {
        free(owners);
        return status;
    }

    uint8_t *snapshot_union = NULL;
    status = snapshot_union_bitmap(vol, &snapshot_union);
    if (status != INFS_STATUS_OK) {
        free(owners);
        return status;
    }
    for (size_t i = 0; i < vol->bitmap_bytes; ++i)
        owners[i] |= snapshot_union[i];
    free(snapshot_union);

    for (uint64_t b = 0; b < total; ++b) {
        if (bitmap_get(owners, b) != bitmap_get(vol->bitmap, b)) {
            status = INFS_STATUS_CORRUPT;
            break;
        }
    }
    free(owners);
    return status;
}

static int validate_integrity_metadata(struct infs_volume *vol)
{
    const int trace = getenv("INFS_TEST_GRAPH_TRACE") != NULL;
    struct infs_index_entry_disk *entries = NULL;
    uint32_t count = 0;
    int status = index_snapshot(vol, &entries, &count);
    if (status != INFS_STATUS_OK) {
        if (trace)
            fprintf(stderr, "INFS_INTEGRITY_TRACE index_snapshot=%d\n", status);
        return status;
    }

    status = validate_bitmap_ownership(vol, entries, count);
    if (trace)
        fprintf(stderr, "INFS_INTEGRITY_TRACE bitmap_ownership=%d count=%u\n",
                status, count);
    if (status != INFS_STATUS_OK)
        goto out;

    for (uint32_t i = 0; i < count; ++i) {
        uint16_t type = infs_le16_to_cpu(entries[i].object_type);
        uint64_t object_block = infs_le64_to_cpu(entries[i].object_block);
        if (type == INFS_OBJECT_CHECKSUM) {
            uint8_t block[INFS_BLOCK_SIZE];
            status = read_object(vol, object_block, block);
            if (status != INFS_STATUS_OK) {
                if (trace)
                    fprintf(stderr, "INFS_INTEGRITY_TRACE checksum_read i=%u block=%llu status=%d\n",
                            i, (unsigned long long)object_block, status);
                goto out;
            }
            struct infs_object_header_disk *hdr =
                (struct infs_object_header_disk *)block;
            struct infs_checksum_payload_disk *cp;
            int checksum_status = checksum_validate(block, NULL, &cp, NULL);
            if (checksum_status != 0 ||
                memcmp(hdr->parent_id, cp->owner_object_id, 16) != 0 ||
                (infs_le64_to_cpu(cp->start_logical_block) %
                 INFS_CHECKSUMS_PER_OBJECT) != 0) {
                if (trace)
                    fprintf(stderr, "INFS_INTEGRITY_TRACE checksum_shape i=%u block=%llu checksum=%d parent_match=%d start=%llu\n",
                            i, (unsigned long long)object_block, checksum_status,
                            memcmp(hdr->parent_id, cp->owner_object_id, 16) == 0,
                            (unsigned long long)infs_le64_to_cpu(cp->start_logical_block));
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            struct infs_lookup owner;
            status = index_find(vol, cp->owner_object_id, &owner);
            if (status != INFS_STATUS_OK || owner.type != INFS_OBJECT_FILE) {
                if (trace)
                    fprintf(stderr, "INFS_INTEGRITY_TRACE checksum_owner i=%u block=%llu status=%d owner_type=%u\n",
                            i, (unsigned long long)object_block, status,
                            status == INFS_STATUS_OK ? owner.type : 0u);
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
                    if (trace)
                        fprintf(stderr, "INFS_INTEGRITY_TRACE checksum_next_index i=%u block=%llu status=%d next_type=%u\n",
                                i, (unsigned long long)object_block, status,
                                status == INFS_STATUS_OK ? next.type : 0u);
                    status = INFS_STATUS_CORRUPT;
                    goto out;
                }
                status = read_object(vol, next.block, next_block);
                if (status != INFS_STATUS_OK) {
                    if (trace)
                        fprintf(stderr, "INFS_INTEGRITY_TRACE checksum_next_read i=%u block=%llu next=%llu status=%d\n",
                                i, (unsigned long long)object_block,
                                (unsigned long long)next.block, status);
                    goto out;
                }
                int next_status = checksum_validate(
                    next_block, cp->owner_object_id, &next_cp, NULL);
                if (next_status != 0 ||
                    infs_le64_to_cpu(next_cp->start_logical_block) <=
                        infs_le64_to_cpu(cp->start_logical_block)) {
                    if (trace)
                        fprintf(stderr, "INFS_INTEGRITY_TRACE checksum_next_shape i=%u block=%llu next=%llu checksum=%d start=%llu next_start=%llu\n",
                                i, (unsigned long long)object_block,
                                (unsigned long long)next.block, next_status,
                                (unsigned long long)infs_le64_to_cpu(cp->start_logical_block),
                                (unsigned long long)infs_le64_to_cpu(next_cp->start_logical_block));
                    status = INFS_STATUS_CORRUPT;
                    goto out;
                }
            }
        } else if (type == INFS_OBJECT_FILE) {
            uint8_t object[INFS_BLOCK_SIZE];
            status = read_object(vol, object_block, object);
            if (status != INFS_STATUS_OK) {
                if (trace)
                    fprintf(stderr, "INFS_INTEGRITY_TRACE file_read i=%u block=%llu status=%d\n",
                            i, (unsigned long long)object_block, status);
                goto out;
            }
            struct infs_object_header_disk *hdr =
                (struct infs_object_header_disk *)object;
            struct infs_file_payload_disk *file;
            struct infs_extent_disk *ext;
            status = file_validate_volume(vol, object, &file, &ext);
            if (status != INFS_STATUS_OK) {
                if (trace)
                    fprintf(stderr, "INFS_INTEGRITY_TRACE file_validate i=%u block=%llu version=%u payload=%u extents=%u status=%d\n",
                            i, (unsigned long long)object_block,
                            (unsigned)infs_le16_to_cpu(hdr->object_version),
                            infs_le32_to_cpu(hdr->payload_size),
                            infs_le32_to_cpu(((struct infs_file_payload_disk *)(hdr + 1))->extent_count),
                            status);
                goto out;
            }
            uint64_t allocated = 0;
            status = file_allocated_blocks_volume(
                vol, object, file, ext, &allocated);
            if (status != INFS_STATUS_OK) {
                if (trace)
                    fprintf(stderr, "INFS_INTEGRITY_TRACE file_allocated i=%u block=%llu status=%d\n",
                            i, (unsigned long long)object_block, status);
                goto out;
            }
            if (allocated && id_is_zero(file->checksum_head_id)) {
                if (trace)
                    fprintf(stderr, "INFS_INTEGRITY_TRACE file_missing_checksum i=%u block=%llu allocated=%llu\n",
                            i, (unsigned long long)object_block,
                            (unsigned long long)allocated);
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            struct infs_extent_disk *all = NULL;
            uint32_t extent_count = 0;
            int owned = 0;
            status = file_extent_snapshot(vol, object, file, ext,
                                          &all, &extent_count, &owned);
            if (status != INFS_STATUS_OK) {
                if (trace)
                    fprintf(stderr, "INFS_INTEGRITY_TRACE file_extent_snapshot i=%u block=%llu status=%d\n",
                            i, (unsigned long long)object_block, status);
                goto out;
            }
            for (uint32_t j = 0; j < extent_count; ++j) {
                if (infs_le32_to_cpu(all[j].flags) != INFS_EXTENT_NORMAL)
                    continue;
                uint64_t logical = infs_le64_to_cpu(all[j].logical_block);
                uint64_t remaining = infs_le32_to_cpu(all[j].block_count);
                while (remaining) {
                    uint64_t chunk = INFS_CHECKSUMS_PER_OBJECT -
                        (logical % INFS_CHECKSUMS_PER_OBJECT);
                    if (chunk > remaining)
                        chunk = remaining;
                    struct infs_data_checksum_disk digest;
                    uint64_t wanted = logical + chunk - 1u;
                    status = checksum_get(vol, file, hdr->object_id,
                                          wanted, &digest);
                    if (status != INFS_STATUS_OK) {
                        if (trace)
                            fprintf(stderr, "INFS_INTEGRITY_TRACE file_checksum_get i=%u block=%llu extent=%u logical=%llu status=%d\n",
                                    i, (unsigned long long)object_block, j,
                                    (unsigned long long)wanted, status);
                        if (owned) free(all);
                        all = NULL;
                        owned = 0;
                        goto out;
                    }
                    logical += chunk;
                    remaining -= chunk;
                }
            }
            if (owned) free(all);
        } else if (type == INFS_OBJECT_DIRECTORY) {
            uint8_t object[INFS_BLOCK_SIZE];
            status = read_object(vol, object_block, object);
            if (status != INFS_STATUS_OK) {
                if (trace)
                    fprintf(stderr, "INFS_INTEGRITY_TRACE dir_read i=%u block=%llu status=%d\n",
                            i, (unsigned long long)object_block, status);
                goto out;
            }
            struct infs_directory_payload_disk *dir;
            status = dir_validate(object, &dir);
            if (status != INFS_STATUS_OK) {
                if (trace)
                    fprintf(stderr, "INFS_INTEGRITY_TRACE dir_validate i=%u block=%llu status=%d\n",
                            i, (unsigned long long)object_block, status);
                goto out;
            }
            struct infs_object_header_disk *header =
                (struct infs_object_header_disk *)object;
            if (infs_le16_to_cpu(header->object_version) ==
                INFS_OBJECT_VERSION_PAGED) {
                uint8_t *snapshot = NULL;
                size_t bytes = 0;
                uint32_t entries_count = 0;
                status = paged_dir_snapshot(vol, object, &snapshot, &bytes,
                                            &entries_count);
                free(snapshot);
                if (status != INFS_STATUS_OK ||
                    entries_count != infs_le32_to_cpu(dir->entry_count)) {
                    if (trace)
                        fprintf(stderr, "INFS_INTEGRITY_TRACE dir_snapshot i=%u block=%llu status=%d entries=%u expected=%u\n",
                                i, (unsigned long long)object_block, status,
                                entries_count, infs_le32_to_cpu(dir->entry_count));
                    if (status == INFS_STATUS_OK)
                        status = INFS_STATUS_CORRUPT;
                    goto out;
                }
                (void)bytes;
            }
        } else if (type == INFS_OBJECT_SYMLINK) {
            uint8_t object[INFS_BLOCK_SIZE];
            status = read_object(vol, object_block, object);
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
            status = snapshot_validate_catalog_graphs(vol);
            if (status != INFS_STATUS_OK)
                goto out;
        }
    }
    status = INFS_STATUS_OK;

out:
    free(entries);
    return status;
}
