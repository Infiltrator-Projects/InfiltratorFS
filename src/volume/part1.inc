// SPDX-License-Identifier: GPL-3.0-or-later
static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
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
    if (!vol || block_no >= le64toh(vol->sb.total_blocks)) {
        errno = EINVAL;
        return -1;
    }
    return infs_pwrite_full(vol->fd, block, INFS_BLOCK_SIZE,
                            (off_t)(block_no * INFS_BLOCK_SIZE));
}

static int read_block(struct infs_volume *vol, uint64_t block_no,
                      uint8_t block[INFS_BLOCK_SIZE])
{
    if (!vol || block_no >= le64toh(vol->sb.total_blocks)) {
        errno = EINVAL;
        return -1;
    }
    return infs_pread_full(vol->fd, block, INFS_BLOCK_SIZE,
                           (off_t)(block_no * INFS_BLOCK_SIZE));
}

static int read_object(struct infs_volume *vol, uint64_t block_no,
                       uint8_t block[INFS_BLOCK_SIZE])
{
    if (read_block(vol, block_no, block) != 0)
        return -1;
    if (!infs_validate_object_block(block)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int write_object(struct infs_volume *vol, uint64_t block_no,
                        uint8_t block[INFS_BLOCK_SIZE])
{
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)block;
    hdr->generation = htole64(le64toh(vol->sb.generation) + 1u);
    if (infs_object_finalize(block) != 0)
        return -1;
    return write_block(vol, block_no, block);
}

static int zero_blocks(struct infs_volume *vol, uint64_t start, uint64_t count)
{
    uint8_t zero[INFS_BLOCK_SIZE] = {0};
    for (uint64_t i = 0; i < count; ++i) {
        if (write_block(vol, start + i, zero) != 0)
            return -1;
    }
    return 0;
}

static int alloc_run(struct infs_volume *vol, uint64_t wanted,
                     uint64_t *start_out, uint64_t *count_out)
{
    if (!vol->writable) {
        errno = EROFS;
        return -1;
    }
    if (wanted == 0 || le64toh(vol->sb.free_blocks) == 0) {
        errno = ENOSPC;
        return -1;
    }

    uint64_t total = le64toh(vol->sb.total_blocks);
    uint64_t bitmap_end = le64toh(vol->sb.bitmap_start_block) +
                          le64toh(vol->sb.bitmap_block_count);
    uint64_t best_start = 0;
    uint64_t best_count = 0;

    for (uint64_t b = bitmap_end; b < total; ) {
        while (b < total && bitmap_get(vol->bitmap, b))
            ++b;
        if (b == total)
            break;
        uint64_t start = b;
        while (b < total && !bitmap_get(vol->bitmap, b) && b - start < wanted)
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

    if (best_count == 0) {
        errno = ENOSPC;
        return -1;
    }

    for (uint64_t b = best_start; b < best_start + best_count; ++b)
        bitmap_set(vol->bitmap, b, 1);
    vol->sb.free_blocks = htole64(le64toh(vol->sb.free_blocks) - best_count);
    if (zero_blocks(vol, best_start, best_count) != 0) {
        for (uint64_t b = best_start; b < best_start + best_count; ++b)
            bitmap_set(vol->bitmap, b, 0);
        vol->sb.free_blocks = htole64(le64toh(vol->sb.free_blocks) + best_count);
        return -1;
    }

    *start_out = best_start;
    *count_out = best_count;
    return 0;
}

static void free_run(struct infs_volume *vol, uint64_t start, uint64_t count)
{
    uint64_t total = le64toh(vol->sb.total_blocks);
    for (uint64_t b = start; b < start + count && b < total; ++b) {
        if (bitmap_get(vol->bitmap, b)) {
            bitmap_set(vol->bitmap, b, 0);
            vol->sb.free_blocks = htole64(le64toh(vol->sb.free_blocks) + 1u);
        }
    }
}

static int index_load(struct infs_volume *vol, uint8_t block[INFS_BLOCK_SIZE],
                      struct infs_index_payload_disk **payload_out)
{
    uint64_t index_block = le64toh(vol->sb.object_index_block);
    if (read_object(vol, index_block, block) != 0)
        return -1;
    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
    if (le16toh(hdr->object_type) != INFS_OBJECT_INDEX ||
        le32toh(hdr->payload_size) < sizeof(struct infs_index_payload_disk)) {
        errno = EIO;
        return -1;
    }
    struct infs_index_payload_disk *payload =
        (struct infs_index_payload_disk *)(block + sizeof(*hdr));
    uint32_t count = le32toh(payload->entry_count);
    size_t need = sizeof(*payload) + (size_t)count * sizeof(struct infs_index_entry_disk);
    if (need > le32toh(hdr->payload_size) ||
        need > INFS_BLOCK_SIZE - sizeof(*hdr)) {
        errno = EIO;
        return -1;
    }
    if (payload_out)
        *payload_out = payload;
    return 0;
}

static int index_find(struct infs_volume *vol, const uint8_t id[16],
                      struct infs_lookup *out)
{
    if (memcmp(id, vol->sb.root_object_id, 16) == 0) {
        if (out) {
            memcpy(out->object_id, id, 16);
            out->block = le64toh(vol->sb.root_object_block);
            out->type = INFS_OBJECT_DIRECTORY;
        }
        return 0;
    }

    uint8_t block[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    if (index_load(vol, block, &payload) != 0)
        return -1;
    uint32_t count = le32toh(payload->entry_count);
    struct infs_index_entry_disk *entries =
        (struct infs_index_entry_disk *)(payload + 1);
    for (uint32_t i = 0; i < count; ++i) {
        if (memcmp(entries[i].object_id, id, 16) == 0) {
            uint64_t obj_block = le64toh(entries[i].object_block);
            if (obj_block >= le64toh(vol->sb.total_blocks) ||
                !bitmap_get(vol->bitmap, obj_block)) {
                errno = EIO;
                return -1;
            }
            if (out) {
                memcpy(out->object_id, id, 16);
                out->block = obj_block;
                out->type = le16toh(entries[i].object_type);
            }
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}

static int index_add(struct infs_volume *vol, const uint8_t id[16],
                     uint64_t object_block, uint16_t type)
{
    struct infs_lookup existing;
    if (index_find(vol, id, &existing) == 0) {
        errno = EEXIST;
        return -1;
    }
    if (errno != ENOENT)
        return -1;

    uint8_t block[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    if (index_load(vol, block, &payload) != 0)
        return -1;
    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
    uint32_t count = le32toh(payload->entry_count);
    size_t new_size = sizeof(*payload) + (size_t)(count + 1u) * sizeof(struct infs_index_entry_disk);
    if (new_size > INFS_BLOCK_SIZE - sizeof(*hdr)) {
        errno = ENOSPC;
        return -1;
    }
    struct infs_index_entry_disk *entries =
        (struct infs_index_entry_disk *)(payload + 1);
    struct infs_index_entry_disk *e = &entries[count];
    memset(e, 0, sizeof(*e));
    memcpy(e->object_id, id, 16);
    e->object_block = htole64(object_block);
    e->object_type = htole16(type);
    payload->entry_count = htole32(count + 1u);
    hdr->payload_size = htole32((uint32_t)new_size);
    return write_object(vol, le64toh(vol->sb.object_index_block), block);
}

static int index_remove(struct infs_volume *vol, const uint8_t id[16])
{
    uint8_t block[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    if (index_load(vol, block, &payload) != 0)
        return -1;
    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
    uint32_t count = le32toh(payload->entry_count);
    struct infs_index_entry_disk *entries =
        (struct infs_index_entry_disk *)(payload + 1);
    for (uint32_t i = 0; i < count; ++i) {
        if (memcmp(entries[i].object_id, id, 16) == 0) {
            if (i + 1u < count)
                memmove(&entries[i], &entries[i + 1u],
                        (size_t)(count - i - 1u) * sizeof(*entries));
            memset(&entries[count - 1u], 0, sizeof(*entries));
            payload->entry_count = htole32(count - 1u);
            hdr->payload_size = htole32((uint32_t)(sizeof(*payload) +
                (size_t)(count - 1u) * sizeof(*entries)));
            return write_object(vol, le64toh(vol->sb.object_index_block), block);
        }
    }
    errno = ENOENT;
    return -1;
}
