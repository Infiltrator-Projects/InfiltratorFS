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

/* Common Format 0.19 graph admission. Normal writable open repairs replicas
 * after validation; strict fixture mode instead requires intact redundancy
 * and refuses physical devices. Read-only open never changes storage. */
static infs_status open_019_volume_mode(
    struct infs_volume *vol, struct infs_storage *storage, int writable,
    int image_only, int repair)
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
    if (is_device && image_only) {
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
    /* Admission must validate the requested access mode. Graph loading is
     * read-only; setting this now cannot publish writes before validation. */
    vol->writable = writable != 0;

    if (writable && !repair) {
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

    infs_status read_error = INFS_STATUS_OK;
    status = infs_locator_discover_019_headers_with_errors(
        &vol->storage, bytes, checkpoints, valid, &found, &read_error);
    if (status != INFS_STATUS_OK)
        goto fail;
    if (writable && read_error != INFS_STATUS_OK) {
        status = read_error;
        goto fail;
    }
    if (writable && !repair) {
        /* Six independently valid candidate copies: 2 x 3 replicas.
         * Refuse an ambiguous startup, including any torn last commit. */
        if (found != 2u * INFS_CHECKPOINT_COUNT) {
            status = INFS_STATUS_CORRUPT;
            goto fail;
        }
        for (unsigned i = 0; i < 2u * INFS_CHECKPOINT_COUNT; ++i) {
            if (!valid[i] || memcmp(&checkpoints[i], &checkpoints[0],
                                    sizeof(checkpoints[0])) != 0) {
                status = INFS_STATUS_CORRUPT;
                goto fail;
            }
        }
    }

    int full_verify = !is_device || found != 2u * INFS_CHECKPOINT_COUNT;
    for (unsigned i = 0; !full_verify && i < 2u * INFS_CHECKPOINT_COUNT; ++i)
        if (!valid[i] || memcmp(&checkpoints[i], &checkpoints[0],
                                sizeof(checkpoints[0])) != 0)
            full_verify = 1;
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
            vol, &checkpoints[best], full_verify, INFS_LOCATOR_FORMAT_MINOR);
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

    if (writable && repair) {
        status = heal_checkpoints(vol);
        if (status == INFS_STATUS_OK)
            status = heal_locators(vol);
        if (status != INFS_STATUS_OK)
            goto fail;
    }
    (void)free_extent_index_rebuild(vol);
    object_cache_init(vol);
    directory_cache_init(vol);
    if (!is_device)
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
    return open_019_volume_mode(vol, storage, 0, 1, 0);
}

infs_status infs_volume_open_019_rw_test_image(
    struct infs_volume *vol, struct infs_storage *storage)
{
    return open_019_volume_mode(vol, storage, 1, 1, 0);
}

infs_status infs_volume_open_019_storage(
    struct infs_volume *vol, struct infs_storage *storage, int writable)
{
    return open_019_volume_mode(vol, storage, writable, 0, 1);
}

infs_status infs_volume_open_storage(struct infs_volume *vol,
                                     struct infs_storage *storage, int writable)
{
    return open_019_volume_mode(vol, storage, writable, 0, 1);
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
