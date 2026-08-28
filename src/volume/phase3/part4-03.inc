// SPDX-License-Identifier: GPL-3.0-or-later
static int64_t write_file_tx(struct infs_volume *vol, const char *path,
                             const void *buf, size_t size, uint64_t offset)
{
    if (!vol || !path || (size && !buf))
        return INFS_STATUS_INVALID_ARGUMENT;
    if (!vol->writable)
        return INFS_STATUS_READ_ONLY;
    if ((uint64_t)size > INT64_MAX || offset > UINT64_MAX - size)
        return INFS_STATUS_OVERFLOW;
    if (size == 0)
        return 0;

    struct infs_lookup lk;
    infs_status status = infs_lookup_path(vol, path, &lk);
    if (status != INFS_STATUS_OK)
        return status;
    if (lk.type != INFS_OBJECT_FILE)
        return INFS_STATUS_IS_DIRECTORY;

    uint8_t object[INFS_BLOCK_SIZE];
    status = read_object(vol, lk.block, object);
    if (status != INFS_STATUS_OK)
        return status;
    struct infs_file_payload_disk *p;
    struct infs_extent_disk *ext;
    status = file_validate_volume(vol, object, &p, &ext);
    if (status != INFS_STATUS_OK)
        return status;

    uint64_t old_size = infs_le64_to_cpu(p->attributes.logical_size);
    uint64_t end = offset + size;
    int inline_enabled =
        (infs_le64_to_cpu(vol->sb.incompat_flags) &
         INFS_INCOMPAT_INLINE_DATA) != 0;
    if (inline_enabled && old_size <= INFS_INLINE_DATA_MAX &&
        end <= INFS_INLINE_DATA_MAX) {
        uint8_t inline_data[INFS_INLINE_DATA_MAX];
        status = file_read_small_content(vol, object, inline_data);
        if (status != INFS_STATUS_OK)
            return status;
        memcpy(inline_data + (size_t)offset, buf, size);
        uint64_t new_size = old_size > end ? old_size : end;
        status = file_store_inline(vol, object, inline_data, (size_t)new_size);
        if (status != INFS_STATUS_OK)
            return status;
        p = (struct infs_file_payload_disk *)(
            object + sizeof(struct infs_object_header_disk));
        touch_mctime(vol, &p->attributes);
        if (vol->tx_error != INFS_STATUS_OK)
            return vol->tx_error;
        status = infs_object_finalize(object);
        if (status != INFS_STATUS_OK)
            return status;
        status = file_validate_volume(vol, object, &p, &ext);
        if (status != INFS_STATUS_OK)
            return status;
        status = write_object(vol, lk.block, object);
        if (status != INFS_STATUS_OK)
            return status;
        return (int64_t)size;
    }

    if (file_is_inline(object, p)) {
        status = file_promote_inline(vol, object);
        if (status != INFS_STATUS_OK)
            return status;
        status = file_validate_volume(vol, object, &p, &ext);
        if (status != INFS_STATUS_OK)
            return status;
    }

    uint64_t needed_blocks = end / INFS_BLOCK_SIZE +
        ((end % INFS_BLOCK_SIZE) != 0);
    status = file_ensure_logical_blocks(vol, object, needed_blocks);
    if (status != INFS_STATUS_OK)
        return status;
    if (end > infs_le64_to_cpu(p->attributes.logical_size))
        p->attributes.logical_size = infs_cpu_to_le64(end);

    status = infs_object_finalize(object);
    if (status != INFS_STATUS_OK)
        return status;
    status = file_validate_volume(vol, object, &p, &ext);
    if (status != INFS_STATUS_OK)
        return status;

    struct infs_object_header_disk *file_hdr =
        (struct infs_object_header_disk *)object;
    size_t done = 0;
    uint8_t data[INFS_BLOCK_SIZE];
    struct infs_data_checksum_disk pending_digests[INFS_CHECKSUMS_PER_OBJECT];
    uint64_t pending_start = 0;
    uint32_t pending_count = 0;

    while (done < size) {
        uint64_t pos = offset + done;
        uint64_t logical = pos / INFS_BLOCK_SIZE;
        size_t within = (size_t)(pos % INFS_BLOCK_SIZE);
        size_t chunk = INFS_BLOCK_SIZE - within;
        if (chunk > size - done)
            chunk = size - done;

        uint64_t physical;
        uint32_t flags;
        status = file_map_block(vol, object, p, ext, logical, &physical, &flags);
        if (status != INFS_STATUS_OK)
            return status;

        if (within == 0 && flags == INFS_EXTENT_HOLE &&
    size - done >= INFS_BLOCK_SIZE) {
    uint64_t wanted = (uint64_t)((size - done) / INFS_BLOCK_SIZE);
    uint64_t hole_blocks = 0;
    if (file_has_paged_extents(object)) {
        uint8_t extent_page[INFS_BLOCK_SIZE];
        struct infs_extent_disk *page_extents = NULL;
        uint32_t page_extent_count = 0;
        status = paged_extent_find_page(
            vol, object, p, logical, NULL, extent_page,
            &page_extents, &page_extent_count);
        if (status != INFS_STATUS_OK)
            return status;
        for (uint32_t i = 0; i < page_extent_count; ++i) {
            uint64_t extent_logical =
                infs_le64_to_cpu(page_extents[i].logical_block);
            uint64_t extent_blocks =
                infs_le32_to_cpu(page_extents[i].block_count);
            if (logical >= extent_logical &&
                logical < extent_logical + extent_blocks) {
                if (infs_le32_to_cpu(page_extents[i].flags) !=
                    INFS_EXTENT_HOLE)
                    return INFS_STATUS_CORRUPT;
                hole_blocks = extent_logical + extent_blocks - logical;
                break;
            }
        }
    } else {
        uint32_t extent_count = infs_le32_to_cpu(p->extent_count);
        for (uint32_t i = 0; i < extent_count; ++i) {
            uint64_t extent_logical =
                infs_le64_to_cpu(ext[i].logical_block);
            uint64_t extent_blocks =
                infs_le32_to_cpu(ext[i].block_count);
            if (logical >= extent_logical &&
                logical < extent_logical + extent_blocks) {
                if (infs_le32_to_cpu(ext[i].flags) != INFS_EXTENT_HOLE)
                    return INFS_STATUS_CORRUPT;
                hole_blocks = extent_logical + extent_blocks - logical;
                break;
            }
        }
    }
    if (hole_blocks == 0)
        return INFS_STATUS_CORRUPT;
    if (wanted > hole_blocks)
        wanted = hole_blocks;
    if (wanted > UINT32_MAX)
        wanted = UINT32_MAX;

    if (pending_count != 0) {
        status = checksum_set_range(vol, p, file_hdr->object_id,
                                    pending_start, pending_digests,
                                    pending_count);
        if (status != INFS_STATUS_OK)
            return status;
        pending_count = 0;
    }

    uint64_t replacement = 0, got = 0;
    status = alloc_data_run_uninitialized(
        vol, wanted, &replacement, &got);
    if (status != INFS_STATUS_OK)
        return status;
    if (got == 0 || got > wanted)
        return INFS_STATUS_CORRUPT;

    size_t batch_bytes = (size_t)got * INFS_BLOCK_SIZE;
    status = infs_storage_write(
        &vol->storage, replacement * INFS_BLOCK_SIZE,
        (const uint8_t *)buf + done, batch_bytes);
    if (status != INFS_STATUS_OK)
        return status;
    status = file_replace_range(vol, object, logical, got,
                                replacement, INFS_EXTENT_NORMAL);
    if (status != INFS_STATUS_OK)
        return status;

    uint64_t digest_logical = logical;
    size_t digest_bytes = 0;
    uint64_t digest_blocks_left = got;
    while (digest_blocks_left != 0) {
        uint32_t digest_count = digest_blocks_left >
            INFS_CHECKSUMS_PER_OBJECT ?
            INFS_CHECKSUMS_PER_OBJECT :
            (uint32_t)digest_blocks_left;
        for (uint32_t i = 0; i < digest_count; ++i)
            data_block_checksum(
                (const uint8_t *)buf + done + digest_bytes +
                    (size_t)i * INFS_BLOCK_SIZE,
                &pending_digests[i]);
        status = checksum_set_range(
            vol, p, file_hdr->object_id, digest_logical,
            pending_digests, digest_count);
        if (status != INFS_STATUS_OK)
            return status;
        digest_logical += digest_count;
        digest_blocks_left -= digest_count;
        digest_bytes += (size_t)digest_count * INFS_BLOCK_SIZE;
    }

    done += batch_bytes;
    continue;
}

        int full_overwrite = within == 0 && chunk == INFS_BLOCK_SIZE;
        if (flags == INFS_EXTENT_HOLE || full_overwrite) {
            memset(data, 0, sizeof(data));
        } else {
            status = read_block(vol, physical, data);
            if (status != INFS_STATUS_OK)
                return status;
            status = data_block_verify(vol, p, file_hdr->object_id, logical,
                                       data);
            if (status != INFS_STATUS_OK)
                return status;
        }
        memcpy(data + within, (const uint8_t *)buf + done, chunk);

        /* A block that existed in generation N is never overwritten. Replace
         * it with an unreachable block and repoint the extent metadata. Blocks
         * allocated earlier in this same transaction are already unreachable
         * from N and may be filled in place before publication. */
        if (flags == INFS_EXTENT_HOLE ||
            !tx_block_private(vol, physical)) {
            uint64_t replacement = 0, got = 0;
            status = alloc_data_run(vol, 1, &replacement, &got);
            if (status != INFS_STATUS_OK)
                return status;
            if (got != 1)
                return INFS_STATUS_CORRUPT;
            status = write_block(vol, replacement, data);
            if (status != INFS_STATUS_OK)
                return status;
            status = file_replace_range(vol, object, logical, 1, replacement,
                                        INFS_EXTENT_NORMAL);
            if (status != INFS_STATUS_OK)
                return status;
            physical = replacement;
        } else {
            status = write_block(vol, physical, data);
            if (status != INFS_STATUS_OK)
                return status;
        }

        struct infs_data_checksum_disk digest;
        data_block_checksum(data, &digest);

        /* One FUSE write commonly contains tens of 4 KiB blocks. Updating the
         * checksum object for every block separately forced repeated object
         * index walks and metadata writes. Accumulate consecutive digests and
         * let checksum_set_range update each checksum object in bulk. */
        if (pending_count != 0 &&
            logical != pending_start + pending_count) {
            status = checksum_set_range(vol, p, file_hdr->object_id,
                                        pending_start, pending_digests,
                                        pending_count);
            if (status != INFS_STATUS_OK)
                return status;
            pending_count = 0;
        }
        if (pending_count == 0)
            pending_start = logical;
        pending_digests[pending_count++] = digest;
        if (pending_count == INFS_CHECKSUMS_PER_OBJECT) {
            status = checksum_set_range(vol, p, file_hdr->object_id,
                                        pending_start, pending_digests,
                                        pending_count);
            if (status != INFS_STATUS_OK)
                return status;
            pending_count = 0;
        }
        done += chunk;
    }

    if (pending_count != 0) {
        status = checksum_set_range(vol, p, file_hdr->object_id,
                                    pending_start, pending_digests,
                                    pending_count);
        if (status != INFS_STATUS_OK)
            return status;
    }

    touch_mctime(vol, &p->attributes);
    if (vol->tx_error != INFS_STATUS_OK)
        return vol->tx_error;
    status = write_object(vol, lk.block, object);
    if (status != INFS_STATUS_OK)
        return status;
    return (int64_t)done;
}

static int64_t write_file_common(struct infs_volume *vol, const char *path,
                                 const void *buf, size_t size, uint64_t offset,
                                 int force_publish)
{
    if (!vol || !path || (size && !buf))
        return INFS_STATUS_INVALID_ARGUMENT;
    if (!vol->writable)
        return INFS_STATUS_READ_ONLY;
    if ((uint64_t)size > INT64_MAX || offset > UINT64_MAX - size)
        return INFS_STATUS_OVERFLOW;
    if (size == 0)
        return 0;

    int started_transaction = 0;
    infs_status status = mutation_scope_begin(vol, &started_transaction);
    if (status != INFS_STATUS_OK)
        return status;
    int64_t rc = write_file_tx(vol, path, buf, size, offset);
    if (rc < 0) {
        mutation_scope_fail(vol, started_transaction);
        return rc;
    }

    if (force_publish)
        status = infs_volume_sync(vol);
    else
        status = finish_mutation(vol, (uint64_t)size + INFS_BLOCK_SIZE);
    if (status != INFS_STATUS_OK) {
        mutation_scope_fail(vol, started_transaction);
        return status;
    }
    mutation_scope_success(vol);
    return rc;
}

int64_t infs_write_file(struct infs_volume *vol, const char *path,
                        const void *buf, size_t size, uint64_t offset)
{
    return write_file_common(vol, path, buf, size, offset, 1);
}

int64_t infs_write_file_buffered(struct infs_volume *vol, const char *path,
                                 const void *buf, size_t size, uint64_t offset)
{
    return write_file_common(vol, path, buf, size, offset, 0);
}
