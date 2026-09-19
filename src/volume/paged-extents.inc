// SPDX-License-Identifier: GPL-3.0-or-later
/* Scalable extent metadata. Low-fragmentation files stay in the compact
 * classic representation. Paged files retain the direct page-pointer layout
 * while it fits in the file object. Files that exceed that representation are
 * promoted to a checksummed extent-pointer tree whose leaves reference the
 * ordinary extent pages. This keeps random page replacement bounded without a
 * fixed file-head pointer ceiling. */

static int file_has_paged_extents(const uint8_t object[INFS_BLOCK_SIZE])
{
    const struct infs_object_header_disk *hdr =
        (const struct infs_object_header_disk *)object;
    uint16_t version = infs_le16_to_cpu(hdr->object_version);
    return infs_le16_to_cpu(hdr->object_type) == INFS_OBJECT_FILE &&
           (version == INFS_OBJECT_VERSION_PAGED ||
            version == INFS_OBJECT_VERSION_TREE);
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


static uint32_t extent_pointer_tree_levels(uint32_t page_count)
{
    uint64_t capacity = INFS_EXTENT_INDEX_POINTERS_PER_PAGE;
    uint32_t levels = 1;

    if (!page_count)
        return 0;
    while (capacity < page_count && levels < INFS_EXTENT_INDEX_MAX_LEVELS) {
        if (capacity > UINT64_MAX / INFS_EXTENT_INDEX_POINTERS_PER_PAGE)
            return 0;
        capacity *= INFS_EXTENT_INDEX_POINTERS_PER_PAGE;
        ++levels;
    }
    return capacity >= page_count ? levels : 0;
}

static int extent_pointer_page_validate(
    struct infs_volume *vol, const uint8_t block[INFS_BLOCK_SIZE],
    const uint8_t owner_id[16], uint32_t expected_level,
    uint64_t **pointers_out, uint32_t *count_out)
{
    const struct infs_metadata_page_disk *page;
    uint32_t count, bytes;
    uint64_t *pointers;
    uint64_t total;

    if (!infs_validate_metadata_page(
            block, INFS_EXTENT_INDEX_PAGE_MAGIC, owner_id) ||
        !metadata_page_generation_valid(vol, block))
        return INFS_STATUS_CORRUPT;
    page = (const struct infs_metadata_page_disk *)block;
    count = infs_le32_to_cpu(page->entry_count);
    bytes = infs_le32_to_cpu(page->bytes_used);
    if (!count || count > INFS_EXTENT_INDEX_POINTERS_PER_PAGE ||
        bytes != count * sizeof(uint64_t) ||
        infs_le32_to_cpu(page->reserved) != expected_level)
        return INFS_STATUS_CORRUPT;
    pointers = (uint64_t *)((uint8_t *)block + sizeof(*page));
    total = infs_le64_to_cpu(vol->sb.total_blocks);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t child = infs_le64_to_cpu(pointers[i]);
        if (!child || child >= total || !bitmap_get(vol->bitmap, child))
            return INFS_STATUS_CORRUPT;
    }
    if (pointers_out)
        *pointers_out = pointers;
    if (count_out)
        *count_out = count;
    return INFS_STATUS_OK;
}

static int extent_pointer_tree_resolve(
    struct infs_volume *vol, const uint8_t owner_id[16], uint64_t root,
    uint32_t levels, uint32_t page_index, uint64_t *block_out)
{
    uint64_t node = root;
    uint64_t index = page_index;

    if (!block_out || !levels || levels > INFS_EXTENT_INDEX_MAX_LEVELS)
        return INFS_STATUS_CORRUPT;
    while (levels) {
        uint32_t level = levels - 1u;
        uint64_t span = 1;
        uint8_t page_data[INFS_BLOCK_SIZE];
        uint64_t *pointers;
        uint32_t count;
        infs_status status;

        for (uint32_t i = 0; i < level; ++i) {
            if (span > UINT64_MAX / INFS_EXTENT_INDEX_POINTERS_PER_PAGE)
                return INFS_STATUS_CORRUPT;
            span *= INFS_EXTENT_INDEX_POINTERS_PER_PAGE;
        }
        status = read_block(vol, node, page_data);
        if (status != INFS_STATUS_OK)
            return status;
        status = extent_pointer_page_validate(
            vol, page_data, owner_id, level, &pointers, &count);
        if (status != INFS_STATUS_OK)
            return status;
        uint64_t slot = index / span;
        if (slot >= count)
            return INFS_STATUS_CORRUPT;
        node = infs_le64_to_cpu(pointers[slot]);
        index %= span;
        if (!level) {
            *block_out = node;
            return INFS_STATUS_OK;
        }
        --levels;
    }
    return INFS_STATUS_CORRUPT;
}

static int extent_pointer_tree_build(
    struct infs_volume *vol, const uint8_t owner_id[16],
    const uint64_t *extent_pages, uint32_t page_count,
    uint64_t *root_out, uint32_t *levels_out)
{
    uint64_t *current = NULL;
    uint32_t current_count = page_count;
    uint32_t level = 0;
    infs_status status = INFS_STATUS_OK;

    if (!extent_pages || !page_count || !root_out || !levels_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (!extent_pointer_tree_levels(page_count))
        return INFS_STATUS_FILE_TOO_LARGE;
    size_t current_bytes = 0;
    if (!infiltratr_size_multiply_checked(
            (size_t)page_count, sizeof(*current), &current_bytes))
        return INFS_STATUS_OVERFLOW;
    current = malloc(current_bytes);
    if (!current)
        return INFS_STATUS_NO_MEMORY;
    memcpy(current, extent_pages, current_bytes);

    for (;;) {
        uint32_t parent_count =
            current_count / INFS_EXTENT_INDEX_POINTERS_PER_PAGE +
            ((current_count % INFS_EXTENT_INDEX_POINTERS_PER_PAGE) != 0);
        uint64_t *parents = calloc(parent_count, sizeof(*parents));
        uint32_t at = 0;

        if (!parents) {
            status = INFS_STATUS_NO_MEMORY;
            goto out;
        }
        for (uint32_t p = 0; p < parent_count; ++p) {
            uint8_t page_data[INFS_BLOCK_SIZE];
            struct infs_metadata_page_disk *page;
            uint64_t *slots;
            uint32_t count = current_count - at;
            uint64_t stored = 0;
            uint64_t generation = 0;

            if (count > INFS_EXTENT_INDEX_POINTERS_PER_PAGE)
                count = INFS_EXTENT_INDEX_POINTERS_PER_PAGE;
            status = transaction_next_generation(vol, &generation);
            if (status != INFS_STATUS_OK) {
                free(parents);
                goto out;
            }
            status = infs_metadata_page_init(
                page_data, INFS_EXTENT_INDEX_PAGE_MAGIC, owner_id, generation);
            if (status != INFS_STATUS_OK) {
                free(parents);
                goto out;
            }
            page = (struct infs_metadata_page_disk *)page_data;
            page->entry_count = infs_cpu_to_le32(count);
            page->bytes_used = infs_cpu_to_le32(count * sizeof(uint64_t));
            page->reserved = infs_cpu_to_le32(level);
            slots = (uint64_t *)(page + 1);
            for (uint32_t i = 0; i < count; ++i)
                slots[i] = infs_cpu_to_le64(current[at + i]);
            status = metadata_page_store(vol, 0, page_data, &stored);
            if (status != INFS_STATUS_OK) {
                free(parents);
                goto out;
            }
            parents[p] = stored;
            at += count;
        }
        free(current);
        current = parents;
        current_count = parent_count;
        ++level;
        if (current_count == 1u) {
            *root_out = current[0];
            *levels_out = level;
            status = INFS_STATUS_OK;
            goto out;
        }
        if (level >= INFS_EXTENT_INDEX_MAX_LEVELS) {
            status = INFS_STATUS_FILE_TOO_LARGE;
            goto out;
        }
    }
out:
    free(current);
    return status;
}

static int extent_pointer_tree_free_node(
    struct infs_volume *vol, const uint8_t owner_id[16],
    uint64_t block, uint32_t level)
{
    uint8_t page_data[INFS_BLOCK_SIZE];
    uint64_t *pointers;
    uint32_t count;
    infs_status status;

    status = read_block(vol, block, page_data);
    if (status != INFS_STATUS_OK)
        return status;
    status = extent_pointer_page_validate(
        vol, page_data, owner_id, level, &pointers, &count);
    if (status != INFS_STATUS_OK)
        return status;
    if (level) {
        for (uint32_t i = 0; i < count; ++i) {
            status = extent_pointer_tree_free_node(
                vol, owner_id, infs_le64_to_cpu(pointers[i]), level - 1u);
            if (status != INFS_STATUS_OK)
                return status;
        }
    }
    free_run(vol, block, 1);
    return vol->tx_error;
}

static int paged_extent_page_block(
    struct infs_volume *vol, uint8_t object[INFS_BLOCK_SIZE],
    struct infs_file_payload_disk *file, uint32_t page_index,
    uint64_t *block_out)
{
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    struct infs_extent_head_disk *head = file_extent_head(file);
    uint32_t pages = infs_le32_to_cpu(head->page_count);
    uint16_t version = infs_le16_to_cpu(hdr->object_version);

    if (!block_out || page_index >= pages)
        return INFS_STATUS_CORRUPT;
    if (version == INFS_OBJECT_VERSION_PAGED) {
        *block_out = infs_le64_to_cpu(file_extent_page_pointers(file)[page_index]);
        return INFS_STATUS_OK;
    }
    if (version == INFS_OBJECT_VERSION_TREE) {
        uint32_t levels = infs_le32_to_cpu(head->reserved);
        uint64_t root = infs_le64_to_cpu(file_extent_page_pointers(file)[0]);
        return extent_pointer_tree_resolve(
            vol, hdr->object_id, root, levels, page_index, block_out);
    }
    return INFS_STATUS_CORRUPT;
}

static int paged_extent_collect_page_blocks(
    struct infs_volume *vol, uint8_t object[INFS_BLOCK_SIZE],
    struct infs_file_payload_disk *file, uint64_t **blocks_out,
    uint32_t *count_out)
{
    struct infs_extent_head_disk *head = file_extent_head(file);
    uint32_t count = infs_le32_to_cpu(head->page_count);
    uint64_t *blocks;

    if (!blocks_out || !count_out || !count)
        return INFS_STATUS_INVALID_ARGUMENT;
    size_t blocks_bytes = 0;
    if (!infiltratr_size_multiply_checked(
            (size_t)count, sizeof(*blocks), &blocks_bytes))
        return INFS_STATUS_OVERFLOW;
    blocks = malloc(blocks_bytes);
    if (!blocks)
        return INFS_STATUS_NO_MEMORY;
    for (uint32_t i = 0; i < count; ++i) {
        infs_status status = paged_extent_page_block(
            vol, object, file, i, &blocks[i]);
        if (status != INFS_STATUS_OK) {
            free(blocks);
            return status;
        }
    }
    *blocks_out = blocks;
    *count_out = count;
    return INFS_STATUS_OK;
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
    if (!count || count > INFS_EXTENTS_PER_PAGE ||
        bytes != count * sizeof(struct infs_extent_disk))
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
    uint64_t generation = 0;
    infs_status status = transaction_next_generation(vol, &generation);
    if (status != INFS_STATUS_OK)
        return status;
    status = infs_metadata_page_init(
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
    struct infs_extent_head_disk *head = file_extent_head(file);
    uint32_t pages = infs_le32_to_cpu(head->page_count);
    uint32_t total_extents = infs_le32_to_cpu(file->extent_count);
    uint16_t version = infs_le16_to_cpu(hdr->object_version);
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    size_t need;

    if ((infs_le64_to_cpu(vol->sb.incompat_flags) &
         INFS_INCOMPAT_PAGED_EXTENTS) == 0 ||
        !pages || !total_extents || pages > total_extents)
        return INFS_STATUS_CORRUPT;

    if (version == INFS_OBJECT_VERSION_PAGED) {
        uint64_t *ptr;
        if (pages > INFS_EXTENT_PAGE_POINTERS ||
            infs_le32_to_cpu(head->reserved) != 0)
            return INFS_STATUS_CORRUPT;
        need = sizeof(*file) + sizeof(*head) +
            (size_t)pages * sizeof(uint64_t);
        if (need != infs_le32_to_cpu(hdr->payload_size) ||
            need > INFS_BLOCK_SIZE - sizeof(*hdr))
            return INFS_STATUS_CORRUPT;
        ptr = file_extent_page_pointers(file);
        for (uint32_t i = 0; i < pages; ++i) {
            uint64_t block = infs_le64_to_cpu(ptr[i]);
            if (!block || block >= total || !bitmap_get(vol->bitmap, block))
                return INFS_STATUS_CORRUPT;
        }
        return INFS_STATUS_OK;
    }

    if (version == INFS_OBJECT_VERSION_TREE) {
        uint32_t levels = infs_le32_to_cpu(head->reserved);
        uint64_t root = infs_le64_to_cpu(file_extent_page_pointers(file)[0]);
        uint8_t root_page[INFS_BLOCK_SIZE];
        if (pages <= INFS_EXTENT_PAGE_POINTERS ||
            levels != extent_pointer_tree_levels(pages) ||
            !root || root >= total || !bitmap_get(vol->bitmap, root))
            return INFS_STATUS_CORRUPT;
        need = sizeof(*file) + sizeof(*head) + sizeof(uint64_t);
        if (need != infs_le32_to_cpu(hdr->payload_size))
            return INFS_STATUS_CORRUPT;
        infs_status status = read_block(vol, root, root_page);
        if (status != INFS_STATUS_OK)
            return status;
        return extent_pointer_page_validate(
            vol, root_page, hdr->object_id, levels - 1u, NULL, NULL);
    }
    return INFS_STATUS_CORRUPT;
}



static int paged_extent_snapshot(struct infs_volume *vol,
                                 uint8_t object[INFS_BLOCK_SIZE],
                                 struct infs_file_payload_disk *file,
                                 struct infs_extent_disk **entries_out,
                                 uint32_t *count_out)
{
    infs_status status;
    uint32_t total_count, page_count, at = 0;
    size_t extent_bytes = 0;
    struct infs_extent_disk *all;
    struct infs_object_header_disk *hdr;
    uint64_t next_logical = 0;

    if (!entries_out || !count_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    *entries_out = NULL;
    *count_out = 0;
    status = paged_extent_head_validate(vol, object, file);
    if (status != INFS_STATUS_OK)
        return status;
    total_count = infs_le32_to_cpu(file->extent_count);
    if (!infiltratr_size_multiply_checked(
            (size_t)total_count, sizeof(struct infs_extent_disk),
            &extent_bytes))
        return INFS_STATUS_OVERFLOW;
    all = malloc(extent_bytes);
    if (!all)
        return INFS_STATUS_NO_MEMORY;
    hdr = (struct infs_object_header_disk *)object;
    page_count = infs_le32_to_cpu(file_extent_head(file)->page_count);

    for (uint32_t p = 0; p < page_count; ++p) {
        uint8_t page_data[INFS_BLOCK_SIZE];
        struct infs_extent_disk *ext;
        uint32_t count;
        uint64_t page_block;

        status = paged_extent_page_block(vol, object, file, p, &page_block);
        if (status != INFS_STATUS_OK)
            goto fail;
        status = read_block(vol, page_block, page_data);
        if (status != INFS_STATUS_OK)
            goto fail;
        status = extent_page_validate(
            vol, page_data, hdr->object_id, &ext, &count);
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
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    uint64_t block;
    infs_status status = paged_extent_page_block(
        vol, object, file, page_index, &block);
    if (status != INFS_STATUS_OK)
        return status;
    status = read_block(vol, block, page_data);
    if (status != INFS_STATUS_OK)
        return status;
    return extent_page_validate(
        vol, page_data, hdr->object_id, ext_out, count_out);
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
    uint32_t pages = infs_le32_to_cpu(head->page_count);
    uint32_t lo = 0, hi = pages;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        uint8_t local[INFS_BLOCK_SIZE];
        struct infs_extent_disk *ext;
        uint32_t count;
        infs_status status = paged_extent_load_page(
            vol, object, file, mid, local, &ext, &count);
        if (status != INFS_STATUS_OK)
            return status;
        uint64_t first = infs_le64_to_cpu(ext[0].logical_block);
        struct infs_extent_disk *last = &ext[count - 1u];
        uint64_t end = infs_le64_to_cpu(last->logical_block) +
            infs_le32_to_cpu(last->block_count);
        if (logical < first) {
            hi = mid;
        } else if (logical >= end) {
            lo = mid + 1u;
        } else {
            if (page_out)
                *page_out = mid;
            if (page_data)
                memcpy(page_data, local, INFS_BLOCK_SIZE);
            if (ext_out)
                *ext_out = (struct infs_extent_disk *)(
                    page_data + sizeof(struct infs_metadata_page_disk));
            if (count_out)
                *count_out = count;
            return INFS_STATUS_OK;
        }
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
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    uint64_t *old_ptr = NULL, *new_ptr = NULL;
    uint32_t old_pages = 0;
    uint32_t new_pages;
    uint16_t old_version = infs_le16_to_cpu(hdr->object_version);
    infs_status status = INFS_STATUS_OK;

    if (!count)
        return INFS_STATUS_INVALID_ARGUMENT;
    new_pages = count / INFS_EXTENTS_PER_PAGE +
        ((count % INFS_EXTENTS_PER_PAGE) != 0);
    if (!new_pages || !extent_pointer_tree_levels(new_pages))
        return INFS_STATUS_FILE_TOO_LARGE;

    if (!converting && file_has_paged_extents(object)) {
        status = paged_extent_head_validate(vol, object, file);
        if (status != INFS_STATUS_OK)
            return status;
        status = paged_extent_collect_page_blocks(
            vol, object, file, &old_ptr, &old_pages);
        if (status != INFS_STATUS_OK)
            return status;
    }

    new_ptr = calloc(new_pages, sizeof(*new_ptr));
    if (!new_ptr) {
        free(old_ptr);
        return INFS_STATUS_NO_MEMORY;
    }
    uint32_t at = 0;
    for (uint32_t p = 0; p < new_pages; ++p) {
        uint32_t chunk = count - at;
        uint8_t page_data[INFS_BLOCK_SIZE];
        uint64_t replacement = 0;
        if (chunk > INFS_EXTENTS_PER_PAGE)
            chunk = INFS_EXTENTS_PER_PAGE;
        status = extent_page_encode(
            vol, hdr->object_id, &entries[at], chunk, page_data);
        if (status != INFS_STATUS_OK)
            goto out;
        status = metadata_page_store(
            vol, p < old_pages ? old_ptr[p] : 0, page_data, &replacement);
        if (status != INFS_STATUS_OK)
            goto out;
        new_ptr[p] = replacement;
        at += chunk;
    }
    for (uint32_t p = new_pages; p < old_pages; ++p) {
        free_run(vol, old_ptr[p], 1);
        if (vol->tx_error != INFS_STATUS_OK) {
            status = vol->tx_error;
            goto out;
        }
    }

    /* The old pointer tree becomes unreachable from the replacement head. */
    if (!converting && old_version == INFS_OBJECT_VERSION_TREE) {
        struct infs_extent_head_disk *old_head = file_extent_head(file);
        uint32_t old_levels = infs_le32_to_cpu(old_head->reserved);
        uint64_t old_root = infs_le64_to_cpu(file_extent_page_pointers(file)[0]);
        status = extent_pointer_tree_free_node(
            vol, hdr->object_id, old_root, old_levels - 1u);
        if (status != INFS_STATUS_OK)
            goto out;
    }

    memset(file + 1, 0,
           INFS_BLOCK_SIZE - sizeof(*hdr) - sizeof(*file));
    struct infs_extent_head_disk *head = file_extent_head(file);
    head->page_count = infs_cpu_to_le32(new_pages);
    file->extent_count = infs_cpu_to_le32(count);

    if (new_pages <= INFS_EXTENT_PAGE_POINTERS) {
        uint64_t *ptr = file_extent_page_pointers(file);
        hdr->object_version = infs_cpu_to_le16(INFS_OBJECT_VERSION_PAGED);
        head->reserved = 0;
        for (uint32_t p = 0; p < new_pages; ++p)
            ptr[p] = infs_cpu_to_le64(new_ptr[p]);
        hdr->payload_size = infs_cpu_to_le32((uint32_t)(
            sizeof(*file) + sizeof(*head) +
            (size_t)new_pages * sizeof(uint64_t)));
    } else {
        uint64_t root = 0;
        uint32_t levels = 0;
        status = extent_pointer_tree_build(
            vol, hdr->object_id, new_ptr, new_pages, &root, &levels);
        if (status != INFS_STATUS_OK)
            goto out;
        hdr->object_version = infs_cpu_to_le16(INFS_OBJECT_VERSION_TREE);
        head->reserved = infs_cpu_to_le32(levels);
        file_extent_page_pointers(file)[0] = infs_cpu_to_le64(root);
        hdr->payload_size = infs_cpu_to_le32((uint32_t)(
            sizeof(*file) + sizeof(*head) + sizeof(uint64_t)));
    }
    status = INFS_STATUS_OK;
out:
    free(new_ptr);
    free(old_ptr);
    return status;
}



static int paged_extent_destroy_pages(struct infs_volume *vol,
                                      uint8_t object[INFS_BLOCK_SIZE],
                                      struct infs_file_payload_disk *file)
{
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    uint64_t *blocks = NULL;
    uint32_t pages = 0;
    uint16_t version;
    infs_status status;

    if (!file_has_paged_extents(object))
        return INFS_STATUS_OK;
    status = paged_extent_head_validate(vol, object, file);
    if (status != INFS_STATUS_OK)
        return status;
    version = infs_le16_to_cpu(hdr->object_version);
    status = paged_extent_collect_page_blocks(
        vol, object, file, &blocks, &pages);
    if (status != INFS_STATUS_OK)
        return status;
    for (uint32_t i = 0; i < pages; ++i) {
        free_run(vol, blocks[i], 1);
        if (vol->tx_error != INFS_STATUS_OK) {
            free(blocks);
            return vol->tx_error;
        }
    }
    free(blocks);
    if (version == INFS_OBJECT_VERSION_TREE) {
        struct infs_extent_head_disk *head = file_extent_head(file);
        uint32_t levels = infs_le32_to_cpu(head->reserved);
        uint64_t root = infs_le64_to_cpu(file_extent_page_pointers(file)[0]);
        return extent_pointer_tree_free_node(
            vol, hdr->object_id, root, levels - 1u);
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
    struct infs_object_header_disk *object_header =
        (struct infs_object_header_disk *)object;
    if (infs_le16_to_cpu(object_header->object_version) ==
        INFS_OBJECT_VERSION_TREE)
        return paged_extent_replace_rebuild(
            vol, object, file, logical_start, block_count,
            new_physical, new_flags);

    uint64_t replace_end = logical_start + block_count;
    uint8_t page_data[INFS_BLOCK_SIZE];
    struct infs_extent_disk *old;
    uint32_t old_count, page_index;
    infs_status status = paged_extent_find_page(
        vol, object, file, logical_start, &page_index, page_data,
        &old, &old_count);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_extent_disk *last = &old[old_count - 1u];
    uint64_t page_end = infs_le64_to_cpu(last->logical_block) +
        infs_le32_to_cpu(last->block_count);
    if (replace_end > page_end)
        return paged_extent_replace_rebuild(
            vol, object, file, logical_start, block_count,
            new_physical, new_flags);

    struct infs_extent_disk rebuilt[INFS_EXTENTS_PER_PAGE + 2u];
    uint32_t new_count = 0;
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
            status = extent_emit_capacity(
                rebuilt, &new_count, INFS_EXTENTS_PER_PAGE + 2u,
                l, phys, n, flags);
            if (status != INFS_STATUS_OK)
                return status;
            continue;
        }
        if (l < logical_start) {
            status = extent_emit_capacity(
                rebuilt, &new_count, INFS_EXTENTS_PER_PAGE + 2u,
                l, phys, (uint32_t)(logical_start - l), flags);
            if (status != INFS_STATUS_OK)
                return status;
        }
        uint64_t overlap_start = l > logical_start ? l : logical_start;
        uint64_t overlap_end = e < replace_end ? e : replace_end;
        if (!emitted) {
            status = extent_emit_capacity(
                rebuilt, &new_count, INFS_EXTENTS_PER_PAGE + 2u,
                logical_start, new_physical, (uint32_t)block_count, new_flags);
            if (status != INFS_STATUS_OK)
                return status;
            emitted = 1;
        }
        if (flags == INFS_EXTENT_NORMAL) {
            status = file_free_unshared_run(
                vol, hdr->object_id, phys + (overlap_start - l),
                overlap_end - overlap_start);
            if (status != INFS_STATUS_OK)
                return status;
        }
        if (e > replace_end) {
            status = extent_emit_capacity(
                rebuilt, &new_count, INFS_EXTENTS_PER_PAGE + 2u,
                replace_end,
                flags == INFS_EXTENT_HOLE ? 0 : phys + (replace_end - l),
                (uint32_t)(e - replace_end), flags);
            if (status != INFS_STATUS_OK)
                return status;
        }
    }
    if (!emitted)
        return INFS_STATUS_CORRUPT;

    struct infs_extent_head_disk *head = file_extent_head(file);
    uint32_t pages_count = infs_le32_to_cpu(head->page_count);
    uint64_t *pages = file_extent_page_pointers(file);
    uint64_t old_page_block = infs_le64_to_cpu(pages[page_index]);
    uint32_t total_extents = infs_le32_to_cpu(file->extent_count);
    if (new_count <= INFS_EXTENTS_PER_PAGE) {
        uint8_t encoded[INFS_BLOCK_SIZE];
        status = extent_page_encode(vol, hdr->object_id, rebuilt,
                                    new_count, encoded);
        if (status != INFS_STATUS_OK)
            return status;
        uint64_t replacement = 0;
        status = metadata_page_store(
            vol, old_page_block, encoded, &replacement);
        if (status != INFS_STATUS_OK)
            return status;
        pages[page_index] = infs_cpu_to_le64(replacement);
    } else {
        if (pages_count >= INFS_EXTENT_PAGE_POINTERS)
            return paged_extent_replace_rebuild(
                vol, object, file, logical_start, block_count,
                new_physical, new_flags);
        uint32_t left_count = new_count / 2u;
        uint32_t right_count = new_count - left_count;
        uint8_t left[INFS_BLOCK_SIZE], right[INFS_BLOCK_SIZE];
        status = extent_page_encode(vol, hdr->object_id, rebuilt,
                                    left_count, left);
        if (status != INFS_STATUS_OK)
            return status;
        status = extent_page_encode(vol, hdr->object_id,
                                    &rebuilt[left_count], right_count, right);
        if (status != INFS_STATUS_OK)
            return status;
        uint64_t left_block = 0, right_block = 0;
        status = metadata_page_store(
            vol, old_page_block, left, &left_block);
        if (status != INFS_STATUS_OK)
            return status;
        status = metadata_page_store(vol, 0, right, &right_block);
        if (status != INFS_STATUS_OK)
            return status;
        memmove(&pages[page_index + 2u], &pages[page_index + 1u],
                (size_t)(pages_count - page_index - 1u) * sizeof(*pages));
        pages[page_index] = infs_cpu_to_le64(left_block);
        pages[page_index + 1u] = infs_cpu_to_le64(right_block);
        ++pages_count;
        head->page_count = infs_cpu_to_le32(pages_count);
        hdr->payload_size = infs_cpu_to_le32((uint32_t)(
            sizeof(*file) + sizeof(*head) +
            (size_t)pages_count * sizeof(uint64_t)));
    }
    file->extent_count = infs_cpu_to_le32(
        total_extents - old_count + new_count);
    return INFS_STATUS_OK;
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
    uint64_t extra = remaining / UINT32_MAX;
    uint64_t capacity64 = 0;
    size_t grown_bytes = 0;
    if (!infiltratr_u64_add_checked((uint64_t)count, extra, &capacity64) ||
        !infiltratr_u64_add_checked(capacity64, UINT64_C(2), &capacity64) ||
        capacity64 > UINT32_MAX ||
        !infiltratr_size_multiply_checked(
            (size_t)capacity64, sizeof(*ext), &grown_bytes)) {
        free(ext);
        return INFS_STATUS_OVERFLOW;
    }
    uint32_t capacity = (uint32_t)capacity64;
    struct infs_extent_disk *grown = realloc(ext, grown_bytes);
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
