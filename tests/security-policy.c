// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/posix_security.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "security-policy: %s\n", message);
        exit(1);
    }
}

int main(void)
{
    const infs_rights_mask administrative =
        INFS_RIGHT_READ_ATTRIBUTES | INFS_RIGHT_WRITE_ATTRIBUTES |
        INFS_RIGHT_READ_NAMED_METADATA | INFS_RIGHT_WRITE_NAMED_METADATA |
        INFS_RIGHT_READ_PERMISSIONS | INFS_RIGHT_CHANGE_PERMISSIONS |
        INFS_RIGHT_TAKE_OWNERSHIP;

    expect(sizeof(infs_rights_mask) == sizeof(uint64_t),
           "rights mask is not 64-bit");
    expect(INFS_RIGHT_ALL == ((UINT64_C(1) << 17) - 1u),
           "portable right bit assignments are not canonical");

    infs_rights_mask owner =
        infs_posix_mode_to_rights(0754u, INFS_POSIX_SUBJECT_OWNER, 0);
    expect(owner == (INFS_RIGHT_READ_DATA | INFS_RIGHT_WRITE_DATA |
                     INFS_RIGHT_APPEND_DATA | INFS_RIGHT_EXECUTE),
           "regular-file owner mode projection");

    infs_rights_mask group =
        infs_posix_mode_to_rights(0754u, INFS_POSIX_SUBJECT_GROUP, 0);
    expect(group == (INFS_RIGHT_READ_DATA | INFS_RIGHT_EXECUTE),
           "regular-file group mode projection");

    infs_rights_mask other =
        infs_posix_mode_to_rights(0754u, INFS_POSIX_SUBJECT_OTHER, 0);
    expect(other == INFS_RIGHT_READ_DATA,
           "regular-file other mode projection");

    infs_rights_mask directory =
        infs_posix_acl_perms_to_rights(7u, 1);
    expect(directory == (INFS_RIGHT_LIST_DIRECTORY |
                         INFS_RIGHT_CREATE_FILE |
                         INFS_RIGHT_CREATE_DIRECTORY |
                         INFS_RIGHT_DELETE_CHILD |
                         INFS_RIGHT_TRAVERSE_DIRECTORY),
           "directory rwx projection");

    infs_rights_mask acl =
        infs_posix_acl_perms_to_rights(
            INFS_POSIX_ACL_READ | INFS_POSIX_ACL_WRITE, 0);
    expect(acl == (INFS_RIGHT_READ_DATA | INFS_RIGHT_WRITE_DATA |
                   INFS_RIGHT_APPEND_DATA),
           "POSIX ACL rw projection");

    expect((owner & administrative) == 0 &&
           (directory & administrative) == 0,
           "mode bits must not manufacture administrative rights");
    expect(infs_posix_mode_to_rights(
               0777u, (enum infs_posix_subject_class)99, 0) == 0,
           "invalid POSIX subject class must map to no rights");

    puts("security-policy: PASS");
    return 0;
}
