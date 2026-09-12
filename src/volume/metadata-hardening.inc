// SPDX-License-Identifier: GPL-3.0-or-later
/* Format hardening. These checks validate namespace reachability, link counts,
 * checksum ownership and the scalable Format 0.18 metadata representation. */

static int validate_common_metadata(
    const struct infs_attributes_disk *attributes,
    const struct infs_posix_compat_disk *posix)
{
    if (!attributes || !posix)
        return INFS_STATUS_CORRUPT;
    if ((infs_le64_to_cpu(attributes->portable_flags) &
         ~INFS_KNOWN_ATTR_FLAGS) != 0 ||
        !timestamp_disk_valid(&attributes->birth_time) ||
        !timestamp_disk_valid(&attributes->access_time) ||
        !timestamp_disk_valid(&attributes->modification_time) ||
        !timestamp_disk_valid(&attributes->change_time))
        return INFS_STATUS_CORRUPT;
    if (!id_is_zero(attributes->security_object_id) ||
        !id_is_zero(attributes->extended_attributes_object_id))
        return INFS_STATUS_CORRUPT;
    if ((infs_le32_to_cpu(posix->permissions) & ~07777u) != 0 ||
        infs_le32_to_cpu(posix->flags) != 0)
        return INFS_STATUS_CORRUPT;
    return INFS_STATUS_OK;
}

static infs_status generate_unique_object_id(struct infs_volume *vol,
                                             uint8_t id[16])
{
    if (!vol || !id)
        return INFS_STATUS_INVALID_ARGUMENT;
    for (unsigned attempt = 0; attempt < 32u; ++attempt) {
        infs_status status = infs_storage_random(&vol->storage, id, 16);
        if (status != INFS_STATUS_OK)
            return status;
        if (id_is_zero(id))
            continue;
        if (memcmp(id, snapshot_catalog_id, sizeof(snapshot_catalog_id)) == 0)
            continue;
        struct infs_lookup lookup;
        status = index_find(vol, id, &lookup);
        if (status == INFS_STATUS_NOT_FOUND)
            return INFS_STATUS_OK;
        if (status != INFS_STATUS_OK)
            return status;
    }
    return INFS_STATUS_ERROR;
}

/* Validation callers sort their private index snapshot by object id once.
 * Lookups are therefore logarithmic instead of repeatedly scanning the whole
 * index for every namespace entry and checksum object. */
static int index_entry_index(const struct infs_index_entry_disk *entries,
                             uint32_t count, const uint8_t id[16],
                             uint32_t *index_out)
{
    uint32_t low = 0;
    uint32_t high = count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2u;
        int cmp = memcmp(entries[mid].object_id, id, 16);
        if (cmp < 0)
            low = mid + 1u;
        else
            high = mid;
    }
    if (low < count && memcmp(entries[low].object_id, id, 16) == 0) {
        if (index_out)
            *index_out = low;
        return INFS_STATUS_OK;
    }
    return INFS_STATUS_NOT_FOUND;
}

static int directory_entries_snapshot(
    struct infs_volume *vol, uint8_t directory[INFS_BLOCK_SIZE],
    uint8_t **owned_out, const uint8_t **base_out, size_t *bytes_out,
    uint32_t *count_out)
{
    if (!owned_out || !base_out || !bytes_out || !count_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    *owned_out = NULL;
    *base_out = NULL;
    *bytes_out = 0;
    *count_out = 0;

    struct infs_directory_payload_disk *payload;
    infs_status status = dir_validate(directory, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)directory;
    uint32_t count = infs_le32_to_cpu(payload->entry_count);
    if (infs_le16_to_cpu(header->object_version) ==
            INFS_OBJECT_VERSION_TREE ||
        infs_le16_to_cpu(header->object_version) ==
            INFS_OBJECT_VERSION_PAGED) {
        uint8_t *snapshot = NULL;
        size_t bytes = 0;
        uint32_t snapshot_count = 0;
        status = infs_le16_to_cpu(header->object_version) ==
                INFS_OBJECT_VERSION_TREE ?
            tree_dir_snapshot(vol, directory, &snapshot, &bytes,
                              &snapshot_count) :
            paged_dir_snapshot(vol, directory, &snapshot, &bytes,
                               &snapshot_count);
        if (status != INFS_STATUS_OK)
            return status;
        if (snapshot_count != count) {
            free(snapshot);
            return INFS_STATUS_CORRUPT;
        }
        *owned_out = snapshot;
        *base_out = snapshot;
        *bytes_out = bytes;
        *count_out = count;
        return INFS_STATUS_OK;
    }

    *base_out = (const uint8_t *)(payload + 1);
    *bytes_out = infs_le32_to_cpu(payload->bytes_used);
    *count_out = count;
    return INFS_STATUS_OK;
}

struct directory_name_ref {
    const uint8_t *name;
    uint16_t length;
};

static int directory_name_ref_compare(const void *left, const void *right)
{
    const struct directory_name_ref *a = left;
    const struct directory_name_ref *b = right;
    uint16_t common = a->length < b->length ? a->length : b->length;
    int cmp = memcmp(a->name, b->name, common);
    if (cmp != 0)
        return cmp;
    if (a->length < b->length)
        return -1;
    if (a->length > b->length)
        return 1;
    return 0;
}

static int directory_blob_duplicate_status(const uint8_t *base, size_t bytes)
{
    if (!base && bytes)
        return INFS_STATUS_INVALID_ARGUMENT;

    size_t offset = 0;
    size_t count = 0;
    while (offset < bytes) {
        const struct infs_dirent_disk *entry =
            (const struct infs_dirent_disk *)(base + offset);
        uint16_t rec = infs_le16_to_cpu(entry->record_size);
        if (rec < sizeof(*entry) || rec > bytes - offset)
            return INFS_STATUS_CORRUPT;
        ++count;
        offset += rec;
    }
    if (count < 2u)
        return INFS_STATUS_OK;
    if (count > SIZE_MAX / sizeof(struct directory_name_ref))
        return INFS_STATUS_NO_MEMORY;

    struct directory_name_ref *names = malloc(count * sizeof(*names));
    if (!names)
        return INFS_STATUS_NO_MEMORY;
    offset = 0;
    size_t at = 0;
    while (offset < bytes) {
        const struct infs_dirent_disk *entry =
            (const struct infs_dirent_disk *)(base + offset);
        uint16_t rec = infs_le16_to_cpu(entry->record_size);
        names[at].length = infs_le16_to_cpu(entry->name_length);
        names[at].name = (const uint8_t *)entry + sizeof(*entry);
        ++at;
        offset += rec;
    }

    qsort(names, count, sizeof(*names), directory_name_ref_compare);
    int status = INFS_STATUS_OK;
    for (size_t i = 1; i < count; ++i) {
        if (names[i - 1u].length == names[i].length &&
            memcmp(names[i - 1u].name, names[i].name, names[i].length) == 0) {
            status = INFS_STATUS_CORRUPT;
            break;
        }
    }
    free(names);
    return status;
}

static int validate_namespace_graph_progress(
    struct infs_volume *vol, struct scrub_metadata_progress_context *progress)
{
    struct infs_index_entry_disk *entries = NULL;
    uint32_t count = 0;
    int status = index_snapshot(vol, &entries, &count);
    if (status != INFS_STATUS_OK)
        return status;
    if (count == 0) {
        free(entries);
        return INFS_STATUS_CORRUPT;
    }
    qsort(entries, count, sizeof(*entries), index_entry_id_compare);

    uint32_t directory_total = 0;
    uint32_t namespace_total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t type = infs_le16_to_cpu(entries[i].object_type);
        if (type == INFS_OBJECT_DIRECTORY)
            ++directory_total;
        if (namespace_object_type(type))
            ++namespace_total;
    }

    uint32_t *references = calloc(count, sizeof(*references));
    uint32_t *child_directories = calloc(count, sizeof(*child_directories));
    uint8_t *reachable = calloc(count, 1);
    uint32_t *queue = malloc((size_t)count * sizeof(*queue));
    if (!references || !child_directories || !reachable || !queue) {
        free(entries);
        free(references);
        free(child_directories);
        free(reachable);
        free(queue);
        return INFS_STATUS_NO_MEMORY;
    }

    uint32_t root_index = 0;
    status = index_entry_index(entries, count, vol->sb.root_object_id,
                               &root_index);
    if (status != INFS_STATUS_OK ||
        infs_le16_to_cpu(entries[root_index].object_type) !=
            INFS_OBJECT_DIRECTORY) {
        status = INFS_STATUS_CORRUPT;
        goto out;
    }

    scrub_metadata_progress_begin(
        progress, vol, INFS_SCRUB_METADATA_NAMESPACE_DIRECTORIES,
        directory_total);
    uint64_t directory_done = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (infs_le16_to_cpu(entries[i].object_type) != INFS_OBJECT_DIRECTORY)
            continue;

        uint8_t directory[INFS_BLOCK_SIZE];
        status = read_object(vol, infs_le64_to_cpu(entries[i].object_block),
                             directory);
        if (status != INFS_STATUS_OK)
            goto out;
        uint8_t *owned = NULL;
        const uint8_t *base = NULL;
        size_t bytes = 0;
        uint32_t dir_count = 0;
        status = directory_entries_snapshot(vol, directory, &owned, &base,
                                            &bytes, &dir_count);
        if (status != INFS_STATUS_OK) {
            free(owned);
            goto out;
        }
        status = directory_blob_duplicate_status(base, bytes);
        if (status != INFS_STATUS_OK) {
            free(owned);
            goto out;
        }

        struct infs_object_header_disk *directory_header =
            (struct infs_object_header_disk *)directory;
        size_t offset = 0;
        uint32_t seen = 0;
        while (offset < bytes) {
            struct infs_dirent_disk *entry =
                (struct infs_dirent_disk *)(base + offset);
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

            uint8_t child[INFS_BLOCK_SIZE];
            status = read_object(
                vol, infs_le64_to_cpu(entries[target].object_block), child);
            if (status != INFS_STATUS_OK) {
                free(owned);
                goto out;
            }
            struct infs_object_header_disk *child_header =
                (struct infs_object_header_disk *)child;
            int unparented_hard_link = type == INFS_OBJECT_FILE &&
                hard_links_enabled(vol) && id_is_zero(child_header->parent_id);
            if (!unparented_hard_link &&
                memcmp(child_header->parent_id,
                       directory_header->object_id, 16) != 0) {
                free(owned);
                status = INFS_STATUS_CORRUPT;
                goto out;
            }

            offset += rec;
            ++seen;
        }
        free(owned);
        if (seen != dir_count) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }
        scrub_metadata_progress_update(progress, ++directory_done);
    }

    scrub_metadata_progress_begin(
        progress, vol, INFS_SCRUB_METADATA_NAMESPACE_LINKS,
        namespace_total);
    uint64_t namespace_done = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t type = infs_le16_to_cpu(entries[i].object_type);
        if (!namespace_object_type(type))
            continue;
        uint8_t object[INFS_BLOCK_SIZE];
        status = read_object(vol, infs_le64_to_cpu(entries[i].object_block),
                             object);
        if (status != INFS_STATUS_OK)
            goto out;
        struct infs_object_header_disk *header =
            (struct infs_object_header_disk *)object;
        struct infs_attributes_disk *attributes = object_attributes(object);
        if (!attributes) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }

        if (i == root_index) {
            if (references[i] != 0 || !id_is_zero(header->parent_id)) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
        } else {
            if (references[i] == 0) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            if (type == INFS_OBJECT_FILE && hard_links_enabled(vol)) {
                if (references[i] > 1u && !id_is_zero(header->parent_id)) {
                    status = INFS_STATUS_CORRUPT;
                    goto out;
                }
            } else if (references[i] != 1u ||
                       id_is_zero(header->parent_id)) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
        }

        uint64_t expected_links = type == INFS_OBJECT_DIRECTORY ?
            2u + (uint64_t)child_directories[i] :
            type == INFS_OBJECT_FILE && hard_links_enabled(vol) ?
            references[i] : 1u;
        if (infs_le64_to_cpu(attributes->link_count) != expected_links) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }
        scrub_metadata_progress_update(progress, ++namespace_done);
    }

    /* Reachability used to rescan the complete object array once per graph
     * level. A queue visits each reachable directory exactly once. */
    scrub_metadata_progress_begin(
        progress, vol, INFS_SCRUB_METADATA_NAMESPACE_REACHABILITY,
        directory_total);
    size_t queue_head = 0;
    size_t queue_tail = 0;
    uint64_t reachable_directories = 0;
    reachable[root_index] = 1;
    queue[queue_tail++] = root_index;
    while (queue_head < queue_tail) {
        uint32_t i = queue[queue_head++];
        if (infs_le16_to_cpu(entries[i].object_type) != INFS_OBJECT_DIRECTORY)
            continue;

        uint8_t directory[INFS_BLOCK_SIZE];
        status = read_object(
            vol, infs_le64_to_cpu(entries[i].object_block), directory);
        if (status != INFS_STATUS_OK)
            goto out;
        uint8_t *owned = NULL;
        const uint8_t *base = NULL;
        size_t bytes = 0;
        uint32_t dir_count = 0;
        status = directory_entries_snapshot(vol, directory, &owned, &base,
                                            &bytes, &dir_count);
        if (status != INFS_STATUS_OK) {
            free(owned);
            goto out;
        }
        size_t offset = 0;
        uint32_t seen = 0;
        while (offset < bytes) {
            struct infs_dirent_disk *entry =
                (struct infs_dirent_disk *)(base + offset);
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
        scrub_metadata_progress_update(progress, ++reachable_directories);
    }

    for (uint32_t i = 0; i < count; ++i) {
        uint16_t type = infs_le16_to_cpu(entries[i].object_type);
        if (namespace_object_type(type) && !reachable[i]) {
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

static int validate_namespace_graph(struct infs_volume *vol)
{
    return validate_namespace_graph_progress(vol, NULL);
}

static int validate_checksum_graph_progress(
    struct infs_volume *vol, struct scrub_metadata_progress_context *progress)
{
    struct infs_index_entry_disk *entries = NULL;
    uint32_t count = 0;
    int status = index_snapshot(vol, &entries, &count);
    if (status != INFS_STATUS_OK)
        return status;
    if (count > 1u)
        qsort(entries, count, sizeof(*entries), index_entry_id_compare);

    uint32_t file_total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (infs_le16_to_cpu(entries[i].object_type) == INFS_OBJECT_FILE)
            ++file_total;
    }

    uint8_t *seen = calloc(count ? count : 1u, 1);
    if (!seen) {
        free(entries);
        return INFS_STATUS_NO_MEMORY;
    }

    scrub_metadata_progress_begin(
        progress, vol, INFS_SCRUB_METADATA_CHECKSUM_FILES, file_total);
    uint64_t files_done = 0;

    for (uint32_t i = 0; i < count; ++i) {
        if (infs_le16_to_cpu(entries[i].object_type) != INFS_OBJECT_FILE)
            continue;

        uint8_t file_block[INFS_BLOCK_SIZE];
        status = read_object(vol, infs_le64_to_cpu(entries[i].object_block),
                             file_block);
        if (status != INFS_STATUS_OK)
            goto out;
        struct infs_object_header_disk *file_header =
            (struct infs_object_header_disk *)file_block;
        struct infs_file_payload_disk *file;
        struct infs_extent_disk *extents;
        status = file_validate_volume(vol, file_block, &file, &extents);
        if (status != INFS_STATUS_OK)
            goto out;

        uint64_t allocated = 0;
        status = file_allocated_blocks_volume(
            vol, file_block, file, extents, &allocated);
        if (status != INFS_STATUS_OK)
            goto out;
        if (allocated == 0) {
            if (!id_is_zero(file->checksum_head_id)) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            scrub_metadata_progress_update(progress, ++files_done);
            continue;
        }
        if (id_is_zero(file->checksum_head_id)) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }

        uint8_t current[16];
        memcpy(current, file->checksum_head_id, 16);
        uint64_t previous_start = 0;
        int have_previous = 0;
        while (!id_is_zero(current)) {
            uint32_t checksum_index = 0;
            if (index_entry_index(entries, count, current,
                                  &checksum_index) != INFS_STATUS_OK ||
                infs_le16_to_cpu(entries[checksum_index].object_type) !=
                    INFS_OBJECT_CHECKSUM || seen[checksum_index]) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            seen[checksum_index] = 1;

            uint8_t checksum_block[INFS_BLOCK_SIZE];
            status = read_object(
                vol, infs_le64_to_cpu(entries[checksum_index].object_block),
                checksum_block);
            if (status != INFS_STATUS_OK)
                goto out;
            struct infs_object_header_disk *checksum_header =
                (struct infs_object_header_disk *)checksum_block;
            struct infs_checksum_payload_disk *checksum;
            status = checksum_validate(checksum_block, file_header->object_id,
                                       &checksum, NULL);
            if (status != INFS_STATUS_OK ||
                memcmp(checksum_header->parent_id,
                       file_header->object_id, 16) != 0) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            uint64_t start = infs_le64_to_cpu(checksum->start_logical_block);
            if ((start % INFS_CHECKSUMS_PER_OBJECT) != 0 ||
                (have_previous && start <= previous_start)) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            previous_start = start;
            have_previous = 1;
            memcpy(current, checksum->next_object_id, 16);
        }
        scrub_metadata_progress_update(progress, ++files_done);
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (infs_le16_to_cpu(entries[i].object_type) == INFS_OBJECT_CHECKSUM &&
            !seen[i]) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }
    }

    status = INFS_STATUS_OK;
out:
    free(entries);
    free(seen);
    return status;
}

static int validate_checksum_graph(struct infs_volume *vol)
{
    return validate_checksum_graph_progress(vol, NULL);
}
