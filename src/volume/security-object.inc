// SPDX-License-Identifier: GPL-3.0-or-later

static int security_binding_input_valid(
    const struct infs_security_binding *binding)
{
    return infs_security_binding_is_valid(binding);
}

static int security_ace_input_valid(const struct infs_security_ace *ace)
{
    return infs_security_ace_is_valid(ace);
}

static int security_principal_input_valid(
    const struct infs_security_principal *principal)
{
    if (!principal ||
        principal->kind < INFS_PRINCIPAL_USER ||
        principal->kind > INFS_PRINCIPAL_SERVICE ||
        (!id_is_zero(principal->principal_id) &&
         infs_security_principal_id_is_reserved(principal->principal_id)) ||
        principal->flags != 0 ||
        principal->binding_count > INFS_PRINCIPAL_BINDINGS_PER_OBJECT ||
        (principal->binding_count && !principal->bindings))
        return 0;
    for (size_t i = 0; i < principal->binding_count; ++i) {
        if (!security_binding_input_valid(&principal->bindings[i]))
            return 0;
        for (size_t j = 0; j < i; ++j)
            if (principal->bindings[i].type == principal->bindings[j].type &&
                principal->bindings[i].size == principal->bindings[j].size &&
                memcmp(principal->bindings[i].value,
                       principal->bindings[j].value,
                       principal->bindings[i].size) == 0)
                return 0;
    }
    return 1;
}

static int security_descriptor_input_valid(
    const struct infs_security_descriptor *descriptor)
{
    return !descriptor ||
        (descriptor->ace_count <= INFS_SECURITY_MAX_ACES &&
         infs_security_descriptor_is_valid(descriptor));
}

static void security_binding_encode(
    struct infs_security_binding_disk *disk,
    const struct infs_security_binding *binding)
{
    memset(disk, 0, sizeof(*disk));
    disk->type = infs_cpu_to_le16(binding->type);
    disk->flags = infs_cpu_to_le16(binding->flags);
    disk->value_size = infs_cpu_to_le16(binding->size);
    memcpy(disk->value, binding->value, binding->size);
}

static void security_binding_decode(
    struct infs_security_binding *binding,
    const struct infs_security_binding_disk *disk)
{
    memset(binding, 0, sizeof(*binding));
    binding->type = infs_le16_to_cpu(disk->type);
    binding->flags = infs_le16_to_cpu(disk->flags);
    binding->size = infs_le16_to_cpu(disk->value_size);
    memcpy(binding->value, disk->value, binding->size);
}

static void security_ace_encode(struct infs_security_ace_disk *disk,
                                const struct infs_security_ace *ace)
{
    memset(disk, 0, sizeof(*disk));
    memcpy(disk->principal_id, ace->principal_id, 16);
    disk->rights = infs_cpu_to_le64(ace->rights);
    disk->disposition = infs_cpu_to_le16(ace->disposition);
    disk->flags = infs_cpu_to_le16(ace->flags);
}

static void security_ace_decode(struct infs_security_ace *ace,
                                const struct infs_security_ace_disk *disk)
{
    memset(ace, 0, sizeof(*ace));
    memcpy(ace->principal_id, disk->principal_id, 16);
    ace->rights = infs_le64_to_cpu(disk->rights);
    ace->disposition = infs_le16_to_cpu(disk->disposition);
    ace->flags = infs_le16_to_cpu(disk->flags);
}

static infs_status security_principal_lookup(
    struct infs_volume *vol, const uint8_t principal_id[16],
    struct infs_lookup *lookup_out)
{
    struct infs_lookup lookup;
    infs_status status = index_find(vol, principal_id, &lookup);
    if (status != INFS_STATUS_OK)
        return status;
    if (lookup.type != INFS_OBJECT_PRINCIPAL)
        return INFS_STATUS_CORRUPT;
    if (lookup_out)
        *lookup_out = lookup;
    return INFS_STATUS_OK;
}

static infs_status security_descriptor_validate_reference(
    struct infs_volume *vol, const uint8_t security_id[16])
{
    if (!vol || !security_id || id_is_zero(security_id))
        return INFS_STATUS_INVALID_ARGUMENT;
    struct infs_lookup lookup;
    infs_status status = index_find(vol, security_id, &lookup);
    if (status != INFS_STATUS_OK || lookup.type != INFS_OBJECT_SECURITY)
        return INFS_STATUS_CORRUPT;
    uint8_t object[INFS_BLOCK_SIZE];
    status = read_object(vol, lookup.block, object);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)object;
    return id_is_zero(header->parent_id) ?
        INFS_STATUS_OK : INFS_STATUS_CORRUPT;
}

static int security_binding_equal(
    const struct infs_security_binding *left,
    const struct infs_security_binding *right)
{
    return left && right && left->type == right->type &&
        left->flags == right->flags && left->size == right->size &&
        memcmp(left->value, right->value, left->size) == 0;
}

static int security_principal_has_binding(
    const struct infs_security_principal *principal,
    const struct infs_security_binding *binding)
{
    if (!principal || !binding)
        return 0;
    for (size_t i = 0; i < principal->binding_count; ++i)
        if (security_binding_equal(&principal->bindings[i], binding))
            return 1;
    return 0;
}

static infs_status security_binding_index_locate(
    struct infs_volume *vol, const struct infs_security_binding *binding,
    uint8_t principal_id_out[16], uint8_t object_id_out[16],
    uint16_t *slot_out, int *found_out)
{
    if (!vol || !binding || binding->type == INFS_BINDING_OPAQUE ||
        !security_binding_input_valid(binding))
        return INFS_STATUS_INVALID_ARGUMENT;

    uint8_t digest[32];
    infs_status status = infs_security_binding_digest(binding, digest);
    if (status != INFS_STATUS_OK)
        return status;

    uint8_t first_free_id[16] = {0};
    uint16_t first_free_slot = 0;
    int have_free = 0;

    for (uint16_t slot = 0; slot < INFS_SECURITY_HASH_SLOTS; ++slot) {
        uint8_t id[16];
        infs_security_binding_index_object_id(digest, slot, id);

        /* Derived IDs share the global object namespace. Rare reserved/global
         * collisions are simply skipped like any other occupied candidate. */
        if (infs_security_principal_id_is_reserved(id) ||
            memcmp(id, snapshot_catalog_id, 16) == 0)
            continue;

        struct infs_lookup lookup;
        status = index_find(vol, id, &lookup);
        if (status == INFS_STATUS_NOT_FOUND) {
            if (!have_free) {
                memcpy(first_free_id, id, 16);
                first_free_slot = slot;
                have_free = 1;
            }
            continue;
        }
        if (status != INFS_STATUS_OK)
            return status;
        if (lookup.type != INFS_OBJECT_SECURITY_BINDING)
            continue;

        uint8_t object[INFS_BLOCK_SIZE];
        status = read_object(vol, lookup.block, object);
        if (status != INFS_STATUS_OK)
            return status;
        struct infs_object_header_disk *header =
            (struct infs_object_header_disk *)object;
        struct infs_security_binding_index_payload_disk *payload =
            (struct infs_security_binding_index_payload_disk *)(header + 1);

        if (memcmp(payload->binding_digest, digest, 32) != 0 ||
            infs_le16_to_cpu(payload->binding_type) != binding->type ||
            infs_le16_to_cpu(payload->binding_size) != binding->size ||
            memcmp(payload->value, binding->value, binding->size) != 0)
            continue;

        if (principal_id_out)
            memcpy(principal_id_out, payload->principal_id, 16);
        if (object_id_out)
            memcpy(object_id_out, id, 16);
        if (slot_out)
            *slot_out = slot;
        if (found_out)
            *found_out = 1;
        return INFS_STATUS_OK;
    }

    if (!have_free)
        return INFS_STATUS_NO_SPACE;
    if (object_id_out)
        memcpy(object_id_out, first_free_id, 16);
    if (slot_out)
        *slot_out = first_free_slot;
    if (found_out)
        *found_out = 0;
    return INFS_STATUS_OK;
}

static infs_status security_binding_index_create(
    struct infs_volume *vol, const uint8_t principal_id[16],
    const struct infs_security_binding *binding)
{
    if (binding->type == INFS_BINDING_OPAQUE)
        return INFS_STATUS_OK;

    uint8_t mapped_principal[16] = {0};
    uint8_t id[16] = {0};
    uint16_t slot = 0;
    int found = 0;
    infs_status status = security_binding_index_locate(
        vol, binding, mapped_principal, id, &slot, &found);
    if (status != INFS_STATUS_OK)
        return status;
    if (found)
        return memcmp(mapped_principal, principal_id, 16) == 0 ?
            INFS_STATUS_OK : INFS_STATUS_ALREADY_EXISTS;

    uint8_t digest[32];
    status = infs_security_binding_digest(binding, digest);
    if (status != INFS_STATUS_OK)
        return status;

    uint8_t object[INFS_BLOCK_SIZE];
    status = infs_object_init(
        object, INFS_OBJECT_SECURITY_BINDING, id, principal_id,
        infs_le64_to_cpu(vol->sb.generation) + 1u,
        sizeof(struct infs_security_binding_index_payload_disk));
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_security_binding_index_payload_disk *payload =
        (struct infs_security_binding_index_payload_disk *)(
            object + sizeof(struct infs_object_header_disk));
    payload->version = infs_cpu_to_le16(INFS_SECURITY_VERSION);
    payload->slot = infs_cpu_to_le16(slot);
    payload->binding_type = infs_cpu_to_le16(binding->type);
    payload->binding_size = infs_cpu_to_le16(binding->size);
    memcpy(payload->principal_id, principal_id, 16);
    memcpy(payload->binding_digest, digest, 32);
    memcpy(payload->value, binding->value, binding->size);

    status = infs_object_finalize(object);
    if (status != INFS_STATUS_OK)
        return status;

    uint64_t block = 0, got = 0;
    status = alloc_run(vol, 1, &block, &got);
    if (status != INFS_STATUS_OK)
        return status;
    if (got != 1)
        return INFS_STATUS_CORRUPT;

    status = write_block(vol, block, object);
    if (status == INFS_STATUS_OK)
        status = index_add(vol, id, block, INFS_OBJECT_SECURITY_BINDING);
    if (status != INFS_STATUS_OK) {
        free_run(vol, block, 1);
        return status;
    }
    return INFS_STATUS_OK;
}

static infs_status security_binding_index_remove(
    struct infs_volume *vol, const uint8_t principal_id[16],
    const struct infs_security_binding *binding)
{
    if (binding->type == INFS_BINDING_OPAQUE)
        return INFS_STATUS_OK;

    uint8_t mapped_principal[16] = {0};
    uint8_t id[16] = {0};
    int found = 0;
    infs_status status = security_binding_index_locate(
        vol, binding, mapped_principal, id, NULL, &found);
    if (status != INFS_STATUS_OK)
        return status;
    if (!found || memcmp(mapped_principal, principal_id, 16) != 0)
        return INFS_STATUS_CORRUPT;

    struct infs_lookup lookup;
    status = index_find(vol, id, &lookup);
    if (status != INFS_STATUS_OK ||
        lookup.type != INFS_OBJECT_SECURITY_BINDING)
        return INFS_STATUS_CORRUPT;
    status = index_remove(vol, id);
    if (status != INFS_STATUS_OK)
        return status;
    free_run(vol, lookup.block, 1);
    return vol->tx_error;
}

static infs_status security_binding_conflict(
    struct infs_volume *vol, const uint8_t self_id[16],
    const struct infs_security_binding *binding)
{
    if (binding->type == INFS_BINDING_OPAQUE)
        return INFS_STATUS_OK;

    uint8_t mapped_principal[16] = {0};
    int found = 0;
    infs_status status = security_binding_index_locate(
        vol, binding, mapped_principal, NULL, NULL, &found);
    if (status != INFS_STATUS_OK)
        return status;
    if (!found)
        return INFS_STATUS_OK;
    return self_id && memcmp(mapped_principal, self_id, 16) == 0 ?
        INFS_STATUS_OK : INFS_STATUS_ALREADY_EXISTS;
}

static infs_status put_security_principal_tx(
    struct infs_volume *vol, struct infs_security_principal *principal,
    uint8_t id_out[16])
{
    if (!vol->writable)
        return INFS_STATUS_READ_ONLY;
    if (!security_objects_enabled(vol))
        return INFS_STATUS_NOT_SUPPORTED;

    uint8_t id[16];
    memcpy(id, principal->principal_id, 16);
    if (id_is_zero(id)) {
        infs_status status = generate_unique_object_id(vol, id);
        if (status != INFS_STATUS_OK)
            return status;
    } else if (infs_security_principal_id_is_reserved(id) ||
               memcmp(id, snapshot_catalog_id, 16) == 0) {
        return INFS_STATUS_INVALID_ARGUMENT;
    }

    struct infs_lookup existing;
    infs_status status = index_find(vol, id, &existing);
    int existed = status == INFS_STATUS_OK;
    if (existed && existing.type != INFS_OBJECT_PRINCIPAL)
        return INFS_STATUS_ALREADY_EXISTS;
    if (status != INFS_STATUS_OK && status != INFS_STATUS_NOT_FOUND)
        return status;

    struct infs_security_principal previous = {0};
    if (existed) {
        status = infs_get_security_principal(vol, id, &previous);
        if (status != INFS_STATUS_OK)
            return status;
    }

    for (size_t i = 0; i < principal->binding_count; ++i) {
        status = security_binding_conflict(
            vol, id, &principal->bindings[i]);
        if (status != INFS_STATUS_OK)
            goto out;
    }

    size_t binding_bytes = principal->binding_count *
        sizeof(struct infs_security_binding_disk);
    uint32_t payload_size = (uint32_t)(
        sizeof(struct infs_principal_payload_disk) + binding_bytes);
    uint8_t object[INFS_BLOCK_SIZE];
    status = infs_object_init(
        object, INFS_OBJECT_PRINCIPAL, id, NULL,
        infs_le64_to_cpu(vol->sb.generation) + 1u, payload_size);
    if (status != INFS_STATUS_OK)
        goto out;

    struct infs_principal_payload_disk *payload =
        (struct infs_principal_payload_disk *)(
            object + sizeof(struct infs_object_header_disk));
    payload->version = infs_cpu_to_le16(INFS_SECURITY_VERSION);
    payload->kind = infs_cpu_to_le16(principal->kind);
    payload->binding_count =
        infs_cpu_to_le16((uint16_t)principal->binding_count);
    payload->flags = infs_cpu_to_le16(principal->flags);
    payload->binding_bytes = infs_cpu_to_le32((uint32_t)binding_bytes);
    struct infs_security_binding_disk *bindings =
        (struct infs_security_binding_disk *)(payload + 1);
    for (size_t i = 0; i < principal->binding_count; ++i)
        security_binding_encode(&bindings[i], &principal->bindings[i]);

    status = infs_object_finalize(object);
    if (status != INFS_STATUS_OK)
        goto out;

    if (existed) {
        status = write_object(vol, existing.block, object);
        if (status != INFS_STATUS_OK)
            goto out;
    } else {
        uint64_t block = 0, got = 0;
        status = alloc_run(vol, 1, &block, &got);
        if (status != INFS_STATUS_OK)
            goto out;
        if (got != 1) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }
        status = write_block(vol, block, object);
        if (status == INFS_STATUS_OK)
            status = index_add(vol, id, block, INFS_OBJECT_PRINCIPAL);
        if (status != INFS_STATUS_OK) {
            free_run(vol, block, 1);
            goto out;
        }
    }

    for (size_t i = 0; i < previous.binding_count; ++i) {
        if (previous.bindings[i].type == INFS_BINDING_OPAQUE ||
            security_principal_has_binding(principal, &previous.bindings[i]))
            continue;
        status = security_binding_index_remove(
            vol, id, &previous.bindings[i]);
        if (status != INFS_STATUS_OK)
            goto out;
    }
    for (size_t i = 0; i < principal->binding_count; ++i) {
        status = security_binding_index_create(
            vol, id, &principal->bindings[i]);
        if (status != INFS_STATUS_OK)
            goto out;
    }

    memcpy(id_out, id, 16);
    status = finish_mutation(
        vol, (uint64_t)INFS_BLOCK_SIZE *
            (UINT64_C(4) + (uint64_t)principal->binding_count));
out:
    infs_free_security_principal(&previous);
    return status;
}

infs_status infs_put_security_principal(
    struct infs_volume *vol, struct infs_security_principal *principal)
{
    if (!vol || !security_principal_input_valid(principal))
        return INFS_STATUS_INVALID_ARGUMENT;
    int started = 0;
    infs_status status = mutation_scope_begin(vol, &started);
    if (status != INFS_STATUS_OK)
        return status;
    uint8_t id[16];
    status = put_security_principal_tx(vol, principal, id);
    if (status != INFS_STATUS_OK)
        mutation_scope_fail(vol, started);
    else {
        mutation_scope_success(vol);
        memcpy(principal->principal_id, id, 16);
    }
    return status;
}

infs_status infs_get_security_principal(
    struct infs_volume *vol, const uint8_t principal_id[16],
    struct infs_security_principal *principal)
{
    if (!vol || !principal_id || id_is_zero(principal_id) || !principal)
        return INFS_STATUS_INVALID_ARGUMENT;
    memset(principal, 0, sizeof(*principal));
    struct infs_lookup lookup;
    infs_status status = security_principal_lookup(vol, principal_id, &lookup);
    if (status != INFS_STATUS_OK)
        return status;
    uint8_t object[INFS_BLOCK_SIZE];
    status = read_object(vol, lookup.block, object);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_principal_payload_disk *payload =
        (struct infs_principal_payload_disk *)(
            object + sizeof(struct infs_object_header_disk));
    size_t count = infs_le16_to_cpu(payload->binding_count);
    struct infs_security_binding *bindings =
        calloc(count ? count : 1u, sizeof(*bindings));
    if (!bindings)
        return INFS_STATUS_NO_MEMORY;
    struct infs_security_binding_disk *disk =
        (struct infs_security_binding_disk *)(payload + 1);
    for (size_t i = 0; i < count; ++i)
        security_binding_decode(&bindings[i], &disk[i]);
    memcpy(principal->principal_id, principal_id, 16);
    principal->kind = infs_le16_to_cpu(payload->kind);
    principal->flags = infs_le16_to_cpu(payload->flags);
    principal->bindings = bindings;
    principal->binding_count = count;
    return INFS_STATUS_OK;
}

infs_status infs_find_security_principal_by_binding(
    struct infs_volume *vol, const struct infs_security_binding *binding,
    uint8_t principal_id_out[16])
{
    if (!vol || !binding || !principal_id_out ||
        !security_binding_input_valid(binding))
        return INFS_STATUS_INVALID_ARGUMENT;
    if (binding->type == INFS_BINDING_OPAQUE)
        return INFS_STATUS_NOT_SUPPORTED;

    uint8_t principal_id[16] = {0};
    int found = 0;
    infs_status status = security_binding_index_locate(
        vol, binding, principal_id, NULL, NULL, &found);
    if (status != INFS_STATUS_OK)
        return status;
    if (!found)
        return INFS_STATUS_NOT_FOUND;

    struct infs_security_principal principal = {0};
    status = infs_get_security_principal(vol, principal_id, &principal);
    if (status != INFS_STATUS_OK)
        return status == INFS_STATUS_NOT_FOUND ?
            INFS_STATUS_CORRUPT : status;
    int matches = security_principal_has_binding(&principal, binding);
    infs_free_security_principal(&principal);
    if (!matches)
        return INFS_STATUS_CORRUPT;

    memcpy(principal_id_out, principal_id, 16);
    return INFS_STATUS_OK;
}

static infs_status security_principal_references_valid(
    struct infs_volume *vol, const struct infs_security_descriptor *descriptor)
{
    struct infs_lookup lookup;
    infs_status status = security_principal_lookup(
        vol, descriptor->owner_principal_id, &lookup);
    if (status != INFS_STATUS_OK)
        return status == INFS_STATUS_NOT_FOUND ? INFS_STATUS_INVALID_ARGUMENT : status;
    status = security_principal_lookup(
        vol, descriptor->primary_group_principal_id, &lookup);
    if (status != INFS_STATUS_OK)
        return status == INFS_STATUS_NOT_FOUND ? INFS_STATUS_INVALID_ARGUMENT : status;
    for (size_t i = 0; i < descriptor->ace_count; ++i) {
        if (infs_security_principal_id_is_well_known(
                descriptor->aces[i].principal_id))
            continue;
        status = security_principal_lookup(
            vol, descriptor->aces[i].principal_id, &lookup);
        if (status != INFS_STATUS_OK)
            return status == INFS_STATUS_NOT_FOUND ?
                INFS_STATUS_INVALID_ARGUMENT : status;
    }
    return INFS_STATUS_OK;
}

static infs_status security_descriptor_digest(
    const struct infs_security_descriptor *descriptor, uint8_t digest[32])
{
    size_t ace_bytes = 0, total = 0;
    if (!infiltratr_size_multiply_checked(
            descriptor->ace_count, sizeof(struct infs_security_ace_disk),
            &ace_bytes) ||
        !infiltratr_size_add_checked(40u, ace_bytes, &total))
        return INFS_STATUS_OVERFLOW;
    uint8_t *canonical = calloc(total ? total : 1u, 1u);
    if (!canonical)
        return INFS_STATUS_NO_MEMORY;
    infs_store_le16(canonical, INFS_SECURITY_VERSION);
    infs_store_le16(canonical + 2u, descriptor->flags);
    infs_store_le32(canonical + 4u, (uint32_t)descriptor->ace_count);
    memcpy(canonical + 8u, descriptor->owner_principal_id, 16u);
    memcpy(canonical + 24u, descriptor->primary_group_principal_id, 16u);
    struct infs_security_ace_disk *aces =
        (struct infs_security_ace_disk *)(canonical + 40u);
    for (size_t i = 0; i < descriptor->ace_count; ++i)
        security_ace_encode(&aces[i], &descriptor->aces[i]);
    infs_sha256(canonical, total, digest);
    free(canonical);
    return INFS_STATUS_OK;
}

static infs_status security_ace_page_validate(
    struct infs_volume *vol, const uint8_t block[INFS_BLOCK_SIZE],
    const uint8_t owner_id[16], struct infs_security_ace_disk **aces_out,
    uint32_t *count_out)
{
    if (!infs_validate_metadata_page(
            block, INFS_SECURITY_ACE_PAGE_MAGIC, owner_id) ||
        !metadata_page_generation_valid(vol, block))
        return INFS_STATUS_CORRUPT;
    struct infs_metadata_page_disk *page =
        (struct infs_metadata_page_disk *)block;
    uint32_t count = infs_le32_to_cpu(page->entry_count);
    uint32_t bytes = infs_le32_to_cpu(page->bytes_used);
    if (count > INFS_SECURITY_ACES_PER_PAGE ||
        bytes != count * sizeof(struct infs_security_ace_disk))
        return INFS_STATUS_CORRUPT;
    struct infs_security_ace_disk *aces =
        (struct infs_security_ace_disk *)(page + 1);
    for (uint32_t i = 0; i < count; ++i) {
        struct infs_security_ace decoded;
        security_ace_decode(&decoded, &aces[i]);
        if (!security_ace_input_valid(&decoded) ||
            infs_le32_to_cpu(aces[i].reserved) != 0)
            return INFS_STATUS_CORRUPT;
    }
    if (aces_out)
        *aces_out = aces;
    if (count_out)
        *count_out = count;
    return INFS_STATUS_OK;
}

static infs_status security_descriptor_collect_aces(
    struct infs_volume *vol, uint8_t object[INFS_BLOCK_SIZE],
    struct infs_security_ace **aces_out, size_t *count_out)
{
    if (!aces_out || !count_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    *aces_out = NULL;
    *count_out = 0;
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)object;
    struct infs_security_payload_disk *payload =
        (struct infs_security_payload_disk *)(header + 1);
    uint32_t count = infs_le32_to_cpu(payload->ace_count);
    struct infs_security_ace *aces =
        calloc(count ? count : 1u, sizeof(*aces));
    if (!aces)
        return INFS_STATUS_NO_MEMORY;

    if (infs_le16_to_cpu(header->object_version) ==
        INFS_OBJECT_VERSION_CLASSIC) {
        struct infs_security_ace_disk *disk =
            (struct infs_security_ace_disk *)(payload + 1);
        for (uint32_t i = 0; i < count; ++i)
            security_ace_decode(&aces[i], &disk[i]);
    } else {
        uint32_t pages = infs_le32_to_cpu(payload->page_count);
        uint64_t *pointers = (uint64_t *)(payload + 1);
        uint32_t copied = 0;
        for (uint32_t p = 0; p < pages; ++p) {
            uint64_t block_no = infs_le64_to_cpu(pointers[p]);
            if (!block_no || block_no >= infs_le64_to_cpu(vol->sb.total_blocks) ||
                !bitmap_get(vol->bitmap, block_no)) {
                free(aces);
                return INFS_STATUS_CORRUPT;
            }
            uint8_t page_data[INFS_BLOCK_SIZE];
            infs_status status = read_block(vol, block_no, page_data);
            if (status != INFS_STATUS_OK) {
                free(aces);
                return status;
            }
            struct infs_security_ace_disk *disk;
            uint32_t n = 0;
            status = security_ace_page_validate(
                vol, page_data, header->object_id, &disk, &n);
            if (status != INFS_STATUS_OK || n > count - copied) {
                free(aces);
                return status == INFS_STATUS_OK ?
                    INFS_STATUS_CORRUPT : status;
            }
            for (uint32_t i = 0; i < n; ++i)
                security_ace_decode(&aces[copied + i], &disk[i]);
            copied += n;
        }
        if (copied != count) {
            free(aces);
            return INFS_STATUS_CORRUPT;
        }
    }
    *aces_out = aces;
    *count_out = count;
    return INFS_STATUS_OK;
}

static infs_status security_descriptor_decode(
    struct infs_volume *vol, uint8_t object[INFS_BLOCK_SIZE],
    struct infs_security_descriptor *descriptor)
{
    memset(descriptor, 0, sizeof(*descriptor));
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)object;
    if (infs_le16_to_cpu(header->object_type) != INFS_OBJECT_SECURITY)
        return INFS_STATUS_CORRUPT;
    struct infs_security_payload_disk *payload =
        (struct infs_security_payload_disk *)(header + 1);
    memcpy(descriptor->owner_principal_id, payload->owner_principal_id, 16);
    memcpy(descriptor->primary_group_principal_id,
           payload->primary_group_principal_id, 16);
    descriptor->flags = infs_le16_to_cpu(payload->flags);
    infs_status status = security_descriptor_collect_aces(
        vol, object, &descriptor->aces, &descriptor->ace_count);
    if (status != INFS_STATUS_OK)
        return status;
    uint8_t digest[32];
    status = security_descriptor_digest(descriptor, digest);
    if (status != INFS_STATUS_OK ||
        memcmp(digest, payload->semantic_digest, sizeof(digest)) != 0) {
        infs_free_security_descriptor(descriptor);
        return status == INFS_STATUS_OK ? INFS_STATUS_CORRUPT : status;
    }

    uint16_t slot = infs_le16_to_cpu(payload->identity_slot);
    uint8_t expected_id[16];
    if (slot >= INFS_SECURITY_HASH_SLOTS) {
        infs_free_security_descriptor(descriptor);
        return INFS_STATUS_CORRUPT;
    }
    infs_security_descriptor_object_id(digest, slot, expected_id);
    if (memcmp(expected_id, header->object_id, 16) != 0) {
        infs_free_security_descriptor(descriptor);
        return INFS_STATUS_CORRUPT;
    }
    return INFS_STATUS_OK;
}

static int security_descriptor_equal(
    const struct infs_security_descriptor *a,
    const struct infs_security_descriptor *b)
{
    if (a->flags != b->flags || a->ace_count != b->ace_count ||
        memcmp(a->owner_principal_id, b->owner_principal_id, 16) != 0 ||
        memcmp(a->primary_group_principal_id,
               b->primary_group_principal_id, 16) != 0)
        return 0;
    for (size_t i = 0; i < a->ace_count; ++i)
        if (memcmp(a->aces[i].principal_id, b->aces[i].principal_id, 16) != 0 ||
            a->aces[i].rights != b->aces[i].rights ||
            a->aces[i].disposition != b->aces[i].disposition ||
            a->aces[i].flags != b->aces[i].flags)
            return 0;
    return 1;
}

static infs_status security_descriptor_find_match(
    struct infs_volume *vol, const struct infs_security_descriptor *descriptor,
    const uint8_t digest[32], uint8_t id_out[16], uint16_t *slot_out,
    int *found_out)
{
    uint8_t first_free_id[16] = {0};
    uint16_t first_free_slot = 0;
    int have_free = 0;

    for (uint16_t slot = 0; slot < INFS_SECURITY_HASH_SLOTS; ++slot) {
        uint8_t id[16];
        infs_security_descriptor_object_id(digest, slot, id);
        if (infs_security_principal_id_is_reserved(id) ||
            memcmp(id, snapshot_catalog_id, 16) == 0)
            continue;

        struct infs_lookup lookup;
        infs_status status = index_find(vol, id, &lookup);
        if (status == INFS_STATUS_NOT_FOUND) {
            if (!have_free) {
                memcpy(first_free_id, id, 16);
                first_free_slot = slot;
                have_free = 1;
            }
            continue;
        }
        if (status != INFS_STATUS_OK)
            return status;
        if (lookup.type != INFS_OBJECT_SECURITY)
            continue;

        uint8_t object[INFS_BLOCK_SIZE];
        status = read_object(vol, lookup.block, object);
        if (status != INFS_STATUS_OK)
            return status;
        struct infs_security_payload_disk *payload =
            (struct infs_security_payload_disk *)(
                object + sizeof(struct infs_object_header_disk));
        if (memcmp(payload->semantic_digest, digest, 32) != 0)
            continue;

        struct infs_security_descriptor candidate;
        status = security_descriptor_decode(vol, object, &candidate);
        if (status != INFS_STATUS_OK)
            return status;
        int same = security_descriptor_equal(&candidate, descriptor);
        infs_free_security_descriptor(&candidate);
        if (!same)
            continue;

        memcpy(id_out, id, 16);
        *slot_out = slot;
        *found_out = 1;
        return INFS_STATUS_OK;
    }

    if (!have_free)
        return INFS_STATUS_NO_SPACE;
    memcpy(id_out, first_free_id, 16);
    *slot_out = first_free_slot;
    *found_out = 0;
    return INFS_STATUS_OK;
}

static infs_status security_descriptor_create(
    struct infs_volume *vol, const struct infs_security_descriptor *descriptor,
    const uint8_t digest[32], const uint8_t id[16], uint16_t identity_slot)
{
    uint32_t count = (uint32_t)descriptor->ace_count;
    uint32_t pages = count > INFS_SECURITY_INLINE_ACES ?
        (count + INFS_SECURITY_ACES_PER_PAGE - 1u) /
            INFS_SECURITY_ACES_PER_PAGE : 0u;
    uint32_t payload_size = (uint32_t)sizeof(struct infs_security_payload_disk) +
        (pages ? pages * (uint32_t)sizeof(uint64_t) :
                 count * (uint32_t)sizeof(struct infs_security_ace_disk));

    uint8_t object[INFS_BLOCK_SIZE];
    infs_status status = infs_object_init(
        object, INFS_OBJECT_SECURITY, id, NULL,
        infs_le64_to_cpu(vol->sb.generation) + 1u, payload_size);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)object;
    if (pages)
        header->object_version = infs_cpu_to_le16(INFS_OBJECT_VERSION_PAGED);

    struct infs_security_payload_disk *payload =
        (struct infs_security_payload_disk *)(header + 1);
    payload->version = infs_cpu_to_le16(INFS_SECURITY_VERSION);
    payload->flags = infs_cpu_to_le16(descriptor->flags);
    payload->ace_count = infs_cpu_to_le32(count);
    payload->page_count = infs_cpu_to_le32(pages);
    payload->identity_slot = infs_cpu_to_le16(identity_slot);
    memcpy(payload->owner_principal_id, descriptor->owner_principal_id, 16);
    memcpy(payload->primary_group_principal_id,
           descriptor->primary_group_principal_id, 16);
    memcpy(payload->semantic_digest, digest, 32);

    if (!pages) {
        struct infs_security_ace_disk *disk =
            (struct infs_security_ace_disk *)(payload + 1);
        for (uint32_t i = 0; i < count; ++i)
            security_ace_encode(&disk[i], &descriptor->aces[i]);
    } else {
        uint64_t *pointers = (uint64_t *)(payload + 1);
        uint32_t at = 0;
        for (uint32_t p = 0; p < pages; ++p) {
            uint32_t n = count - at;
            if (n > INFS_SECURITY_ACES_PER_PAGE)
                n = INFS_SECURITY_ACES_PER_PAGE;
            uint64_t generation = 0;
            status = transaction_next_generation(vol, &generation);
            if (status != INFS_STATUS_OK)
                return status;

            uint8_t page_data[INFS_BLOCK_SIZE];
            status = infs_metadata_page_init(
                page_data, INFS_SECURITY_ACE_PAGE_MAGIC, id, generation);
            if (status != INFS_STATUS_OK)
                return status;
            struct infs_metadata_page_disk *page =
                (struct infs_metadata_page_disk *)page_data;
            page->entry_count = infs_cpu_to_le32(n);
            page->bytes_used = infs_cpu_to_le32(
                n * (uint32_t)sizeof(struct infs_security_ace_disk));
            struct infs_security_ace_disk *disk =
                (struct infs_security_ace_disk *)(page + 1);
            for (uint32_t i = 0; i < n; ++i)
                security_ace_encode(&disk[i], &descriptor->aces[at + i]);

            uint64_t stored = 0;
            status = metadata_page_store(vol, 0, page_data, &stored);
            if (status != INFS_STATUS_OK)
                return status;
            pointers[p] = infs_cpu_to_le64(stored);
            at += n;
        }
    }

    status = infs_object_finalize(object);
    if (status != INFS_STATUS_OK)
        return status;

    uint64_t block = 0, got = 0;
    status = alloc_run(vol, 1, &block, &got);
    if (status != INFS_STATUS_OK)
        return status;
    if (got != 1)
        return INFS_STATUS_CORRUPT;

    status = write_block(vol, block, object);
    if (status == INFS_STATUS_OK)
        status = index_add(vol, id, block, INFS_OBJECT_SECURITY);
    if (status != INFS_STATUS_OK) {
        free_run(vol, block, 1);
        return status;
    }
    return INFS_STATUS_OK;
}

static infs_status set_security_descriptor_tx(
    struct infs_volume *vol, const char *path,
    const struct infs_security_descriptor *descriptor)
{
    if (!vol->writable)
        return INFS_STATUS_READ_ONLY;
    if (!security_objects_enabled(vol))
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
    uint8_t old_id[16];
    memcpy(old_id, attributes->security_object_id, 16);

    if (!descriptor) {
        if (id_is_zero(old_id))
            return INFS_STATUS_OK;
        memset(attributes->security_object_id, 0, 16);
        touch_ctime(vol, attributes);
        status = write_object(vol, owner.block, owner_block);
        if (status != INFS_STATUS_OK)
            return status;
        return finish_mutation(vol, (uint64_t)INFS_BLOCK_SIZE * 3u);
    }

    status = security_principal_references_valid(vol, descriptor);
    if (status != INFS_STATUS_OK)
        return status;

    uint8_t digest[32];
    status = security_descriptor_digest(descriptor, digest);
    if (status != INFS_STATUS_OK)
        return status;
    uint8_t new_id[16];
    uint16_t identity_slot = 0;
    int found = 0;
    status = security_descriptor_find_match(
        vol, descriptor, digest, new_id, &identity_slot, &found);
    if (status != INFS_STATUS_OK)
        return status;
    if (!found) {
        status = security_descriptor_create(
            vol, descriptor, digest, new_id, identity_slot);
        if (status != INFS_STATUS_OK)
            return status;
    }
    if (memcmp(old_id, new_id, 16) == 0)
        return INFS_STATUS_OK;

    memcpy(attributes->security_object_id, new_id, 16);
    touch_ctime(vol, attributes);
    status = write_object(vol, owner.block, owner_block);
    if (status != INFS_STATUS_OK)
        return status;
    return finish_mutation(vol, (uint64_t)INFS_BLOCK_SIZE * 4u);
}

infs_status infs_set_security_descriptor(
    struct infs_volume *vol, const char *path,
    const struct infs_security_descriptor *descriptor)
{
    if (!vol || !path || path[0] != '/' ||
        !security_descriptor_input_valid(descriptor))
        return INFS_STATUS_INVALID_ARGUMENT;
    int started = 0;
    infs_status status = mutation_scope_begin(vol, &started);
    if (status != INFS_STATUS_OK)
        return status;
    status = set_security_descriptor_tx(vol, path, descriptor);
    if (status != INFS_STATUS_OK)
        mutation_scope_fail(vol, started);
    else
        mutation_scope_success(vol);
    return status;
}

infs_status infs_get_security_descriptor(
    struct infs_volume *vol, const char *path,
    struct infs_security_descriptor *descriptor)
{
    if (!vol || !path || !descriptor)
        return INFS_STATUS_INVALID_ARGUMENT;
    memset(descriptor, 0, sizeof(*descriptor));
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
    if (id_is_zero(attributes->security_object_id))
        return INFS_STATUS_NOT_FOUND;
    status = security_descriptor_validate_reference(
        vol, attributes->security_object_id);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_lookup security;
    status = index_find(vol, attributes->security_object_id, &security);
    if (status != INFS_STATUS_OK || security.type != INFS_OBJECT_SECURITY)
        return INFS_STATUS_CORRUPT;
    uint8_t object[INFS_BLOCK_SIZE];
    status = read_object(vol, security.block, object);
    if (status != INFS_STATUS_OK)
        return status;
    status = security_descriptor_decode(vol, object, descriptor);
    if (status != INFS_STATUS_OK)
        return status;
    return security_principal_references_valid(vol, descriptor);
}
