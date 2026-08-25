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
#define INFILFS_FORMAT_MINOR 12u
#define INFILFS_CHECKPOINT_COUNT 3u
#define INFILFS_NAME_MAX 255u

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

#define INFILFS_EXTENT_NORMAL 0u
#define INFILFS_EXTENT_HOLE 1u

#define INFILFS_INCOMPAT_UTF8_NAMES ((__u64)0x0000000000000001ULL)
#define INFILFS_INCOMPAT_SPARSE_EXTENTS ((__u64)0x0000000000000002ULL)
#define INFILFS_INCOMPAT_INLINE_DATA ((__u64)0x0000000000000004ULL)
#define INFILFS_INCOMPAT_SHARED_EXTENTS ((__u64)0x0000000000000008ULL)
#define INFILFS_INCOMPAT_PAGED_METADATA ((__u64)0x0000000000000010ULL)
#define INFILFS_INCOMPAT_SYMBOLIC_LINKS ((__u64)0x0000000000000020ULL)
#define INFILFS_INCOMPAT_HARD_LINKS ((__u64)0x0000000000000040ULL)
#define INFILFS_INCOMPAT_SNAPSHOTS ((__u64)0x0000000000000080ULL)
#define INFILFS_INCOMPAT_PAGED_EXTENTS ((__u64)0x0000000000000100ULL)
#define INFILFS_KNOWN_INCOMPAT_FLAGS \
    (INFILFS_INCOMPAT_UTF8_NAMES | INFILFS_INCOMPAT_SPARSE_EXTENTS | \
     INFILFS_INCOMPAT_INLINE_DATA | INFILFS_INCOMPAT_SHARED_EXTENTS | \
     INFILFS_INCOMPAT_PAGED_METADATA | INFILFS_INCOMPAT_SYMBOLIC_LINKS | \
     INFILFS_INCOMPAT_HARD_LINKS | INFILFS_INCOMPAT_SNAPSHOTS | \
     INFILFS_INCOMPAT_PAGED_EXTENTS)

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
    __le64 bitmap_start_block;
    __le64 bitmap_block_count;
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

#define INFILFS_METADATA_PAGE_DATA_SIZE \
    (INFILFS_DISK_BLOCK_SIZE - sizeof(struct infilfs_metadata_page_disk))
#define INFILFS_INDEX_ENTRIES_PER_PAGE \
    (INFILFS_METADATA_PAGE_DATA_SIZE / sizeof(struct infilfs_index_entry_disk))
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

#endif
