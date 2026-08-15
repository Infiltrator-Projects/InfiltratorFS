// SPDX-License-Identifier: GPL-3.0-or-later
int infs_scrub(struct infs_volume *vol, struct infs_scrub_report *report)
{
    if (!vol || !report) {
        errno = EINVAL;
        return -1;
    }
    memset(report, 0, sizeof(*report));

    uint8_t index_block[INFS_BLOCK_SIZE];
    struct infs_index_payload_disk *index_payload;
    if (index_load(vol, index_block, &index_payload) != 0)
        return -1;
    uint32_t entries_count = infs_le32_to_cpu(index_payload->entry_count);
    struct infs_index_entry_disk *entries =
        (struct infs_index_entry_disk *)(index_payload + 1);

    for (uint32_t i = 0; i < entries_count; ++i) {
        if (infs_le16_to_cpu(entries[i].object_type) != INFS_OBJECT_FILE)
            continue;
        ++report->files_checked;

        uint64_t object_block = infs_le64_to_cpu(entries[i].object_block);
        uint8_t object[INFS_BLOCK_SIZE];
        if (read_object(vol, object_block, object) != 0) {
            ++report->metadata_errors;
            continue;
        }
        struct infs_object_header_disk *hdr =
            (struct infs_object_header_disk *)object;
        struct infs_file_payload_disk *file;
        struct infs_extent_disk *ext;
        if (file_validate_volume(vol, object, &file, &ext) != 0 ||
            memcmp(hdr->object_id, entries[i].object_id, 16) != 0) {
            ++report->metadata_errors;
            continue;
        }

        uint64_t blocks = file_allocated_blocks(file, ext);
        for (uint64_t logical = 0; logical < blocks; ++logical) {
            ++report->data_blocks_checked;
            uint64_t physical;
            uint8_t data[INFS_BLOCK_SIZE];
            if (file_map_block(file, ext, logical, &physical) != 0 ||
                read_block(vol, physical, data) != 0) {
                ++report->metadata_errors;
                continue;
            }
            struct infs_data_checksum_disk expected, actual;
            if (checksum_get(vol, file, hdr->object_id, logical, &expected) != 0) {
                ++report->metadata_errors;
                continue;
            }
            data_block_checksum(data, &actual);
            if (memcmp(&expected, &actual, sizeof(expected)) != 0)
                ++report->checksum_errors;
        }
    }
    return 0;
}
