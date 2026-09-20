// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_WIN32_SECURITY_H
#define INFILFS_WIN32_SECURITY_H

#include <stdint.h>

#include "infilfs/security.h"

/*
 * Windows security-descriptor projection policy.
 *
 * Values below are the stable Windows ACCESS_MASK/file-specific bit values,
 * written with InfiltratorFS-prefixed names so this header stays usable on
 * non-Windows conformance hosts and never depends on windows.h.
 *
 * A Windows SID is an adapter binding for a portable principal; it is not the
 * persistent InfiltratorFS principal identity.  DACL allow/deny ordering and
 * inheritance belong to the portable security object.  This helper only maps
 * one ACE's access mask onto the platform-neutral rights vocabulary.
 */
#define INFS_WIN32_FILE_READ_DATA          UINT32_C(0x00000001)
#define INFS_WIN32_FILE_LIST_DIRECTORY     UINT32_C(0x00000001)
#define INFS_WIN32_FILE_WRITE_DATA         UINT32_C(0x00000002)
#define INFS_WIN32_FILE_ADD_FILE           UINT32_C(0x00000002)
#define INFS_WIN32_FILE_APPEND_DATA        UINT32_C(0x00000004)
#define INFS_WIN32_FILE_ADD_SUBDIRECTORY   UINT32_C(0x00000004)
#define INFS_WIN32_FILE_READ_EA            UINT32_C(0x00000008)
#define INFS_WIN32_FILE_WRITE_EA           UINT32_C(0x00000010)
#define INFS_WIN32_FILE_EXECUTE            UINT32_C(0x00000020)
#define INFS_WIN32_FILE_TRAVERSE           UINT32_C(0x00000020)
#define INFS_WIN32_FILE_DELETE_CHILD       UINT32_C(0x00000040)
#define INFS_WIN32_FILE_READ_ATTRIBUTES    UINT32_C(0x00000080)
#define INFS_WIN32_FILE_WRITE_ATTRIBUTES   UINT32_C(0x00000100)

#define INFS_WIN32_DELETE                  UINT32_C(0x00010000)
#define INFS_WIN32_READ_CONTROL            UINT32_C(0x00020000)
#define INFS_WIN32_WRITE_DAC               UINT32_C(0x00040000)
#define INFS_WIN32_WRITE_OWNER             UINT32_C(0x00080000)
#define INFS_WIN32_SYNCHRONIZE             UINT32_C(0x00100000)
#define INFS_WIN32_ACCESS_SYSTEM_SECURITY  UINT32_C(0x01000000)
#define INFS_WIN32_MAXIMUM_ALLOWED         UINT32_C(0x02000000)

#define INFS_WIN32_GENERIC_ALL             UINT32_C(0x10000000)
#define INFS_WIN32_GENERIC_EXECUTE         UINT32_C(0x20000000)
#define INFS_WIN32_GENERIC_WRITE           UINT32_C(0x40000000)
#define INFS_WIN32_GENERIC_READ            UINT32_C(0x80000000)

static inline uint32_t infs_win32_expand_generic_access(uint32_t mask)
{
    uint32_t expanded = mask &
        ~(INFS_WIN32_GENERIC_ALL | INFS_WIN32_GENERIC_EXECUTE |
          INFS_WIN32_GENERIC_WRITE | INFS_WIN32_GENERIC_READ);

    /*
     * These are the documented file-object generic mappings.  The low specific
     * bits are interpreted as file or directory rights by the projection below.
     * SYNCHRONIZE has no persistent portable ACL meaning and is intentionally
     * discarded after expansion.
     */
    if (mask & INFS_WIN32_GENERIC_READ)
        expanded |= UINT32_C(0x00120089);
    if (mask & INFS_WIN32_GENERIC_WRITE)
        expanded |= UINT32_C(0x00120116);
    if (mask & INFS_WIN32_GENERIC_EXECUTE)
        expanded |= UINT32_C(0x001200a0);
    if (mask & INFS_WIN32_GENERIC_ALL)
        expanded |= UINT32_C(0x001f01ff);
    return expanded;
}

static inline infs_rights_mask
infs_win32_access_mask_to_rights(uint32_t access_mask, int directory)
{
    const uint32_t mask = infs_win32_expand_generic_access(access_mask);
    infs_rights_mask rights = 0;

    if (directory) {
        if (mask & INFS_WIN32_FILE_LIST_DIRECTORY)
            rights |= INFS_RIGHT_LIST_DIRECTORY;
        if (mask & INFS_WIN32_FILE_ADD_FILE)
            rights |= INFS_RIGHT_CREATE_FILE;
        if (mask & INFS_WIN32_FILE_ADD_SUBDIRECTORY)
            rights |= INFS_RIGHT_CREATE_DIRECTORY;
        if (mask & INFS_WIN32_FILE_TRAVERSE)
            rights |= INFS_RIGHT_TRAVERSE_DIRECTORY;
        if (mask & INFS_WIN32_FILE_DELETE_CHILD)
            rights |= INFS_RIGHT_DELETE_CHILD;
    } else {
        if (mask & INFS_WIN32_FILE_READ_DATA)
            rights |= INFS_RIGHT_READ_DATA;
        if (mask & INFS_WIN32_FILE_WRITE_DATA)
            rights |= INFS_RIGHT_WRITE_DATA;
        if (mask & INFS_WIN32_FILE_APPEND_DATA)
            rights |= INFS_RIGHT_APPEND_DATA;
        if (mask & INFS_WIN32_FILE_EXECUTE)
            rights |= INFS_RIGHT_EXECUTE;
    }

    if (mask & INFS_WIN32_FILE_READ_EA)
        rights |= INFS_RIGHT_READ_NAMED_METADATA;
    if (mask & INFS_WIN32_FILE_WRITE_EA)
        rights |= INFS_RIGHT_WRITE_NAMED_METADATA;
    if (mask & INFS_WIN32_FILE_READ_ATTRIBUTES)
        rights |= INFS_RIGHT_READ_ATTRIBUTES;
    if (mask & INFS_WIN32_FILE_WRITE_ATTRIBUTES)
        rights |= INFS_RIGHT_WRITE_ATTRIBUTES;
    if (mask & INFS_WIN32_DELETE)
        rights |= INFS_RIGHT_DELETE;
    if (mask & INFS_WIN32_READ_CONTROL)
        rights |= INFS_RIGHT_READ_PERMISSIONS;
    if (mask & INFS_WIN32_WRITE_DAC)
        rights |= INFS_RIGHT_CHANGE_PERMISSIONS;
    if (mask & INFS_WIN32_WRITE_OWNER)
        rights |= INFS_RIGHT_TAKE_OWNERSHIP;

    return rights;
}

#endif
