// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"

#include <linux/random.h>

/*
 * Persistent native checksum-chain storage.
 *
 * The bounded checksum caches and writer-tail locator are rebuildable
 * accelerators. This object owns the authoritative checksum-object chain:
 * validation, sparse group insertion, copy-on-write mutation and tail growth.
 * Keeping that persistent metadata machinery out of the RW compositor makes
 * the boundary between on-disk integrity state and volatile lookup hints
 * explicit.
 */

struct infilfs_native_checksum_node {
    u8 object_id[16];
    u64 object_block;
    u64 start_logical;
    u32 checksum_count;
    bool new_node;
    bool dirty;
};

static int infilfs_native_random_id(u8 id[16])
{
    unsigned int attempt;

    for (attempt = 0; attempt < 8; ++attempt) {
        u8 combined = 0;
        unsigned int i;
        get_random_bytes(id, 16);
        for (i = 0; i < 16; ++i)
            combined |= id[i];
        if (combined)
            return 0;
    }
    return -EAGAIN;
}

int infilfs_native_checksum_decode(
    struct super_block *sb, const u8 owner_id[16], const u8 object_id[16],
    u64 object_block, u8 block[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_native_checksum_payload_disk **payload_out)
{
    struct infilfs_object_header_disk *header;
    struct infilfs_native_checksum_payload_disk *payload;
    u32 count;
    size_t need;
    int ret;

    ret = infilfs_read_object(sb, object_block, INFILFS_OBJECT_CHECKSUM,
                              object_id, block);
    if (ret)
        return ret;
    header = (struct infilfs_object_header_disk *)block;
    if (le16_to_cpu(header->object_version) != INFILFS_OBJECT_VERSION_CLASSIC ||
        le32_to_cpu(header->payload_size) < sizeof(*payload))
        return -EFSCORRUPTED;
    payload = (struct infilfs_native_checksum_payload_disk *)(header + 1);
    count = le32_to_cpu(payload->checksum_count);
    need = sizeof(*payload) + (size_t)count *
        sizeof(struct infilfs_data_checksum_disk);
    if (memcmp(payload->owner_object_id, owner_id, 16) != 0 ||
        le32_to_cpu(payload->reserved) != 0 ||
        count > INFILFS_NATIVE_CHECKSUMS_PER_OBJECT ||
        need != le32_to_cpu(header->payload_size))
        return -EFSCORRUPTED;
    *payload_out = payload;
    return 0;
}

static int infilfs_native_checksum_find_group(
    struct super_block *sb, const u8 owner_id[16], const u8 head_id[16],
    u64 group_start, struct infilfs_native_checksum_cache_entry *out)
{
    struct infilfs_native_checksum_cache_entry cached;
    struct infilfs_native_checksum_payload_disk *payload;
    u8 current_id[16];
    u8 *object;
    u64 previous_start = 0;
    bool have_previous = false;
    unsigned int guard;
    int ret;

    if (!memchr_inv(head_id, 0, 16))
        return -ENOENT;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;

    if (infilfs_native_checksum_group_cache_lookup(
            sb, owner_id, group_start, &cached)) {
        u64 indexed_block = 0;
        u16 indexed_type = 0;

        ret = infilfs_index_lookup(sb, cached.object_id, &indexed_block,
                                   &indexed_type);
        if (!ret && indexed_type == INFILFS_OBJECT_CHECKSUM &&
            indexed_block == cached.object_block) {
            ret = infilfs_native_checksum_decode(
                sb, owner_id, cached.object_id, cached.object_block,
                object, &payload);
            if (!ret &&
                le64_to_cpu(payload->start_logical_block) == group_start) {
                if (out)
                    *out = cached;
                kfree(object);
                return 0;
            }
        }
    }

    memcpy(current_id, head_id, sizeof(current_id));
    for (guard = 0; guard < 1048576u &&
         memchr_inv(current_id, 0, sizeof(current_id)); ++guard) {
        struct infilfs_native_checksum_cache_entry found;
        u8 next_id[16];
        u64 block;
        u64 start;
        u16 type;

        ret = infilfs_index_lookup(sb, current_id, &block, &type);
        if (ret)
            goto out;
        if (type != INFILFS_OBJECT_CHECKSUM) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        ret = infilfs_native_checksum_decode(
            sb, owner_id, current_id, block, object, &payload);
        if (ret)
            goto out;
        start = le64_to_cpu(payload->start_logical_block);
        if ((start % INFILFS_NATIVE_CHECKSUMS_PER_OBJECT) != 0 ||
            (have_previous && start <= previous_start)) {
            ret = -EFSCORRUPTED;
            goto out;
        }

        infilfs_native_checksum_group_cache_store(
            sb, owner_id, current_id, block, start);
        if (start == group_start) {
            memset(&found, 0, sizeof(found));
            found.sb = sb;
            memcpy(found.owner_id, owner_id, 16);
            memcpy(found.object_id, current_id, 16);
            found.object_block = block;
            found.start_logical = start;
            found.valid = true;
            if (out)
                *out = found;
            ret = 0;
            goto out;
        }
        if (start > group_start) {
            ret = -ENOENT;
            goto out;
        }

        memcpy(next_id, payload->next_object_id, sizeof(next_id));
        previous_start = start;
        have_previous = true;
        memcpy(current_id, next_id, sizeof(current_id));
    }
    ret = -ENOENT;
out:
    kfree(object);
    return ret;
}

static int infilfs_native_checksum_tail(
    struct super_block *sb, const u8 owner_id[16], const u8 head_id[16],
    u8 tail_id[16], u64 *tail_block_out, u64 *tail_start_out,
    u8 tail_object[INFILFS_DISK_BLOCK_SIZE])
{
    struct infilfs_native_checksum_cache_entry cached;
    struct infilfs_native_checksum_payload_disk *payload;
    u8 current_id[16];
    u64 block = 0;
    u64 previous_start = 0;
    bool have_previous = false;
    unsigned int guard;
    int ret;

    if (!memcmp(head_id, (u8[16]){0}, 16))
        return -ENOENT;

    if (infilfs_native_checksum_cache_lookup(sb, owner_id, &cached)) {
        u64 indexed_block = 0;
        u16 indexed_type = 0;

        memcpy(current_id, cached.object_id, 16);
        block = cached.object_block;
        /*
         * Readers and appenders deliberately share this bounded cache.  A
         * reader that started before a checksum-tail CoW may publish the old
         * physical block after the writer has cached the replacement.  The
         * old block remains structurally valid, so decoding it alone is not a
         * sufficient freshness check.  Require the live object index to map
         * the cached object ID to the same block before the append path trusts
         * it; otherwise restart from the authoritative checksum head.
         */
        ret = infilfs_index_lookup(sb, current_id, &indexed_block,
                                   &indexed_type);
        if (!ret && (indexed_type != INFILFS_OBJECT_CHECKSUM ||
                     indexed_block != block))
            ret = -ESTALE;
        if (!ret)
            ret = infilfs_native_checksum_decode(sb, owner_id, current_id,
                                                  block, tail_object,
                                                  &payload);
        if (ret) {
            memcpy(current_id, head_id, 16);
            block = 0;
        } else {
            previous_start = le64_to_cpu(payload->start_logical_block);
            have_previous = true;
        }
    } else {
        memcpy(current_id, head_id, 16);
    }

    for (guard = 0; guard < 1048576u; ++guard) {
        u64 start;
        u8 next_id[16];
        u16 type;

        if (!block) {
            ret = infilfs_index_lookup(sb, current_id, &block, &type);
            if (ret)
                return ret;
            if (type != INFILFS_OBJECT_CHECKSUM)
                return -EFSCORRUPTED;
            ret = infilfs_native_checksum_decode(sb, owner_id, current_id,
                                                  block, tail_object, &payload);
            if (ret)
                return ret;
        }
        start = le64_to_cpu(payload->start_logical_block);
        if ((start % INFILFS_NATIVE_CHECKSUMS_PER_OBJECT) != 0 ||
            (have_previous && start < previous_start))
            return -EFSCORRUPTED;
        memcpy(next_id, payload->next_object_id, 16);
        if (!memcmp(next_id, (u8[16]){0}, 16)) {
            memcpy(tail_id, current_id, 16);
            *tail_block_out = block;
            *tail_start_out = start;
            return 0;
        }
        previous_start = start;
        have_previous = true;
        memcpy(current_id, next_id, 16);
        block = 0;
    }
    return -ELOOP;
}

static int infilfs_native_init_checksum_object(
    struct infilfs_native_pending *pending, const u8 owner_id[16],
    const u8 object_id[16], const u8 next_id[16], u64 group_start,
    const struct infilfs_data_checksum_disk *digests, u64 touched_start,
    u64 touched_count, u8 block[INFILFS_DISK_BLOCK_SIZE])
{
    struct infilfs_object_header_disk *header;
    struct infilfs_native_checksum_payload_disk *payload;
    struct infilfs_data_checksum_disk *values;
    u64 first;
    u64 last;
    u32 count;

    memset(block, 0, INFILFS_DISK_BLOCK_SIZE);
    header = (struct infilfs_object_header_disk *)block;
    memcpy(header->magic, infilfs_object_magic, 8);
    header->object_type = cpu_to_le16(INFILFS_OBJECT_CHECKSUM);
    header->object_version = cpu_to_le16(INFILFS_OBJECT_VERSION_CLASSIC);
    header->header_size = cpu_to_le32(sizeof(*header));
    header->generation = cpu_to_le64(pending->tx.generation);
    memcpy(header->object_id, object_id, 16);
    memcpy(header->parent_id, owner_id, 16);
    header->checksum_type = cpu_to_le32(INFILFS_CHECKSUM_CRC64_ECMA);
    payload = (struct infilfs_native_checksum_payload_disk *)(header + 1);
    memcpy(payload->owner_object_id, owner_id, 16);
    if (next_id)
        memcpy(payload->next_object_id, next_id, 16);
    payload->start_logical_block = cpu_to_le64(group_start);
    values = (struct infilfs_data_checksum_disk *)(payload + 1);

    first = max_t(u64, group_start, touched_start);
    last = min_t(u64, group_start + INFILFS_NATIVE_CHECKSUMS_PER_OBJECT,
                 touched_start + touched_count);
    if (last <= first)
        return -EINVAL;
    memcpy(values + (first - group_start), digests + (first - touched_start),
           (size_t)(last - first) * sizeof(*values));
    count = (u32)(last - group_start);
    payload->checksum_count = cpu_to_le32(count);
    header->payload_size = cpu_to_le32(sizeof(*payload) +
        (size_t)count * sizeof(*values));
    return infilfs_rw_finalize_object(block);
}

static int infilfs_native_checksum_collect(
    struct infilfs_native_pending *pending, const u8 owner_id[16],
    const u8 head_id[16], struct infilfs_native_checksum_node **nodes_out,
    u32 *count_out)
{
    struct infilfs_native_checksum_node *nodes = NULL;
    u32 count = 0;
    u32 capacity = 0;
    u8 current_id[16];
    u64 previous_start = 0;
    bool have_previous = false;
    unsigned int guard;
    int ret = 0;

    *nodes_out = NULL;
    *count_out = 0;
    memcpy(current_id, head_id, sizeof(current_id));
    for (guard = 0; memchr_inv(current_id, 0, sizeof(current_id)) &&
         guard < 1048576u; ++guard) {
        struct infilfs_native_checksum_payload_disk *payload;
        struct infilfs_native_checksum_node *grown;
        u8 *object;
        u8 next_id[16];
        u64 block;
        u64 start;
        u32 checksum_count;
        u16 type;

        ret = infilfs_index_lookup(pending->sb, current_id, &block, &type);
        if (ret)
            goto out;
        if (type != INFILFS_OBJECT_CHECKSUM) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!object) {
            ret = -ENOMEM;
            goto out;
        }
        ret = infilfs_native_checksum_decode(pending->sb, owner_id,
                                             current_id, block, object,
                                             &payload);
        if (!ret) {
            start = le64_to_cpu(payload->start_logical_block);
            checksum_count = le32_to_cpu(payload->checksum_count);
            memcpy(next_id, payload->next_object_id, sizeof(next_id));
            if ((start % INFILFS_NATIVE_CHECKSUMS_PER_OBJECT) != 0 ||
                (have_previous && start <= previous_start) ||
                !checksum_count)
                ret = -EFSCORRUPTED;
        }
        kfree(object);
        if (ret)
            goto out;

        if (count == capacity) {
            u32 new_capacity = capacity ? capacity * 2u : 16u;

            grown = kvmalloc_array(new_capacity, sizeof(*grown), GFP_NOFS);
            if (!grown) {
                ret = -ENOMEM;
                goto out;
            }
            if (count)
                memcpy(grown, nodes, (size_t)count * sizeof(*grown));
            kvfree(nodes);
            nodes = grown;
            capacity = new_capacity;
        }
        memset(&nodes[count], 0, sizeof(nodes[count]));
        memcpy(nodes[count].object_id, current_id, 16);
        nodes[count].object_block = block;
        nodes[count].start_logical = start;
        nodes[count].checksum_count = checksum_count;
        count++;
        previous_start = start;
        have_previous = true;
        memcpy(current_id, next_id, sizeof(current_id));
    }
    if (memchr_inv(current_id, 0, sizeof(current_id))) {
        ret = -ELOOP;
        goto out;
    }
    *nodes_out = nodes;
    *count_out = count;
    return 0;
out:
    kvfree(nodes);
    return ret;
}

static int infilfs_native_checksum_insert_node(
    struct infilfs_native_checksum_node **nodes_inout, u32 *count_inout,
    u32 index, u64 start)
{
    struct infilfs_native_checksum_node *old = *nodes_inout;
    struct infilfs_native_checksum_node *grown;
    u32 count = *count_inout;
    int ret;

    grown = kvmalloc_array((size_t)count + 1u, sizeof(*grown), GFP_NOFS);
    if (!grown)
        return -ENOMEM;
    if (index)
        memcpy(grown, old, (size_t)index * sizeof(*grown));
    if (index < count)
        memcpy(grown + index + 1u, old + index,
               (size_t)(count - index) * sizeof(*grown));
    memset(&grown[index], 0, sizeof(grown[index]));
    ret = infilfs_native_random_id(grown[index].object_id);
    if (ret) {
        kvfree(grown);
        return ret;
    }
    grown[index].start_logical = start;
    grown[index].new_node = true;
    grown[index].dirty = true;
    if (index)
        grown[index - 1u].dirty = true;
    kvfree(old);
    *nodes_inout = grown;
    *count_inout = count + 1u;
    return 0;
}

/*
 * Apply checksum changes to any logical range, not merely to the current tail.
 * The chain is ordered by checksum group.  Existing affected objects are
 * copy-on-written, missing sparse groups are inserted in order, and the one
 * predecessor whose next pointer changes is rewritten in the same operation.
 */
int infilfs_native_checksum_update_existing_group(
    struct infilfs_native_pending *pending,
    struct infilfs_file_payload_disk *file, const u8 owner_id[16],
    u64 touched_start, u64 touched_count,
    const struct infilfs_data_checksum_disk *digests,
    struct infilfs_native_index_change *changes, u32 change_capacity,
    u32 *change_count, u8 final_tail_id[16], u64 *final_tail_block,
    u64 *final_tail_start)
{
    const u64 per = INFILFS_NATIVE_CHECKSUMS_PER_OBJECT;
    struct infilfs_native_checksum_cache_entry target;
    struct infilfs_native_checksum_payload_disk *payload;
    struct infilfs_data_checksum_disk *values;
    u8 tail_id[16];
    u8 *tail_object = NULL;
    u8 *object = NULL;
    u64 tail_block = 0;
    u64 tail_start = 0;
    u64 group_start;
    u64 first;
    u64 last;
    u64 stored_block;
    u32 needed;
    int ret;

    if (!touched_count || touched_start > U64_MAX - touched_count)
        return -EINVAL;
    group_start = (touched_start / per) * per;
    if (((touched_start + touched_count - 1u) / per) * per != group_start)
        return -ENOENT;
    if (*change_count >= change_capacity)
        return -ENOSPC;

    /*
     * Preserve the authoritative tail before changing a checksum object.
     * In the steady state this is one validated cache lookup; if the cache was
     * lost, the general tail walker repopulates it once.
     */
    tail_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!tail_object || !object) {
        ret = -ENOMEM;
        goto out;
    }
    ret = infilfs_native_checksum_tail(
        pending->sb, owner_id, file->checksum_head_id,
        tail_id, &tail_block, &tail_start, tail_object);
    if (ret)
        goto out;

    ret = infilfs_native_checksum_find_group(
        pending->sb, owner_id, file->checksum_head_id,
        group_start, &target);
    if (ret)
        goto out;

    ret = infilfs_native_checksum_decode(
        pending->sb, owner_id, target.object_id,
        target.object_block, object, &payload);
    if (ret)
        goto out;
    if (le64_to_cpu(payload->start_logical_block) != group_start) {
        ret = -EFSCORRUPTED;
        goto out;
    }

    first = touched_start - group_start;
    last = first + touched_count;
    if (last > per) {
        ret = -EOVERFLOW;
        goto out;
    }
    needed = max_t(u32, le32_to_cpu(payload->checksum_count), (u32)last);
    values = (struct infilfs_data_checksum_disk *)(payload + 1);
    memcpy(values + first, digests,
           (size_t)touched_count * sizeof(*values));
    payload->checksum_count = cpu_to_le32(needed);
    ((struct infilfs_object_header_disk *)object)->payload_size =
        cpu_to_le32(sizeof(*payload) + (size_t)needed * sizeof(*values));
    ((struct infilfs_object_header_disk *)object)->generation =
        cpu_to_le64(pending->tx.generation);
    ret = infilfs_rw_finalize_object(object);
    if (ret)
        goto out;

    ret = infilfs_native_store_private_or_cow(
        pending, target.object_block, object, &stored_block);
    if (ret)
        goto out;

    memcpy(changes[*change_count].object_id, target.object_id, 16);
    changes[*change_count].object_block = stored_block;
    changes[*change_count].object_type = INFILFS_OBJECT_CHECKSUM;
    changes[*change_count].add = false;
    (*change_count)++;

    infilfs_native_checksum_group_cache_store(
        pending->sb, owner_id, target.object_id, stored_block, group_start);
    if (memcmp(tail_id, target.object_id, 16) == 0)
        tail_block = stored_block;
    memcpy(final_tail_id, tail_id, 16);
    *final_tail_block = tail_block;
    *final_tail_start = tail_start;
    ret = 0;
out:
    kfree(object);
    kfree(tail_object);
    return ret;
}

static int infilfs_native_checksum_append_tail(
    struct infilfs_native_pending *pending,
    struct infilfs_file_payload_disk *file, const u8 owner_id[16],
    u64 touched_start, u64 touched_count,
    const struct infilfs_data_checksum_disk *digests,
    struct infilfs_native_index_change *changes, u32 change_capacity,
    u32 *change_count, u8 final_tail_id[16], u64 *final_tail_block,
    u64 *final_tail_start)
{
    const u64 per = INFILFS_NATIVE_CHECKSUMS_PER_OBJECT;
    struct infilfs_native_checksum_node *new_nodes = NULL;
    struct infilfs_native_writer_tail cached_tail;
    struct infilfs_native_checksum_payload_disk *tail_payload = NULL;
    struct infilfs_data_checksum_disk *tail_values;
    u8 *tail_object = NULL;
    u64 touched_end;
    u64 tail_block = 0;
    u64 tail_start = 0;
    u32 tail_count = 0;
    u32 fill = 0;
    u32 new_count = 0;
    u32 i;
    int ret = 0;

    if (!touched_count || touched_start > U64_MAX - touched_count)
        return -EINVAL;
    touched_end = touched_start + touched_count;

    /*
     * Empty files are the simplest append case: create a short forward chain
     * containing only the checksum groups covered by this write.
     */
    if (!memchr_inv(file->checksum_head_id, 0,
                    sizeof(file->checksum_head_id))) {
        u64 group;

        if (touched_start != 0)
            return -EOPNOTSUPP;
        new_count = (u32)DIV_ROUND_UP_ULL(touched_count, per);
        if (!new_count || *change_count > change_capacity ||
            new_count > change_capacity - *change_count)
            return -ENOSPC;
        new_nodes = kvmalloc_array(new_count, sizeof(*new_nodes), GFP_NOFS);
        tail_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!new_nodes || !tail_object) {
            ret = -ENOMEM;
            goto out;
        }
        memset(new_nodes, 0, (size_t)new_count * sizeof(*new_nodes));
        group = 0;
        for (i = 0; i < new_count; ++i) {
            ret = infilfs_native_random_id(new_nodes[i].object_id);
            if (ret)
                goto out;
            new_nodes[i].start_logical = group;
            if (group > U64_MAX - per && i + 1u < new_count) {
                ret = -EOVERFLOW;
                goto out;
            }
            group += per;
        }
        for (i = 0; i < new_count; ++i) {
            const u8 *next_id = i + 1u < new_count ?
                new_nodes[i + 1u].object_id : NULL;
            u64 stored_block;

            ret = infilfs_native_init_checksum_object(
                pending, owner_id, new_nodes[i].object_id, next_id,
                new_nodes[i].start_logical, digests, touched_start,
                touched_count, tail_object);
            if (ret)
                goto out;
            ret = infilfs_rw_tx_alloc(&pending->tx, 1, &stored_block);
            if (ret)
                goto out;
            ret = infilfs_native_stage_block(
                pending->sb, stored_block, tail_object);
            if (ret)
                goto out;

            memcpy(changes[*change_count].object_id,
                   new_nodes[i].object_id, 16);
            changes[*change_count].object_block = stored_block;
            changes[*change_count].object_type = INFILFS_OBJECT_CHECKSUM;
            changes[*change_count].add = true;
            (*change_count)++;
            new_nodes[i].object_block = stored_block;
            infilfs_native_checksum_group_cache_store(
                pending->sb, owner_id, new_nodes[i].object_id,
                stored_block, new_nodes[i].start_logical);
        }
        memcpy(file->checksum_head_id, new_nodes[0].object_id, 16);
        memcpy(final_tail_id, new_nodes[new_count - 1u].object_id, 16);
        *final_tail_block = new_nodes[new_count - 1u].object_block;
        *final_tail_start = new_nodes[new_count - 1u].start_logical;
        ret = 0;
        goto out;
    }

    tail_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!tail_object)
        return -ENOMEM;

    /*
     * The writer-only tail cache is authoritative while write_lock is held:
     * readers never publish into it, namespace mutations invalidate it, and
     * rollback clears it.  A cache miss pays the historical walk once, then
     * all subsequent sequential appends stay at the tail.
     */
    if (infilfs_native_writer_tail_lookup(
            pending, owner_id, &cached_tail)) {
        ret = infilfs_native_checksum_decode(
            pending->sb, owner_id, cached_tail.object_id,
            cached_tail.object_block, tail_object, &tail_payload);
        if (!ret &&
            le64_to_cpu(tail_payload->start_logical_block) ==
                cached_tail.start_logical &&
            !memchr_inv(tail_payload->next_object_id, 0,
                        sizeof(tail_payload->next_object_id))) {
            memcpy(final_tail_id, cached_tail.object_id, 16);
            tail_block = cached_tail.object_block;
            tail_start = cached_tail.start_logical;
        } else {
            infilfs_native_writer_tail_invalidate(pending);
            tail_payload = NULL;
        }
    }

    if (!tail_payload) {
        ret = infilfs_native_checksum_tail(
            pending->sb, owner_id, file->checksum_head_id,
            final_tail_id, &tail_block, &tail_start, tail_object);
        if (ret)
            goto out;
        ret = infilfs_native_checksum_decode(
            pending->sb, owner_id, final_tail_id, tail_block,
            tail_object, &tail_payload);
        if (ret)
            goto out;
        infilfs_native_writer_tail_store(
            pending, owner_id, final_tail_id, tail_block, tail_start);
    }

    tail_count = le32_to_cpu(tail_payload->checksum_count);
    if (!tail_count || tail_count > per ||
        touched_start != tail_start + tail_count ||
        touched_end <= touched_start) {
        ret = -EOPNOTSUPP;
        goto out;
    }

    fill = min_t(u64, touched_count, per - tail_count);
    if (touched_count > fill)
        new_count = (u32)DIV_ROUND_UP_ULL(touched_count - fill, per);
    if (*change_count > change_capacity ||
        1u + new_count > change_capacity - *change_count) {
        ret = -ENOSPC;
        goto out;
    }

    if (new_count) {
        u64 group = tail_start + per;

        new_nodes = kvmalloc_array(new_count, sizeof(*new_nodes), GFP_NOFS);
        if (!new_nodes) {
            ret = -ENOMEM;
            goto out;
        }
        memset(new_nodes, 0, (size_t)new_count * sizeof(*new_nodes));
        for (i = 0; i < new_count; ++i) {
            ret = infilfs_native_random_id(new_nodes[i].object_id);
            if (ret)
                goto out;
            new_nodes[i].start_logical = group;
            if (group > U64_MAX - per && i + 1u < new_count) {
                ret = -EOVERFLOW;
                goto out;
            }
            group += per;
        }
    }

    tail_values = (struct infilfs_data_checksum_disk *)(tail_payload + 1);
    if (fill) {
        memcpy(tail_values + tail_count, digests,
               (size_t)fill * sizeof(*tail_values));
        tail_count += fill;
        tail_payload->checksum_count = cpu_to_le32(tail_count);
    }
    memset(tail_payload->next_object_id, 0,
           sizeof(tail_payload->next_object_id));
    if (new_count)
        memcpy(tail_payload->next_object_id, new_nodes[0].object_id, 16);
    ((struct infilfs_object_header_disk *)tail_object)->payload_size =
        cpu_to_le32(sizeof(*tail_payload) +
                    (size_t)tail_count * sizeof(*tail_values));
    ((struct infilfs_object_header_disk *)tail_object)->generation =
        cpu_to_le64(pending->tx.generation);
    ret = infilfs_rw_finalize_object(tail_object);
    if (ret)
        goto out;

    {
        u64 stored_block;

        ret = infilfs_native_store_private_or_cow(
            pending, tail_block, tail_object, &stored_block);
        if (ret)
            goto out;
        memcpy(changes[*change_count].object_id, final_tail_id, 16);
        changes[*change_count].object_block = stored_block;
        changes[*change_count].object_type = INFILFS_OBJECT_CHECKSUM;
        changes[*change_count].add = false;
        (*change_count)++;
        tail_block = stored_block;
        infilfs_native_checksum_group_cache_store(
            pending->sb, owner_id, final_tail_id, stored_block, tail_start);
    }

    for (i = 0; i < new_count; ++i) {
        const u8 *next_id = i + 1u < new_count ?
            new_nodes[i + 1u].object_id : NULL;
        u64 stored_block;

        ret = infilfs_native_init_checksum_object(
            pending, owner_id, new_nodes[i].object_id, next_id,
            new_nodes[i].start_logical, digests, touched_start,
            touched_count, tail_object);
        if (ret)
            goto out;
        ret = infilfs_rw_tx_alloc(&pending->tx, 1, &stored_block);
        if (ret)
            goto out;
        ret = infilfs_native_stage_block(
            pending->sb, stored_block, tail_object);
        if (ret)
            goto out;

        memcpy(changes[*change_count].object_id,
               new_nodes[i].object_id, 16);
        changes[*change_count].object_block = stored_block;
        changes[*change_count].object_type = INFILFS_OBJECT_CHECKSUM;
        changes[*change_count].add = true;
        (*change_count)++;
        new_nodes[i].object_block = stored_block;
        infilfs_native_checksum_group_cache_store(
            pending->sb, owner_id, new_nodes[i].object_id,
            stored_block, new_nodes[i].start_logical);
    }

    if (new_count) {
        memcpy(final_tail_id, new_nodes[new_count - 1u].object_id, 16);
        *final_tail_block = new_nodes[new_count - 1u].object_block;
        *final_tail_start = new_nodes[new_count - 1u].start_logical;
    } else {
        *final_tail_block = tail_block;
        *final_tail_start = tail_start;
    }
    ret = 0;
out:
    kvfree(new_nodes);
    kfree(tail_object);
    return ret;
}

int infilfs_native_checksum_set_range(
    struct infilfs_native_pending *pending,
    struct infilfs_file_payload_disk *file, const u8 owner_id[16],
    u64 touched_start, u64 touched_count,
    const struct infilfs_data_checksum_disk *digests,
    struct infilfs_native_index_change *changes, u32 change_capacity,
    u32 *change_count, u8 final_tail_id[16], u64 *final_tail_block,
    u64 *final_tail_start)
{
    const u64 per = INFILFS_NATIVE_CHECKSUMS_PER_OBJECT;
    struct infilfs_native_checksum_node *nodes = NULL;
    u8 *object = NULL;
    u64 first_group;
    u64 last_group;
    u64 group;
    u32 node_count = 0;
    u32 i;
    int ret = 0;

    if (!touched_count || touched_start > U64_MAX - touched_count)
        return -EINVAL;
    first_group = (touched_start / per) * per;
    last_group = ((touched_start + touched_count - 1u) / per) * per;

    if (first_group == last_group) {
        ret = infilfs_native_checksum_update_existing_group(
            pending, file, owner_id, touched_start, touched_count, digests,
            changes, change_capacity, change_count, final_tail_id,
            final_tail_block, final_tail_start);
        if (!ret)
            return 0;
        if (ret != -ENOENT)
            return ret;
    }

    ret = infilfs_native_checksum_collect(pending, owner_id,
                                          file->checksum_head_id,
                                          &nodes, &node_count);
    if (ret)
        return ret;

    for (group = first_group; ; group += per) {
        u32 at = 0;

        while (at < node_count && nodes[at].start_logical < group)
            ++at;
        if (at == node_count || nodes[at].start_logical != group) {
            ret = infilfs_native_checksum_insert_node(&nodes, &node_count,
                                                      at, group);
            if (ret)
                goto out;
        }
        nodes[at].dirty = true;
        if (group == last_group)
            break;
        if (group > U64_MAX - per) {
            ret = -EOVERFLOW;
            goto out;
        }
    }

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object) {
        ret = -ENOMEM;
        goto out;
    }

    for (i = 0; i < node_count; ++i) {
        struct infilfs_native_checksum_payload_disk *payload;
        struct infilfs_data_checksum_disk *values;
        const u8 *next_id = i + 1u < node_count ?
            nodes[i + 1u].object_id : NULL;
        u64 group_start = nodes[i].start_logical;
        u64 first = max_t(u64, group_start, touched_start);
        u64 last = min_t(u64, group_start + per,
                         touched_start + touched_count);
        u64 stored_block;
        u32 needed = nodes[i].checksum_count;

        if (!nodes[i].dirty)
            continue;
        if (nodes[i].new_node) {
            needed = (u32)(last - group_start);
            memset(object, 0, INFILFS_DISK_BLOCK_SIZE);
            ret = infilfs_native_init_checksum_object(
                pending, owner_id, nodes[i].object_id, next_id, group_start,
                digests, touched_start, touched_count, object);
            if (ret)
                goto out;
            ret = infilfs_rw_tx_alloc(&pending->tx, 1, &stored_block);
            if (ret)
                goto out;
            ret = infilfs_native_stage_block(pending->sb, stored_block,
                                             object);
            if (ret)
                goto out;
        } else {
            ret = infilfs_native_checksum_decode(
                pending->sb, owner_id, nodes[i].object_id,
                nodes[i].object_block, object, &payload);
            if (ret)
                goto out;
            values = (struct infilfs_data_checksum_disk *)(payload + 1);
            if (last > first) {
                memcpy(values + (first - group_start),
                       digests + (first - touched_start),
                       (size_t)(last - first) * sizeof(*values));
                needed = max_t(u32, needed,
                               (u32)(last - group_start));
            }
            memset(payload->next_object_id, 0,
                   sizeof(payload->next_object_id));
            if (next_id)
                memcpy(payload->next_object_id, next_id, 16);
            payload->checksum_count = cpu_to_le32(needed);
            ((struct infilfs_object_header_disk *)object)->payload_size =
                cpu_to_le32(sizeof(*payload) +
                    (size_t)needed * sizeof(*values));
            ((struct infilfs_object_header_disk *)object)->generation =
                cpu_to_le64(pending->tx.generation);
            ret = infilfs_rw_finalize_object(object);
            if (ret)
                goto out;
            ret = infilfs_native_store_private_or_cow(
                pending, nodes[i].object_block, object, &stored_block);
            if (ret)
                goto out;
        }
        if (*change_count >= change_capacity) {
            ret = -ENOSPC;
            goto out;
        }
        memcpy(changes[*change_count].object_id, nodes[i].object_id, 16);
        changes[*change_count].object_block = stored_block;
        changes[*change_count].object_type = INFILFS_OBJECT_CHECKSUM;
        changes[*change_count].add = nodes[i].new_node;
        (*change_count)++;
        nodes[i].object_block = stored_block;
        nodes[i].checksum_count = needed;
        infilfs_native_checksum_group_cache_store(
            pending->sb, owner_id, nodes[i].object_id,
            stored_block, group_start);
    }

    if (!node_count) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    memcpy(file->checksum_head_id, nodes[0].object_id, 16);
    memcpy(final_tail_id, nodes[node_count - 1u].object_id, 16);
    *final_tail_block = nodes[node_count - 1u].object_block;
    *final_tail_start = nodes[node_count - 1u].start_logical;
out:
    kfree(object);
    kvfree(nodes);
    return ret;
}

int infilfs_native_checksum_append(
    struct infilfs_native_pending *pending,
    struct infilfs_file_payload_disk *file, const u8 owner_id[16],
    u64 touched_start, u64 touched_count,
    const struct infilfs_data_checksum_disk *digests,
    struct infilfs_native_index_change *changes, u32 change_capacity,
    u32 *change_count, u8 final_tail_id[16], u64 *final_tail_block,
    u64 *final_tail_start)
{
    return infilfs_native_checksum_set_range(
        pending, file, owner_id, touched_start, touched_count, digests,
        changes, change_capacity, change_count, final_tail_id,
        final_tail_block, final_tail_start);
}

