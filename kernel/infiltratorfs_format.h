// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _INFILTRATORFS_KERNEL_FORMAT_H
#define _INFILTRATORFS_KERNEL_FORMAT_H

#include <linux/errno.h>
#include <linux/types.h>

/*
 * Linux defines current as a task-pointer macro.  The native reader has a
 * checkpoint candidate variable with that natural name, so keep the original
 * translation-unit isolation.  Credential acquisition added by the POSIX
 * metadata bridge must still work after that isolation; get_task_cred() can be
 * given the architecture's get_current() result without expanding the current
 * macro.
 */
#ifdef current
#undef current
#endif
#ifdef get_current_cred
#undef get_current_cred
#endif
#define get_current_cred() get_task_cred(get_current())

/* EFSCORRUPTED is not available on every supported distro kernel header set.
 * EUCLEAN is the conventional portable kernel errno for corrupt filesystem
 * metadata and carries the same meaning at this adapter boundary. */
#ifndef EFSCORRUPTED
#define EFSCORRUPTED EUCLEAN
#endif

#define INFILFS_DISK_BLOCK_SIZE 4096u
#define INFILFS_DISK_BLOCK_SHIFT 12u
#define INFILFS_FORMAT_MAJOR 0u
#define INFILFS_FORMAT_MINOR 17u
#define INFILFS_CHECKPOINT_COUNT 3u
#define INFILFS_NAME_MAX 1023u

#define INFILFS_CHECKSUM_CRC64_ECMA 1u
#define INFILFS_CHECKSUM_SHA256 2u

#define INFILFS_OBJECT_DIRECTORY 1u
#define INFILFS_OBJECT_FILE 2u
#define INFILFS_OBJECT_INDEX 3u
#define INFILFS_OBJECT_CHECKSUM 4u
#define INFILFS_OBJECT_SYMLINK 5u
#define INFILFS_OBJECT_SNAPSHOT_CATALOG 6u

#define INFILFS_OBJECT_VERSION_CLASSIC 1u
#define INFILFS_OBJECT_VERSION_PAGED 2u
#define INFILFS_OBJECT_VERSION_TREE 3u

#define INFILFS_EXTENT_NORMAL 0u
#define INFILFS_EXTENT_HOLE 1u
#define INFILFS_EXTENT_KIND_MASK 0x00000003u
#define INFILFS_EXTENT_CODEC_SHIFT 2u
#define INFILFS_EXTENT_CODEC_MASK 0x0000000cu
#define INFILFS_EXTENT_CODEC_EXT_SHIFT 23u
#define INFILFS_EXTENT_CODEC_EXT_MASK 0xff800000u
#define INFILFS_EXTENT_CODEC_MAX 0x000007ffu
#define INFILFS_EXTENT_STORED_BYTES_SHIFT 4u
#define INFILFS_EXTENT_STORED_BYTES_MASK 0x007ffff0u
#define INFILFS_EXTENT_STORED_BYTES_MAX 0x0007ffffu
#define INFILFS_COMPRESSION_NONE 0u
#define INFILFS_COMPRESSION_LZ4 1u
#define INFILFS_COMPRESSION_IAC1 2u
#define INFILFS_COMPRESSION_CLUSTER_BLOCKS 64u

#define INFILFS_ATTR_READ_ONLY           ((__u64)0x0000000000000001ULL)
#define INFILFS_ATTR_HIDDEN              ((__u64)0x0000000000000002ULL)
#define INFILFS_ATTR_SYSTEM              ((__u64)0x0000000000000004ULL)
#define INFILFS_ATTR_ARCHIVE             ((__u64)0x0000000000000008ULL)
#define INFILFS_ATTR_TEMPORARY           ((__u64)0x0000000000000010ULL)
#define INFILFS_ATTR_NOT_CONTENT_INDEXED ((__u64)0x0000000000000020ULL)

#define INFILFS_INCOMPAT_UTF8_NAMES ((__u64)0x0000000000000001ULL)
#define INFILFS_INCOMPAT_SPARSE_EXTENTS ((__u64)0x0000000000000002ULL)
#define INFILFS_INCOMPAT_INLINE_DATA ((__u64)0x0000000000000004ULL)
#define INFILFS_INCOMPAT_SHARED_EXTENTS ((__u64)0x0000000000000008ULL)
#define INFILFS_INCOMPAT_PAGED_METADATA ((__u64)0x0000000000000010ULL)
#define INFILFS_INCOMPAT_SYMBOLIC_LINKS ((__u64)0x0000000000000020ULL)
#define INFILFS_INCOMPAT_HARD_LINKS ((__u64)0x0000000000000040ULL)
#define INFILFS_INCOMPAT_SNAPSHOTS ((__u64)0x0000000000000080ULL)
#define INFILFS_INCOMPAT_PAGED_EXTENTS ((__u64)0x0000000000000100ULL)
#define INFILFS_INCOMPAT_INDEX_TREE ((__u64)0x0000000000000200ULL)
#define INFILFS_INCOMPAT_DIRECTORY_TREE ((__u64)0x0000000000000400ULL)
#define INFILFS_INCOMPAT_ALLOCATION_TREE ((__u64)0x0000000000000800ULL)
#define INFILFS_INCOMPAT_COMPRESSED_EXTENTS ((__u64)0x0000000000001000ULL)
#define INFILFS_KNOWN_INCOMPAT_FLAGS \
    (INFILFS_INCOMPAT_UTF8_NAMES | INFILFS_INCOMPAT_SPARSE_EXTENTS | \
     INFILFS_INCOMPAT_INLINE_DATA | INFILFS_INCOMPAT_SHARED_EXTENTS | \
     INFILFS_INCOMPAT_PAGED_METADATA | INFILFS_INCOMPAT_SYMBOLIC_LINKS | \
     INFILFS_INCOMPAT_HARD_LINKS | INFILFS_INCOMPAT_SNAPSHOTS | \
     INFILFS_INCOMPAT_PAGED_EXTENTS | INFILFS_INCOMPAT_INDEX_TREE | \
     INFILFS_INCOMPAT_DIRECTORY_TREE | INFILFS_INCOMPAT_ALLOCATION_TREE | \
     INFILFS_INCOMPAT_COMPRESSED_EXTENTS)

struct infilfs_superblock_disk {
    __u8 magic[8];
    __le16 format_major;
    __le16 format_minor;
    __le16 header_size;
    __le16 block_shift;
    __le32 checksum_type;
    __le64 generation;
    __le64 total_blocks;
    __le64 free_blocks;
    __le64 allocation_root_block;
    __le64 allocation_leaf_count;
    __le64 object_index_block;
    __le64 root_object_block;
    __le64 checkpoint_block[INFILFS_CHECKPOINT_COUNT];
    __u8 filesystem_uuid[16];
    __u8 root_object_id[16];
    __le64 compat_flags;
    __le64 ro_compat_flags;
    __le64 incompat_flags;
    __u8 label[64];
    __u8 checksum[32];
} __packed;

struct infilfs_object_header_disk {
    __u8 magic[8];
    __le16 object_type;
    __le16 object_version;
    __le32 header_size;
    __le64 generation;
    __u8 object_id[16];
    __u8 parent_id[16];
    __le32 payload_size;
    __le32 checksum_type;
    __u8 checksum[32];
} __packed;

struct infilfs_metadata_page_disk {
    __u8 magic[8];
    __le64 generation;
    __u8 owner_object_id[16];
    __le32 entry_count;
    __le32 bytes_used;
    __le32 checksum_type;
    __le32 reserved;
    __u8 checksum[32];
} __packed;

struct infilfs_allocation_page_disk {
    __u8 magic[8];
    __le64 generation;
    __le64 logical_index;
    __le32 level;
    __le32 entry_count;
    __le32 bytes_used;
    __le32 checksum_type;
    __u8 checksum[32];
} __packed;

struct infilfs_attributes_disk {
    __le64 logical_size;
    __le64 link_count;
    __le64 portable_flags;
    __le64 birth_time_ns;
    __le64 access_time_ns;
    __le64 modification_time_ns;
    __le64 change_time_ns;
    __u8 security_object_id[16];
    __u8 extended_attributes_object_id[16];
} __packed;

struct infilfs_posix_compat_disk {
    __le32 permissions;
    __le32 uid;
    __le32 gid;
    __le32 flags;
} __packed;

struct infilfs_directory_payload_disk {
    struct infilfs_attributes_disk attributes;
    struct infilfs_posix_compat_disk posix;
    __le32 entry_count;
    __le32 bytes_used;
} __packed;

struct infilfs_dirent_disk {
    __le16 record_size;
    __le16 name_length;
    __le16 object_type;
    __le16 flags;
    __u8 object_id[16];
} __packed;

struct infilfs_file_payload_disk {
    struct infilfs_attributes_disk attributes;
    struct infilfs_posix_compat_disk posix;
    __le32 extent_count;
    __le32 data_checksum_type;
    __u8 checksum_head_id[16];
} __packed;

struct infilfs_symlink_payload_disk {
    struct infilfs_attributes_disk attributes;
    struct infilfs_posix_compat_disk posix;
    __le32 target_length;
    __le32 reserved;
} __packed;

struct infilfs_extent_head_disk {
    __le32 page_count;
    __le32 reserved;
} __packed;

struct infilfs_extent_disk {
    __le64 logical_block;
    __le64 physical_block;
    __le32 block_count;
    __le32 flags;
} __packed;

struct infilfs_data_checksum_disk {
    __u8 bytes[32];
} __packed;

struct infilfs_index_payload_disk {
    __le32 entry_count;
    __le32 reserved;
} __packed;

struct infilfs_index_entry_disk {
    __u8 object_id[16];
    __le64 object_block;
    __le16 object_type;
    __le16 flags;
    __le32 reserved;
} __packed;

struct infilfs_snapshot_catalog_payload_disk {
    __le32 snapshot_count;
    __le32 reserved;
} __packed;

#define INFILFS_SNAPSHOT_NAME_MAX 63u

struct infilfs_snapshot_record_disk {
    __le64 generation;
    __le64 created_time_ns;
    __le64 bitmap_start_block;
    __le64 bitmap_block_count;
    __le64 free_blocks;
    __le64 object_index_block;
    __le64 root_object_block;
    __u8 root_object_id[16];
    __le16 name_length;
    __le16 flags;
    __le32 reserved;
    __u8 name[INFILFS_SNAPSHOT_NAME_MAX + 1u];
} __packed;

#define INFILFS_METADATA_PAGE_DATA_SIZE \
    (INFILFS_DISK_BLOCK_SIZE - sizeof(struct infilfs_metadata_page_disk))
#define INFILFS_ALLOCATION_PAGE_DATA_SIZE \
    (INFILFS_DISK_BLOCK_SIZE - sizeof(struct infilfs_allocation_page_disk))
#define INFILFS_ALLOCATION_TREE_FANOUT \
    (INFILFS_ALLOCATION_PAGE_DATA_SIZE / sizeof(__le64))
#define INFILFS_ALLOCATION_TREE_ROOT_LEVEL 3u
#define INFILFS_ALLOCATION_BITS_PER_LEAF \
    ((__u64)INFILFS_ALLOCATION_PAGE_DATA_SIZE * 8ULL)
#define INFILFS_ALLOCATION_TREE_MAX_LEAVES \
    ((__u64)INFILFS_ALLOCATION_TREE_FANOUT * \
     (__u64)INFILFS_ALLOCATION_TREE_FANOUT * \
     (__u64)INFILFS_ALLOCATION_TREE_FANOUT)
#define INFILFS_ALLOCATION_TREE_MAX_BLOCKS \
    (INFILFS_ALLOCATION_TREE_MAX_LEAVES * INFILFS_ALLOCATION_BITS_PER_LEAF)
#define INFILFS_DIRENT_MAX_RECORD_SIZE \
    ALIGN(sizeof(struct infilfs_dirent_disk) + INFILFS_NAME_MAX, 8u)
#define INFILFS_INDEX_ENTRIES_PER_PAGE \
    (INFILFS_METADATA_PAGE_DATA_SIZE / sizeof(struct infilfs_index_entry_disk))
#define INFILFS_INDEX_TREE_FANOUT 256u
#define INFILFS_INDEX_TREE_BRANCH_BYTES \
    (INFILFS_INDEX_TREE_FANOUT * sizeof(__le64))
#define INFILFS_DIRECTORY_TREE_FANOUT 256u
#define INFILFS_DIRECTORY_TREE_DEPTH 32u
#define INFILFS_DIRECTORY_TREE_BRANCH_BYTES \
    (INFILFS_DIRECTORY_TREE_FANOUT * sizeof(__le64))
#define INFILFS_EXTENTS_PER_PAGE \
    (INFILFS_METADATA_PAGE_DATA_SIZE / sizeof(struct infilfs_extent_disk))
#define INFILFS_DIRECTORY_PAGE_POINTERS \
    ((INFILFS_DISK_BLOCK_SIZE - sizeof(struct infilfs_object_header_disk) - \
      sizeof(struct infilfs_directory_payload_disk)) / sizeof(__le64))
#define INFILFS_INDEX_PAGE_POINTERS \
    ((INFILFS_DISK_BLOCK_SIZE - sizeof(struct infilfs_object_header_disk) - \
      sizeof(struct infilfs_index_payload_disk)) / sizeof(__le64))
#define INFILFS_EXTENT_PAGE_POINTERS \
    ((INFILFS_DISK_BLOCK_SIZE - sizeof(struct infilfs_object_header_disk) - \
      sizeof(struct infilfs_file_payload_disk) - \
      sizeof(struct infilfs_extent_head_disk)) / sizeof(__le64))
#define INFILFS_INLINE_DATA_MAX \
    (INFILFS_DISK_BLOCK_SIZE - sizeof(struct infilfs_object_header_disk) - \
     sizeof(struct infilfs_file_payload_disk) - \
     sizeof(struct infilfs_data_checksum_disk))

static_assert(sizeof(struct infilfs_allocation_page_disk) == 72);
static_assert(INFILFS_ALLOCATION_TREE_FANOUT == 503u);
static_assert(INFILFS_ALLOCATION_BITS_PER_LEAF == 32192ULL);
static_assert(INFILFS_DIRENT_MAX_RECORD_SIZE <=
              INFILFS_METADATA_PAGE_DATA_SIZE);
static_assert(sizeof(struct infilfs_superblock_disk) == 252);
static_assert(sizeof(struct infilfs_object_header_disk) == 96);
static_assert(sizeof(struct infilfs_metadata_page_disk) == 80);
static_assert(sizeof(struct infilfs_attributes_disk) == 88);
static_assert(sizeof(struct infilfs_posix_compat_disk) == 16);
static_assert(sizeof(struct infilfs_directory_payload_disk) == 112);
static_assert(sizeof(struct infilfs_dirent_disk) == 24);
static_assert(sizeof(struct infilfs_file_payload_disk) == 128);
static_assert(sizeof(struct infilfs_symlink_payload_disk) == 112);
static_assert(sizeof(struct infilfs_extent_disk) == 24);
static_assert(sizeof(struct infilfs_index_entry_disk) == 32);
static_assert(INFILFS_INDEX_TREE_BRANCH_BYTES <= INFILFS_METADATA_PAGE_DATA_SIZE);
static_assert(INFILFS_DIRECTORY_TREE_BRANCH_BYTES <= INFILFS_METADATA_PAGE_DATA_SIZE);
static_assert(sizeof(struct infilfs_snapshot_catalog_payload_disk) == 8);
static_assert(sizeof(struct infilfs_snapshot_record_disk) == 144);

#endif
