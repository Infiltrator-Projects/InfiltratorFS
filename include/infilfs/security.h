// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_SECURITY_H
#define INFILFS_SECURITY_H

#include <stdint.h>

/*
 * Portable access-right vocabulary.
 *
 * These bit positions describe filesystem meaning only. They are deliberately
 * independent of Linux mode bits/POSIX ACL permission values and Windows
 * ACCESS_MASK constants. Persistent security objects can therefore use this
 * vocabulary without making one operating system's ABI authoritative.
 */
typedef uint64_t infs_rights_mask;

#define INFS_RIGHT_READ_DATA            (UINT64_C(1) << 0)
#define INFS_RIGHT_WRITE_DATA           (UINT64_C(1) << 1)
#define INFS_RIGHT_APPEND_DATA          (UINT64_C(1) << 2)
#define INFS_RIGHT_EXECUTE              (UINT64_C(1) << 3)
#define INFS_RIGHT_LIST_DIRECTORY       (UINT64_C(1) << 4)
#define INFS_RIGHT_TRAVERSE_DIRECTORY   (UINT64_C(1) << 5)
#define INFS_RIGHT_CREATE_FILE          (UINT64_C(1) << 6)
#define INFS_RIGHT_CREATE_DIRECTORY     (UINT64_C(1) << 7)
#define INFS_RIGHT_DELETE               (UINT64_C(1) << 8)
#define INFS_RIGHT_DELETE_CHILD         (UINT64_C(1) << 9)
#define INFS_RIGHT_READ_ATTRIBUTES      (UINT64_C(1) << 10)
#define INFS_RIGHT_WRITE_ATTRIBUTES     (UINT64_C(1) << 11)
#define INFS_RIGHT_READ_NAMED_METADATA  (UINT64_C(1) << 12)
#define INFS_RIGHT_WRITE_NAMED_METADATA (UINT64_C(1) << 13)
#define INFS_RIGHT_READ_PERMISSIONS     (UINT64_C(1) << 14)
#define INFS_RIGHT_CHANGE_PERMISSIONS   (UINT64_C(1) << 15)
#define INFS_RIGHT_TAKE_OWNERSHIP       (UINT64_C(1) << 16)

#define INFS_RIGHT_ALL ( \
    INFS_RIGHT_READ_DATA | INFS_RIGHT_WRITE_DATA | INFS_RIGHT_APPEND_DATA | \
    INFS_RIGHT_EXECUTE | INFS_RIGHT_LIST_DIRECTORY | \
    INFS_RIGHT_TRAVERSE_DIRECTORY | INFS_RIGHT_CREATE_FILE | \
    INFS_RIGHT_CREATE_DIRECTORY | INFS_RIGHT_DELETE | \
    INFS_RIGHT_DELETE_CHILD | INFS_RIGHT_READ_ATTRIBUTES | \
    INFS_RIGHT_WRITE_ATTRIBUTES | INFS_RIGHT_READ_NAMED_METADATA | \
    INFS_RIGHT_WRITE_NAMED_METADATA | INFS_RIGHT_READ_PERMISSIONS | \
    INFS_RIGHT_CHANGE_PERMISSIONS | INFS_RIGHT_TAKE_OWNERSHIP)

#endif
