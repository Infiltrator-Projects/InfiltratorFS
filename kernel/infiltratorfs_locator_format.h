// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _INFILTRATORFS_LOCATOR_FORMAT_H
#define _INFILTRATORFS_LOCATOR_FORMAT_H

#include <linux/types.h>

/* Future Format 0.19 recovery locator contract, exact portable byte parity.
 * This header does not activate Format 0.19 native mounts. */
#define INFILFS_LOCATOR_FORMAT_MINOR 19u
#define INFILFS_LOCATOR_SCHEMA_VERSION 1u
#define INFILFS_LOCATOR_BLOCK_A 1ULL
#define INFILFS_LOCATOR_BLOCK_B 2047ULL
#define INFILFS_LOCATOR_MIN_VOLUME_BLOCKS 4096ULL
#define INFILFS_LOCATOR_HEADER_BYTES 80u

struct infilfs_locator_disk {
    u8 magic[8];
    __le16 schema;
    __le16 header_size;
    __le16 major;
    __le16 minor;
    __le64 generation;
    __le64 total_blocks;
    __le64 checkpoints[3];
    u8 filesystem_uuid[16];
    __le64 checksum;
} __packed;

struct infilfs_recovery_locator {
    u64 generation;
    u64 total_blocks;
    u64 checkpoint[3];
    u8 filesystem_uuid[16];
};

int infilfs_locator_decode_019(const u8 block[4096], u64 device_blocks,
                                struct infilfs_recovery_locator *out);
int infilfs_locator_encode_019(
    const struct infilfs_recovery_locator *loc, u8 block[4096]);

#endif
