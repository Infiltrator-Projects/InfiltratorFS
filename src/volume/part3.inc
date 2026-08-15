// SPDX-License-Identifier: GPL-3.0-or-later
static int file_validate(uint8_t block[INFS_BLOCK_SIZE],
                         struct infs_file_payload_disk **payload_out,
                         struct infs_extent_disk **extents_out)
{
    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
    if (!infs_validate_object_block(block) ||
        le16toh(hdr->object_type) != INFS_OBJECT_FILE ||
        le32toh(hdr->payload_size) < sizeof(struct infs_file_payload_disk)) {
        errno = EIO;
        return -1;
    }
    struct infs_file_payload_disk *p =
        (struct infs_file_payload_disk *)(block + sizeof(*hdr));
    uint32_t count = le32toh(p->extent_count);
    size_t need = sizeof(*p) + (size_t)count * sizeof(struct infs_extent_disk);
    if (need > le32toh(hdr->payload_size) || need > INFS_BLOCK_SIZE - sizeof(*hdr)) {
        errno = EIO;
        return -1;
    }
    struct infs_extent_disk *ext = (struct infs_extent_disk *)(p + 1);
    uint64_t next_logical = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t logical = le64toh(ext[i].logical_block);
        uint64_t physical = le64toh(ext[i].physical_block);
        uint32_t blocks = le32toh(ext[i].block_count);
        (void)physical; /* physical bounds are checked with volume context */
        if (blocks == 0 || logical != next_logical) {
            errno = EIO;
            return -1;
        }
        next_logical += blocks;
    }
    if (payload_out)
        *payload_out = p;
    if (extents_out)
        *extents_out = ext;
    return 0;
}

static int file_validate_volume(struct infs_volume *vol, uint8_t block[INFS_BLOCK_SIZE],
                                struct infs_file_payload_disk **payload_out,
                                struct infs_extent_disk **extents_out)
{
    if (file_validate(block, payload_out, extents_out) != 0)
        return -1;
    struct infs_file_payload_disk *p = *payload_out;
    struct infs_extent_disk *ext = *extents_out;
    uint32_t count = le32toh(p->extent_count);
    uint64_t total = le64toh(vol->sb.total_blocks);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t physical = le64toh(ext[i].physical_block);
        uint32_t blocks = le32toh(ext[i].block_count);
        if (physical >= total || blocks > total - physical) {
            errno = EIO;
            return -1;
        }
        for (uint32_t j = 0; j < blocks; ++j) {
            if (!bitmap_get(vol->bitmap, physical + j)) {
                errno = EIO;
                return -1;
            }
        }
    }
    return 0;
}

static uint64_t file_allocated_blocks(const struct infs_file_payload_disk *p,
                                      const struct infs_extent_disk *ext)
{
    uint32_t count = le32toh(p->extent_count);
    if (!count)
        return 0;
    const struct infs_extent_disk *last = &ext[count - 1u];
    return le64toh(last->logical_block) + le32toh(last->block_count);
}

static int file_ensure_blocks(struct infs_volume *vol, uint8_t block[INFS_BLOCK_SIZE],
                              uint64_t needed)
{
    struct infs_file_payload_disk *p;
    struct infs_extent_disk *ext;
    if (file_validate_volume(vol, block, &p, &ext) != 0)
        return -1;
    uint64_t have = file_allocated_blocks(p, ext);
    if (needed <= have)
        return 0;

    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
    uint32_t count = le32toh(p->extent_count);
    const size_t max_extents = (INFS_BLOCK_SIZE - sizeof(*hdr) - sizeof(*p)) /
                               sizeof(struct infs_extent_disk);

    while (have < needed) {
        uint64_t start, got;
        if (alloc_run(vol, needed - have, &start, &got) != 0)
            return -1;
        if (count &&
            le64toh(ext[count - 1u].physical_block) + le32toh(ext[count - 1u].block_count) == start) {
            uint64_t new_count = (uint64_t)le32toh(ext[count - 1u].block_count) + got;
            if (new_count > UINT32_MAX) {
                free_run(vol, start, got);
                errno = EFBIG;
                return -1;
            }
            ext[count - 1u].block_count = htole32((uint32_t)new_count);
        } else {
            if (count >= max_extents) {
                free_run(vol, start, got);
                errno = EFBIG;
                return -1;
            }
            ext[count].logical_block = htole64(have);
            ext[count].physical_block = htole64(start);
            ext[count].block_count = htole32((uint32_t)got);
            ext[count].flags = htole32(INFS_EXTENT_NORMAL);
            ++count;
            p->extent_count = htole32(count);
        }
        have += got;
    }
    hdr->payload_size = htole32((uint32_t)(sizeof(*p) +
        (size_t)count * sizeof(struct infs_extent_disk)));
    return 0;
}

static int file_map_block(const struct infs_file_payload_disk *p,
                          const struct infs_extent_disk *ext,
                          uint64_t logical, uint64_t *physical)
{
    uint32_t count = le32toh(p->extent_count);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t l = le64toh(ext[i].logical_block);
        uint32_t n = le32toh(ext[i].block_count);
        if (logical >= l && logical < l + n) {
            *physical = le64toh(ext[i].physical_block) + (logical - l);
            return 0;
        }
    }
    errno = EIO;
    return -1;
}

static int validate_index_entries(struct infs_volume *vol)
{
    uint8_t index_block[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *payload;
    if (index_load(vol, index_block, &payload) != 0)
        return -1;
    uint32_t count = le32toh(payload->entry_count);
    struct infs_index_entry_disk *entries =
        (struct infs_index_entry_disk *)(payload + 1);
    uint64_t total = le64toh(vol->sb.total_blocks);
    int root_seen = 0;

    for (uint32_t i = 0; i < count; ++i) {
        uint64_t b = le64toh(entries[i].object_block);
        uint16_t type = le16toh(entries[i].object_type);
        if (b >= total || !bitmap_get(vol->bitmap, b) ||
            (type != INFS_OBJECT_DIRECTORY && type != INFS_OBJECT_FILE)) {
            errno = EIO;
            return -1;
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (memcmp(entries[i].object_id, entries[j].object_id, 16) == 0) {
                errno = EIO;
                return -1;
            }
        }
        uint8_t object[INFS_BLOCK_SIZE];
        if (read_object(vol, b, object) != 0)
            return -1;
        struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)object;
        if (memcmp(hdr->object_id, entries[i].object_id, 16) != 0 ||
            le16toh(hdr->object_type) != type) {
            errno = EIO;
            return -1;
        }
        if (memcmp(entries[i].object_id, vol->sb.root_object_id, 16) == 0) {
            if (b != le64toh(vol->sb.root_object_block) || type != INFS_OBJECT_DIRECTORY)
                return errno = EIO, -1;
            ++root_seen;
        }
    }
    if (root_seen != 1) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int checkpoint_matches(struct infs_volume *vol, uint64_t block_no,
                              const struct infs_superblock_disk *best)
{
    uint8_t raw[INFS_BLOCK_SIZE];
    if (read_block(vol, block_no, raw) != 0)
        return 0;
    struct infs_superblock_disk sb;
    if (infs_decode_superblock(raw, &sb) != 0)
        return 0;
    return memcmp(&sb, best, sizeof(sb)) == 0;
}

static int heal_checkpoints(struct infs_volume *vol)
{
    int need_heal = 0;
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        uint64_t b = le64toh(vol->sb.checkpoint_block[i]);
        if (!checkpoint_matches(vol, b, &vol->sb)) {
            need_heal = 1;
            break;
        }
    }
    if (!need_heal)
        return 0;

    uint8_t raw[INFS_BLOCK_SIZE];
    if (infs_encode_superblock(raw, &vol->sb) != 0)
        return -1;
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        uint64_t b = le64toh(vol->sb.checkpoint_block[i]);
        if (write_block(vol, b, raw) != 0)
            return -1;
    }
    return fsync(vol->fd);
}

static void phase2_failpoint(const char *stage)
{
    const char *requested = getenv("INFS_TEST_CRASH_STAGE");
    if (requested && strcmp(requested, stage) == 0)
        _exit(97);
}

int infs_volume_open(struct infs_volume *vol, const char *path, int writable)
{
    if (!vol || !path) {
        errno = EINVAL;
        return -1;
    }
    memset(vol, 0, sizeof(*vol));
    vol->fd = -1;
    vol->writable = writable != 0;
    vol->fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (vol->fd < 0)
        return -1;
    int is_block = 0;
    if (infs_get_size_bytes(vol->fd, &vol->size_bytes, &is_block) != 0)
        goto fail;
    (void)is_block;
    if (infs_read_best_superblock(vol->fd, vol->size_bytes, &vol->sb, NULL) != 0)
        goto fail;
    if (le16toh(vol->sb.format_minor) != INFS_FORMAT_MINOR) {
        errno = EOPNOTSUPP;
        goto fail;
    }

    uint64_t bitmap_start = le64toh(vol->sb.bitmap_start_block);
    uint64_t bitmap_blocks = le64toh(vol->sb.bitmap_block_count);
    uint64_t total = le64toh(vol->sb.total_blocks);
    if (bitmap_start == 0 || bitmap_blocks == 0 || bitmap_start >= total ||
        bitmap_blocks > total - bitmap_start ||
        bitmap_blocks > SIZE_MAX / INFS_BLOCK_SIZE) {
        errno = EIO;
        goto fail;
    }
    vol->bitmap_bytes = (size_t)(bitmap_blocks * INFS_BLOCK_SIZE);
    vol->bitmap = malloc(vol->bitmap_bytes);
    if (!vol->bitmap)
        goto fail;
    if (infs_pread_full(vol->fd, vol->bitmap, vol->bitmap_bytes,
                        (off_t)(bitmap_start * INFS_BLOCK_SIZE)) != 0)
        goto fail;

    uint64_t free_count = 0;
    for (uint64_t b = 0; b < total; ++b) {
        if (!bitmap_get(vol->bitmap, b))
            ++free_count;
    }
    if (free_count != le64toh(vol->sb.free_blocks)) {
        errno = EIO;
        goto fail;
    }
    for (uint64_t b = total; b < (uint64_t)vol->bitmap_bytes * 8u; ++b) {
        if (!bitmap_get(vol->bitmap, b)) {
            errno = EIO;
            goto fail;
        }
    }
    for (uint64_t b = bitmap_start; b < bitmap_start + bitmap_blocks; ++b) {
        if (!bitmap_get(vol->bitmap, b)) {
            errno = EIO;
            goto fail;
        }
    }

    uint64_t critical[] = {
        le64toh(vol->sb.checkpoint_block[0]),
        le64toh(vol->sb.checkpoint_block[1]),
        le64toh(vol->sb.checkpoint_block[2]),
        le64toh(vol->sb.object_index_block),
        le64toh(vol->sb.root_object_block)
    };
    for (size_t i = 0; i < sizeof(critical) / sizeof(critical[0]); ++i) {
        if (critical[i] >= total || !bitmap_get(vol->bitmap, critical[i])) {
            errno = EIO;
            goto fail;
        }
    }

    uint8_t object[INFS_BLOCK_SIZE];
    if (read_object(vol, le64toh(vol->sb.root_object_block), object) != 0 ||
        dir_validate(object, NULL) != 0)
        goto fail;
    struct infs_object_header_disk *root_hdr = (struct infs_object_header_disk *)object;
    if (memcmp(root_hdr->object_id, vol->sb.root_object_id, 16) != 0 ||
        !id_is_zero(root_hdr->parent_id)) {
        errno = EIO;
        goto fail;
    }
    if (validate_index_entries(vol) != 0)
        goto fail;
    if (vol->writable && heal_checkpoints(vol) != 0)
        goto fail;
    return 0;

fail:
    {
        int saved = errno;
        free(vol->bitmap);
        vol->bitmap = NULL;
        if (vol->fd >= 0)
            close(vol->fd);
        vol->fd = -1;
        errno = saved;
        return -1;
    }
}

void infs_volume_close(struct infs_volume *vol)
{
    if (!vol)
        return;
    tx_abort(vol);
    free(vol->tx_base_bitmap);
    vol->tx_base_bitmap = NULL;
    free(vol->tx_deferred);
    vol->tx_deferred = NULL;
    vol->tx_deferred_count = 0;
    vol->tx_deferred_capacity = 0;
    free(vol->bitmap);
    vol->bitmap = NULL;
    if (vol->fd >= 0)
        close(vol->fd);
    vol->fd = -1;
}

int infs_volume_sync(struct infs_volume *vol)
{
    if (!vol || vol->fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (!vol->writable)
        return 0;
    if (!vol->tx_active)
        return fsync(vol->fd);

    const uint64_t old_bitmap_start = le64toh(vol->tx_base_sb.bitmap_start_block);
    const uint64_t bitmap_blocks = le64toh(vol->tx_base_sb.bitmap_block_count);
    if (tx_defer_range(vol, old_bitmap_start, bitmap_blocks) != 0)
        return -1;

    uint64_t new_bitmap_start = 0;
    if (alloc_exact_run(vol, bitmap_blocks, &new_bitmap_start) != 0)
        return -1;

    /* Deferred frees remain reserved until every new metadata/data write is on
     * stable storage. They become reusable only in the new bitmap image. */
    for (size_t i = 0; i < vol->tx_deferred_count; ++i) {
        uint64_t start = vol->tx_deferred[i].start;
        uint64_t count = vol->tx_deferred[i].count;
        uint64_t total = le64toh(vol->sb.total_blocks);
        for (uint64_t b = start; b < start + count && b < total; ++b)
            bitmap_set(vol->bitmap, b, 0);
    }
    for (uint64_t i = 0; i < bitmap_blocks; ++i)
        bitmap_set(vol->bitmap, new_bitmap_start + i, 1);
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        bitmap_set(vol->bitmap, le64toh(vol->sb.checkpoint_block[i]), 1);
    for (uint64_t b = le64toh(vol->sb.total_blocks);
         b < (uint64_t)vol->bitmap_bytes * 8u; ++b)
        bitmap_set(vol->bitmap, b, 1);

    vol->sb.bitmap_start_block = htole64(new_bitmap_start);
    vol->sb.generation = htole64(le64toh(vol->tx_base_sb.generation) + 1u);
    vol->sb.free_blocks = htole64(bitmap_free_count(vol));

    /* Ordered transaction protocol:
     *   1. new metadata/data already written to unreachable blocks;
     *   2. flush those blocks;
     *   3. write and flush a new bitmap image;
     *   4. publish one checksummed checkpoint (commit point);
     *   5. replicate that checkpoint to the other physical copies.
     * A crash before step 4 selects the old generation. A crash after step 4
     * selects the new generation and a writable open heals the other copies. */
    if (fsync(vol->fd) != 0)
        return -1;
    phase2_failpoint("before-bitmap");

    if (infs_pwrite_full(vol->fd, vol->bitmap, vol->bitmap_bytes,
                         (off_t)(new_bitmap_start * INFS_BLOCK_SIZE)) != 0)
        return -1;
    if (fsync(vol->fd) != 0)
        return -1;
    phase2_failpoint("after-bitmap");

    uint8_t sb_block[INFS_BLOCK_SIZE];
    if (infs_encode_superblock(sb_block, &vol->sb) != 0)
        return -1;
    uint64_t generation = le64toh(vol->sb.generation);
    unsigned first = (unsigned)(generation % INFS_CHECKPOINT_COUNT);
    uint64_t first_block = le64toh(vol->sb.checkpoint_block[first]);
    if (write_block(vol, first_block, sb_block) != 0)
        return -1;
    if (fsync(vol->fd) != 0)
        return -1;

    /* The transaction is committed from this point onward. */
    free(vol->tx_base_bitmap);
    vol->tx_base_bitmap = NULL;
    vol->tx_deferred_count = 0;
    vol->tx_active = 0;
    phase2_failpoint("after-checkpoint");

    int replica_error = 0;
    int saved_errno = 0;
    for (unsigned n = 1; n < INFS_CHECKPOINT_COUNT; ++n) {
        unsigned i = (first + n) % INFS_CHECKPOINT_COUNT;
        uint64_t b = le64toh(vol->sb.checkpoint_block[i]);
        if (write_block(vol, b, sb_block) != 0) {
            if (!replica_error)
                saved_errno = errno;
            replica_error = 1;
        }
    }
    if (!replica_error && fsync(vol->fd) != 0) {
        saved_errno = errno;
        replica_error = 1;
    }
    if (replica_error) {
        errno = saved_errno ? saved_errno : EIO;
        return -1;
    }
    return 0;
}
