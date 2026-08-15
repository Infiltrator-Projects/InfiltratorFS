// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_FS_H
#define INFILFS_FS_H

#include <stdint.h>
#include "infilfs/format.h"

int infs_encode_superblock(uint8_t block[INFS_BLOCK_SIZE],
                           const struct infs_superblock_disk *sb);
int infs_decode_superblock(const uint8_t block[INFS_BLOCK_SIZE],
                           struct infs_superblock_disk *sb);
int infs_validate_superblock_block(const uint8_t block[INFS_BLOCK_SIZE]);

int infs_encode_root_directory(uint8_t block[INFS_BLOCK_SIZE],
                               const uint8_t object_id[16],
                               uint64_t generation);
int infs_validate_object_block(const uint8_t block[INFS_BLOCK_SIZE]);

int infs_read_best_superblock(int fd, uint64_t size_bytes,
                              struct infs_superblock_disk *out,
                              unsigned *valid_copies);

void infs_uuid_to_string(const uint8_t id[16], char out[37]);

#endif
