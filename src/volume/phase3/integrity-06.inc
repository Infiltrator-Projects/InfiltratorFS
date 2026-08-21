// SPDX-License-Identifier: GPL-3.0-or-later
infs_status infs_scrub(struct infs_volume *vol,
                       struct infs_scrub_report *report)
{
    if (!vol || !report)
        return INFS_STATUS_INVALID_ARGUMENT;
    memset(report, 0, sizeof(*report));

    uint8_t index_block[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *index_payload;
    infs_status status = index_load(vol, index_block, &index_payload);
    if (status != INFS_STATUS_OK)
        return status;
    uint32_t entries_count = infs_le32_to_cpu(index_payload->entry_count);
    struct infs_index_entry_disk *entries =
        (struct infs_index_entry_disk *)(index_payload + 1);

    for (uint32_t i = 0; i < entries_count; ++i) {
        if (infs_le16_to_cpu(entries[i].object_type) != INFS_OBJECT_FILE)
            continue;
        ++report->files_checked;

        uint64_t object_block = infs_le64_to_cpu(entries[i].object_block);
        uint8_t object[INFS_BLOCK_SIZE];
        if (read_object(vol, object_block, object) != INFS_STATUS_OK) {
            ++report->metadata_errors;
            continue;
        }
        struct infs_object_header_disk *hdr =
            (struct infs_object_header_disk *)object;
        struct infs_file_payload_disk *file;
        struct infs_extent_disk *ext;
        if (file_validate_volume(vol, object, &file, &ext) != INFS_STATUS_OK ||
            memcmp(hdr->object_id, entries[i].object_id, 16) != 0) {
            ++report->metadata_errors;
            continue;
        }

        if (file_is_inline(object, file)) {
            ++report->data_blocks_checked;
            continue;
        }

        uint32_t extent_count = infs_le32_to_cpu(file->extent_count);
        for (uint32_t j = 0; j < extent_count; ++j) {
            if (infs_le32_to_cpu(ext[j].flags) != INFS_EXTENT_NORMAL)
                continue;
            uint64_t logical = infs_le64_to_cpu(ext[j].logical_block);
            uint64_t physical = infs_le64_to_cpu(ext[j].physical_block);
            uint32_t blocks = infs_le32_to_cpu(ext[j].block_count);
            for (uint32_t k = 0; k < blocks; ++k) {
                ++report->data_blocks_checked;
                uint8_t data[INFS_BLOCK_SIZE];
                if (read_block(vol, physical + k, data) != INFS_STATUS_OK) {
                    ++report->metadata_errors;
                    continue;
                }
                struct infs_data_checksum_disk expected, actual;
                if (checksum_get(vol, file, hdr->object_id, logical + k,
                                 &expected) != INFS_STATUS_OK) {
                    ++report->metadata_errors;
                    continue;
                }
                data_block_checksum(data, &actual);
                if (memcmp(&expected, &actual, sizeof(expected)) != 0)
                    ++report->checksum_errors;
            }
        }
    }
    return INFS_STATUS_OK;
}
