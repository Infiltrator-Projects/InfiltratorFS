// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_FS_H
#define INFILFS_FS_H

#include <stdint.h>
#include "infilfs/format.h"
#include "infilfs/storage.h"

infs_status infs_encode_superblock(uint8_t block[INFS_BLOCK_SIZE],
                                   const struct infs_superblock_disk *sb);
infs_status infs_decode_superblock(const uint8_t block[INFS_BLOCK_SIZE],
                                   struct infs_superblock_disk *sb);
int infs_validate_superblock_block(const uint8_t block[INFS_BLOCK_SIZE]);

infs_status infs_object_init(uint8_t block[INFS_BLOCK_SIZE],
                             uint16_t object_type,
                             const uint8_t object_id[16],
                             const uint8_t parent_id[16],
                             uint64_t generation, uint32_t payload_size);
infs_status infs_object_finalize(uint8_t block[INFS_BLOCK_SIZE]);
int infs_validate_object_block(const uint8_t block[INFS_BLOCK_SIZE]);

infs_status infs_metadata_page_init(uint8_t block[INFS_BLOCK_SIZE],
                                    const uint8_t magic[8],
                                    const uint8_t owner_object_id[16],
                                    uint64_t generation);
infs_status infs_metadata_page_finalize(uint8_t block[INFS_BLOCK_SIZE]);
int infs_validate_metadata_page(const uint8_t block[INFS_BLOCK_SIZE],
                                const uint8_t magic[8],
                                const uint8_t owner_object_id[16]);
infs_status infs_allocation_page_init(uint8_t block[INFS_BLOCK_SIZE],
                                      const uint8_t magic[8],
                                      uint64_t generation,
                                      uint64_t logical_index,
                                      uint32_t level);
infs_status infs_allocation_page_finalize(uint8_t block[INFS_BLOCK_SIZE]);
int infs_validate_allocation_page(const uint8_t block[INFS_BLOCK_SIZE],
                                  const uint8_t magic[8],
                                  uint64_t max_generation,
                                  uint64_t logical_index,
                                  uint32_t level);

/* The original encoders intentionally remain classic single-block encoders so
 * older conformance fixtures and callers keep their established semantics.
 * Format 0.17 formatters use the paged variants below. */
infs_status infs_encode_root_directory(uint8_t block[INFS_BLOCK_SIZE],
                                       const uint8_t object_id[16],
                                       uint64_t generation,
                                       uint32_t permissions,
                                       uint32_t uid, uint32_t gid,
                                       int64_t now_ns);
infs_status infs_encode_object_index(uint8_t block[INFS_BLOCK_SIZE],
                                     const uint8_t object_id[16],
                                     uint64_t generation);
infs_status infs_encode_paged_root_directory(uint8_t block[INFS_BLOCK_SIZE],
                                             const uint8_t object_id[16],
                                             uint64_t generation,
                                             uint32_t permissions,
                                             uint32_t uid, uint32_t gid,
                                             int64_t now_ns);
infs_status infs_encode_tree_root_directory(uint8_t block[INFS_BLOCK_SIZE],
                                            const uint8_t object_id[16],
                                            uint64_t generation,
                                            uint32_t permissions,
                                            uint32_t uid, uint32_t gid,
                                            int64_t now_ns);
infs_status infs_encode_paged_object_index(uint8_t block[INFS_BLOCK_SIZE],
                                           const uint8_t object_id[16],
                                           uint64_t generation);
infs_status infs_encode_tree_object_index(uint8_t block[INFS_BLOCK_SIZE],
                                          const uint8_t object_id[16],
                                          uint64_t generation,
                                          uint64_t root_node_block,
                                          uint32_t entry_count);

infs_status infs_read_best_superblock(const struct infs_storage *storage,
                                      uint64_t size_bytes,
                                      struct infs_superblock_disk *out,
                                      unsigned *valid_copies);

void infs_uuid_to_string(const uint8_t id[16], char out[37]);

#endif
