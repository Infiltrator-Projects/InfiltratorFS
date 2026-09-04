// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_FS_H
#define INFILFS_FS_H

#include <stdint.h>
#include "infilfs/format.h"
#include "infilfs/storage.h"

/*
 * Low-level format helpers operate on one complete filesystem block supplied
 * by the caller. Encode/init routines overwrite the canonical fields they own;
 * finalize routines compute the record checksum after the caller has finished
 * populating the payload. Validation is read-only and returns nonzero only for
 * a structurally valid current-format block, including canonical reserved
 * bytes where the format requires them.
 *
 * These helpers do not perform storage I/O and do not retain caller buffers.
 */
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

/*
 * The classic encoders remain intentionally available for fixtures and format
 * validation. Current Format 0.17 formatters use the tree variants. Keeping
 * the helpers separate prevents a test fixture from silently changing shape
 * when the normal formatter policy evolves.
 */
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

/*
 * Read the checkpoint replicas discoverable from the supplied backing size and
 * return the highest-generation structurally valid superblock. valid_copies,
 * when non-NULL, receives the number of independently valid replicas observed;
 * this helper selects checkpoint records only and does not validate the entire
 * object/namespace graph behind the winning candidate.
 */
infs_status infs_read_best_superblock(const struct infs_storage *storage,
                                      uint64_t size_bytes,
                                      struct infs_superblock_disk *out,
                                      unsigned *valid_copies);

/* out must reference at least 37 bytes; the result is canonical NUL-terminated
 * UUID text and no pointer is retained after return. */
void infs_uuid_to_string(const uint8_t id[16], char out[37]);

#endif