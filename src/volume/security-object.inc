static infs_status security_object_validate_owner(
    struct infs_volume *vol, const uint8_t security_id[16],
    const uint8_t owner_id[16])
{
    if (!vol || !security_id || !owner_id || id_is_zero(security_id))
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
    if (memcmp(header->parent_id, owner_id, 16) != 0)
        return INFS_STATUS_CORRUPT;
    return INFS_STATUS_OK;
}

// SPDX-License-Identifier: GPL-3.0-or-later

static int security_descriptor_input_valid(
    const struct infs_security_descriptor *descriptor)
{
    if (!descriptor)
        return 1;
    if ((descriptor->principal_count && !descriptor->principals) ||
        (descriptor->ace_count && !descriptor->aces) ||
        descriptor->principal_count > UINT16_MAX ||
        descriptor->ace_count > UINT16_MAX)
        return 0;
    size_t payload = sizeof(struct infs_security_payload_disk);
    size_t pbytes = 0, abytes = 0;
    if (!infiltratr_size_multiply_checked(
            descriptor->principal_count,
            sizeof(struct infs_security_principal_disk), &pbytes) ||
        !infiltratr_size_multiply_checked(
            descriptor->ace_count, sizeof(struct infs_security_ace_disk),
            &abytes) ||
        !infiltratr_size_add_checked(payload, pbytes, &payload) ||
        !infiltratr_size_add_checked(payload, abytes, &payload) ||
        payload > INFS_BLOCK_SIZE - sizeof(struct infs_object_header_disk))
        return 0;
    for (size_t i = 0; i < descriptor->principal_count; ++i) {
        const struct infs_security_principal *p = &descriptor->principals[i];
        if (id_is_zero(p->principal_id) ||
            p->kind < INFS_PRINCIPAL_USER || p->kind > INFS_PRINCIPAL_WELL_KNOWN ||
            p->binding_size > INFS_SECURITY_BINDING_MAX || p->flags != 0 ||
            (p->binding_type == INFS_BINDING_NONE && p->binding_size != 0))
            return 0;
        for (size_t j = 0; j < i; ++j)
            if (memcmp(descriptor->principals[j].principal_id,
                       p->principal_id, 16) == 0)
                return 0;
    }
    for (size_t i = 0; i < descriptor->ace_count; ++i) {
        const struct infs_security_ace *a = &descriptor->aces[i];
        if (id_is_zero(a->principal_id) || !a->rights ||
            (a->rights & ~INFS_RIGHT_ALL) != 0 ||
            (a->disposition != INFS_ACE_ALLOW &&
             a->disposition != INFS_ACE_DENY) ||
            (a->flags & ~INFS_ACE_KNOWN_FLAGS) != 0)
            return 0;
        int found = 0;
        for (size_t j = 0; j < descriptor->principal_count; ++j)
            if (memcmp(descriptor->principals[j].principal_id,
                       a->principal_id, 16) == 0) {
                found = 1;
                break;
            }
        if (!found)
            return 0;
    }
    return 1;
}

static infs_status security_object_destroy_by_id(
    struct infs_volume *vol, const uint8_t id[16])
{
    if (id_is_zero(id))
        return INFS_STATUS_OK;
    struct infs_lookup lookup;
    infs_status status = index_find(vol, id, &lookup);
    if (status != INFS_STATUS_OK || lookup.type != INFS_OBJECT_SECURITY)
        return INFS_STATUS_CORRUPT;
    status = index_remove(vol, id);
    if (status != INFS_STATUS_OK)
        return status;
    free_run(vol, lookup.block, 1);
    return vol->tx_error;
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
        if (status == INFS_STATUS_OK)
            status = security_object_destroy_by_id(vol, old_id);
        if (status != INFS_STATUS_OK)
            return status;
        return finish_mutation(vol, (uint64_t)INFS_BLOCK_SIZE * 4u);
    }

    uint8_t security_id[16];
    status = generate_unique_object_id(vol, security_id);
    if (status != INFS_STATUS_OK)
        return status;
    uint64_t security_block = 0, got = 0;
    status = alloc_run(vol, 1, &security_block, &got);
    if (status != INFS_STATUS_OK)
        return status;
    if (got != 1)
        return INFS_STATUS_CORRUPT;

    size_t principal_bytes = descriptor->principal_count *
        sizeof(struct infs_security_principal_disk);
    size_t ace_bytes = descriptor->ace_count *
        sizeof(struct infs_security_ace_disk);
    uint32_t payload_size = (uint32_t)(
        sizeof(struct infs_security_payload_disk) + principal_bytes + ace_bytes);
    uint8_t object[INFS_BLOCK_SIZE];
    status = infs_object_init(
        object, INFS_OBJECT_SECURITY, security_id, owner.object_id,
        infs_le64_to_cpu(vol->sb.generation) + 1u, payload_size);
    if (status != INFS_STATUS_OK) {
        free_run(vol, security_block, 1);
        return status;
    }
    struct infs_security_payload_disk *payload =
        (struct infs_security_payload_disk *)(
            object + sizeof(struct infs_object_header_disk));
    payload->version = infs_cpu_to_le16(INFS_SECURITY_VERSION_V1);
    payload->principal_count =
        infs_cpu_to_le16((uint16_t)descriptor->principal_count);
    payload->ace_count = infs_cpu_to_le16((uint16_t)descriptor->ace_count);
    payload->principal_bytes = infs_cpu_to_le32((uint32_t)principal_bytes);
    payload->ace_bytes = infs_cpu_to_le32((uint32_t)ace_bytes);
    struct infs_security_principal_disk *pd =
        (struct infs_security_principal_disk *)(payload + 1);
    for (size_t i = 0; i < descriptor->principal_count; ++i) {
        const struct infs_security_principal *src = &descriptor->principals[i];
        memcpy(pd[i].principal_id, src->principal_id, 16);
        pd[i].kind = infs_cpu_to_le16(src->kind);
        pd[i].binding_type = infs_cpu_to_le16(src->binding_type);
        pd[i].binding_size = infs_cpu_to_le16(src->binding_size);
        pd[i].flags = infs_cpu_to_le16(src->flags);
        memcpy(pd[i].binding, src->binding, src->binding_size);
    }
    struct infs_security_ace_disk *ad =
        (struct infs_security_ace_disk *)((uint8_t *)pd + principal_bytes);
    for (size_t i = 0; i < descriptor->ace_count; ++i) {
        const struct infs_security_ace *src = &descriptor->aces[i];
        memcpy(ad[i].principal_id, src->principal_id, 16);
        ad[i].rights = infs_cpu_to_le64(src->rights);
        ad[i].disposition = infs_cpu_to_le16(src->disposition);
        ad[i].flags = infs_cpu_to_le16(src->flags);
    }
    status = infs_object_finalize(object);
    if (status == INFS_STATUS_OK)
        status = write_block(vol, security_block, object);
    if (status != INFS_STATUS_OK) {
        free_run(vol, security_block, 1);
        return status;
    }
    status = index_add(vol, security_id, security_block, INFS_OBJECT_SECURITY);
    if (status != INFS_STATUS_OK) {
        free_run(vol, security_block, 1);
        return status;
    }

    memcpy(attributes->security_object_id, security_id, 16);
    touch_ctime(vol, attributes);
    status = write_object(vol, owner.block, owner_block);
    if (status != INFS_STATUS_OK)
        return status;
    if (!id_is_zero(old_id)) {
        status = security_object_destroy_by_id(vol, old_id);
        if (status != INFS_STATUS_OK)
            return status;
    }
    return finish_mutation(vol, (uint64_t)INFS_BLOCK_SIZE * 5u);
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
    struct infs_lookup security;
    status = index_find(vol, attributes->security_object_id, &security);
    if (status != INFS_STATUS_OK || security.type != INFS_OBJECT_SECURITY)
        return INFS_STATUS_CORRUPT;
    uint8_t object[INFS_BLOCK_SIZE];
    status = read_object(vol, security.block, object);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_object_header_disk *header =
        (struct infs_object_header_disk *)object;
    if (memcmp(header->parent_id, owner.object_id, 16) != 0)
        return INFS_STATUS_CORRUPT;
    struct infs_security_payload_disk *payload =
        (struct infs_security_payload_disk *)(header + 1);
    size_t pc = infs_le16_to_cpu(payload->principal_count);
    size_t ac = infs_le16_to_cpu(payload->ace_count);
    struct infs_security_principal *principals =
        calloc(pc ? pc : 1u, sizeof(*principals));
    struct infs_security_ace *aces =
        calloc(ac ? ac : 1u, sizeof(*aces));
    if (!principals || !aces) {
        free(principals);
        free(aces);
        return INFS_STATUS_NO_MEMORY;
    }
    struct infs_security_principal_disk *pd =
        (struct infs_security_principal_disk *)(payload + 1);
    struct infs_security_ace_disk *ad =
        (struct infs_security_ace_disk *)((uint8_t *)pd +
            infs_le32_to_cpu(payload->principal_bytes));
    for (size_t i = 0; i < pc; ++i) {
        memcpy(principals[i].principal_id, pd[i].principal_id, 16);
        principals[i].kind = infs_le16_to_cpu(pd[i].kind);
        principals[i].binding_type = infs_le16_to_cpu(pd[i].binding_type);
        principals[i].binding_size = infs_le16_to_cpu(pd[i].binding_size);
        principals[i].flags = infs_le16_to_cpu(pd[i].flags);
        memcpy(principals[i].binding, pd[i].binding,
               principals[i].binding_size);
    }
    for (size_t i = 0; i < ac; ++i) {
        memcpy(aces[i].principal_id, ad[i].principal_id, 16);
        aces[i].rights = infs_le64_to_cpu(ad[i].rights);
        aces[i].disposition = infs_le16_to_cpu(ad[i].disposition);
        aces[i].flags = infs_le16_to_cpu(ad[i].flags);
    }
    descriptor->principals = principals;
    descriptor->principal_count = pc;
    descriptor->aces = aces;
    descriptor->ace_count = ac;
    return INFS_STATUS_OK;
}

void infs_free_security_descriptor(struct infs_security_descriptor *descriptor)
{
    if (!descriptor)
        return;
    free(descriptor->principals);
    free(descriptor->aces);
    memset(descriptor, 0, sizeof(*descriptor));
}
