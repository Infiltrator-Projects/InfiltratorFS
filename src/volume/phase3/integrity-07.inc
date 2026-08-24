// SPDX-License-Identifier: GPL-3.0-or-later
/* Format hardening. These checks validate namespace reachability, link counts,
 * checksum ownership and the scalable Format 0.8 metadata representation. */

static int validate_common_metadata(
    const struct infs_attributes_disk *attributes,
    const struct infs_posix_compat_disk *posix)
{
    if (!attributes || !posix)
        return INFS_STATUS_CORRUPT;
    if ((infs_le64_to_cpu(attributes->portable_flags) &
         ~INFS_KNOWN_ATTR_FLAGS) != 0)
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
        struct infs_lookup lookup;
        status = index_find(vol, id, &lookup);
        if (status == INFS_STATUS_NOT_FOUND)
            return INFS_STATUS_OK;
        if (status != INFS_STATUS_OK)
            return status;
    }
    return INFS_STATUS_ERROR;
}

static int index_entry_index(const struct infs_index_entry_disk *entries,
                             uint32_t count, const uint8_t id[16],
                             uint32_t *index_out)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (memcmp(entries[i].object_id, id, 16) == 0) {
            if (index_out)
                *index_out = i;
            return INFS_STATUS_OK;
        }
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
        INFS_OBJECT_VERSION_PAGED) {
        uint8_t *snapshot = NULL;
        size_t bytes = 0;
        uint32_t snapshot_count = 0;
        status = paged_dir_snapshot(vol, directory, &snapshot, &bytes,
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

static int directory_blob_has_duplicate_names(const uint8_t *base,
                                               size_t bytes)
{
    size_t first_off = 0;
    while (first_off < bytes) {
        const struct infs_dirent_disk *first =
            (const struct infs_dirent_disk *)(base + first_off);
        uint16_t first_rec = infs_le16_to_cpu(first->record_size);
        uint16_t first_len = infs_le16_to_cpu(first->name_length);
        const uint8_t *first_name = (const uint8_t *)first + sizeof(*first);

        size_t second_off = first_off + first_rec;
        while (second_off < bytes) {
            const struct infs_dirent_disk *second =
                (const struct infs_dirent_disk *)(base + second_off);
            uint16_t second_rec = infs_le16_to_cpu(second->record_size);
            uint16_t second_len = infs_le16_to_cpu(second->name_length);
            const uint8_t *second_name =
                (const uint8_t *)second + sizeof(*second);
            if (first_len == second_len &&
                memcmp(first_name, second_name, first_len) == 0)
                return 1;
            second_off += second_rec;
        }
        first_off += first_rec;
    }
    return 0;
}

static int validate_namespace_graph(struct infs_volume *vol)
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

    uint32_t *references = calloc(count, sizeof(*references));
    uint32_t *child_directories = calloc(count, sizeof(*child_directories));
    uint8_t *reachable = calloc(count, 1);
    uint8_t *expanded = calloc(count, 1);
    if (!references || !child_directories || !reachable || !expanded) {
        free(entries);
        free(references);
        free(child_directories);
        free(reachable);
        free(expanded);
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
        if (directory_blob_has_duplicate_names(base, bytes)) {
            free(owned);
            status = INFS_STATUS_CORRUPT;
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
            if (++references[target] != 1u) {
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
            if (memcmp(child_header->parent_id,
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
    }

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
        } else if (references[i] != 1u || id_is_zero(header->parent_id)) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }

        uint64_t expected_links = type == INFS_OBJECT_DIRECTORY ?
            2u + (uint64_t)child_directories[i] : 1u;
        if (infs_le64_to_cpu(attributes->link_count) != expected_links) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }
    }

    reachable[root_index] = 1;
    for (;;) {
        int progress = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (!reachable[i] || expanded[i] ||
                infs_le16_to_cpu(entries[i].object_type) !=
                    INFS_OBJECT_DIRECTORY)
                continue;
            expanded[i] = 1;
            progress = 1;

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
                reachable[target] = 1;
                offset += rec;
                ++seen;
            }
            free(owned);
            if (seen != dir_count) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
        }
        if (!progress)
            break;
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
    free(expanded);
    return status;
}

static int validate_checksum_graph(struct infs_volume *vol)
{
    struct infs_index_entry_disk *entries = NULL;
    uint32_t count = 0;
    int status = index_snapshot(vol, &entries, &count);
    if (status != INFS_STATUS_OK)
        return status;

    uint8_t *seen = calloc(count ? count : 1u, 1);
    if (!seen) {
        free(entries);
        return INFS_STATUS_NO_MEMORY;
    }

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

        uint64_t allocated = file_allocated_blocks(file, extents);
        if (allocated == 0) {
            if (!id_is_zero(file->checksum_head_id)) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
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
