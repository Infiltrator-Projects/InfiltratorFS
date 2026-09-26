// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATORFS_WINDOWS_NATIVE_PROTOCOL_H
#define INFILTRATORFS_WINDOWS_NATIVE_PROTOCOL_H

/*
 * Private kernel-FSD <-> user-mode portable-core protocol.
 *
 * The native Windows filesystem driver owns the Windows I/O Manager / Cache
 * Manager contract. The companion service owns portable InfiltratorFS
 * namespace/data semantics by linking the same userspace core used by the
 * Windows Manager. Raw volume I/O remains in the kernel driver so the service
 * never opens or aliases the mounted filesystem through Win32 paths.
 *
 * Protocol structures are fixed-width and versioned. No native pointers cross
 * the boundary. All strings are UTF-16 path components supplied by Windows;
 * the service converts them to canonical InfiltratorFS UTF-8 without changing
 * persistent object identity.
 */

#include <stdint.h>

#define INFILFS_WIN_NATIVE_PROTOCOL_VERSION UINT32_C(2)
#define INFILFS_WIN_NATIVE_PATH_CHARS 32768u
#define INFILFS_WIN_NATIVE_NAME_CHARS 1024u
#define INFILFS_WIN_NATIVE_LABEL_BYTES 64u
#define INFILFS_WIN_NATIVE_IO_CHUNK (1024u * 1024u)

#define INFILFS_WIN_NATIVE_DEVICE_TYPE 0x00008337u
#define INFILFS_WIN_NATIVE_CTL(function, access) \
    ((INFILFS_WIN_NATIVE_DEVICE_TYPE << 16) | ((access) << 14) | \
     ((function) << 2) | 0u)

#define INFILFS_WIN_NATIVE_FILE_ANY_ACCESS 0u
#define INFILFS_WIN_NATIVE_FILE_READ_DATA  1u
#define INFILFS_WIN_NATIVE_FILE_WRITE_DATA 2u

#define IOCTL_INFILFS_NATIVE_WAIT_REQUEST \
    INFILFS_WIN_NATIVE_CTL(0x800u, INFILFS_WIN_NATIVE_FILE_READ_DATA)
#define IOCTL_INFILFS_NATIVE_COMPLETE_REQUEST \
    INFILFS_WIN_NATIVE_CTL(0x801u, INFILFS_WIN_NATIVE_FILE_WRITE_DATA)
#define IOCTL_INFILFS_NATIVE_RAW_READ \
    INFILFS_WIN_NATIVE_CTL(0x802u, INFILFS_WIN_NATIVE_FILE_READ_DATA)
#define IOCTL_INFILFS_NATIVE_RAW_WRITE \
    INFILFS_WIN_NATIVE_CTL(0x803u, INFILFS_WIN_NATIVE_FILE_WRITE_DATA)
#define IOCTL_INFILFS_NATIVE_RAW_FLUSH \
    INFILFS_WIN_NATIVE_CTL(0x804u, INFILFS_WIN_NATIVE_FILE_WRITE_DATA)
#define IOCTL_INFILFS_NATIVE_VOLUME_QUERY \
    INFILFS_WIN_NATIVE_CTL(0x805u, INFILFS_WIN_NATIVE_FILE_READ_DATA)

enum infilfs_win_native_opcode {
    INFILFS_WIN_NATIVE_OP_LOOKUP = 1,
    INFILFS_WIN_NATIVE_OP_ENUMERATE = 2,
    INFILFS_WIN_NATIVE_OP_READ = 3,
    INFILFS_WIN_NATIVE_OP_WRITE = 4,
    INFILFS_WIN_NATIVE_OP_FLUSH = 5,
    INFILFS_WIN_NATIVE_OP_CREATE = 6,
    INFILFS_WIN_NATIVE_OP_MKDIR = 7,
    INFILFS_WIN_NATIVE_OP_UNLINK = 8,
    INFILFS_WIN_NATIVE_OP_RMDIR = 9,
    INFILFS_WIN_NATIVE_OP_RENAME = 10,
    INFILFS_WIN_NATIVE_OP_TRUNCATE = 11,
    INFILFS_WIN_NATIVE_OP_QUERY_SECURITY = 12,
    INFILFS_WIN_NATIVE_OP_SET_SECURITY = 13,
    INFILFS_WIN_NATIVE_OP_QUERY_VOLUME = 14,
    INFILFS_WIN_NATIVE_OP_LINK = 15,
    INFILFS_WIN_NATIVE_OP_SET_BASIC = 16,
};

#define INFILFS_WIN_NATIVE_OBJECT_FILE      UINT32_C(1)
#define INFILFS_WIN_NATIVE_OBJECT_DIRECTORY UINT32_C(2)
#define INFILFS_WIN_NATIVE_OBJECT_SYMLINK   UINT32_C(3)

#define INFILFS_WIN_NATIVE_REQ_DIRECTORY    UINT32_C(0x00000001)
#define INFILFS_WIN_NATIVE_REQ_REPLACE      UINT32_C(0x00000002)
#define INFILFS_WIN_NATIVE_REQ_WRITE_THROUGH UINT32_C(0x00000004)
#define INFILFS_WIN_NATIVE_REQ_PAGING_IO    UINT32_C(0x00000008)
#define INFILFS_WIN_NATIVE_REQ_DELETE_ON_CLOSE UINT32_C(0x00000010)

#pragma pack(push, 1)
struct infilfs_win_native_request {
    uint32_t protocol_version;
    uint32_t opcode;
    uint64_t request_id;
    uint64_t volume_id;
    uint32_t flags;
    uint32_t desired_access;
    uint64_t offset;
    uint64_t length;
    uint64_t allocation_size;
    uint32_t path_chars;
    uint32_t second_path_chars;
    uint32_t input_bytes;
    uint32_t reserved;
    uint16_t path[INFILFS_WIN_NATIVE_PATH_CHARS];
    uint16_t second_path[INFILFS_WIN_NATIVE_PATH_CHARS];
    uint8_t input[INFILFS_WIN_NATIVE_IO_CHUNK];
};

struct infilfs_win_native_basic {
    int64_t creation_time_100ns;
    int64_t access_time_100ns;
    int64_t write_time_100ns;
    int64_t change_time_100ns;
    uint32_t file_attributes;
    uint32_t reserved;
};

struct infilfs_win_native_attributes {
    uint64_t file_id;
    uint64_t logical_size;
    uint64_t allocation_size;
    uint64_t creation_time_100ns;
    uint64_t access_time_100ns;
    uint64_t write_time_100ns;
    uint64_t change_time_100ns;
    uint32_t object_type;
    uint32_t file_attributes;
    uint32_t link_count;
    uint32_t reparse_tag;
};

struct infilfs_win_native_dirent {
    struct infilfs_win_native_attributes attributes;
    uint32_t name_chars;
    uint32_t reserved;
    uint16_t name[INFILFS_WIN_NATIVE_NAME_CHARS];
};

struct infilfs_win_native_volume_state {
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t generation;
    uint8_t filesystem_uuid[16];
    uint32_t label_bytes;
    uint32_t reserved;
    uint8_t label[INFILFS_WIN_NATIVE_LABEL_BYTES];
};

struct infilfs_win_native_response {
    uint32_t protocol_version;
    int32_t status;
    uint64_t request_id;
    struct infilfs_win_native_attributes attributes;
    uint32_t output_bytes;
    uint32_t entry_count;
    uint8_t output[INFILFS_WIN_NATIVE_IO_CHUNK];
};

struct infilfs_win_native_raw_io {
    uint32_t protocol_version;
    uint32_t reserved;
    uint64_t volume_id;
    uint64_t offset;
    uint32_t size;
    uint32_t flags;
    uint8_t data[INFILFS_WIN_NATIVE_IO_CHUNK];
};

struct infilfs_win_native_volume_info {
    uint32_t protocol_version;
    uint32_t flags;
    uint64_t volume_id;
    uint64_t size_bytes;
    uint32_t sector_size;
    uint32_t read_only;
};
#pragma pack(pop)

#endif
