#define INFILFS_REFLINK_INDEX_BATCH 1024u

static int infilfs_reflink_clone_checksum_chain(
    struct infilfs_native_pending *pending,
    const struct infilfs_file_payload_disk *source_file,
    const u8 source_owner[16], const u8 clone_owner[16],
    struct infilfs_file_payload_disk *clone_file)
{
    struct infilfs_native_index_change *changes = NULL;
    u8 source_id[16];
    u8 clone_id[16] = {0};
    u8 *object = NULL;
    u64 logical_blocks;
    u64 max_nodes;
    u64 visited = 0;
    u32 batch_count = 0;
    u32 batch_capacity;
    int ret = 0;

    memset(clone_file->checksum_head_id, 0,
           sizeof(clone_file->checksum_head_id));
    memcpy(source_id, source_file->checksum_head_id, sizeof(source_id));
    if (!memchr_inv(source_id, 0, sizeof(source_id)))
        return 0;

    logical_blocks = DIV_ROUND_UP_ULL(
        le64_to_cpu(source_file->attributes.logical_size),
        INFILFS_DISK_BLOCK_SIZE);
    max_nodes = DIV_ROUND_UP_ULL(
        logical_blocks, INFILFS_NATIVE_CHECKSUMS_PER_OBJECT);
    if (!max_nodes)
        return -EFSCORRUPTED;

    batch_capacity = (u32)min_t(
        u64, max_nodes, INFILFS_REFLINK_INDEX_BATCH);
    changes = kvmalloc_array(
        batch_capacity, sizeof(*changes), GFP_NOFS);
    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!changes || !object) {
        ret = -ENOMEM;
        goto out;
    }

    ret = infilfs_native_random_id(clone_id);
    if (ret)
        goto out;
    memcpy(clone_file->checksum_head_id, clone_id, 16);

    while (memchr_inv(source_id, 0, sizeof(source_id))) {
        struct infilfs_object_header_disk *header;
        struct infilfs_native_checksum_payload_disk *payload;
        u8 next_source_id[16];
        u8 next_clone_id[16] = {0};
        u64 source_block;
        u64 clone_block;
        u16 type;

        if (visited >= max_nodes) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        ret = infilfs_index_lookup(
            pending->sb, source_id, &source_block, &type);
        if (ret)
            goto out;
        if (type != INFILFS_OBJECT_CHECKSUM) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        ret = infilfs_native_checksum_decode(
            pending->sb, source_owner, source_id,
            source_block, object, &payload);
        if (ret)
            goto out;
        memcpy(next_source_id, payload->next_object_id, 16);
        if (memchr_inv(next_source_id, 0, sizeof(next_source_id))) {
            ret = infilfs_native_random_id(next_clone_id);
            if (ret)
                goto out;
        }

        header = (struct infilfs_object_header_disk *)object;
        header->generation = cpu_to_le64(pending->tx.generation);
        memcpy(header->object_id, clone_id, 16);
        memcpy(header->parent_id, clone_owner, 16);
        memcpy(payload->owner_object_id, clone_owner, 16);
        memcpy(payload->next_object_id, next_clone_id, 16);
        ret = infilfs_rw_finalize_object(object);
        if (ret)
            goto out;
        ret = infilfs_rw_tx_alloc(&pending->tx, 1, &clone_block);
        if (ret)
            goto out;
        ret = infilfs_native_stage_block(
            pending->sb, clone_block, object);
        if (ret)
            goto out;

        memset(&changes[batch_count], 0, sizeof(changes[batch_count]));
        memcpy(changes[batch_count].object_id, clone_id, 16);
        changes[batch_count].object_block = clone_block;
        changes[batch_count].object_type = INFILFS_OBJECT_CHECKSUM;
        changes[batch_count].add = true;
        batch_count++;
        visited++;

        if (batch_count == batch_capacity) {
            ret = infilfs_native_index_update(
                pending, changes, batch_count);
            if (ret)
                goto out;
            batch_count = 0;
        }
        memcpy(source_id, next_source_id, 16);
        memcpy(clone_id, next_clone_id, 16);
    }

    if (!visited) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    if (batch_count)
        ret = infilfs_native_index_update(
            pending, changes, batch_count);
out:
    if (ret)
        memset(clone_file->checksum_head_id, 0,
               sizeof(clone_file->checksum_head_id));
    kfree(object);
    kvfree(changes);
    return ret;
}
