// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Portable named metadata streams.
 *
 * A namespace object's extended_attributes_object_id may reference either the
 * historical single typed-extension envelope or a metadata-set object.  The
 * latter owns an optional typed-extension reference plus exact UTF-8 stream
 * names.  Named-stream contents are ordinary CoW file-layout objects and reuse
 * the normal extent/checksum engine; they are never directory entries.
 */

static int named_stream_name_valid(const char *name, size_t *length_out)
{
    if (!name)
        return 0;
    size_t length = 0;
    while (length <= INFS_NAME_MAX && name[length] != '\0')
        ++length;
    if (length == 0 || length > INFS_NAME_MAX ||
        !infs_utf8_validate((const uint8_t *)name, length))
        return 0;
    if (length_out)
        *length_out = length;
    return 1;
}

static infs_status metadata_set_load(
    struct infs_volume *vol, const uint8_t metadata_id[16],
    uint8_t block[INFS_BLOCK_SIZE],
    struct infs_metadata_set_payload_disk **payload_out,
    struct infs_lookup *lookup_out)
{
    if (!vol || !metadata_id || id_is_zero(metadata_id) || !block)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (!named_streams_enabled(vol))
        return INFS_STATUS_CORRUPT;

    struct infs_lookup lookup;
    infs_status status = index_find(vol, metadata_id, &lookup);
    if (status != INFS_STATUS_OK || lookup.type != INFS_OBJECT_METADATA_SET)
        return INFS_STATUS_CORRUPT;
    status = read_object(vol, lookup.block, block);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)block;
    if (infs_le16_to_cpu(header->object_type) != INFS_OBJECT_METADATA_SET ||
        infs_le16_to_cpu(header->object_version) !=
            INFS_OBJECT_VERSION_CLASSIC ||
        !id_is_zero(header->parent_id))
        return INFS_STATUS_CORRUPT;

    struct infs_metadata_set_payload_disk *payload =
        (struct infs_metadata_set_payload_disk *)(header + 1);
    if (payload_out)
        *payload_out = payload;
    if (lookup_out)
        *lookup_out = lookup;
    return INFS_STATUS_OK;
}

static infs_status metadata_set_find_entry(
    const struct infs_metadata_set_payload_disk *payload,
    const char *name, size_t name_length,
    size_t *offset_out,
    const struct infs_metadata_stream_entry_disk **entry_out)
{
    if (!payload || !name)
        return INFS_STATUS_INVALID_ARGUMENT;
    const uint8_t *base = (const uint8_t *)(payload + 1);
    size_t bytes = infs_le32_to_cpu(payload->bytes_used);
    size_t offset = 0;

    while (offset < bytes) {
        const struct infs_metadata_stream_entry_disk *entry =
            (const struct infs_metadata_stream_entry_disk *)(base + offset);
        uint16_t record = infs_le16_to_cpu(entry->record_size);
        uint16_t length = infs_le16_to_cpu(entry->name_length);
        if (record < sizeof(*entry) || record > bytes - offset)
            return INFS_STATUS_CORRUPT;
        if (length == name_length &&
            memcmp(entry + 1, name, name_length) == 0) {
            if (offset_out)
                *offset_out = offset;
            if (entry_out)
                *entry_out = entry;
            return INFS_STATUS_OK;
        }
        offset += record;
    }
    return offset == bytes ? INFS_STATUS_NOT_FOUND : INFS_STATUS_CORRUPT;
}

static infs_status metadata_set_validate_reference(
    struct infs_volume *vol, const uint8_t metadata_id[16])
{
    uint8_t block[INFS_BLOCK_SIZE];
    struct infs_metadata_set_payload_disk *payload = NULL;
    infs_status status = metadata_set_load(
        vol, metadata_id, block, &payload, NULL);
    if (status != INFS_STATUS_OK)
        return status;

    if (!id_is_zero(payload->typed_extension_object_id)) {
        if (!typed_extensions_enabled(vol))
            return INFS_STATUS_CORRUPT;
        status = typed_extension_validate_reference(
            vol, payload->typed_extension_object_id);
        if (status != INFS_STATUS_OK)
            return status;
    }

    const uint8_t *base = (const uint8_t *)(payload + 1);
    size_t bytes = infs_le32_to_cpu(payload->bytes_used);
    size_t offset = 0;
    while (offset < bytes) {
        const struct infs_metadata_stream_entry_disk *entry =
            (const struct infs_metadata_stream_entry_disk *)(base + offset);
        uint16_t record = infs_le16_to_cpu(entry->record_size);
        struct infs_lookup stream;
        status = index_find(vol, entry->stream_object_id, &stream);
        if (status != INFS_STATUS_OK ||
            stream.type != INFS_OBJECT_NAMED_STREAM)
            return INFS_STATUS_CORRUPT;

        uint8_t stream_block[INFS_BLOCK_SIZE];
        struct infs_file_payload_disk *file = NULL;
        status = read_object(vol, stream.block, stream_block);
        if (status != INFS_STATUS_OK)
            return status;
        status = file_validate_volume(vol, stream_block, &file, NULL);
        if (status != INFS_STATUS_OK)
            return status;

        /* One metadata name owns one independent stream object.  Rejecting
         * aliases keeps replacement/deletion ownership unambiguous. */
        size_t prior = 0;
        while (prior < offset) {
            const struct infs_metadata_stream_entry_disk *p =
                (const struct infs_metadata_stream_entry_disk *)(base + prior);
            if (memcmp(p->stream_object_id, entry->stream_object_id, 16) == 0)
                return INFS_STATUS_CORRUPT;
            prior += infs_le16_to_cpu(p->record_size);
        }
        offset += record;
    }
    return offset == bytes ? INFS_STATUS_OK : INFS_STATUS_CORRUPT;
}

static infs_status metadata_reference_typed_extension(
    struct infs_volume *vol, const uint8_t reference_id[16],
    uint8_t extension_id_out[16])
{
    if (!vol || !reference_id || !extension_id_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    memset(extension_id_out, 0, 16);
    if (id_is_zero(reference_id))
        return INFS_STATUS_NOT_FOUND;

    struct infs_lookup lookup;
    infs_status status = index_find(vol, reference_id, &lookup);
    if (status != INFS_STATUS_OK)
        return status;
    if (lookup.type == INFS_OBJECT_EXTENSION) {
        status = typed_extension_validate_reference(vol, reference_id);
        if (status == INFS_STATUS_OK)
            memcpy(extension_id_out, reference_id, 16);
        return status;
    }
    if (lookup.type != INFS_OBJECT_METADATA_SET ||
        !named_streams_enabled(vol))
        return INFS_STATUS_CORRUPT;

    uint8_t block[INFS_BLOCK_SIZE];
    struct infs_metadata_set_payload_disk *payload = NULL;
    status = metadata_set_load(vol, reference_id, block, &payload, NULL);
    if (status != INFS_STATUS_OK)
        return status;
    status = metadata_set_validate_reference(vol, reference_id);
    if (status != INFS_STATUS_OK)
        return status;
    if (id_is_zero(payload->typed_extension_object_id))
        return INFS_STATUS_NOT_FOUND;
    memcpy(extension_id_out, payload->typed_extension_object_id, 16);
    return INFS_STATUS_OK;
}

static infs_status metadata_set_store(
    struct infs_volume *vol,
    const struct infs_lookup *existing,
    const uint8_t typed_extension_id[16],
    const uint8_t *entries, size_t entries_bytes, uint32_t entry_count,
    uint8_t metadata_id_out[16])
{
    if (!vol || !metadata_id_out || (entries_bytes && !entries) ||
        entries_bytes > INFS_METADATA_SET_DATA_MAX)
        return INFS_STATUS_INVALID_ARGUMENT;

    uint64_t generation = 0;
    infs_status status = transaction_next_generation(vol, &generation);
    if (status != INFS_STATUS_OK)
        return status;

    uint8_t object[INFS_BLOCK_SIZE];
    uint8_t metadata_id[16];
    uint64_t object_block = 0;
    int creating = existing == NULL;

    if (creating) {
        status = generate_unique_object_id(vol, metadata_id);
        if (status != INFS_STATUS_OK)
            return status;
    } else {
        memcpy(metadata_id, existing->object_id, 16);
        object_block = existing->block;
    }

    status = infs_object_init(
        object, INFS_OBJECT_METADATA_SET, metadata_id, NULL, generation,
        (uint32_t)(sizeof(struct infs_metadata_set_payload_disk) +
                   entries_bytes));
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_metadata_set_payload_disk *payload =
        (struct infs_metadata_set_payload_disk *)(
            object + sizeof(struct infs_object_header_disk));
    payload->version = infs_cpu_to_le16(INFS_METADATA_SET_VERSION);
    payload->flags = 0;
    payload->entry_count = infs_cpu_to_le32(entry_count);
    payload->bytes_used = infs_cpu_to_le32((uint32_t)entries_bytes);
    payload->reserved = 0;
    if (typed_extension_id)
        memcpy(payload->typed_extension_object_id, typed_extension_id, 16);
    if (entries_bytes)
        memcpy(payload + 1, entries, entries_bytes);

    status = infs_object_finalize(object);
    if (status != INFS_STATUS_OK)
        return status;

    if (!creating) {
        status = write_object(vol, object_block, object);
        if (status == INFS_STATUS_OK)
            memcpy(metadata_id_out, metadata_id, 16);
        return status;
    }

    uint64_t got = 0;
    status = alloc_run(vol, 1u, &object_block, &got);
    if (status != INFS_STATUS_OK)
        return status;
    if (got != 1u) {
        if (got)
            free_run(vol, object_block, got);
        return INFS_STATUS_CORRUPT;
    }
    status = write_block(vol, object_block, object);
    if (status == INFS_STATUS_OK)
        status = index_add(
            vol, metadata_id, object_block, INFS_OBJECT_METADATA_SET);
    if (status != INFS_STATUS_OK) {
        free_run(vol, object_block, 1u);
        return status;
    }
    memcpy(metadata_id_out, metadata_id, 16);
    return INFS_STATUS_OK;
}

static infs_status metadata_set_destroy(
    struct infs_volume *vol, const struct infs_lookup *lookup)
{
    if (!vol || !lookup || lookup->type != INFS_OBJECT_METADATA_SET)
        return INFS_STATUS_INVALID_ARGUMENT;
    infs_status status = index_remove(vol, lookup->object_id);
    if (status != INFS_STATUS_OK)
        return status;
    free_run(vol, lookup->block, 1u);
    return vol->tx_error;
}

static infs_status metadata_reference_replace_typed_extension(
    struct infs_volume *vol, uint8_t owner_block[INFS_BLOCK_SIZE],
    struct infs_attributes_disk *attributes,
    const uint8_t extension_id[16])
{
    (void)owner_block;
    if (!vol || !attributes || !extension_id)
        return INFS_STATUS_INVALID_ARGUMENT;

    uint8_t current[16];
    memcpy(current, attributes->extended_attributes_object_id, 16);
    if (id_is_zero(current)) {
        memcpy(attributes->extended_attributes_object_id, extension_id, 16);
        return INFS_STATUS_OK;
    }

    struct infs_lookup lookup;
    infs_status status = index_find(vol, current, &lookup);
    if (status != INFS_STATUS_OK)
        return status;
    if (lookup.type == INFS_OBJECT_EXTENSION) {
        memcpy(attributes->extended_attributes_object_id, extension_id, 16);
        return INFS_STATUS_OK;
    }
    if (lookup.type != INFS_OBJECT_METADATA_SET ||
        !named_streams_enabled(vol))
        return INFS_STATUS_CORRUPT;

    uint8_t metadata[INFS_BLOCK_SIZE];
    struct infs_metadata_set_payload_disk *payload = NULL;
    status = metadata_set_load(vol, current, metadata, &payload, &lookup);
    if (status != INFS_STATUS_OK)
        return status;
    size_t bytes = infs_le32_to_cpu(payload->bytes_used);
    uint32_t count = infs_le32_to_cpu(payload->entry_count);

    if (count == 0) {
        memcpy(attributes->extended_attributes_object_id, extension_id, 16);
        return metadata_set_destroy(vol, &lookup);
    }

    uint8_t new_id[16];
    status = metadata_set_store(
        vol, &lookup, extension_id, (const uint8_t *)(payload + 1),
        bytes, count, new_id);
    if (status == INFS_STATUS_OK)
        memcpy(attributes->extended_attributes_object_id, new_id, 16);
    return status;
}

static infs_status named_stream_create_object(
    struct infs_volume *vol, struct infs_lookup *lookup_out)
{
    uint8_t object_id[16];
    infs_status status = generate_unique_object_id(vol, object_id);
    if (status != INFS_STATUS_OK)
        return status;

    uint64_t generation = 0;
    status = transaction_next_generation(vol, &generation);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_timestamp now = {0};
    status = infs_storage_current_time(&vol->storage, &now);
    if (status != INFS_STATUS_OK)
        return status;

    uint8_t object[INFS_BLOCK_SIZE];
    status = infs_object_init(
        object, INFS_OBJECT_NAMED_STREAM, object_id, NULL, generation,
        sizeof(struct infs_file_payload_disk));
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_file_payload_disk *file =
        (struct infs_file_payload_disk *)(
            object + sizeof(struct infs_object_header_disk));
    memset(file, 0, sizeof(*file));
    file->attributes.link_count = infs_cpu_to_le64(1u);
    timestamp_disk_from_value(&file->attributes.birth_time, &now);
    timestamp_disk_from_value(&file->attributes.access_time, &now);
    timestamp_disk_from_value(&file->attributes.modification_time, &now);
    timestamp_disk_from_value(&file->attributes.change_time, &now);
    file->posix.permissions = infs_cpu_to_le32(0600u);
    file->data_checksum_type = infs_cpu_to_le32(INFS_CHECKSUM_SHA256);

    status = infs_object_finalize(object);
    if (status != INFS_STATUS_OK)
        return status;

    uint64_t block = 0, got = 0;
    status = alloc_run(vol, 1u, &block, &got);
    if (status != INFS_STATUS_OK)
        return status;
    if (got != 1u) {
        if (got)
            free_run(vol, block, got);
        return INFS_STATUS_CORRUPT;
    }
    status = write_block(vol, block, object);
    if (status == INFS_STATUS_OK)
        status = index_add(
            vol, object_id, block, INFS_OBJECT_NAMED_STREAM);
    if (status != INFS_STATUS_OK) {
        free_run(vol, block, 1u);
        return status;
    }

    if (lookup_out) {
        memcpy(lookup_out->object_id, object_id, 16);
        lookup_out->block = block;
        lookup_out->type = INFS_OBJECT_NAMED_STREAM;
    }
    return INFS_STATUS_OK;
}

static infs_status named_stream_destroy_object(
    struct infs_volume *vol, const uint8_t stream_id[16])
{
    struct infs_lookup lookup;
    infs_status status = index_find(vol, stream_id, &lookup);
    if (status != INFS_STATUS_OK ||
        lookup.type != INFS_OBJECT_NAMED_STREAM)
        return INFS_STATUS_CORRUPT;

    uint8_t object[INFS_BLOCK_SIZE];
    status = read_object(vol, lookup.block, object);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_file_payload_disk *file = NULL;
    struct infs_extent_disk *classic = NULL;
    status = file_validate_volume(vol, object, &file, &classic);
    if (status != INFS_STATUS_OK)
        return status;

    status = checksum_destroy_chain(vol, file, stream_id);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_extent_disk *all = NULL;
    uint32_t count = 0;
    int owned = 0;
    status = file_extent_snapshot(
        vol, object, file, classic, &all, &count, &owned);
    if (status != INFS_STATUS_OK)
        return status;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t flags = infs_le32_to_cpu(all[i].flags);
        if (extent_kind(flags) != INFS_EXTENT_NORMAL)
            continue;
        uint32_t logical_blocks = infs_le32_to_cpu(all[i].block_count);
        status = file_free_unshared_run(
            vol, stream_id, infs_le64_to_cpu(all[i].physical_block),
            extent_physical_blocks(logical_blocks, flags));
        if (status != INFS_STATUS_OK) {
            if (owned)
                free(all);
            return status;
        }
    }
    if (owned)
        free(all);

    status = paged_extent_destroy_pages(vol, object, file);
    if (status != INFS_STATUS_OK)
        return status;
    status = index_remove(vol, stream_id);
    if (status != INFS_STATUS_OK)
        return status;
    free_run(vol, lookup.block, 1u);
    return vol->tx_error;
}

static infs_status named_stream_owner_metadata(
    struct infs_volume *vol, const char *path,
    struct infs_lookup *owner_out,
    uint8_t owner_block[INFS_BLOCK_SIZE],
    struct infs_attributes_disk **attributes_out,
    struct infs_lookup *metadata_lookup_out,
    uint8_t metadata_block[INFS_BLOCK_SIZE],
    struct infs_metadata_set_payload_disk **payload_out)
{
    struct infs_lookup owner;
    infs_status status = infs_lookup_path(vol, path, &owner);
    if (status != INFS_STATUS_OK)
        return status;
    status = read_object(vol, owner.block, owner_block);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_attributes_disk *attributes = object_attributes(owner_block);
    if (!attributes)
        return INFS_STATUS_CORRUPT;

    if (owner_out)
        *owner_out = owner;
    if (attributes_out)
        *attributes_out = attributes;
    if (payload_out)
        *payload_out = NULL;
    if (metadata_lookup_out)
        memset(metadata_lookup_out, 0, sizeof(*metadata_lookup_out));

    if (id_is_zero(attributes->extended_attributes_object_id))
        return INFS_STATUS_OK;

    struct infs_lookup reference;
    status = index_find(
        vol, attributes->extended_attributes_object_id, &reference);
    if (status != INFS_STATUS_OK)
        return status;
    if (reference.type == INFS_OBJECT_EXTENSION)
        return typed_extension_validate_reference(
            vol, attributes->extended_attributes_object_id);
    if (reference.type != INFS_OBJECT_METADATA_SET)
        return INFS_STATUS_CORRUPT;

    status = metadata_set_load(
        vol, attributes->extended_attributes_object_id, metadata_block,
        payload_out, metadata_lookup_out);
    if (status != INFS_STATUS_OK)
        return status;
    return metadata_set_validate_reference(
        vol, attributes->extended_attributes_object_id);
}

static size_t named_stream_record_size(size_t name_length)
{
    size_t raw = sizeof(struct infs_metadata_stream_entry_disk) + name_length;
    return (raw + 7u) & ~(size_t)7u;
}

static infs_status metadata_set_replace_stream(
    struct infs_volume *vol,
    struct infs_lookup *owner,
    uint8_t owner_block[INFS_BLOCK_SIZE],
    struct infs_attributes_disk *attributes,
    struct infs_lookup *metadata_lookup,
    struct infs_metadata_set_payload_disk *payload,
    const char *name, size_t name_length,
    const uint8_t replacement_id[16],
    uint8_t replaced_id_out[16])
{
    if (replaced_id_out)
        memset(replaced_id_out, 0, 16);

    uint8_t entries[INFS_METADATA_SET_DATA_MAX];
    size_t old_bytes = payload ? infs_le32_to_cpu(payload->bytes_used) : 0u;
    uint32_t old_count = payload ? infs_le32_to_cpu(payload->entry_count) : 0u;
    const uint8_t *old_base = payload ? (const uint8_t *)(payload + 1) : NULL;
    uint8_t typed_extension[16] = {0};

    if (payload) {
        memcpy(typed_extension, payload->typed_extension_object_id, 16);
    } else if (!id_is_zero(attributes->extended_attributes_object_id)) {
        struct infs_lookup direct;
        infs_status status = index_find(
            vol, attributes->extended_attributes_object_id, &direct);
        if (status != INFS_STATUS_OK)
            return status;
        if (direct.type != INFS_OBJECT_EXTENSION)
            return INFS_STATUS_CORRUPT;
        memcpy(typed_extension, direct.object_id, 16);
    }

    size_t offset = 0, out = 0;
    uint32_t count = 0;
    int found = 0;
    while (offset < old_bytes) {
        const struct infs_metadata_stream_entry_disk *entry =
            (const struct infs_metadata_stream_entry_disk *)(old_base + offset);
        uint16_t record = infs_le16_to_cpu(entry->record_size);
        uint16_t length = infs_le16_to_cpu(entry->name_length);
        int matches = length == name_length &&
            memcmp(entry + 1, name, name_length) == 0;
        if (matches) {
            found = 1;
            if (replaced_id_out)
                memcpy(replaced_id_out, entry->stream_object_id, 16);
            if (replacement_id) {
                if (out + record > sizeof(entries))
                    return INFS_STATUS_NO_SPACE;
                memcpy(entries + out, entry, record);
                struct infs_metadata_stream_entry_disk *copy =
                    (struct infs_metadata_stream_entry_disk *)(entries + out);
                memcpy(copy->stream_object_id, replacement_id, 16);
                out += record;
                ++count;
            }
        } else {
            if (out + record > sizeof(entries))
                return INFS_STATUS_NO_SPACE;
            memcpy(entries + out, entry, record);
            out += record;
            ++count;
        }
        offset += record;
    }
    if (offset != old_bytes || count > old_count + 1u)
        return INFS_STATUS_CORRUPT;

    if (!found && replacement_id) {
        size_t record = named_stream_record_size(name_length);
        if (record > sizeof(entries) - out)
            return INFS_STATUS_NO_SPACE;
        struct infs_metadata_stream_entry_disk *entry =
            (struct infs_metadata_stream_entry_disk *)(entries + out);
        memset(entry, 0, record);
        entry->record_size = infs_cpu_to_le16((uint16_t)record);
        entry->name_length = infs_cpu_to_le16((uint16_t)name_length);
        entry->flags = infs_cpu_to_le32(INFS_METADATA_STREAM_FLAG_NONE);
        memcpy(entry->stream_object_id, replacement_id, 16);
        memcpy(entry + 1, name, name_length);
        out += record;
        ++count;
    }

    infs_status status;
    if (count == 0) {
        if (metadata_lookup && metadata_lookup->type == INFS_OBJECT_METADATA_SET) {
            status = metadata_set_destroy(vol, metadata_lookup);
            if (status != INFS_STATUS_OK)
                return status;
        }
        memcpy(attributes->extended_attributes_object_id, typed_extension, 16);
    } else {
        uint8_t metadata_id[16];
        const struct infs_lookup *existing =
            metadata_lookup && metadata_lookup->type == INFS_OBJECT_METADATA_SET ?
            metadata_lookup : NULL;
        status = metadata_set_store(
            vol, existing, typed_extension, entries, out, count, metadata_id);
        if (status != INFS_STATUS_OK)
            return status;
        memcpy(attributes->extended_attributes_object_id, metadata_id, 16);
    }

    touch_ctime(vol, attributes);
    if (vol->tx_error != INFS_STATUS_OK)
        return vol->tx_error;
    return write_object(vol, owner->block, owner_block);
}

infs_status infs_named_stream_set(
    struct infs_volume *vol, const char *path, const char *name,
    const void *data, size_t size)
{
    size_t name_length = 0;
    if (!vol || !path || path[0] != '/' ||
        !named_stream_name_valid(name, &name_length) || (size && !data))
        return INFS_STATUS_INVALID_ARGUMENT;
    if (!vol->writable)
        return INFS_STATUS_READ_ONLY;
    if (!named_streams_enabled(vol))
        return INFS_STATUS_NOT_SUPPORTED;

    int started = 0;
    infs_status status = mutation_scope_begin(vol, &started);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_lookup owner, metadata_lookup;
    uint8_t owner_block[INFS_BLOCK_SIZE];
    uint8_t metadata_block[INFS_BLOCK_SIZE];
    struct infs_attributes_disk *attributes = NULL;
    struct infs_metadata_set_payload_disk *payload = NULL;
    status = named_stream_owner_metadata(
        vol, path, &owner, owner_block, &attributes, &metadata_lookup,
        metadata_block, &payload);
    if (status != INFS_STATUS_OK)
        goto fail;

    struct infs_lookup stream;
    status = named_stream_create_object(vol, &stream);
    if (status != INFS_STATUS_OK)
        goto fail;
    if (size) {
        int64_t written = write_file_lookup_tx(vol, &stream, data, size, 0);
        if (written < 0) {
            status = (infs_status)written;
            goto fail;
        }
        if ((size_t)written != size) {
            status = INFS_STATUS_IO_ERROR;
            goto fail;
        }
    }

    uint8_t replaced[16];
    status = metadata_set_replace_stream(
        vol, &owner, owner_block, attributes,
        payload ? &metadata_lookup : NULL, payload,
        name, name_length, stream.object_id, replaced);
    if (status != INFS_STATUS_OK)
        goto fail;

    if (!id_is_zero(replaced)) {
        status = named_stream_destroy_object(vol, replaced);
        if (status != INFS_STATUS_OK)
            goto fail;
    }

    status = finish_mutation(
        vol, infiltratr_u64_add_saturating(
            (uint64_t)size, (uint64_t)INFS_BLOCK_SIZE * 5u));
    if (status != INFS_STATUS_OK)
        goto fail;
    mutation_scope_success(vol);
    return INFS_STATUS_OK;

fail:
    mutation_scope_fail(vol, started);
    return status;
}

int64_t infs_named_stream_read(
    struct infs_volume *vol, const char *path, const char *name,
    void *data, size_t size, uint64_t offset)
{
    size_t name_length = 0;
    if (!vol || !path || path[0] != '/' ||
        !named_stream_name_valid(name, &name_length) || (size && !data))
        return INFS_STATUS_INVALID_ARGUMENT;
    if (!named_streams_enabled(vol))
        return INFS_STATUS_NOT_SUPPORTED;

    struct infs_lookup owner, metadata_lookup;
    uint8_t owner_block[INFS_BLOCK_SIZE];
    uint8_t metadata_block[INFS_BLOCK_SIZE];
    struct infs_attributes_disk *attributes = NULL;
    struct infs_metadata_set_payload_disk *payload = NULL;
    infs_status status = named_stream_owner_metadata(
        vol, path, &owner, owner_block, &attributes, &metadata_lookup,
        metadata_block, &payload);
    (void)owner;
    (void)attributes;
    (void)metadata_lookup;
    if (status != INFS_STATUS_OK)
        return status;
    if (!payload)
        return INFS_STATUS_NOT_FOUND;

    const struct infs_metadata_stream_entry_disk *entry = NULL;
    status = metadata_set_find_entry(
        payload, name, name_length, NULL, &entry);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_lookup stream;
    status = index_find(vol, entry->stream_object_id, &stream);
    if (status != INFS_STATUS_OK ||
        stream.type != INFS_OBJECT_NAMED_STREAM)
        return INFS_STATUS_CORRUPT;
    return read_file_lookup(vol, &stream, data, size, offset);
}

infs_status infs_named_stream_delete(
    struct infs_volume *vol, const char *path, const char *name)
{
    size_t name_length = 0;
    if (!vol || !path || path[0] != '/' ||
        !named_stream_name_valid(name, &name_length))
        return INFS_STATUS_INVALID_ARGUMENT;
    if (!vol->writable)
        return INFS_STATUS_READ_ONLY;
    if (!named_streams_enabled(vol))
        return INFS_STATUS_NOT_SUPPORTED;

    int started = 0;
    infs_status status = mutation_scope_begin(vol, &started);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_lookup owner, metadata_lookup;
    uint8_t owner_block[INFS_BLOCK_SIZE];
    uint8_t metadata_block[INFS_BLOCK_SIZE];
    struct infs_attributes_disk *attributes = NULL;
    struct infs_metadata_set_payload_disk *payload = NULL;
    status = named_stream_owner_metadata(
        vol, path, &owner, owner_block, &attributes, &metadata_lookup,
        metadata_block, &payload);
    if (status != INFS_STATUS_OK)
        goto fail;
    if (!payload) {
        status = INFS_STATUS_NOT_FOUND;
        goto fail;
    }

    uint8_t removed[16];
    status = metadata_set_replace_stream(
        vol, &owner, owner_block, attributes, &metadata_lookup, payload,
        name, name_length, NULL, removed);
    if (status != INFS_STATUS_OK)
        goto fail;
    if (id_is_zero(removed)) {
        status = INFS_STATUS_NOT_FOUND;
        goto fail;
    }
    status = named_stream_destroy_object(vol, removed);
    if (status != INFS_STATUS_OK)
        goto fail;

    status = finish_mutation(vol, (uint64_t)INFS_BLOCK_SIZE * 5u);
    if (status != INFS_STATUS_OK)
        goto fail;
    mutation_scope_success(vol);
    return INFS_STATUS_OK;

fail:
    mutation_scope_fail(vol, started);
    return status;
}

infs_status infs_named_stream_list(
    struct infs_volume *vol, const char *path,
    struct infs_named_stream_info **streams, size_t *count)
{
    if (!vol || !path || path[0] != '/' || !streams || !count)
        return INFS_STATUS_INVALID_ARGUMENT;
    *streams = NULL;
    *count = 0;
    if (!named_streams_enabled(vol))
        return INFS_STATUS_NOT_SUPPORTED;

    struct infs_lookup owner, metadata_lookup;
    uint8_t owner_block[INFS_BLOCK_SIZE];
    uint8_t metadata_block[INFS_BLOCK_SIZE];
    struct infs_attributes_disk *attributes = NULL;
    struct infs_metadata_set_payload_disk *payload = NULL;
    infs_status status = named_stream_owner_metadata(
        vol, path, &owner, owner_block, &attributes, &metadata_lookup,
        metadata_block, &payload);
    (void)owner;
    (void)attributes;
    (void)metadata_lookup;
    if (status != INFS_STATUS_OK)
        return status;
    if (!payload)
        return INFS_STATUS_OK;

    uint32_t total = infs_le32_to_cpu(payload->entry_count);
    if (!total)
        return INFS_STATUS_OK;
    struct infs_named_stream_info *items =
        calloc(total, sizeof(*items));
    if (!items)
        return INFS_STATUS_NO_MEMORY;

    const uint8_t *base = (const uint8_t *)(payload + 1);
    size_t bytes = infs_le32_to_cpu(payload->bytes_used);
    size_t at = 0;
    uint32_t seen = 0;
    while (at < bytes && seen < total) {
        const struct infs_metadata_stream_entry_disk *entry =
            (const struct infs_metadata_stream_entry_disk *)(base + at);
        uint16_t record = infs_le16_to_cpu(entry->record_size);
        uint16_t length = infs_le16_to_cpu(entry->name_length);
        memcpy(items[seen].name, entry + 1, length);
        items[seen].name[length] = '\0';

        struct infs_lookup stream;
        status = index_find(vol, entry->stream_object_id, &stream);
        if (status != INFS_STATUS_OK ||
            stream.type != INFS_OBJECT_NAMED_STREAM) {
            free(items);
            return INFS_STATUS_CORRUPT;
        }
        uint8_t stream_block[INFS_BLOCK_SIZE];
        struct infs_file_payload_disk *file = NULL;
        status = read_object(vol, stream.block, stream_block);
        if (status != INFS_STATUS_OK ||
            file_validate_volume(vol, stream_block, &file, NULL) !=
                INFS_STATUS_OK) {
            free(items);
            return status != INFS_STATUS_OK ? status : INFS_STATUS_CORRUPT;
        }
        items[seen].logical_size =
            infs_le64_to_cpu(file->attributes.logical_size);
        ++seen;
        at += record;
    }
    if (at != bytes || seen != total) {
        free(items);
        return INFS_STATUS_CORRUPT;
    }

    *streams = items;
    *count = total;
    return INFS_STATUS_OK;
}

void infs_free_named_stream_infos(struct infs_named_stream_info *streams)
{
    free(streams);
}
