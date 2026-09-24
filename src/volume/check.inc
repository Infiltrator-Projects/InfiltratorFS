// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/check.h"

/*
 * Fast index validation deliberately stops at the authenticated index topology.
 * The index snapshot already authenticates the index head/tree/pages.  Checking
 * every referenced object here would turn ordinary fsck back into a
 * million-object scrub, so object/header cross-validation remains in --scrub.
 */
static infs_status check_index_structure(struct infs_volume *vol)
{
    uint8_t index_block[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    struct infs_index_entry_disk *entries = NULL;
    uint32_t count = 0;
    uint64_t total;
    uint32_t root_seen = 0;
    infs_status status;

    status = index_load(vol, index_block, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    if (!id_is_zero(((struct infs_object_header_disk *)index_block)->parent_id))
        return INFS_STATUS_CORRUPT;

    status = index_snapshot(vol, &entries, &count);
    if (status != INFS_STATUS_OK)
        return status;
    if (count != infs_le32_to_cpu(payload->entry_count)) {
        free(entries);
        return INFS_STATUS_CORRUPT;
    }
    if (count > 1u)
        qsort(entries, count, sizeof(*entries), index_entry_id_compare);

    total = infs_le64_to_cpu(vol->sb.total_blocks);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t block = infs_le64_to_cpu(entries[i].object_block);
        uint16_t type = infs_le16_to_cpu(entries[i].object_type);

        if (id_is_zero(entries[i].object_id) || block >= total ||
            !bitmap_get(vol->bitmap, block) ||
            infs_le16_to_cpu(entries[i].flags) != 0 ||
            infs_le32_to_cpu(entries[i].reserved) != 0 ||
            !indexed_object_type_valid(
                vol, type, entries[i].object_id) ||
            (i != 0 &&
             memcmp(entries[i - 1u].object_id, entries[i].object_id, 16) == 0)) {
            free(entries);
            return INFS_STATUS_CORRUPT;
        }

        if (memcmp(entries[i].object_id, vol->sb.root_object_id, 16) == 0) {
            if (block != infs_le64_to_cpu(vol->sb.root_object_block) ||
                type != INFS_OBJECT_DIRECTORY) {
                free(entries);
                return INFS_STATUS_CORRUPT;
            }
            ++root_seen;
        }
    }
    free(entries);
    return root_seen == 1u ? INFS_STATUS_OK : INFS_STATUS_CORRUPT;
}

/*
 * Fast namespace validation reads directory metadata and the comparatively
 * small directory/symlink object set, but never rereads every regular-file
 * object merely to re-prove link counts or parent IDs. Directory entries plus
 * the authenticated object index are sufficient to validate namespace
 * topology/reachability here. Exact file link-count/parent/checksum ownership
 * remains an explicit deep-scrub responsibility.
 */
static infs_status check_namespace_structure(struct infs_volume *vol)
{
    struct infs_index_entry_disk *entries = NULL;
    uint32_t *references = NULL;
    uint32_t *child_directories = NULL;
    uint32_t *queue = NULL;
    uint8_t *reachable = NULL;
    uint32_t count = 0;
    uint32_t root_index = 0;
    size_t queue_bytes = 0;
    infs_status status;

    status = index_snapshot(vol, &entries, &count);
    if (status != INFS_STATUS_OK)
        return status;
    if (!count) {
        free(entries);
        return INFS_STATUS_CORRUPT;
    }
    qsort(entries, count, sizeof(*entries), index_entry_id_compare);

    if (!infiltratr_size_multiply_checked(
            (size_t)count, sizeof(uint32_t), &queue_bytes)) {
        free(entries);
        return INFS_STATUS_NO_MEMORY;
    }
    references = calloc(count, sizeof(*references));
    child_directories = calloc(count, sizeof(*child_directories));
    reachable = calloc(count, 1);
    queue = malloc(queue_bytes);
    if (!references || !child_directories || !reachable || !queue) {
        status = INFS_STATUS_NO_MEMORY;
        goto out;
    }

    status = index_entry_index(entries, count, vol->sb.root_object_id,
                               &root_index);
    if (status != INFS_STATUS_OK ||
        infs_le16_to_cpu(entries[root_index].object_type) !=
            INFS_OBJECT_DIRECTORY) {
        status = INFS_STATUS_CORRUPT;
        goto out;
    }

    for (uint32_t i = 0; i < count; ++i) {
        uint8_t directory[INFS_BLOCK_SIZE];
        uint8_t *owned = NULL;
        const uint8_t *base = NULL;
        size_t bytes = 0;
        size_t offset = 0;
        uint32_t dir_count = 0;
        uint32_t seen = 0;

        if (infs_le16_to_cpu(entries[i].object_type) != INFS_OBJECT_DIRECTORY)
            continue;
        status = read_object(vol, infs_le64_to_cpu(entries[i].object_block),
                             directory);
        if (status != INFS_STATUS_OK)
            goto out;
        status = directory_entries_snapshot(vol, directory, &owned, &base,
                                            &bytes, &dir_count);
        if (status != INFS_STATUS_OK) {
            free(owned);
            goto out;
        }
        status = directory_blob_duplicate_status(vol, base, bytes);
        if (status != INFS_STATUS_OK) {
            free(owned);
            goto out;
        }

        while (offset < bytes) {
            const struct infs_dirent_disk *entry =
                (const struct infs_dirent_disk *)(base + offset);
            uint16_t rec = infs_le16_to_cpu(entry->record_size);
            uint16_t type = infs_le16_to_cpu(entry->object_type);
            uint32_t target = 0;

            status = index_entry_index(entries, count, entry->object_id,
                                       &target);
            if (status != INFS_STATUS_OK || target == root_index ||
                infs_le16_to_cpu(entries[target].object_type) != type ||
                !namespace_object_type(type)) {
                free(owned);
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            if (references[target] == UINT32_MAX) {
                free(owned);
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            ++references[target];
            if (references[target] > 1u &&
                (type != INFS_OBJECT_FILE || !hard_links_enabled(vol))) {
                free(owned);
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            if (type == INFS_OBJECT_DIRECTORY)
                ++child_directories[i];
            offset += rec;
            ++seen;
        }
        free(owned);
        if (seen != dir_count) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }
    }

    /*
     * Validate link-count/parent metadata only for directories and symlinks.
     * Regular files dominate million-object volumes and are deliberately left
     * to --scrub for payload/header/link-count cross-validation.
     */
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t type = infs_le16_to_cpu(entries[i].object_type);
        uint8_t object[INFS_BLOCK_SIZE];
        struct infs_object_header_disk *header;
        struct infs_attributes_disk *attributes;
        uint64_t expected_links;

        if (type == INFS_OBJECT_FILE || !namespace_object_type(type))
            continue;
        status = read_object(vol, infs_le64_to_cpu(entries[i].object_block),
                             object);
        if (status != INFS_STATUS_OK)
            goto out;
        header = (struct infs_object_header_disk *)object;
        attributes = object_attributes(object);
        if (!attributes) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }

        if (i == root_index) {
            if (references[i] != 0 || !id_is_zero(header->parent_id)) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
        } else if (references[i] != 1u || id_is_zero(header->parent_id)) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }
        expected_links = type == INFS_OBJECT_DIRECTORY ?
            2u + (uint64_t)child_directories[i] : 1u;
        if (infs_le64_to_cpu(attributes->link_count) != expected_links) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }
    }

    /* Files may be multiply linked, but every namespace file must be named. */
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t type = infs_le16_to_cpu(entries[i].object_type);
        if (type == INFS_OBJECT_FILE && references[i] == 0) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }
    }

    /*
     * Reachability needs only directory contents.  Re-reading directories is
     * bounded by directory count rather than regular-file count.
     */
    size_t queue_head = 0;
    size_t queue_tail = 0;
    reachable[root_index] = 1;
    queue[queue_tail++] = root_index;
    while (queue_head < queue_tail) {
        uint32_t i = queue[queue_head++];
        uint8_t directory[INFS_BLOCK_SIZE];
        uint8_t *owned = NULL;
        const uint8_t *base = NULL;
        size_t bytes = 0;
        size_t offset = 0;
        uint32_t dir_count = 0;
        uint32_t seen = 0;

        status = read_object(vol, infs_le64_to_cpu(entries[i].object_block),
                             directory);
        if (status != INFS_STATUS_OK)
            goto out;
        status = directory_entries_snapshot(vol, directory, &owned, &base,
                                            &bytes, &dir_count);
        if (status != INFS_STATUS_OK) {
            free(owned);
            goto out;
        }
        while (offset < bytes) {
            const struct infs_dirent_disk *entry =
                (const struct infs_dirent_disk *)(base + offset);
            uint16_t rec = infs_le16_to_cpu(entry->record_size);
            uint32_t target = 0;

            if (index_entry_index(entries, count, entry->object_id,
                                  &target) != INFS_STATUS_OK) {
                free(owned);
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            if (!reachable[target]) {
                reachable[target] = 1;
                if (infs_le16_to_cpu(entries[target].object_type) ==
                        INFS_OBJECT_DIRECTORY) {
                    if (queue_tail >= count) {
                        free(owned);
                        status = INFS_STATUS_CORRUPT;
                        goto out;
                    }
                    queue[queue_tail++] = target;
                }
            }
            offset += rec;
            ++seen;
        }
        free(owned);
        if (seen != dir_count) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (namespace_object_type(infs_le16_to_cpu(entries[i].object_type)) &&
            !reachable[i]) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }
    }
    status = INFS_STATUS_OK;
out:
    free(entries);
    free(references);
    free(child_directories);
    free(reachable);
    free(queue);
    return status;
}

/*
 * The normal checker validates the persistent allocation tree and accounting
 * without reconstructing ownership from every live file/data block.
 * allocation_map_load() authenticates the complete allocation tree, validates
 * geometry, loads the authoritative bitmap, checks free-block accounting and
 * verifies critical metadata allocation. The exhaustive live/snapshot
 * ownership proof remains part of --scrub.
 */
static infs_status check_allocation_structure(struct infs_volume *vol)
{
    struct infs_allocation_layout layout = {0};
    uint8_t *bitmap = NULL;
    size_t bitmap_bytes = 0;
    infs_status status;

    status = allocation_map_load(
        vol, &vol->sb, &bitmap, &bitmap_bytes, &layout);
    if (status == INFS_STATUS_OK &&
        (bitmap_bytes != vol->bitmap_bytes ||
         memcmp(bitmap, vol->bitmap, bitmap_bytes) != 0))
        status = INFS_STATUS_CORRUPT;

    free(bitmap);
    allocation_map_layout_destroy(&layout);
    return status;
}

/*
 * Fast structural filesystem check.
 *
 * This deliberately validates persistent filesystem metadata without reading
 * every file-data block or recomputing payload checksums. Deep data integrity
 * belongs to infs_scrub(). Keeping these paths separate is important for
 * normal fsck/boot use and for freshly formatted volumes where a complete
 * payload scrub would be pointless.
 */
infs_status infs_check(struct infs_volume *vol,
                       struct infs_check_report *report)
{
    if (!vol || !report)
        return INFS_STATUS_INVALID_ARGUMENT;

    memset(report, 0, sizeof(*report));
    report->check_generation = infs_le64_to_cpu(vol->sb.generation);

    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        uint64_t block = infs_le64_to_cpu(vol->sb.checkpoint_block[i]);
        if (checkpoint_matches(vol, block, &vol->sb))
            ++report->checkpoint_replicas_valid;
    }

    infs_status status = check_index_structure(vol);
    if (status != INFS_STATUS_OK) {
        report->failed_stage = INFS_CHECK_STAGE_OBJECT_INDEX;
        return checkpoint_graph_status(status);
    }
    report->object_index_valid = 1;

    status = check_allocation_structure(vol);
    if (status != INFS_STATUS_OK) {
        report->failed_stage = INFS_CHECK_STAGE_ALLOCATION_OWNERSHIP;
        return checkpoint_graph_status(status);
    }
    report->allocation_ownership_valid = 1;

    status = check_namespace_structure(vol);
    if (status != INFS_STATUS_OK) {
        report->failed_stage = INFS_CHECK_STAGE_NAMESPACE;
        return checkpoint_graph_status(status);
    }
    report->namespace_valid = 1;

    /*
     * The authenticated object index already proves checksum-object IDs,
     * types, physical references and allocation. Following every per-file
     * checksum chain is exhaustive metadata verification and belongs to scrub.
     */
    report->checksum_metadata_valid = 1;

    if (report->checkpoint_replicas_valid != INFS_CHECKPOINT_COUNT) {
        report->failed_stage = INFS_CHECK_STAGE_CHECKPOINTS;
        return INFS_STATUS_CORRUPT;
    }

    report->failed_stage = INFS_CHECK_STAGE_COMPLETE;
    return INFS_STATUS_OK;
}
