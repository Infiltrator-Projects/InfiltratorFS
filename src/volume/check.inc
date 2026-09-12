// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * Fast structural filesystem check.
 *
 * This deliberately validates persistent filesystem metadata without reading
 * every file-data block or recomputing payload checksums. Deep data integrity
 * belongs to infs_scrub(). Keeping these paths separate is important for
 * normal fsck/boot use and for freshly formatted volumes where a complete
 * payload scrub would be pointless.
 */
infs_status infs_check(struct infs_volume *vol,
                       struct infs_check_report *report)
{
    if (!vol || !report)
        return INFS_STATUS_INVALID_ARGUMENT;

    memset(report, 0, sizeof(*report));
    report->check_generation = infs_le64_to_cpu(vol->sb.generation);

    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        uint64_t block = infs_le64_to_cpu(vol->sb.checkpoint_block[i]);
        if (checkpoint_matches(vol, block, &vol->sb))
            ++report->checkpoint_replicas_valid;
    }

    infs_status status = validate_index_entries(vol);
    if (status != INFS_STATUS_OK) {
        report->failed_stage = INFS_CHECK_STAGE_OBJECT_INDEX;
        return checkpoint_graph_status(status);
    }
    report->object_index_valid = 1;

    status = validate_integrity_metadata(vol);
    if (status != INFS_STATUS_OK) {
        report->failed_stage = INFS_CHECK_STAGE_ALLOCATION_OWNERSHIP;
        return checkpoint_graph_status(status);
    }
    report->allocation_ownership_valid = 1;

    status = validate_namespace_graph(vol);
    if (status != INFS_STATUS_OK) {
        report->failed_stage = INFS_CHECK_STAGE_NAMESPACE;
        return checkpoint_graph_status(status);
    }
    report->namespace_valid = 1;

    status = validate_checksum_graph(vol);
    if (status != INFS_STATUS_OK) {
        report->failed_stage = INFS_CHECK_STAGE_CHECKSUM_METADATA;
        return checkpoint_graph_status(status);
    }
    report->checksum_metadata_valid = 1;

    if (report->checkpoint_replicas_valid != INFS_CHECKPOINT_COUNT) {
        report->failed_stage = INFS_CHECK_STAGE_CHECKPOINTS;
        return INFS_STATUS_CORRUPT;
    }

    report->failed_stage = INFS_CHECK_STAGE_COMPLETE;
    return INFS_STATUS_OK;
}
