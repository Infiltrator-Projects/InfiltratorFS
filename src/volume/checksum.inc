// SPDX-License-Identifier: GPL-3.0-or-later
/* Data integrity helpers. Data checksums are stored in hidden,
 * copy-on-write checksum objects addressed through the persistent object
 * index. Each checksum authenticates one complete 4096-byte logical block. */

static void data_block_checksum(const uint8_t block[INFS_BLOCK_SIZE],
                                struct infs_data_checksum_disk *out)
{
    infs_sha256(block, INFS_BLOCK_SIZE, out->bytes);
}

static int checksum_validate(uint8_t block[INFS_BLOCK_SIZE],
                             const uint8_t owner_id[16],
                             struct infs_checksum_payload_disk **payload_out,
                             struct infs_data_checksum_disk **checksums_out)
{
    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
    if (!infs_validate_object_block(block) ||
        infs_le16_to_cpu(hdr->object_type) != INFS_OBJECT_CHECKSUM ||
        infs_le32_to_cpu(hdr->payload_size) < sizeof(struct infs_checksum_payload_disk)) {
        return INFS_STATUS_CORRUPT;
    }
    struct infs_checksum_payload_disk *p =
        (struct infs_checksum_payload_disk *)(block + sizeof(*hdr));
    uint32_t count = infs_le32_to_cpu(p->checksum_count);
    if (count > INFS_CHECKSUMS_PER_OBJECT)
        return INFS_STATUS_CORRUPT;
    size_t need = sizeof(*p) + (size_t)count * sizeof(struct infs_data_checksum_disk);
    if (need != infs_le32_to_cpu(hdr->payload_size) ||
        need > INFS_BLOCK_SIZE - sizeof(*hdr) ||
        infs_le32_to_cpu(p->reserved) != 0 || id_is_zero(p->owner_object_id)) {
        return INFS_STATUS_CORRUPT;
    }
    if (owner_id && memcmp(p->owner_object_id, owner_id, 16) != 0) {
        return INFS_STATUS_CORRUPT;
    }
    if (payload_out)
        *payload_out = p;
    if (checksums_out)
        *checksums_out = (struct infs_data_checksum_disk *)(p + 1);
    return INFS_STATUS_OK;
}

static int checksum_create_object(struct infs_volume *vol,
                                  const uint8_t owner_id[16],
                                  uint64_t start_logical,
                                  const uint8_t next_id[16],
                                  uint8_t id_out[16])
{
    uint64_t block_no = 0, got = 0;
    infs_status status = alloc_run(vol, 1, &block_no, &got);
    if (status != INFS_STATUS_OK)
        return status;
    if (got != 1)
        return INFS_STATUS_CORRUPT;

    uint8_t id[16];
    status = generate_unique_object_id(vol, id);
    if (status != INFS_STATUS_OK) {
        free_run(vol, block_no, 1);
        return status;
    }

    uint8_t block[INFS_BLOCK_SIZE];
    status = infs_object_init(block, INFS_OBJECT_CHECKSUM, id, owner_id,
                              infs_le64_to_cpu(vol->sb.generation) + 1u,
                              sizeof(struct infs_checksum_payload_disk));
    if (status != INFS_STATUS_OK) {
        free_run(vol, block_no, 1);
        return status;
    }
    struct infs_checksum_payload_disk *p =
        (struct infs_checksum_payload_disk *)(block + sizeof(struct infs_object_header_disk));
    memcpy(p->owner_object_id, owner_id, 16);
    p->start_logical_block = infs_cpu_to_le64(start_logical);
    p->checksum_count = infs_cpu_to_le32(0);
    p->reserved = 0;
    if (next_id)
        memcpy(p->next_object_id, next_id, 16);
    status = infs_object_finalize(block);
    if (status == INFS_STATUS_OK)
        status = write_block(vol, block_no, block);
    if (status != INFS_STATUS_OK) {
        free_run(vol, block_no, 1);
        return status;
    }
    status = index_add(vol, id, block_no, INFS_OBJECT_CHECKSUM);
    if (status != INFS_STATUS_OK) {
        free_run(vol, block_no, 1);
        return status;
    }
    memcpy(id_out, id, 16);
    return INFS_STATUS_OK;
}

/* Cached checksum-object traversal and creation. */
static void checksum_cursor_store(struct infs_volume *vol,
                        const uint8_t owner_id[16],
                        const uint8_t object_id[16],
                        uint64_t start)
{
    if (!vol)
        return;
    memcpy(vol->checksum_cursor_owner_id, owner_id, 16);
    memcpy(vol->checksum_cursor_object_id, object_id, 16);
    vol->checksum_cursor_start = start;
    vol->checksum_cursor_valid = 1;
}

static void checksum_cursor_clear_owner(struct infs_volume *vol,
                              const uint8_t owner_id[16])
{
    if (vol && vol->checksum_cursor_valid &&
        memcmp(vol->checksum_cursor_owner_id, owner_id, 16) == 0)
        vol->checksum_cursor_valid = 0;
}

static int checksum_locate(struct infs_volume *vol,
                 struct infs_file_payload_disk *file,
                 const uint8_t owner_id[16], uint64_t logical,
                 int create, struct infs_lookup *lookup_out,
                 uint8_t block_out[INFS_BLOCK_SIZE],
                 struct infs_checksum_payload_disk **payload_out,
                 struct infs_data_checksum_disk **checksums_out)
{
    if (!vol || !file || !owner_id ||
        ((payload_out || checksums_out) && !block_out))
        return INFS_STATUS_INVALID_ARGUMENT;

    uint64_t target_start =
        (logical / INFS_CHECKSUMS_PER_OBJECT) * INFS_CHECKSUMS_PER_OBJECT;
    uint8_t current_id[16];
    uint8_t previous_id[16] = {0};
    uint64_t previous_start = 0;
    int have_previous = 0;
    int started_from_cursor = 0;
    infs_status status;

    if (!id_is_zero(file->checksum_head_id) &&
        vol->checksum_cursor_valid &&
        memcmp(vol->checksum_cursor_owner_id, owner_id, 16) == 0 &&
        vol->checksum_cursor_start <= target_start) {
        memcpy(current_id, vol->checksum_cursor_object_id, 16);
        started_from_cursor = 1;
        ++vol->checksum_cursor_hits;
    } else if (id_is_zero(file->checksum_head_id)) {
        if (!create)
  return INFS_STATUS_CORRUPT;
        status = checksum_create_object(vol, owner_id, target_start, NULL,
                              current_id);
        if (status != INFS_STATUS_OK)
  return status;
        memcpy(file->checksum_head_id, current_id, 16);
    } else {
        memcpy(current_id, file->checksum_head_id, 16);
    }

    for (uint64_t guard = 0; guard < 1048576u; ++guard) {
        ++vol->checksum_chain_steps;
        struct infs_lookup lk;
        status = index_find(vol, current_id, &lk);
        if (status != INFS_STATUS_OK) {
  if (started_from_cursor && guard == 0) {
      vol->checksum_cursor_valid = 0;
      memcpy(current_id, file->checksum_head_id, 16);
      started_from_cursor = 0;
      continue;
  }
  return status;
        }
        if (lk.type != INFS_OBJECT_CHECKSUM)
  return INFS_STATUS_CORRUPT;

        uint8_t block[INFS_BLOCK_SIZE];
        status = read_object(vol, lk.block, block);
        if (status != INFS_STATUS_OK)
  return status;

        struct infs_checksum_payload_disk *p;
        struct infs_data_checksum_disk *values;
        status = checksum_validate(block, owner_id, &p, &values);
        if (status != INFS_STATUS_OK)
  return status;

        uint64_t start = infs_le64_to_cpu(p->start_logical_block);
        if ((start % INFS_CHECKSUMS_PER_OBJECT) != 0 ||
  (have_previous && start <= previous_start))
  return INFS_STATUS_CORRUPT;

        if (start == target_start) {
  checksum_cursor_store(vol, owner_id, current_id, start);
  if (lookup_out)
      *lookup_out = lk;
  if (block_out)
      memcpy(block_out, block, INFS_BLOCK_SIZE);
  if (payload_out)
      *payload_out = (struct infs_checksum_payload_disk *)(block_out + sizeof(struct infs_object_header_disk));
  if (checksums_out) {
      struct infs_checksum_payload_disk *copy =
          (struct infs_checksum_payload_disk *)(block_out + sizeof(struct infs_object_header_disk));
      *checksums_out = (struct infs_data_checksum_disk *)(copy + 1);
  }
  return INFS_STATUS_OK;
        }

        if (start > target_start) {
  if (!create)
      return INFS_STATUS_CORRUPT;
  uint8_t inserted_id[16];
  status = checksum_create_object(vol, owner_id, target_start,
                                  current_id, inserted_id);
  if (status != INFS_STATUS_OK)
      return status;
  if (!have_previous) {
      memcpy(file->checksum_head_id, inserted_id, 16);
  } else {
      struct infs_lookup previous_lk;
      uint8_t previous_block[INFS_BLOCK_SIZE];
      struct infs_checksum_payload_disk *previous;
      status = index_find(vol, previous_id, &previous_lk);
      if (status != INFS_STATUS_OK)
          return status;
      if (previous_lk.type != INFS_OBJECT_CHECKSUM)
          return INFS_STATUS_CORRUPT;
      status = read_object(vol, previous_lk.block, previous_block);
      if (status != INFS_STATUS_OK)
          return status;
      status = checksum_validate(previous_block, owner_id, &previous, NULL);
      if (status != INFS_STATUS_OK)
          return status;
      memcpy(previous->next_object_id, inserted_id, 16);
      status = write_object(vol, previous_lk.block, previous_block);
      if (status != INFS_STATUS_OK)
          return status;
  }
  memcpy(current_id, inserted_id, 16);
  started_from_cursor = 0;
  continue;
        }

        uint8_t next_id[16];
        memcpy(next_id, p->next_object_id, 16);
        if (id_is_zero(next_id)) {
  if (!create)
      return INFS_STATUS_CORRUPT;
  uint8_t inserted_id[16];
  status = checksum_create_object(vol, owner_id, target_start, NULL,
                                  inserted_id);
  if (status != INFS_STATUS_OK)
      return status;
  memcpy(p->next_object_id, inserted_id, 16);
  status = write_object(vol, lk.block, block);
  if (status != INFS_STATUS_OK)
      return status;
  memcpy(previous_id, current_id, 16);
  previous_start = start;
  have_previous = 1;
  memcpy(current_id, inserted_id, 16);
  started_from_cursor = 0;
  continue;
        }

        memcpy(previous_id, current_id, 16);
        previous_start = start;
        have_previous = 1;
        memcpy(current_id, next_id, 16);
        started_from_cursor = 0;
    }
    return INFS_STATUS_LOOP_DETECTED;
}

/* Checksum mutation, lookup, and ownership cleanup. */
static int checksum_set_range(struct infs_volume *vol,
                              struct infs_file_payload_disk *file,
                              const uint8_t owner_id[16], uint64_t logical,
                              const struct infs_data_checksum_disk *digests,
                              uint64_t count)
{
    if (!vol || !file || !owner_id || (count && !digests))
        return INFS_STATUS_INVALID_ARGUMENT;

    while (count) {
        uint64_t offset = logical % INFS_CHECKSUMS_PER_OBJECT;
        uint64_t chunk = INFS_CHECKSUMS_PER_OBJECT - offset;
        if (chunk > count)
            chunk = count;

        struct infs_lookup lk;
        uint8_t block[INFS_BLOCK_SIZE];
        struct infs_checksum_payload_disk *p;
        struct infs_data_checksum_disk *values;
        infs_status status = checksum_locate(vol, file, owner_id, logical, 1,
                                             &lk, block, &p, &values);
        if (status != INFS_STATUS_OK)
            return status;
        for (uint64_t i = 0; i < chunk; ++i)
            values[offset + i] = digests[i];
        uint32_t needed = (uint32_t)(offset + chunk);
        if (needed > infs_le32_to_cpu(p->checksum_count))
            p->checksum_count = infs_cpu_to_le32(needed);
        struct infs_object_header_disk *hdr =
            (struct infs_object_header_disk *)block;
        hdr->payload_size = infs_cpu_to_le32((uint32_t)(sizeof(*p) +
            (size_t)infs_le32_to_cpu(p->checksum_count) *
            sizeof(struct infs_data_checksum_disk)));
        status = write_object(vol, lk.block, block);
        if (status != INFS_STATUS_OK)
            return status;

        logical += chunk;
        digests += chunk;
        count -= chunk;
    }
    return INFS_STATUS_OK;
}

static int checksum_get(struct infs_volume *vol,
                        struct infs_file_payload_disk *file,
                        const uint8_t owner_id[16], uint64_t logical,
                        struct infs_data_checksum_disk *digest_out)
{
    if (!vol || !file || !owner_id || !digest_out)
        return INFS_STATUS_INVALID_ARGUMENT;

    uint8_t block[INFS_BLOCK_SIZE];
    struct infs_checksum_payload_disk *p;
    struct infs_data_checksum_disk *values;
    infs_status status = checksum_locate(vol, file, owner_id, logical, 0, NULL,
                                         block, &p, &values);
    if (status != INFS_STATUS_OK)
        return status;
    uint64_t offset = logical % INFS_CHECKSUMS_PER_OBJECT;
    if (offset >= infs_le32_to_cpu(p->checksum_count))
        return INFS_STATUS_CORRUPT;
    *digest_out = values[offset];
    return INFS_STATUS_OK;
}

static int checksum_destroy_chain(struct infs_volume *vol,
                                  struct infs_file_payload_disk *file,
                                  const uint8_t owner_id[16])
{
    if (!vol || !file || !owner_id)
        return INFS_STATUS_INVALID_ARGUMENT;

    checksum_cursor_clear_owner(vol, owner_id);
    uint8_t current[16];
    memcpy(current, file->checksum_head_id, 16);
    for (uint64_t guard = 0; !id_is_zero(current) && guard < 1048576u; ++guard) {
        struct infs_lookup lk;
        infs_status status = index_find(vol, current, &lk);
        if (status != INFS_STATUS_OK)
            return status;
        if (lk.type != INFS_OBJECT_CHECKSUM)
            return INFS_STATUS_CORRUPT;

        uint8_t block[INFS_BLOCK_SIZE];
        status = read_object(vol, lk.block, block);
        if (status != INFS_STATUS_OK)
            return status;
        struct infs_checksum_payload_disk *p;
        status = checksum_validate(block, owner_id, &p, NULL);
        if (status != INFS_STATUS_OK)
            return status;

        uint8_t next[16];
        memcpy(next, p->next_object_id, 16);
        status = index_remove(vol, current);
        if (status != INFS_STATUS_OK)
            return status;
        free_run(vol, lk.block, 1);
        if (vol->tx_error != INFS_STATUS_OK)
            return vol->tx_error;
        memcpy(current, next, 16);
    }
    if (!id_is_zero(current))
        return INFS_STATUS_LOOP_DETECTED;
    memset(file->checksum_head_id, 0, 16);
    return INFS_STATUS_OK;
}

#define INFS_MAX_FILE_EXTENTS \
    ((INFS_BLOCK_SIZE - sizeof(struct infs_object_header_disk) - \
      sizeof(struct infs_file_payload_disk)) / sizeof(struct infs_extent_disk))
