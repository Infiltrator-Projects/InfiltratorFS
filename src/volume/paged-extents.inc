// SPDX-License-Identifier: GPL-3.0-or-later
/* Format 0.12 scalable extent pages. Low-fragmentation files stay in the
 * compact classic representation. When an edit would overflow the file
 * object, its extent vector is promoted to independently checksummed pages.
 * The file object remains the stable head and owns an ordered page-pointer
 * array. */

static int file_has_paged_extents(const uint8_t object[INFS_BLOCK_SIZE])
{
    const struct infs_object_header_disk *hdr =
        (const struct infs_object_header_disk *)object;
    return infs_le16_to_cpu(hdr->object_type) == INFS_OBJECT_FILE &&
           infs_le16_to_cpu(hdr->object_version) == INFS_OBJECT_VERSION_PAGED;
}

static struct infs_extent_head_disk *file_extent_head(
    struct infs_file_payload_disk *file)
{
    return (struct infs_extent_head_disk *)(file + 1);
}

static uint64_t *file_extent_page_pointers(struct infs_file_payload_disk *file)
{
    return (uint64_t *)(file_extent_head(file) + 1);
}

/* The 4016-byte metadata payload has eight spare bytes after 167 24-byte
 * extent records. Use those bytes as a physical next-page link. The file head
 * therefore stores one root pointer rather than a bounded pointer vector. */
static uint64_t extent_page_next_block(const uint8_t block[INFS_BLOCK_SIZE])
{
    const struct infs_metadata_page_disk *page =
        (const struct infs_metadata_page_disk *)block;
    uint32_t count = infs_le32_to_cpu(page->entry_count);
    size_t offset = sizeof(*page) +
        (size_t)count * sizeof(struct infs_extent_disk);
    uint64_t encoded = 0;

    if (!count || count > INFS_EXTENTS_PER_PAGE ||
        offset > INFS_BLOCK_SIZE - sizeof(encoded))
        return 0;
    memcpy(&encoded, block + offset, sizeof(encoded));
    return infs_le64_to_cpu(encoded);
}


static void extent_page_set_next_block(uint8_t block[INFS_BLOCK_SIZE],
                                       uint64_t next)
{
    struct infs_metadata_page_disk *page =
        (struct infs_metadata_page_disk *)block;
    uint32_t count = infs_le32_to_cpu(page->entry_count);
    size_t extent_bytes = (size_t)count * sizeof(struct infs_extent_disk);
    size_t offset = sizeof(*page) + extent_bytes;
    uint64_t encoded = infs_cpu_to_le64(next);

    if (!count || count > INFS_EXTENTS_PER_PAGE ||
        offset > INFS_BLOCK_SIZE - sizeof(encoded))
        return;
    memcpy(block + offset, &encoded, sizeof(encoded));
    page->bytes_used = infs_cpu_to_le32(
        (uint32_t)(extent_bytes + sizeof(encoded)));
}


static int extent_page_validate(struct infs_volume *vol,
                                const uint8_t block[INFS_BLOCK_SIZE],
                                const uint8_t owner_id[16],
                                struct infs_extent_disk **entries_out,
                                uint32_t *count_out)
{
    if (!infs_validate_metadata_page(block, INFS_EXTENT_PAGE_MAGIC, owner_id) ||
        !metadata_page_generation_valid(vol, block))
        return INFS_STATUS_CORRUPT;
    const struct infs_metadata_page_disk *page =
        (const struct infs_metadata_page_disk *)block;
    uint32_t count = infs_le32_to_cpu(page->entry_count);
    uint32_t bytes = infs_le32_to_cpu(page->bytes_used);
    uint64_t extent_bytes;

    if (!count || count > INFS_EXTENTS_PER_PAGE)
        return INFS_STATUS_CORRUPT;
    extent_bytes = (uint64_t)count * sizeof(struct infs_extent_disk);
    if (extent_bytes + sizeof(uint64_t) > INFS_METADATA_PAGE_DATA_SIZE ||
        bytes != extent_bytes + sizeof(uint64_t))
        return INFS_STATUS_CORRUPT;

    struct infs_extent_disk *ext = (struct infs_extent_disk *)(
        (uint8_t *)block + sizeof(*page));
    uint64_t next = infs_le64_to_cpu(ext[0].logical_block);
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t logical = infs_le64_to_cpu(ext[i].logical_block);
        uint64_t physical = infs_le64_to_cpu(ext[i].physical_block);
        uint32_t blocks = infs_le32_to_cpu(ext[i].block_count);
        uint32_t flags = infs_le32_to_cpu(ext[i].flags);
        uint64_t physical_blocks;
        if (!blocks || logical != next || logical > UINT64_MAX - blocks ||
            !extent_flags_valid(blocks, physical, flags))
            return INFS_STATUS_CORRUPT;
        if (extent_kind(flags) == INFS_EXTENT_HOLE) {
            next += blocks;
            continue;
        }
        if (extent_is_compressed(flags) &&
            (infs_le64_to_cpu(vol->sb.incompat_flags) &
             INFS_INCOMPAT_COMPRESSED_EXTENTS) == 0)
            return INFS_STATUS_CORRUPT;
        physical_blocks = extent_physical_blocks(blocks, flags);
        if (!physical_blocks || physical >= total ||
            physical_blocks > total - physical)
            return INFS_STATUS_CORRUPT;
        next += blocks;
    }
    if (entries_out)
        *entries_out = ext;
    if (count_out)
        *count_out = count;
    return INFS_STATUS_OK;
}


static int extent_page_encode(struct infs_volume *vol,
                              const uint8_t owner_id[16],
                              const struct infs_extent_disk *entries,
                              uint32_t count,
                              uint8_t block[INFS_BLOCK_SIZE])
{
    if (!count || count > INFS_EXTENTS_PER_PAGE)
        return INFS_STATUS_INVALID_ARGUMENT;
    uint64_t generation = infs_le64_to_cpu(vol->tx_base_sb.generation) + 1u;
    infs_status status = infs_metadata_page_init(
        block, INFS_EXTENT_PAGE_MAGIC, owner_id, generation);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_metadata_page_disk *page =
        (struct infs_metadata_page_disk *)block;
    memcpy(page + 1, entries, (size_t)count * sizeof(*entries));
    page->entry_count = infs_cpu_to_le32(count);
    page->bytes_used = infs_cpu_to_le32(count * sizeof(*entries));
    return INFS_STATUS_OK;
}

static int paged_extent_head_validate(struct infs_volume *vol,
                                      uint8_t object[INFS_BLOCK_SIZE],
                                      struct infs_file_payload_disk *file)
{
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    struct infs_extent_head_disk *head;
    uint64_t *root_ptr;
    uint64_t root;
    uint64_t total;
    uint32_t pages;
    uint32_t total_extents;
    size_t need;

    if ((infs_le64_to_cpu(vol->sb.incompat_flags) &
         INFS_INCOMPAT_PAGED_EXTENTS) == 0)
        return INFS_STATUS_CORRUPT;
    head = file_extent_head(file);
    pages = infs_le32_to_cpu(head->page_count);
    total_extents = infs_le32_to_cpu(file->extent_count);
    if (!pages || !total_extents || pages > total_extents ||
        infs_le32_to_cpu(head->reserved) != 0)
        return INFS_STATUS_CORRUPT;
    need = sizeof(*file) + sizeof(*head) + sizeof(uint64_t);
    if (need != infs_le32_to_cpu(hdr->payload_size) ||
        need > INFS_BLOCK_SIZE - sizeof(*hdr))
        return INFS_STATUS_CORRUPT;
    root_ptr = file_extent_page_pointers(file);
    root = infs_le64_to_cpu(root_ptr[0]);
    total = infs_le64_to_cpu(vol->sb.total_blocks);
    if (!root || root >= total || pages > total || !bitmap_get(vol->bitmap, root))
        return INFS_STATUS_CORRUPT;
    return INFS_STATUS_OK;
}


static int paged_extent_snapshot(struct infs_volume *vol,
                                 uint8_t object[INFS_BLOCK_SIZE],
                                 struct infs_file_payload_disk *file,
                                 struct infs_extent_disk **entries_out,
                                 uint32_t *count_out)
{
    struct infs_extent_disk *all;
    struct infs_object_header_disk *hdr;
    struct infs_extent_head_disk *head;
    uint64_t *root_ptr;
    uint64_t page_block;
    uint64_t total_blocks;
    uint64_t next_logical = 0;
    uint32_t page_count;
    uint32_t total_count;
    uint32_t at = 0;
    infs_status status;

    if (!entries_out || !count_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    *entries_out = NULL;
    *count_out = 0;
    status = paged_extent_head_validate(vol, object, file);
    if (status != INFS_STATUS_OK)
        return status;
    total_count = infs_le32_to_cpu(file->extent_count);
    if ((size_t)total_count > SIZE_MAX / sizeof(*all))
        return INFS_STATUS_OVERFLOW;
    all = malloc((size_t)total_count * sizeof(*all));
    if (!all)
        return INFS_STATUS_NO_MEMORY;
    hdr = (struct infs_object_header_disk *)object;
    head = file_extent_head(file);
    page_count = infs_le32_to_cpu(head->page_count);
    root_ptr = file_extent_page_pointers(file);
    page_block = infs_le64_to_cpu(root_ptr[0]);
    total_blocks = infs_le64_to_cpu(vol->sb.total_blocks);

    for (uint32_t p = 0; p < page_count; ++p) {
        uint8_t page_data[INFS_BLOCK_SIZE];
        struct infs_extent_disk *ext;
        uint32_t count;
        uint64_t next;

        if (!page_block || page_block >= total_blocks ||
            !bitmap_get(vol->bitmap, page_block)) {
            status = INFS_STATUS_CORRUPT;
            goto fail;
        }
        status = read_block(vol, page_block, page_data);
        if (status != INFS_STATUS_OK)
            goto fail;
        status = extent_page_validate(vol, page_data, hdr->object_id, &ext, &count);
        if (status != INFS_STATUS_OK)
            goto fail;
        if (count > total_count - at ||
            infs_le64_to_cpu(ext[0].logical_block) != next_logical) {
            status = INFS_STATUS_CORRUPT;
            goto fail;
        }
        memcpy(&all[at], ext, (size_t)count * sizeof(*ext));
        at += count;
        next_logical = infs_le64_to_cpu(ext[count - 1u].logical_block) +
            infs_le32_to_cpu(ext[count - 1u].block_count);
        next = extent_page_next_block(page_data);
        if ((p + 1u < page_count &&
             (!next || next >= total_blocks || !bitmap_get(vol->bitmap, next))) ||
            (p + 1u == page_count && next != 0)) {
            status = INFS_STATUS_CORRUPT;
            goto fail;
        }
        page_block = next;
    }
    if (at != total_count) {
        status = INFS_STATUS_CORRUPT;
        goto fail;
    }
    {
        uint64_t logical_size = infs_le64_to_cpu(file->attributes.logical_size);
        uint64_t required = logical_size / INFS_BLOCK_SIZE +
            ((logical_size % INFS_BLOCK_SIZE) != 0);
        if (next_logical != required) {
            status = INFS_STATUS_CORRUPT;
            goto fail;
        }
    }
    *entries_out = all;
    *count_out = total_count;
    return INFS_STATUS_OK;
fail:
    free(all);
    return status;
}


static int paged_extent_load_page(struct infs_volume *vol,
                                  uint8_t object[INFS_BLOCK_SIZE],
                                  struct infs_file_payload_disk *file,
                                  uint32_t page_index,
                                  uint8_t page_data[INFS_BLOCK_SIZE],
                                  struct infs_extent_disk **ext_out,
                                  uint32_t *count_out)
{
    struct infs_extent_head_disk *head = file_extent_head(file);
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    uint32_t pages = infs_le32_to_cpu(head->page_count);
    uint64_t *root_ptr = file_extent_page_pointers(file);
    uint64_t block;
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    infs_status status;

    if (page_index >= pages)
        return INFS_STATUS_CORRUPT;
    block = infs_le64_to_cpu(root_ptr[0]);
    for (uint32_t p = 0; p <= page_index; ++p) {
        if (!block || block >= total || !bitmap_get(vol->bitmap, block))
            return INFS_STATUS_CORRUPT;
        status = read_block(vol, block, page_data);
        if (status != INFS_STATUS_OK)
            return status;
        status = extent_page_validate(vol, page_data, hdr->object_id,
                                      ext_out, count_out);
        if (status != INFS_STATUS_OK)
            return status;
        if (p == page_index)
            return INFS_STATUS_OK;
        block = extent_page_next_block(page_data);
    }
    return INFS_STATUS_CORRUPT;
}


static int paged_extent_find_page(struct infs_volume *vol,
                                  uint8_t object[INFS_BLOCK_SIZE],
                                  struct infs_file_payload_disk *file,
                                  uint64_t logical, uint32_t *page_out,
                                  uint8_t page_data[INFS_BLOCK_SIZE],
                                  struct infs_extent_disk **ext_out,
                                  uint32_t *count_out)
{
    struct infs_extent_head_disk *head = file_extent_head(file);
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    uint32_t pages = infs_le32_to_cpu(head->page_count);
    uint64_t block = infs_le64_to_cpu(file_extent_page_pointers(file)[0]);
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);

    for (uint32_t p = 0; p < pages; ++p) {
        struct infs_extent_disk *ext;
        uint32_t count;
        uint64_t first, end, next;
        infs_status status;

        if (!block || block >= total || !bitmap_get(vol->bitmap, block))
            return INFS_STATUS_CORRUPT;
        status = read_block(vol, block, page_data);
        if (status != INFS_STATUS_OK)
            return status;
        status = extent_page_validate(vol, page_data, hdr->object_id, &ext, &count);
        if (status != INFS_STATUS_OK)
            return status;
        first = infs_le64_to_cpu(ext[0].logical_block);
        end = infs_le64_to_cpu(ext[count - 1u].logical_block) +
            infs_le32_to_cpu(ext[count - 1u].block_count);
        if (logical >= first && logical < end) {
            if (page_out) *page_out = p;
            if (ext_out) *ext_out = ext;
            if (count_out) *count_out = count;
            return INFS_STATUS_OK;
        }
        if (logical < first)
            return INFS_STATUS_CORRUPT;
        next = extent_page_next_block(page_data);
        if ((p + 1u < pages && !next) || (p + 1u == pages && next))
            return INFS_STATUS_CORRUPT;
        block = next;
    }
    return INFS_STATUS_CORRUPT;
}


static int paged_extent_map(struct infs_volume *vol,
                            uint8_t object[INFS_BLOCK_SIZE],
                            struct infs_file_payload_disk *file,
                            uint64_t logical, uint64_t *physical,
                            uint32_t *flags_out)
{
    uint8_t page_data[INFS_BLOCK_SIZE];
    struct infs_extent_disk *ext;
    uint32_t count;
    infs_status status = paged_extent_find_page(
        vol, object, file, logical, NULL, page_data, &ext, &count);
    if (status != INFS_STATUS_OK)
        return status;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t l = infs_le64_to_cpu(ext[i].logical_block);
        uint32_t n = infs_le32_to_cpu(ext[i].block_count);
        if (logical < l || logical >= l + n)
            continue;
        uint32_t flags = infs_le32_to_cpu(ext[i].flags);
        uint64_t mapped;
        if (extent_kind(flags) == INFS_EXTENT_HOLE)
            mapped = 0;
        else if (extent_is_compressed(flags))
            mapped = infs_le64_to_cpu(ext[i].physical_block);
        else
            mapped = infs_le64_to_cpu(ext[i].physical_block) + (logical - l);
        if (extent_kind(flags) == INFS_EXTENT_NORMAL &&
            !bitmap_get(vol->bitmap, mapped))
            return INFS_STATUS_CORRUPT;
        if (physical)
            *physical = mapped;
        if (flags_out)
            *flags_out = flags;
        return INFS_STATUS_OK;
    }
    return INFS_STATUS_CORRUPT;
}

static int extent_emit_capacity(struct infs_extent_disk *out, uint32_t *count,
                                uint32_t capacity, uint64_t logical,
                                uint64_t physical, uint32_t blocks,
                                uint32_t flags)
{
    if (!blocks)
        return INFS_STATUS_OK;
    if (*count) {
        struct infs_extent_disk *last = &out[*count - 1u];
        if (!extent_is_compressed(flags) &&
            infs_le32_to_cpu(last->flags) == flags &&
            infs_le64_to_cpu(last->logical_block) +
                infs_le32_to_cpu(last->block_count) == logical &&
            ((flags == INFS_EXTENT_HOLE && physical == 0) ||
             (flags == INFS_EXTENT_NORMAL &&
              infs_le64_to_cpu(last->physical_block) +
                  infs_le32_to_cpu(last->block_count) == physical))) {
            uint64_t merged =
                (uint64_t)infs_le32_to_cpu(last->block_count) + blocks;
            if (merged <= UINT32_MAX) {
                last->block_count = infs_cpu_to_le32((uint32_t)merged);
                return INFS_STATUS_OK;
            }
        }
    }
    if (*count >= capacity)
        return INFS_STATUS_FILE_TOO_LARGE;
    out[*count].logical_block = infs_cpu_to_le64(logical);
    out[*count].physical_block = infs_cpu_to_le64(physical);
    out[*count].block_count = infs_cpu_to_le32(blocks);
    out[*count].flags = infs_cpu_to_le32(flags);
    ++*count;
    return INFS_STATUS_OK;
}

static int paged_extent_rewrite_all(struct infs_volume *vol,
                                    uint8_t object[INFS_BLOCK_SIZE],
                                    struct infs_file_payload_disk *file,
                                    const struct infs_extent_disk *entries,
                                    uint32_t count, int converting)
{
    struct infs_object_header_disk *hdr;
    uint64_t *old_ptr = NULL;
    uint32_t old_pages = 0;
    uint32_t new_pages;
    uint64_t next_block = 0;
    infs_status status = INFS_STATUS_OK;

    if (!count)
        return INFS_STATUS_INVALID_ARGUMENT;
    new_pages = (count + INFS_EXTENTS_PER_PAGE - 1u) / INFS_EXTENTS_PER_PAGE;
    if (!new_pages)
        return INFS_STATUS_OVERFLOW;
    hdr = (struct infs_object_header_disk *)object;

    if (!converting && file_has_paged_extents(object)) {
        struct infs_extent_head_disk *old_head = file_extent_head(file);
        uint64_t block;
        uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);

        status = paged_extent_head_validate(vol, object, file);
        if (status != INFS_STATUS_OK)
            return status;
        old_pages = infs_le32_to_cpu(old_head->page_count);
        if ((size_t)old_pages > SIZE_MAX / sizeof(*old_ptr))
            return INFS_STATUS_OVERFLOW;
        old_ptr = malloc((size_t)old_pages * sizeof(*old_ptr));
        if (!old_ptr)
            return INFS_STATUS_NO_MEMORY;
        block = infs_le64_to_cpu(file_extent_page_pointers(file)[0]);
        for (uint32_t p = 0; p < old_pages; ++p) {
            uint8_t page_data[INFS_BLOCK_SIZE];
            if (!block || block >= total || !bitmap_get(vol->bitmap, block)) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            old_ptr[p] = block;
            status = read_block(vol, block, page_data);
            if (status != INFS_STATUS_OK)
                goto out;
            status = extent_page_validate(vol, page_data, hdr->object_id, NULL, NULL);
            if (status != INFS_STATUS_OK)
                goto out;
            block = extent_page_next_block(page_data);
            if ((p + 1u < old_pages && !block) ||
                (p + 1u == old_pages && block)) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
        }
    }

    for (uint32_t p = new_pages; p-- > 0;) {
        uint8_t page_data[INFS_BLOCK_SIZE];
        uint32_t at = p * INFS_EXTENTS_PER_PAGE;
        uint32_t chunk = count - at;
        uint64_t replacement = 0;
        uint64_t old_block = p < old_pages ? old_ptr[p] : 0;

        if (chunk > INFS_EXTENTS_PER_PAGE)
            chunk = INFS_EXTENTS_PER_PAGE;
        status = extent_page_encode(vol, hdr->object_id, &entries[at], chunk,
                                    page_data);
        if (status != INFS_STATUS_OK)
            goto out;
        extent_page_set_next_block(page_data, next_block);
        status = metadata_page_store(vol, old_block, page_data, &replacement);
        if (status != INFS_STATUS_OK)
            goto out;
        next_block = replacement;
    }

    for (uint32_t p = new_pages; p < old_pages; ++p) {
        free_run(vol, old_ptr[p], 1);
        if (vol->tx_error != INFS_STATUS_OK) {
            status = vol->tx_error;
            goto out;
        }
    }

    {
        struct infs_extent_head_disk *head = file_extent_head(file);
        uint64_t *root = file_extent_page_pointers(file);
        size_t payload = sizeof(*file) + sizeof(*head) + sizeof(uint64_t);
        memset(head, 0, INFS_BLOCK_SIZE - sizeof(*hdr) - sizeof(*file));
        head->page_count = infs_cpu_to_le32(new_pages);
        head->reserved = 0;
        root[0] = infs_cpu_to_le64(next_block);
        hdr->object_version = infs_cpu_to_le16(INFS_OBJECT_VERSION_PAGED);
        hdr->payload_size = infs_cpu_to_le32((uint32_t)payload);
        file->extent_count = infs_cpu_to_le32(count);
    }
    status = INFS_STATUS_OK;
out:
    free(old_ptr);
    return status;
}


static int paged_extent_destroy_pages(struct infs_volume *vol,
                                      uint8_t object[INFS_BLOCK_SIZE],
                                      struct infs_file_payload_disk *file)
{
    struct infs_object_header_disk *hdr;
    struct infs_extent_head_disk *head;
    uint64_t block;
    uint64_t total;
    uint32_t pages;
    infs_status status;

    if (!file_has_paged_extents(object))
        return INFS_STATUS_OK;
    status = paged_extent_head_validate(vol, object, file);
    if (status != INFS_STATUS_OK)
        return status;
    hdr = (struct infs_object_header_disk *)object;
    head = file_extent_head(file);
    pages = infs_le32_to_cpu(head->page_count);
    block = infs_le64_to_cpu(file_extent_page_pointers(file)[0]);
    total = infs_le64_to_cpu(vol->sb.total_blocks);
    for (uint32_t p = 0; p < pages; ++p) {
        uint8_t page_data[INFS_BLOCK_SIZE];
        uint64_t next;
        if (!block || block >= total || !bitmap_get(vol->bitmap, block))
            return INFS_STATUS_CORRUPT;
        status = read_block(vol, block, page_data);
        if (status != INFS_STATUS_OK)
            return status;
        status = extent_page_validate(vol, page_data, hdr->object_id, NULL, NULL);
        if (status != INFS_STATUS_OK)
            return status;
        next = extent_page_next_block(page_data);
        if ((p + 1u < pages && !next) || (p + 1u == pages && next))
            return INFS_STATUS_CORRUPT;
        free_run(vol, block, 1);
        if (vol->tx_error != INFS_STATUS_OK)
            return vol->tx_error;
        block = next;
    }
    return INFS_STATUS_OK;
}


static int paged_extent_replace_rebuild(
    struct infs_volume *vol, uint8_t object[INFS_BLOCK_SIZE],
    struct infs_file_payload_disk *file, uint64_t logical_start,
    uint64_t block_count, uint64_t new_physical, uint32_t new_flags)
{
    struct infs_extent_disk *old = NULL;
    uint32_t old_count = 0;
    infs_status status = paged_extent_snapshot(vol, object, file,
                                               &old, &old_count);
    if (status != INFS_STATUS_OK)
        return status;
    uint32_t capacity = old_count + 3u;
    struct infs_extent_disk *rebuilt = calloc(capacity, sizeof(*rebuilt));
    if (!rebuilt) {
        free(old);
        return INFS_STATUS_NO_MEMORY;
    }
    uint32_t new_count = 0;
    uint64_t replace_end = logical_start + block_count;
    int emitted = 0;
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    for (uint32_t i = 0; i < old_count; ++i) {
        uint64_t l = infs_le64_to_cpu(old[i].logical_block);
        uint64_t phys = infs_le64_to_cpu(old[i].physical_block);
        uint32_t n = infs_le32_to_cpu(old[i].block_count);
        uint32_t flags = infs_le32_to_cpu(old[i].flags);
        uint64_t e = l + n;
        if (e <= logical_start || l >= replace_end) {
            status = extent_emit_capacity(rebuilt, &new_count, capacity,
                                          l, phys, n, flags);
            if (status != INFS_STATUS_OK)
                goto out;
            continue;
        }
        if (extent_is_compressed(flags) &&
            (logical_start > l || replace_end < e)) {
            status = INFS_STATUS_NOT_SUPPORTED;
            goto out;
        }
        if (l < logical_start) {
            status = extent_emit_capacity(
                rebuilt, &new_count, capacity, l, phys,
                (uint32_t)(logical_start - l), flags);
            if (status != INFS_STATUS_OK)
                goto out;
        }
        uint64_t overlap_start = l > logical_start ? l : logical_start;
        uint64_t overlap_end = e < replace_end ? e : replace_end;
        if (!emitted) {
            status = extent_emit_capacity(rebuilt, &new_count, capacity,
                logical_start, new_physical, (uint32_t)block_count, new_flags);
            if (status != INFS_STATUS_OK)
                goto out;
            emitted = 1;
        }
        if (extent_kind(flags) == INFS_EXTENT_NORMAL) {
            uint64_t release_start = extent_is_compressed(flags) ?
                phys : phys + (overlap_start - l);
            uint64_t release_count = extent_is_compressed(flags) ?
                extent_physical_blocks(n, flags) :
                overlap_end - overlap_start;
            status = file_free_unshared_run(
                vol, hdr->object_id, release_start, release_count);
            if (status != INFS_STATUS_OK)
                goto out;
        }
        if (e > replace_end) {
            status = extent_emit_capacity(
                rebuilt, &new_count, capacity, replace_end,
                extent_kind(flags) == INFS_EXTENT_HOLE ? 0 :
                    phys + (replace_end - l),
                (uint32_t)(e - replace_end), flags);
            if (status != INFS_STATUS_OK)
                goto out;
        }
    }
    if (!emitted) {
        status = INFS_STATUS_CORRUPT;
        goto out;
    }
    status = paged_extent_rewrite_all(
        vol, object, file, rebuilt, new_count, 0);
out:
    free(rebuilt);
    free(old);
    return status;
}

static int paged_extent_replace(struct infs_volume *vol,
                                uint8_t object[INFS_BLOCK_SIZE],
                                struct infs_file_payload_disk *file,
                                uint64_t logical_start, uint64_t block_count,
                                uint64_t new_physical, uint32_t new_flags)
{
    /* The linked representation has no head-array insertion ceiling. Rebuild
     * transaction-private pages as one CoW chain; page-local updates can be
     * reintroduced later without changing the disk graph. */
    return paged_extent_replace_rebuild(vol, object, file, logical_start,
                                        block_count, new_physical, new_flags);
}


static int paged_extent_ensure(struct infs_volume *vol,
                               uint8_t object[INFS_BLOCK_SIZE],
                               struct infs_file_payload_disk *file,
                               uint64_t needed)
{
    struct infs_extent_disk *ext = NULL;
    uint32_t count = 0;
    infs_status status = paged_extent_snapshot(vol, object, file, &ext, &count);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_extent_disk *last = &ext[count - 1u];
    uint64_t have = infs_le64_to_cpu(last->logical_block) +
        infs_le32_to_cpu(last->block_count);
    if (needed <= have) {
        free(ext);
        return INFS_STATUS_OK;
    }
    uint64_t remaining = needed - have;
    uint32_t capacity = count +
        (uint32_t)(remaining / UINT32_MAX) + 2u;
    struct infs_extent_disk *grown = realloc(
        ext, (size_t)capacity * sizeof(*grown));
    if (!grown) {
        free(ext);
        return INFS_STATUS_NO_MEMORY;
    }
    ext = grown;
    uint32_t out = count;
    while (remaining) {
        uint32_t chunk = remaining > UINT32_MAX ? UINT32_MAX :
            (uint32_t)remaining;
        status = extent_emit_capacity(ext, &out, capacity, have, 0,
                                      chunk, INFS_EXTENT_HOLE);
        if (status != INFS_STATUS_OK) {
            free(ext);
            return status;
        }
        have += chunk;
        remaining -= chunk;
    }
    status = paged_extent_rewrite_all(vol, object, file, ext, out, 0);
    free(ext);
    return status;
}

static int paged_extent_truncate(struct infs_volume *vol,
                                 uint8_t object[INFS_BLOCK_SIZE],
                                 struct infs_file_payload_disk *file,
                                 uint64_t target_blocks)
{
    struct infs_extent_disk *old = NULL;
    uint32_t old_count = 0;
    infs_status status = paged_extent_snapshot(vol, object, file,
                                               &old, &old_count);
    if (status != INFS_STATUS_OK)
        return status;
    if (!target_blocks) {
        free(old);
        return INFS_STATUS_INVALID_ARGUMENT;
    }
    struct infs_extent_disk *kept = calloc(old_count, sizeof(*kept));
    if (!kept) {
        free(old);
        return INFS_STATUS_NO_MEMORY;
    }
    uint32_t kept_count = 0;
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    for (uint32_t i = 0; i < old_count; ++i) {
        uint64_t l = infs_le64_to_cpu(old[i].logical_block);
        uint64_t phys = infs_le64_to_cpu(old[i].physical_block);
        uint32_t n = infs_le32_to_cpu(old[i].block_count);
        uint32_t flags = infs_le32_to_cpu(old[i].flags);
        uint64_t e = l + n;
        if (l >= target_blocks) {
            if (extent_kind(flags) == INFS_EXTENT_NORMAL) {
                status = file_free_unshared_run(
                    vol, hdr->object_id, phys,
                    extent_physical_blocks(n, flags));
                if (status != INFS_STATUS_OK)
                    goto out;
            }
            continue;
        }
        uint32_t retain = e > target_blocks ?
            (uint32_t)(target_blocks - l) : n;
        kept[kept_count] = old[i];
        kept[kept_count].block_count = infs_cpu_to_le32(retain);
        ++kept_count;
        if (retain < n && extent_is_compressed(flags)) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }
        if (retain < n && extent_kind(flags) == INFS_EXTENT_NORMAL) {
            status = file_free_unshared_run(
                vol, hdr->object_id, phys + retain, n - retain);
            if (status != INFS_STATUS_OK)
                goto out;
        }
    }
    if (!kept_count) {
        status = INFS_STATUS_CORRUPT;
        goto out;
    }
    status = paged_extent_rewrite_all(
        vol, object, file, kept, kept_count, 0);
out:
    free(kept);
    free(old);
    return status;
}

static int file_extent_snapshot(struct infs_volume *vol,
                                uint8_t object[INFS_BLOCK_SIZE],
                                struct infs_file_payload_disk *file,
                                struct infs_extent_disk *classic,
                                struct infs_extent_disk **entries_out,
                                uint32_t *count_out, int *owned_out)
{
    if (file_has_paged_extents(object)) {
        infs_status status = paged_extent_snapshot(
            vol, object, file, entries_out, count_out);
        if (status == INFS_STATUS_OK && owned_out)
            *owned_out = 1;
        return status;
    }
    if (entries_out)
        *entries_out = classic;
    if (count_out)
        *count_out = infs_le32_to_cpu(file->extent_count);
    if (owned_out)
        *owned_out = 0;
    return INFS_STATUS_OK;
}
