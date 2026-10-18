// SPDX-License-Identifier: GPL-3.0-or-later
static void discard_candidate_state(struct infs_volume *vol)
{
    free_extent_index_destroy(vol);
    directory_cache_destroy(vol);
    object_cache_destroy(vol);
    free(vol->bitmap);
    vol->bitmap = NULL;
    vol->bitmap_bytes = 0;
    allocation_map_cache_destroy(vol);
    vol->data_allocation_cursor = 1u;
    vol->metadata_allocation_cursor = 0;
}

static infs_status checkpoint_graph_status(infs_status status)
{
    if (status == INFS_STATUS_NOT_FOUND ||
        status == INFS_STATUS_LOOP_DETECTED)
        return INFS_STATUS_CORRUPT;
    return status;
}

static infs_status read_checkpoint_candidates(
    struct infs_volume *vol,
    struct infs_superblock_disk candidates[INFS_CHECKPOINT_COUNT],
    uint8_t valid[INFS_CHECKPOINT_COUNT])
{
    if (!vol || !candidates || !valid ||
        vol->size_bytes < INFS_BLOCK_SIZE * 3u)
        return INFS_STATUS_INVALID_ARGUMENT;

    memset(valid, 0, INFS_CHECKPOINT_COUNT);
    const uint64_t storage_blocks = vol->size_bytes / INFS_BLOCK_SIZE;
    uint64_t blocks[INFS_CHECKPOINT_COUNT] = {
        0, storage_blocks / 2u, storage_blocks - 1u
    };
    struct infs_superblock_disk anchor;
    int anchored = 0;
    uint8_t raw[INFS_BLOCK_SIZE];

    infs_status anchor_read = infs_storage_read(
        &vol->storage, 0, raw, sizeof(raw));
    if (anchor_read == INFS_STATUS_OK &&
        infs_validate_superblock_block(raw)) {
        memcpy(&anchor, raw, sizeof(anchor));
        uint64_t total = infs_le64_to_cpu(anchor.total_blocks);
        uint64_t c0 = infs_le64_to_cpu(anchor.checkpoint_block[0]);
        uint64_t c1 = infs_le64_to_cpu(anchor.checkpoint_block[1]);
        uint64_t c2 = infs_le64_to_cpu(anchor.checkpoint_block[2]);
        if (total >= 3u && total <= storage_blocks &&
            c0 == 0 && c1 < total && c2 < total &&
            c1 != c0 && c2 != c0 && c2 != c1) {
            blocks[0] = c0;
            blocks[1] = c1;
            blocks[2] = c2;
            anchored = 1;
        }
    }

    unsigned found = 0;
    infs_status first_read_error = anchor_read == INFS_STATUS_OK ?
        INFS_STATUS_OK : anchor_read;

    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        infs_status status = infs_storage_read(
            &vol->storage, blocks[i] * INFS_BLOCK_SIZE, raw, sizeof(raw));
        if (status != INFS_STATUS_OK) {
            if (first_read_error == INFS_STATUS_OK)
                first_read_error = status;
            continue;
        }
        if (!infs_validate_superblock_block(raw))
            continue;

        struct infs_superblock_disk sb;
        memcpy(&sb, raw, sizeof(sb));
        uint64_t total = infs_le64_to_cpu(sb.total_blocks);
        if (total < 3u || total > storage_blocks ||
            infs_le64_to_cpu(sb.checkpoint_block[0]) != blocks[0] ||
            infs_le64_to_cpu(sb.checkpoint_block[1]) != blocks[1] ||
            infs_le64_to_cpu(sb.checkpoint_block[2]) != blocks[2])
            continue;
        if (anchored &&
            (total != infs_le64_to_cpu(anchor.total_blocks) ||
             memcmp(sb.filesystem_uuid, anchor.filesystem_uuid,
                    sizeof(sb.filesystem_uuid)) != 0))
            continue;

        candidates[i] = sb;
        valid[i] = 1;
        ++found;
    }

    if (first_read_error != INFS_STATUS_OK && vol->writable)
        return first_read_error;
    if (found)
        return INFS_STATUS_OK;
    return first_read_error != INFS_STATUS_OK ?
        first_read_error : INFS_STATUS_CORRUPT;
}

static int checkpoint_candidates_all_agree(
    const struct infs_superblock_disk candidates[INFS_CHECKPOINT_COUNT],
    const uint8_t valid[INFS_CHECKPOINT_COUNT])
{
    int first = -1;
    unsigned count = 0;
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        if (!valid[i])
            continue;
        if (first < 0)
            first = (int)i;
        ++count;
    }
    if (count != INFS_CHECKPOINT_COUNT || first < 0)
        return 0;
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        if (i == (unsigned)first)
            continue;
        if (memcmp(&candidates[i], &candidates[first],
                   sizeof(candidates[i])) != 0)
            return 0;
    }
    return 1;
}

static infs_status load_checkpoint_graph(
    struct infs_volume *vol, const struct infs_superblock_disk *candidate,
    int full_verify, uint16_t expected_minor)
{
    if (!vol || !candidate)
        return INFS_STATUS_INVALID_ARGUMENT;

    discard_candidate_state(vol);
    vol->sb = *candidate;

    if (infs_le16_to_cpu(vol->sb.format_minor) != expected_minor)
        return INFS_STATUS_NOT_SUPPORTED;
    if (vol->writable &&
        (infs_le64_to_cpu(vol->sb.ro_compat_flags) &
         ~INFS_KNOWN_RO_COMPAT_FLAGS) != 0)
        return INFS_STATUS_NOT_SUPPORTED;

    struct infs_allocation_layout committed_layout = {0};
    infs_status status = allocation_map_load(
        vol, candidate, &vol->bitmap, &vol->bitmap_bytes, &committed_layout);
    if (status != INFS_STATUS_OK)
        return status;
    allocation_map_cache_replace(vol, &committed_layout);

    uint8_t object[INFS_BLOCK_SIZE];
    status = read_object(vol, infs_le64_to_cpu(vol->sb.root_object_block), object);
    if (status != INFS_STATUS_OK)
        return checkpoint_graph_status(status);
    status = dir_validate(object, NULL);
    if (status != INFS_STATUS_OK)
        return checkpoint_graph_status(status);
    struct infs_object_header_disk *root_hdr =
        (struct infs_object_header_disk *)object;
    if (memcmp(root_hdr->object_id, vol->sb.root_object_id, 16) != 0 ||
        !id_is_zero(root_hdr->parent_id))
        return INFS_STATUS_CORRUPT;

    uint8_t index_object[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *index_payload;
    status = index_load(vol, index_object, &index_payload);
    if (status != INFS_STATUS_OK)
        return checkpoint_graph_status(status);
    (void)index_payload;
    struct infs_object_header_disk *index_hdr =
        (struct infs_object_header_disk *)index_object;
    int paged_feature =
        (infs_le64_to_cpu(vol->sb.incompat_flags) &
         INFS_INCOMPAT_PAGED_METADATA) != 0;
    int index_tree_feature =
        (infs_le64_to_cpu(vol->sb.incompat_flags) &
         INFS_INCOMPAT_INDEX_TREE) != 0;
    int directory_tree_feature =
        (infs_le64_to_cpu(vol->sb.incompat_flags) &
         INFS_INCOMPAT_DIRECTORY_TREE) != 0;
    int root_paged =
        infs_le16_to_cpu(root_hdr->object_version) == INFS_OBJECT_VERSION_PAGED;
    int root_tree =
        infs_le16_to_cpu(root_hdr->object_version) == INFS_OBJECT_VERSION_TREE;
    int index_paged =
        infs_le16_to_cpu(index_hdr->object_version) == INFS_OBJECT_VERSION_PAGED;
    int index_tree =
        infs_le16_to_cpu(index_hdr->object_version) == INFS_OBJECT_VERSION_TREE;
    if (directory_tree_feature != root_tree ||
        (!directory_tree_feature && paged_feature != root_paged) ||
        index_tree_feature != index_tree ||
        (!index_tree_feature && paged_feature != index_paged))
        return INFS_STATUS_CORRUPT;

    /* A real mounted device should not perform an fsck-equivalent graph walk
     * merely to become usable. Verify the root can be resolved through the
     * active index, then leave whole-volume ownership/namespace/checksum graph
     * validation to infs_scrub(). Regular image files retain the historical
     * strict-open behaviour so conformance and forensic/offline workflows stay
     * maximally defensive. */
    struct infs_lookup root_lookup;
    status = index_find(vol, vol->sb.root_object_id, &root_lookup);
    if (status != INFS_STATUS_OK ||
        root_lookup.block != infs_le64_to_cpu(vol->sb.root_object_block) ||
        root_lookup.type != INFS_OBJECT_DIRECTORY)
        return status == INFS_STATUS_OK ? INFS_STATUS_CORRUPT :
            checkpoint_graph_status(status);

    if (!full_verify)
        return INFS_STATUS_OK;

    status = validate_index_entries(vol);
    if (status != INFS_STATUS_OK)
        return checkpoint_graph_status(status);
    status = validate_integrity_metadata(vol);
    if (status != INFS_STATUS_OK)
        return checkpoint_graph_status(status);
    status = validate_namespace_graph(vol);
    if (status != INFS_STATUS_OK)
        return checkpoint_graph_status(status);
    return checkpoint_graph_status(validate_checksum_graph(vol));
}

/*
 * Standalone Format 0.19 *read-only test-image* graph verification. This
 * does not give the caller a mounted volume, does not consume or close their
 * storage handle, and never heals/writes checkpoint or locator records.
 * It deliberately refuses block-device targets until native and portable
 * 0.19 mount, resize, scrub and crash qualifications are complete.
 */
infs_status infs_volume_verify_019_test_image(
    const struct infs_storage *storage,
    struct infs_superblock_disk *selected)
{
    struct infs_volume candidate_vol = {0};
    struct infs_superblock_disk checkpoints[INFS_LOCATOR_MAX_CANDIDATES];
    uint8_t valid[INFS_LOCATOR_MAX_CANDIDATES] = {0};
    uint64_t bytes = 0;
    int is_device = 0;
    unsigned found = 0;
    infs_status status;
    int selected_valid = 0;

    if (!storage || !infs_storage_valid(storage) || !selected)
        return INFS_STATUS_INVALID_ARGUMENT;
    status = infs_storage_get_size(storage, &bytes, &is_device);
    if (status != INFS_STATUS_OK)
        return status;
    if (is_device)
        return INFS_STATUS_NOT_SUPPORTED;

    status = infs_locator_discover_019_headers(
        storage, bytes, checkpoints, valid, &found);
    if (status != INFS_STATUS_OK)
        return status;

    candidate_vol.storage = *storage; /* Borrowed; never close backend. */
    candidate_vol.size_bytes = bytes;
    candidate_vol.writable = 0;

    for (unsigned attempt = 0; attempt < INFS_LOCATOR_MAX_CANDIDATES;
         ++attempt) {
        int best = -1;
        uint64_t generation = 0;
        for (unsigned i = 0; i < INFS_LOCATOR_MAX_CANDIDATES; ++i) {
            if (!valid[i])
                continue;
            uint64_t g = infs_le64_to_cpu(checkpoints[i].generation);
            if (best < 0 || g > generation) {
                best = (int)i;
                generation = g;
            }
        }
        if (best < 0)
            break;
        valid[best] = 0;
        status = load_checkpoint_graph(
            &candidate_vol, &checkpoints[best], 1,
            INFS_LOCATOR_FORMAT_MINOR);
        if (status == INFS_STATUS_OK) {
            *selected = checkpoints[best];
            selected_valid = 1;
            break;
        }
        /* A bad checkpoint graph may be skipped; I/O, OOM and unsupported
         * features are operational errors, never masked by older data. */
        if (status != INFS_STATUS_CORRUPT)
            break;
    }
    discard_candidate_state(&candidate_vol);
    memset(&candidate_vol, 0, sizeof(candidate_vol));
    return selected_valid ? INFS_STATUS_OK : status;
}

/* Experimental Format 0.19 disposable-image open. The writable mode needs
 * two consistent locators, three identical checkpoint replicas and a fully
 * verified graph; it never accepts physical devices or silently repairs
 * degraded media. The production 0.18 open path remains separate. */
static infs_status open_019_test_image_mode(
    struct infs_volume *vol, struct infs_storage *storage, int writable)
{
    struct infs_superblock_disk checkpoints[INFS_LOCATOR_MAX_CANDIDATES];
    uint8_t valid[INFS_LOCATOR_MAX_CANDIDATES] = {0};
    uint64_t bytes = 0;
    int is_device = 0;
    unsigned found = 0;
    int selected = 0;
    infs_status status = INFS_STATUS_OK;

    if (!vol || !storage || !infs_storage_valid(storage))
        return INFS_STATUS_INVALID_ARGUMENT;

    memset(vol, 0, sizeof(*vol));
    /* Keep the storage handle caller-owned if any validation step fails. */
    vol->storage = *storage;
    vol->data_allocation_cursor = 1u;
    status = infs_storage_get_size(&vol->storage, &bytes, &is_device);
    if (status != INFS_STATUS_OK)
        goto fail;
    if (is_device) {
        status = INFS_STATUS_NOT_SUPPORTED;
        goto fail;
    }
    if (writable && (!storage->ops->write_at || !storage->ops->flush ||
                     !storage->ops->random_bytes ||
                     !storage->ops->current_time)) {
        status = INFS_STATUS_INVALID_ARGUMENT;
        goto fail;
    }
    vol->size_bytes = bytes;

    if (writable) {
        uint8_t raw[INFS_BLOCK_SIZE];
        struct infs_locator a = {0}, b = {0};
        /* Locator generation may lag ordinary same-geometry commits, but
         * the two durable records must still describe identical geometry. */
        status = infs_storage_read(&vol->storage,
            INFS_LOCATOR_BLOCK_A * INFS_BLOCK_SIZE, raw, sizeof(raw));
        if (status != INFS_STATUS_OK)
            goto fail;
        status = infs_locator_decode(raw, bytes / INFS_BLOCK_SIZE, &a);
        if (status != INFS_STATUS_OK)
            goto fail;
        status = infs_storage_read(&vol->storage,
            INFS_LOCATOR_BLOCK_B * INFS_BLOCK_SIZE, raw, sizeof(raw));
        if (status != INFS_STATUS_OK)
            goto fail;
        status = infs_locator_decode(raw, bytes / INFS_BLOCK_SIZE, &b);
        if (status != INFS_STATUS_OK)
            goto fail;
        if (memcmp(&a, &b, sizeof(a)) != 0) {
            status = INFS_STATUS_CORRUPT;
            goto fail;
        }
    }

    status = infs_locator_discover_019_headers(
        &vol->storage, bytes, checkpoints, valid, &found);
    if (status != INFS_STATUS_OK)
        goto fail;
    if (writable) {
        /* Six independently valid candidate copies: 2 x 3 replicas.
         * Refuse an ambiguous startup, including any torn last commit. */
        if (found != INFS_LOCATOR_MAX_CANDIDATES) {
            status = INFS_STATUS_CORRUPT;
            goto fail;
        }
        for (unsigned i = 0; i < INFS_LOCATOR_MAX_CANDIDATES; ++i) {
            if (!valid[i] || memcmp(&checkpoints[i], &checkpoints[0],
                                    sizeof(checkpoints[0])) != 0) {
                status = INFS_STATUS_CORRUPT;
                goto fail;
            }
        }
    }

    for (unsigned attempt = 0; attempt < INFS_LOCATOR_MAX_CANDIDATES;
         ++attempt) {
        int best = -1;
        uint64_t generation = 0;
        for (unsigned i = 0; i < INFS_LOCATOR_MAX_CANDIDATES; ++i) {
            uint64_t candidate_generation;
            if (!valid[i])
                continue;
            candidate_generation =
                infs_le64_to_cpu(checkpoints[i].generation);
            if (best < 0 || candidate_generation > generation) {
                best = (int)i;
                generation = candidate_generation;
            }
        }
        if (best < 0)
            break;
        valid[best] = 0;
        status = load_checkpoint_graph(
            vol, &checkpoints[best], 1, INFS_LOCATOR_FORMAT_MINOR);
        if (status == INFS_STATUS_OK) {
            selected = 1;
            break;
        }
        if (status != INFS_STATUS_CORRUPT)
            goto fail;
    }
    if (!selected) {
        status = INFS_STATUS_CORRUPT;
        goto fail;
    }

    /* Persistent locator blocks must be reserved by the committed bitmap,
     * not merely present in the two discovery records. */
    if (!vol->bitmap ||
        !bitmap_get(vol->bitmap, INFS_LOCATOR_BLOCK_A) ||
        !bitmap_get(vol->bitmap, INFS_LOCATOR_BLOCK_B)) {
        status = INFS_STATUS_CORRUPT;
        goto fail;
    }

    (void)free_extent_index_rebuild(vol);
    object_cache_init(vol);
    directory_cache_init(vol);
    (void)object_cache_rebuild(vol);
    vol->metadata_allocation_cursor =
        infs_le64_to_cpu(vol->sb.total_blocks);
    /* Enable writes only after all full-graph/replica/locator checks. */
    vol->writable = writable != 0;
    storage->ops = NULL;
    storage->context = NULL;
    return INFS_STATUS_OK;

fail:
    discard_candidate_state(vol);
    memset(vol, 0, sizeof(*vol));
    return status == INFS_STATUS_OK ? INFS_STATUS_CORRUPT : status;
}

infs_status infs_volume_open_019_test_image(
    struct infs_volume *vol, struct infs_storage *storage)
{
    return open_019_test_image_mode(vol, storage, 0);
}

infs_status infs_volume_open_019_rw_test_image(
    struct infs_volume *vol, struct infs_storage *storage)
{
    return open_019_test_image_mode(vol, storage, 1);
}

infs_status infs_volume_open_storage(struct infs_volume *vol,
                                     struct infs_storage *storage, int writable)
{
    if (!vol || !storage || !infs_storage_valid(storage))
        return INFS_STATUS_INVALID_ARGUMENT;
    memset(vol, 0, sizeof(*vol));
    vol->writable = writable != 0;
    /* Keep caller ownership intact until open has completed successfully.
     * The volume may borrow the handle while validating candidates, but a
     * failed open must not consume or close a backend the API still assigns
     * to the caller. */
    vol->storage = *storage;
    vol->data_allocation_cursor = 1u;
    vol->metadata_allocation_cursor = 0;

    infs_status status = INFS_STATUS_OK;
    if (vol->writable &&
        (!vol->storage.ops->write_at || !vol->storage.ops->flush ||
         !vol->storage.ops->random_bytes ||
         !vol->storage.ops->current_time)) {
        status = INFS_STATUS_INVALID_ARGUMENT;
        goto fail;
    }

    int is_block = 0;
    status = infs_storage_get_size(&vol->storage, &vol->size_bytes, &is_block);
    if (status != INFS_STATUS_OK)
        goto fail;

    struct infs_superblock_disk candidates[INFS_CHECKPOINT_COUNT];
    uint8_t valid[INFS_CHECKPOINT_COUNT];
    status = read_checkpoint_candidates(vol, candidates, valid);
    if (status != INFS_STATUS_OK)
        goto fail;

    /* Ordinary healthy block-device mounts stay lightweight. A writable
     * recovery mount is different: if checkpoint replicas are missing or do
     * not agree byte-for-byte, healing them could destroy the last known-good
     * generation. In that degraded state require the complete graph checks
     * while selecting the recovery candidate, then and only then repair the
     * replicas. */
    int full_verify = !is_block;
    if (vol->writable && is_block &&
        !checkpoint_candidates_all_agree(candidates, valid))
        full_verify = 1;

    int selected = 0;
    for (unsigned attempt = 0; attempt < INFS_CHECKPOINT_COUNT; ++attempt) {
        int best = -1;
        uint64_t best_generation = 0;
        for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
            if (!valid[i])
                continue;
            uint64_t generation = infs_le64_to_cpu(candidates[i].generation);
            if (best < 0 || generation > best_generation) {
                best = (int)i;
                best_generation = generation;
            }
        }
        if (best < 0)
            break;

        valid[best] = 0;
        status = load_checkpoint_graph(
            vol, &candidates[best], full_verify, INFS_FORMAT_MINOR);
        if (status == INFS_STATUS_OK) {
            selected = 1;
            break;
        }

        discard_candidate_state(vol);
        if (status != INFS_STATUS_CORRUPT)
            goto fail;
    }

    if (!selected) {
        status = INFS_STATUS_CORRUPT;
        goto fail;
    }

    if (vol->writable) {
        status = heal_checkpoints(vol);
        if (status != INFS_STATUS_OK)
            goto fail;
    }

    /* The bitmap is authoritative. Build the free-run index from it once;
     * allocation remains correct if memory pressure disables this cache. */
    (void)free_extent_index_rebuild(vol);

    /* Runtime caches are deliberately best-effort: failure to allocate a
     * cache must never make an otherwise valid filesystem unmountable. */
    object_cache_init(vol);
    directory_cache_init(vol);
    /*
     * A raw block-device mount must not become an implicit object-index walk.
     * The cache is only an accelerator: the tree supplies bounded lookup
     * without pre-population, and individual successful lookups fill it
     * naturally. Offline/image workflows retain eager cache population.
     */
    if (!is_block)
        (void)object_cache_rebuild(vol);
    vol->data_allocation_cursor = 1u;
    vol->metadata_allocation_cursor =
        infs_le64_to_cpu(vol->sb.total_blocks);
    storage->ops = NULL;
    storage->context = NULL;
    return INFS_STATUS_OK;

fail:
    discard_candidate_state(vol);
    /* The caller still owns the original storage handle. Clear the borrowed
     * alias and all failed-open session state without invoking backend close. */
    memset(vol, 0, sizeof(*vol));
    return status == INFS_STATUS_OK ? INFS_STATUS_ERROR : status;
}

void infs_volume_close(struct infs_volume *vol)
{
    if (!vol)
        return;
    tx_abort(vol);
    tx_operation_discard(vol);
    free(vol->tx_allocated);
    vol->tx_allocated = NULL;
    vol->tx_allocated_count = 0;
    vol->tx_allocated_capacity = 0;
    free(vol->tx_deferred);
    vol->tx_deferred = NULL;
    vol->tx_deferred_count = 0;
    vol->tx_deferred_capacity = 0;
    discard_candidate_state(vol);
    infs_storage_close(&vol->storage);
}
