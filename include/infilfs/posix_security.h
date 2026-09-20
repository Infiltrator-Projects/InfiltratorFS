// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_POSIX_SECURITY_H
#define INFILFS_POSIX_SECURITY_H

#include <stdint.h>

#include "infilfs/security.h"

/*
 * Linux/POSIX projection policy.
 *
 * Numeric UID/GID values remain adapter-local compatibility bindings. They are
 * not portable principal IDs. Mode and POSIX ACL rwx triplets project onto the
 * portable rights below; operations may require multiple projected rights
 * (for example directory creation requires write plus traverse).
 */
enum infs_posix_subject_class {
    INFS_POSIX_SUBJECT_OWNER = 0,
    INFS_POSIX_SUBJECT_GROUP = 1,
    INFS_POSIX_SUBJECT_OTHER = 2
};

#define INFS_POSIX_ACL_EXECUTE 1u
#define INFS_POSIX_ACL_WRITE   2u
#define INFS_POSIX_ACL_READ    4u

static inline infs_rights_mask
infs_posix_acl_perms_to_rights(uint32_t perms, int directory)
{
    infs_rights_mask rights = 0;

    perms &= 7u;
    if (directory) {
        if (perms & INFS_POSIX_ACL_READ)
            rights |= INFS_RIGHT_LIST_DIRECTORY;
        if (perms & INFS_POSIX_ACL_WRITE)
            rights |= INFS_RIGHT_CREATE_FILE |
                      INFS_RIGHT_CREATE_DIRECTORY |
                      INFS_RIGHT_DELETE_CHILD;
        if (perms & INFS_POSIX_ACL_EXECUTE)
            rights |= INFS_RIGHT_TRAVERSE_DIRECTORY;
    } else {
        if (perms & INFS_POSIX_ACL_READ)
            rights |= INFS_RIGHT_READ_DATA;
        if (perms & INFS_POSIX_ACL_WRITE)
            rights |= INFS_RIGHT_WRITE_DATA | INFS_RIGHT_APPEND_DATA;
        if (perms & INFS_POSIX_ACL_EXECUTE)
            rights |= INFS_RIGHT_EXECUTE;
    }
    return rights;
}

static inline infs_rights_mask
infs_posix_mode_to_rights(uint32_t mode,
                          enum infs_posix_subject_class subject,
                          int directory)
{
    unsigned int shift;

    switch (subject) {
    case INFS_POSIX_SUBJECT_OWNER:
        shift = 6u;
        break;
    case INFS_POSIX_SUBJECT_GROUP:
        shift = 3u;
        break;
    case INFS_POSIX_SUBJECT_OTHER:
        shift = 0u;
        break;
    default:
        return 0;
    }
    return infs_posix_acl_perms_to_rights((mode >> shift) & 7u, directory);
}

#endif
