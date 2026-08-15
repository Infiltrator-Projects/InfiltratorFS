// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_FORMAT_H
#define INFILFS_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#define INFS_BLOCK_SIZE 4096u
#define INFS_BLOCK_SHIFT 12u
#define INFS_MAGIC "INFS2026"
#define INFS_OBJECT_MAGIC "INFOBJ01"
#define INFS_FORMAT_MAJOR 0u
#define INFS_FORMAT_MINOR 2u
#define INFS_CHECKPOINT_COUNT 3u
#define INFS_CHECKSUM_CRC64_ECMA 1u
#define INFS_LABEL_MAX 64u
#define INFS_NAME_MAX 255u

#define INFS_OBJECT_DIRECTORY  1u
#define INFS_OBJECT_FILE       2u
#define INFS_OBJECT_INDEX      3u

#define INFS_EXTENT_NORMAL     0u

struct __attribute__((packed)) infs_superblock_disk {
    uint8_t  magic[8];
    uint16_t format_major;
    uint16_t format_minor;
    uint16_t header_size;
    uint16_t block_shift;
    uint32_t checksum_type;
    uint64_t generation;
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t bitmap_start_block;
    uint64_t bitmap_block_count;
    uint64_t object_index_block;
    uint64_t root_object_block;
    uint64_t checkpoint_block[INFS_CHECKPOINT_COUNT];
    uint8_t  filesystem_uuid[16];
    uint8_t  root_object_id[16];
    uint64_t compat_flags;
    uint64_t ro_compat_flags;
    uint64_t incompat_flags;
    uint8_t  label[INFS_LABEL_MAX];
    uint8_t  checksum[32];
};

struct __attribute__((packed)) infs_object_header_disk {
    uint8_t  magic[8];
    uint16_t object_type;
    uint16_t object_version;
    uint32_t header_size;
    uint64_t generation;
    uint8_t  object_id[16];
    uint8_t  parent_id[16];
    uint32_t payload_size;
    uint32_t checksum_type;
    uint8_t  checksum[32];
};

struct __attribute__((packed)) infs_stat_disk {
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint64_t size;
    int64_t  atime_ns;
    int64_t  mtime_ns;
    int64_t  ctime_ns;
    uint64_t flags;
};

struct __attribute__((packed)) infs_directory_payload_disk {
    struct infs_stat_disk stat;
    uint32_t entry_count;
    uint32_t bytes_used;
};

struct __attribute__((packed)) infs_dirent_disk {
    uint16_t record_size;
    uint16_t name_length;
    uint16_t object_type;
    uint16_t flags;
    uint8_t  object_id[16];
    /* name bytes immediately follow; record padded to 8-byte alignment */
};

struct __attribute__((packed)) infs_file_payload_disk {
    struct infs_stat_disk stat;
    uint32_t extent_count;
    uint32_t reserved;
};

struct __attribute__((packed)) infs_extent_disk {
    uint64_t logical_block;
    uint64_t physical_block;
    uint32_t block_count;
    uint32_t flags;
};

struct __attribute__((packed)) infs_index_payload_disk {
    uint32_t entry_count;
    uint32_t reserved;
};

struct __attribute__((packed)) infs_index_entry_disk {
    uint8_t  object_id[16];
    uint64_t object_block;
    uint16_t object_type;
    uint16_t flags;
    uint32_t reserved;
};

_Static_assert(sizeof(struct infs_superblock_disk) < INFS_BLOCK_SIZE,
               "superblock header must fit in one filesystem block");
_Static_assert(sizeof(struct infs_object_header_disk) < INFS_BLOCK_SIZE,
               "object header must fit in one filesystem block");
_Static_assert(sizeof(struct infs_dirent_disk) == 24,
               "directory entry header layout changed");
_Static_assert(sizeof(struct infs_extent_disk) == 24,
               "extent layout changed");
_Static_assert(sizeof(struct infs_index_entry_disk) == 32,
               "object index entry layout changed");

#endif
