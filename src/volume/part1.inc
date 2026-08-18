// SPDX-License-Identifier: GPL-3.0-or-later
static int64_t now_ns(const struct infs_volume *vol)
{
    int64_t value = 0;
    (void)infs_storage_current_time_ns(&vol->storage, &value);
    return value;
}

static size_t align8(size_t n)
{
    return (n + (INFS_DIRENT_ALIGN - 1u)) & ~(size_t)(INFS_DIRENT_ALIGN - 1u);
}

static int id_is_zero(const uint8_t id[16])
{
    uint8_t v = 0;
    for (unsigned i = 0; i < 16; ++i)
        v |= id[i];
    return v == 0;
}

static int bitmap_get(const uint8_t *bitmap, uint64_t block)
{
    return (bitmap[block >> 3] >> (block & 7u)) & 1u;
}

static void bitmap_set(uint8_t *bitmap, uint64_t block, int allocated)
{
    uint8_t mask = (uint8_t)(1u << (block & 7u));
    if (allocated)
        bitmap[block >> 3] |= mask;
    else
        bitmap[block >> 3] &= (uint8_t)~mask;
}

static int write_block(struct infs_volume *vol, uint64_t block_no,
                       const uint8_t block[INFS_BLOCK_SIZE])
{
    if (!vol || block_no >= infs_le64_to_cpu(vol->sb.total_blocks))
        return INFS_STATUS_INVALID_ARGUMENT;
    return infs_storage_write(&vol->storage, block_no * INFS_BLOCK_SIZE,
                              block, INFS_BLOCK_SIZE);
}

static int read_block(struct infs_volume *vol, uint64_t block_no,
                      uint8_t block[INFS_BLOCK_SIZE])
{
    if (!vol || block_no >= infs_le64_to_cpu(vol->sb.total_blocks))
        return INFS_STATUS_INVALID_ARGUMENT;
    return infs_storage_read(&vol->storage, block_no * INFS_BLOCK_SIZE,
                             block, INFS_BLOCK_SIZE);
}

static int read_object(struct infs_volume *vol, uint64_t block_no,
                       uint8_t block[INFS_BLOCK_SIZE])
{
    infs_status status = read_block(vol, block_no, block);
    if (status != INFS_STATUS_OK)
        return status;
    if (!infs_validate_object_block(block))
        return INFS_STATUS_CORRUPT;
    const struct infs_object_header_disk *hdr =
        (const struct infs_object_header_disk *)block;
    if (!vol->tx_active &&
        infs_le64_to_cpu(hdr->generation) >
            infs_le64_to_cpu(vol->sb.generation))
        return INFS_STATUS_CORRUPT;
    return INFS_STATUS_OK;
}

static int alloc_run(struct infs_volume *vol, uint64_t wanted,
                     uint64_t *start_out, uint64_t *count_out);
static void free_run(struct infs_volume *vol, uint64_t start, uint64_t count);
static int index_repoint(struct infs_volume *vol, const uint8_t id[16],
                         uint64_t object_block, uint16_t type);

static int tx_begin(struct infs_volume *vol)
{
    if (!vol || !vol->writable)
        return INFS_STATUS_READ_ONLY;
    if (vol->tx_active)
        return vol->tx_error;

    vol->tx_base_bitmap = malloc(vol->bitmap_bytes);
    if (!vol->tx_base_bitmap)
        return INFS_STATUS_NO_MEMORY;
    memcpy(vol->tx_base_bitmap, vol->bitmap, vol->bitmap_bytes);
    memcpy(&vol->tx_base_sb, &vol->sb, sizeof(vol->sb));
    vol->tx_deferred_count = 0;
    vol->tx_error = INFS_STATUS_OK;
    vol->tx_active = 1;
    return INFS_STATUS_OK;
}

static void tx_abort(struct infs_volume *vol)
{
    if (!vol || !vol->tx_active)
        return;
    memcpy(&vol->sb, &vol->tx_base_sb, sizeof(vol->sb));
    memcpy(vol->bitmap, vol->tx_base_bitmap, vol->bitmap_bytes);
    free(vol->tx_base_bitmap);
    vol->tx_base_bitmap = NULL;
    vol->tx_deferred_count = 0;
    vol->tx_error = INFS_STATUS_OK;
    vol->tx_active = 0;
}

static int tx_defer_range(struct infs_volume *vol, uint64_t start, uint64_t count)
{
    if (!count)
        return INFS_STATUS_OK;
    if (!vol || !vol->tx_active)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (vol->tx_error != INFS_STATUS_OK)
        return vol->tx_error;

    if (vol->tx_deferred_count) {
        struct infs_deferred_range *last =
            &vol->tx_deferred[vol->tx_deferred_count - 1u];
        if (last->start <= UINT64_MAX - last->count &&
            last->start + last->count == start) {
            if (count > UINT64_MAX - last->count) {
                vol->tx_error = INFS_STATUS_OVERFLOW;
                return vol->tx_error;
            }
            last->count += count;
            return INFS_STATUS_OK;
        }
    }

    if (vol->tx_deferred_count == vol->tx_deferred_capacity) {
        size_t next = vol->tx_deferred_capacity ?
            vol->tx_deferred_capacity * 2u : 16u;
        if (next < vol->tx_deferred_capacity ||
            next > SIZE_MAX / sizeof(*vol->tx_deferred)) {
            vol->tx_error = INFS_STATUS_NO_MEMORY;
            return vol->tx_error;
        }
        struct infs_deferred_range *grown =
            realloc(vol->tx_deferred, next * sizeof(*grown));
        if (!grown) {
            vol->tx_error = INFS_STATUS_NO_MEMORY;
            return vol->tx_error;
        }
        vol->tx_deferred = grown;
        vol->tx_deferred_capacity = next;
    }
    vol->tx_deferred[vol->tx_deferred_count].start = start;
    vol->tx_deferred[vol->tx_deferred_count].count = count;
    ++vol->tx_deferred_count;
    return INFS_STATUS_OK;
}

static uint64_t bitmap_free_count(const struct infs_volume *vol)
{
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    uint64_t free_count = 0;
    for (uint64_t b = 0; b < total; ++b)
        if (!bitmap_get(vol->bitmap, b))
            ++free_count;
    return free_count;
}

static int zero_blocks(struct infs_volume *vol, uint64_t start, uint64_t count)
{
    uint8_t zero[INFS_BLOCK_SIZE] = {0};
    for (uint64_t i = 0; i < count; ++i) {
        infs_status status = write_block(vol, start + i, zero);
        if (status != INFS_STATUS_OK)
            return status;
    }
    return INFS_STATUS_OK;
}

static int alloc_run(struct infs_volume *vol, uint64_t wanted,
                     uint64_t *start_out, uint64_t *count_out)
{
    if (!vol || !start_out || !count_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    infs_status status = tx_begin(vol);
    if (status != INFS_STATUS_OK)
        return status;
    if (wanted == 0 || infs_le64_to_cpu(vol->sb.free_blocks) == 0)
        return INFS_STATUS_NO_SPACE;

    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    uint64_t best_start = 0;
    uint64_t best_count = 0;

    for (uint64_t b = 1; b < total; ) {
        while (b < total && bitmap_get(vol->bitmap, b))
            ++b;
        if (b == total)
            break;
        uint64_t start = b;
        while (b < total && !bitmap_get(vol->bitmap, b) &&
               b - start < wanted)
            ++b;
        uint64_t len = b - start;
        if (len > best_count) {
            best_start = start;
            best_count = len;
        }
        if (len == wanted)
            break;
        while (b < total && !bitmap_get(vol->bitmap, b))
            ++b;
    }

    if (best_count == 0)
        return INFS_STATUS_NO_SPACE;

    for (uint64_t b = best_start; b < best_start + best_count; ++b)
        bitmap_set(vol->bitmap, b, 1);
    vol->sb.free_blocks = infs_cpu_to_le64(
        infs_le64_to_cpu(vol->sb.free_blocks) - best_count);
    status = zero_blocks(vol, best_start, best_count);
    if (status != INFS_STATUS_OK) {
        for (uint64_t b = best_start; b < best_start + best_count; ++b)
            bitmap_set(vol->bitmap, b, 0);
        vol->sb.free_blocks = infs_cpu_to_le64(
            infs_le64_to_cpu(vol->sb.free_blocks) + best_count);
        return status;
    }

    *start_out = best_start;
    *count_out = best_count;
    return INFS_STATUS_OK;
}

static int alloc_exact_run(struct infs_volume *vol, uint64_t wanted,
                           uint64_t *start_out)
{
    if (!wanted || !start_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    infs_status status = tx_begin(vol);
    if (status != INFS_STATUS_OK)
        return status;
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    for (uint64_t start = 1; start + wanted <= total; ) {
        while (start < total && bitmap_get(vol->bitmap, start))
            ++start;
        if (start + wanted > total)
            break;
        uint64_t n = 0;
        while (n < wanted && !bitmap_get(vol->bitmap, start + n))
            ++n;
        if (n == wanted) {
            for (uint64_t i = 0; i < wanted; ++i)
                bitmap_set(vol->bitmap, start + i, 1);
            vol->sb.free_blocks = infs_cpu_to_le64(
                infs_le64_to_cpu(vol->sb.free_blocks) - wanted);
            status = zero_blocks(vol, start, wanted);
            if (status != INFS_STATUS_OK) {
                for (uint64_t i = 0; i < wanted; ++i)
                    bitmap_set(vol->bitmap, start + i, 0);
                vol->sb.free_blocks = infs_cpu_to_le64(
                    infs_le64_to_cpu(vol->sb.free_blocks) + wanted);
                return status;
            }
            *start_out = start;
            return INFS_STATUS_OK;
        }
        start += n ? n : 1u;
    }
    return INFS_STATUS_NO_SPACE;
}

static void free_run(struct infs_volume *vol, uint64_t start, uint64_t count)
{
    if (!vol || !count)
        return;
    infs_status status = tx_begin(vol);
    if (status != INFS_STATUS_OK) {
        if (vol)
            vol->tx_error = status;
        return;
    }
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    uint64_t end = start + count;
    if (end < start || end > total)
        end = total;

    uint64_t b = start;
    while (b < end) {
        if (!bitmap_get(vol->bitmap, b)) {
            ++b;
            continue;
        }
        if (!bitmap_get(vol->tx_base_bitmap, b)) {
            bitmap_set(vol->bitmap, b, 0);
            vol->sb.free_blocks = infs_cpu_to_le64(
                infs_le64_to_cpu(vol->sb.free_blocks) + 1u);
            ++b;
            continue;
        }

        uint64_t run = b;
        while (b < end && bitmap_get(vol->bitmap, b) &&
               bitmap_get(vol->tx_base_bitmap, b))
            ++b;
        status = tx_defer_range(vol, run, b - run);
        if (status != INFS_STATUS_OK)
            return;
    }
}

static int write_object(struct infs_volume *vol, uint64_t block_no,
                        uint8_t block[INFS_BLOCK_SIZE])
{
    infs_status status = tx_begin(vol);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)block;
    hdr->generation = infs_cpu_to_le64(
        infs_le64_to_cpu(vol->tx_base_sb.generation) + 1u);
    status = infs_object_finalize(block);
    if (status != INFS_STATUS_OK)
        return status;

    uint64_t new_block = 0, got = 0;
    status = alloc_run(vol, 1, &new_block, &got);
    if (status != INFS_STATUS_OK)
        return status;
    if (got != 1)
        return INFS_STATUS_CORRUPT;

    status = write_block(vol, new_block, block);
    if (status != INFS_STATUS_OK) {
        free_run(vol, new_block, 1);
        return status;
    }

    uint16_t type = infs_le16_to_cpu(hdr->object_type);
    if (type == INFS_OBJECT_INDEX) {
        vol->sb.object_index_block = infs_cpu_to_le64(new_block);
        free_run(vol, block_no, 1);
        return vol->tx_error;
    }

    status = index_repoint(vol, hdr->object_id, new_block, type);
    if (status != INFS_STATUS_OK) {
        free_run(vol, new_block, 1);
        return status;
    }
    if (memcmp(hdr->object_id, vol->sb.root_object_id, 16) == 0)
        vol->sb.root_object_block = infs_cpu_to_le64(new_block);
    free_run(vol, block_no, 1);
    return vol->tx_error;
}

static int index_load(struct infs_volume *vol, uint8_t block[INFS_BLOCK_SIZE],
                      struct infs_index_payload_disk **payload_out)
{
    uint64_t index_block = infs_le64_to_cpu(vol->sb.object_index_block);
    infs_status status = read_object(vol, index_block, block);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)block;
    if (infs_le16_to_cpu(hdr->object_type) != INFS_OBJECT_INDEX ||
        infs_le32_to_cpu(hdr->payload_size) <
            sizeof(struct infs_index_payload_disk))
        return INFS_STATUS_CORRUPT;
    struct infs_index_payload_disk *payload =
        (struct infs_index_payload_disk *)(block + sizeof(*hdr));
    uint32_t count = infs_le32_to_cpu(payload->entry_count);
    size_t need = sizeof(*payload) +
        (size_t)count * sizeof(struct infs_index_entry_disk);
    if (need > infs_le32_to_cpu(hdr->payload_size) ||
        need > INFS_BLOCK_SIZE - sizeof(*hdr))
        return INFS_STATUS_CORRUPT;
    if (payload_out)
        *payload_out = payload;
    return INFS_STATUS_OK;
}

static int index_find(struct infs_volume *vol, const uint8_t id[16],
                      struct infs_lookup *out)
{
    if (memcmp(id, vol->sb.root_object_id, 16) == 0) {
        if (out) {
            memcpy(out->object_id, id, 16);
            out->block = infs_le64_to_cpu(vol->sb.root_object_block);
            out->type = INFS_OBJECT_DIRECTORY;
        }
        return INFS_STATUS_OK;
    }

    uint8_t block[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    infs_status status = index_load(vol, block, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    uint32_t count = infs_le32_to_cpu(payload->entry_count);
    struct infs_index_entry_disk *entries =
        (struct infs_index_entry_disk *)(payload + 1);
    for (uint32_t i = 0; i < count; ++i) {
        if (memcmp(entries[i].object_id, id, 16) == 0) {
            uint64_t obj_block = infs_le64_to_cpu(entries[i].object_block);
            if (obj_block >= infs_le64_to_cpu(vol->sb.total_blocks) ||
                !bitmap_get(vol->bitmap, obj_block))
                return INFS_STATUS_CORRUPT;
            if (out) {
                memcpy(out->object_id, id, 16);
                out->block = obj_block;
                out->type = infs_le16_to_cpu(entries[i].object_type);
            }
            return INFS_STATUS_OK;
        }
    }
    return INFS_STATUS_NOT_FOUND;
}

static int index_repoint(struct infs_volume *vol, const uint8_t id[16],
                         uint64_t object_block, uint16_t type)
{
    uint8_t block[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    uint64_t index_block = infs_le64_to_cpu(vol->sb.object_index_block);
    infs_status status = index_load(vol, block, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    uint32_t count = infs_le32_to_cpu(payload->entry_count);
    struct infs_index_entry_disk *entries =
        (struct infs_index_entry_disk *)(payload + 1);
    for (uint32_t i = 0; i < count; ++i) {
        if (memcmp(entries[i].object_id, id, 16) == 0) {
            entries[i].object_block = infs_cpu_to_le64(object_block);
            entries[i].object_type = infs_cpu_to_le16(type);
            return write_object(vol, index_block, block);
        }
    }
    return INFS_STATUS_NOT_FOUND;
}

static int index_add(struct infs_volume *vol, const uint8_t id[16],
                     uint64_t object_block, uint16_t type)
{
    struct infs_lookup existing;
    infs_status status = index_find(vol, id, &existing);
    if (status == INFS_STATUS_OK)
        return INFS_STATUS_ALREADY_EXISTS;
    if (status != INFS_STATUS_NOT_FOUND)
        return status;

    uint8_t block[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    status = index_load(vol, block, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)block;
    uint32_t count = infs_le32_to_cpu(payload->entry_count);
    size_t new_size = sizeof(*payload) +
        (size_t)(count + 1u) * sizeof(struct infs_index_entry_disk);
    if (new_size > INFS_BLOCK_SIZE - sizeof(*hdr))
        return INFS_STATUS_NO_SPACE;
    struct infs_index_entry_disk *entries =
        (struct infs_index_entry_disk *)(payload + 1);
    struct infs_index_entry_disk *e = &entries[count];
    memset(e, 0, sizeof(*e));
    memcpy(e->object_id, id, 16);
    e->object_block = infs_cpu_to_le64(object_block);
    e->object_type = infs_cpu_to_le16(type);
    payload->entry_count = infs_cpu_to_le32(count + 1u);
    hdr->payload_size = infs_cpu_to_le32((uint32_t)new_size);
    return write_object(vol,
                        infs_le64_to_cpu(vol->sb.object_index_block), block);
}

static int index_remove(struct infs_volume *vol, const uint8_t id[16])
{
    uint8_t block[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    infs_status status = index_load(vol, block, &payload);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)block;
    uint32_t count = infs_le32_to_cpu(payload->entry_count);
    struct infs_index_entry_disk *entries =
        (struct infs_index_entry_disk *)(payload + 1);
    for (uint32_t i = 0; i < count; ++i) {
        if (memcmp(entries[i].object_id, id, 16) == 0) {
            if (i + 1u < count)
                memmove(&entries[i], &entries[i + 1u],
                        (size_t)(count - i - 1u) * sizeof(*entries));
            memset(&entries[count - 1u], 0, sizeof(*entries));
            payload->entry_count = infs_cpu_to_le32(count - 1u);
            hdr->payload_size = infs_cpu_to_le32((uint32_t)(
                sizeof(*payload) +
                (size_t)(count - 1u) * sizeof(*entries)));
            return write_object(
                vol, infs_le64_to_cpu(vol->sb.object_index_block), block);
        }
    }
    return INFS_STATUS_NOT_FOUND;
}
