// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Generic typed extension objects.
 *
 * extended_attributes_object_id references one immutable extension envelope.
 * The portable core authenticates the type/version/flags and opaque bytes but
 * never interprets adapter-specific semantics.
 */

static int typed_extension_input_valid(
    const struct infs_typed_extension *extension)
{
    if (!extension)
        return 1;
    return !id_is_zero(extension->type_id) && extension->type_version != 0 &&
        (extension->flags & ~INFS_KNOWN_EXTENSION_FLAGS) == 0 &&
        extension->data_size <= INFS_EXTENSION_DATA_MAX &&
        (!extension->data_size || extension->data != NULL);
}

static infs_status typed_extension_validate_reference(
    struct infs_volume *vol, const uint8_t extension_id[16])
{
    if (!vol || !extension_id || id_is_zero(extension_id))
        return INFS_STATUS_INVALID_ARGUMENT;
    if (!typed_extensions_enabled(vol))
        return INFS_STATUS_CORRUPT;

    struct infs_lookup lookup;
    infs_status status = index_find(vol, extension_id, &lookup);
    if (status != INFS_STATUS_OK || lookup.type != INFS_OBJECT_EXTENSION)
        return INFS_STATUS_CORRUPT;

    uint8_t object[INFS_BLOCK_SIZE];
    status = read_object(vol, lookup.block, object);
    if (status != INFS_STATUS_OK)
        return status;
    const struct infs_object_header_disk *header =
        (const struct infs_object_header_disk *)object;
    if (infs_le16_to_cpu(header->object_type) != INFS_OBJECT_EXTENSION ||
        infs_le16_to_cpu(header->object_version) !=
            INFS_OBJECT_VERSION_CLASSIC ||
        !id_is_zero(header->parent_id))
        return INFS_STATUS_CORRUPT;
    return INFS_STATUS_OK;
}

static infs_status typed_extension_create(
    struct infs_volume *vol, const struct infs_typed_extension *extension,
    uint8_t object_id[16])
{
    infs_status status = generate_unique_object_id(vol, object_id);
    if (status != INFS_STATUS_OK)
        return status;

    uint64_t generation = 0;
    status = transaction_next_generation(vol, &generation);
    if (status != INFS_STATUS_OK)
        return status;

    uint8_t object[INFS_BLOCK_SIZE];
    uint32_t payload_size = (uint32_t)(
        sizeof(struct infs_extension_payload_disk) + extension->data_size);
    status = infs_object_init(object, INFS_OBJECT_EXTENSION, object_id, NULL,
                              generation, payload_size);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_extension_payload_disk *payload =
        (struct infs_extension_payload_disk *)(
            object + sizeof(struct infs_object_header_disk));
    payload->version = infs_cpu_to_le16(INFS_EXTENSION_VERSION);
    payload->flags = infs_cpu_to_le16(extension->flags);
    payload->type_version = infs_cpu_to_le32(extension->type_version);
    payload->data_size = infs_cpu_to_le32((uint32_t)extension->data_size);
    memcpy(payload->type_id, extension->type_id, sizeof(payload->type_id));
    if (extension->data_size)
        memcpy(payload + 1, extension->data, extension->data_size);
    infs_sha256(payload + 1, extension->data_size, payload->data_digest);

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
        status = index_add(vol, object_id, block, INFS_OBJECT_EXTENSION);
    if (status != INFS_STATUS_OK)
        free_run(vol, block, 1u);
    return status;
}

static infs_status set_typed_extension_tx(
    struct infs_volume *vol, const char *path,
    const struct infs_typed_extension *extension)
{
    if (!vol->writable)
        return INFS_STATUS_READ_ONLY;
    if (!typed_extensions_enabled(vol))
        return INFS_STATUS_NOT_SUPPORTED;

    struct infs_lookup owner;
    infs_status status = infs_lookup_path(vol, path, &owner);
    if (status != INFS_STATUS_OK)
        return status;
    uint8_t owner_block[INFS_BLOCK_SIZE];
    status = read_object(vol, owner.block, owner_block);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_attributes_disk *attributes = object_attributes(owner_block);
    if (!attributes)
        return INFS_STATUS_CORRUPT;

    uint8_t extension_id[16] = {0};
    if (extension) {
        status = typed_extension_create(vol, extension, extension_id);
        if (status != INFS_STATUS_OK)
            return status;
    } else {
        uint8_t existing_extension[16];
        status = metadata_reference_typed_extension(
            vol, attributes->extended_attributes_object_id,
            existing_extension);
        if (status == INFS_STATUS_NOT_FOUND)
            return INFS_STATUS_OK;
        if (status != INFS_STATUS_OK)
            return status;
    }

    status = metadata_reference_replace_typed_extension(
        vol, owner_block, attributes, extension_id);
    if (status != INFS_STATUS_OK)
        return status;
    touch_ctime(vol, attributes);
    status = write_object(vol, owner.block, owner_block);
    if (status != INFS_STATUS_OK)
        return status;
    return finish_mutation(vol, (uint64_t)INFS_BLOCK_SIZE * 4u);
}

infs_status infs_set_typed_extension(
    struct infs_volume *vol, const char *path,
    const struct infs_typed_extension *extension)
{
    if (!vol || !path || path[0] != '/' ||
        !typed_extension_input_valid(extension))
        return INFS_STATUS_INVALID_ARGUMENT;

    int started = 0;
    infs_status status = mutation_scope_begin(vol, &started);
    if (status != INFS_STATUS_OK)
        return status;
    status = set_typed_extension_tx(vol, path, extension);
    if (status != INFS_STATUS_OK)
        mutation_scope_fail(vol, started);
    else
        mutation_scope_success(vol);
    return status;
}

infs_status infs_get_typed_extension(
    struct infs_volume *vol, const char *path,
    struct infs_typed_extension *extension)
{
    if (!vol || !path || !extension)
        return INFS_STATUS_INVALID_ARGUMENT;
    memset(extension, 0, sizeof(*extension));

    struct infs_lookup owner;
    infs_status status = infs_lookup_path(vol, path, &owner);
    if (status != INFS_STATUS_OK)
        return status;
    uint8_t owner_block[INFS_BLOCK_SIZE];
    status = read_object(vol, owner.block, owner_block);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_attributes_disk *attributes = object_attributes(owner_block);
    if (!attributes)
        return INFS_STATUS_CORRUPT;
    if (id_is_zero(attributes->extended_attributes_object_id))
        return INFS_STATUS_NOT_FOUND;

    uint8_t extension_id[16];
    status = metadata_reference_typed_extension(
        vol, attributes->extended_attributes_object_id, extension_id);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_lookup lookup;
    status = index_find(vol, extension_id, &lookup);
    if (status != INFS_STATUS_OK || lookup.type != INFS_OBJECT_EXTENSION)
        return INFS_STATUS_CORRUPT;
    uint8_t object[INFS_BLOCK_SIZE];
    status = read_object(vol, lookup.block, object);
    if (status != INFS_STATUS_OK)
        return status;

    const struct infs_extension_payload_disk *payload =
        (const struct infs_extension_payload_disk *)(
            object + sizeof(struct infs_object_header_disk));
    size_t data_size = infs_le32_to_cpu(payload->data_size);
    uint8_t *data = malloc(data_size ? data_size : 1u);
    if (!data)
        return INFS_STATUS_NO_MEMORY;
    if (data_size)
        memcpy(data, payload + 1, data_size);
    memcpy(extension->type_id, payload->type_id, 16);
    extension->type_version = infs_le32_to_cpu(payload->type_version);
    extension->flags = infs_le16_to_cpu(payload->flags);
    extension->data = data;
    extension->data_size = data_size;
    return INFS_STATUS_OK;
}

void infs_free_typed_extension(struct infs_typed_extension *extension)
{
    if (!extension)
        return;
    free(extension->data);
    memset(extension, 0, sizeof(*extension));
}
