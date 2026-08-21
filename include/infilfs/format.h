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
#define INFS_FORMAT_MINOR 7u
#define INFS_CHECKPOINT_COUNT 3u
#define INFS_CHECKSUM_CRC64_ECMA 1u
#define INFS_CHECKSUM_SHA256     2u
#define INFS_LABEL_MAX 64u
#define INFS_NAME_MAX 255u

#define INFS_OBJECT_DIRECTORY  1u
#define INFS_OBJECT_FILE       2u
#define INFS_OBJECT_INDEX      3u
#define INFS_OBJECT_CHECKSUM   4u

#define INFS_EXTENT_NORMAL     0u
#define INFS_EXTENT_HOLE       1u

#define INFS_INCOMPAT_UTF8_NAMES UINT64_C(0x0000000000000001)
#define INFS_INCOMPAT_SPARSE_EXTENTS UINT64_C(0x0000000000000002)
#define INFS_INCOMPAT_INLINE_DATA UINT64_C(0x0000000000000004)
#define INFS_KNOWN_COMPAT_FLAGS UINT64_C(0)
#define INFS_KNOWN_RO_COMPAT_FLAGS UINT64_C(0)
#define INFS_KNOWN_INCOMPAT_FLAGS \
    (INFS_INCOMPAT_UTF8_NAMES | INFS_INCOMPAT_SPARSE_EXTENTS | \
     INFS_INCOMPAT_INLINE_DATA)

#define INFS_ATTR_READ_ONLY           UINT64_C(0x0000000000000001)
#define INFS_ATTR_HIDDEN              UINT64_C(0x0000000000000002)
#define INFS_ATTR_SYSTEM              UINT64_C(0x0000000000000004)
#define INFS_ATTR_ARCHIVE             UINT64_C(0x0000000000000008)
#define INFS_ATTR_TEMPORARY           UINT64_C(0x0000000000000010)
#define INFS_ATTR_NOT_CONTENT_INDEXED UINT64_C(0x0000000000000020)
#define INFS_KNOWN_ATTR_FLAGS \
    (INFS_ATTR_READ_ONLY | INFS_ATTR_HIDDEN | INFS_ATTR_SYSTEM | \
     INFS_ATTR_ARCHIVE | INFS_ATTR_TEMPORARY | INFS_ATTR_NOT_CONTENT_INDEXED)

#if defined(_MSC_VER)
#pragma pack(push, 1)
#define INFS_PACKED
#else
#define INFS_PACKED __attribute__((packed))
#endif

struct INFS_PACKED infs_superblock_disk {
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

struct INFS_PACKED infs_object_header_disk {
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

struct INFS_PACKED infs_attributes_disk {
    uint64_t logical_size;
    uint64_t link_count;
    uint64_t portable_flags;
    int64_t  birth_time_ns;
    int64_t  access_time_ns;
    int64_t  modification_time_ns;
    int64_t  change_time_ns;
    uint8_t  security_object_id[16];
    uint8_t  extended_attributes_object_id[16];
};

/* Optional POSIX compatibility data. It is an adapter record, not the
 * filesystem's security or object-identity model. */
struct INFS_PACKED infs_posix_compat_disk {
    uint32_t permissions;
    uint32_t uid;
    uint32_t gid;
    uint32_t flags;
};

struct INFS_PACKED infs_directory_payload_disk {
    struct infs_attributes_disk attributes;
    struct infs_posix_compat_disk posix;
    uint32_t entry_count;
    uint32_t bytes_used;
};

struct INFS_PACKED infs_dirent_disk {
    uint16_t record_size;
    uint16_t name_length;
    uint16_t object_type;
    uint16_t flags;
    uint8_t  object_id[16];
    /* name bytes immediately follow; record padded to 8-byte alignment */
};

struct INFS_PACKED infs_file_payload_disk {
    struct infs_attributes_disk attributes;
    struct infs_posix_compat_disk posix;
    uint32_t extent_count;
    uint32_t data_checksum_type;
    uint8_t  checksum_head_id[16];
};

struct INFS_PACKED infs_extent_disk {
    uint64_t logical_block;
    uint64_t physical_block;
    uint32_t block_count;
    uint32_t flags;
};

struct INFS_PACKED infs_checksum_payload_disk {
    uint8_t  owner_object_id[16];
    uint8_t  next_object_id[16];
    uint64_t start_logical_block;
    uint32_t checksum_count;
    uint32_t reserved;
    /* struct infs_data_checksum_disk entries immediately follow */
};

struct INFS_PACKED infs_data_checksum_disk {
    uint8_t bytes[32];
};

#define INFS_CHECKSUMS_PER_OBJECT \
    ((INFS_BLOCK_SIZE - sizeof(struct infs_object_header_disk) - \
      sizeof(struct infs_checksum_payload_disk)) / \
     sizeof(struct infs_data_checksum_disk))

/* Format 0.7 inline files reuse the existing file object block. A non-empty
 * inline file stores one SHA-256 digest immediately after the fixed file
 * payload, followed by logical_size bytes of data. The digest authenticates
 * the same zero-padded 4096-byte logical block used by normal data storage. */
#define INFS_INLINE_DATA_MAX \
    (INFS_BLOCK_SIZE - sizeof(struct infs_object_header_disk) - \
     sizeof(struct infs_file_payload_disk) - \
     sizeof(struct infs_data_checksum_disk))

struct INFS_PACKED infs_index_payload_disk {
    uint32_t entry_count;
    uint32_t reserved;
};

struct INFS_PACKED infs_index_entry_disk {
    uint8_t  object_id[16];
    uint64_t object_block;
    uint16_t object_type;
    uint16_t flags;
    uint32_t reserved;
};

_Static_assert(sizeof(struct infs_superblock_disk) == 252,
               "superblock header layout changed");
_Static_assert(sizeof(struct infs_object_header_disk) == 96,
               "object header layout changed");
_Static_assert(sizeof(struct infs_attributes_disk) == 88,
               "common attributes layout changed");
_Static_assert(sizeof(struct infs_posix_compat_disk) == 16,
               "POSIX compatibility layout changed");
_Static_assert(sizeof(struct infs_directory_payload_disk) == 112,
               "directory payload layout changed");
_Static_assert(sizeof(struct infs_dirent_disk) == 24,
               "directory entry header layout changed");
_Static_assert(sizeof(struct infs_file_payload_disk) == 128,
               "file payload layout changed");
_Static_assert(sizeof(struct infs_extent_disk) == 24,
               "extent layout changed");
_Static_assert(sizeof(struct infs_checksum_payload_disk) == 48,
               "checksum payload layout changed");
_Static_assert(sizeof(struct infs_data_checksum_disk) == 32,
               "data checksum layout changed");
_Static_assert(sizeof(struct infs_index_payload_disk) == 8,
               "index payload layout changed");
_Static_assert(sizeof(struct infs_index_entry_disk) == 32,
               "object index entry layout changed");
_Static_assert(INFS_CHECKSUMS_PER_OBJECT >= 120,
               "checksum object capacity unexpectedly small");
_Static_assert(INFS_INLINE_DATA_MAX == 3840u,
               "inline-data capacity unexpectedly changed");

#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#undef INFS_PACKED

#endif
