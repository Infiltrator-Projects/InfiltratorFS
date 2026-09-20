// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/posix_security.h"
#include "infilfs/win32_security.h"

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

    const uint32_t win_file_mask =
        INFS_WIN32_FILE_READ_DATA | INFS_WIN32_FILE_WRITE_DATA |
        INFS_WIN32_FILE_APPEND_DATA | INFS_WIN32_FILE_EXECUTE |
        INFS_WIN32_FILE_READ_EA | INFS_WIN32_FILE_WRITE_EA |
        INFS_WIN32_FILE_READ_ATTRIBUTES | INFS_WIN32_FILE_WRITE_ATTRIBUTES |
        INFS_WIN32_DELETE | INFS_WIN32_READ_CONTROL |
        INFS_WIN32_WRITE_DAC | INFS_WIN32_WRITE_OWNER;
    const infs_rights_mask win_file =
        infs_win32_access_mask_to_rights(win_file_mask, 0);
    expect(win_file ==
               (INFS_RIGHT_READ_DATA | INFS_RIGHT_WRITE_DATA |
                INFS_RIGHT_APPEND_DATA | INFS_RIGHT_EXECUTE |
                INFS_RIGHT_READ_NAMED_METADATA |
                INFS_RIGHT_WRITE_NAMED_METADATA |
                INFS_RIGHT_READ_ATTRIBUTES | INFS_RIGHT_WRITE_ATTRIBUTES |
                INFS_RIGHT_DELETE | INFS_RIGHT_READ_PERMISSIONS |
                INFS_RIGHT_CHANGE_PERMISSIONS | INFS_RIGHT_TAKE_OWNERSHIP),
           "Windows file ACCESS_MASK projection");

    const uint32_t win_dir_mask =
        INFS_WIN32_FILE_LIST_DIRECTORY | INFS_WIN32_FILE_ADD_FILE |
        INFS_WIN32_FILE_ADD_SUBDIRECTORY | INFS_WIN32_FILE_TRAVERSE |
        INFS_WIN32_FILE_DELETE_CHILD | INFS_WIN32_DELETE;
    const infs_rights_mask win_dir =
        infs_win32_access_mask_to_rights(win_dir_mask, 1);
    expect(win_dir ==
               (INFS_RIGHT_LIST_DIRECTORY | INFS_RIGHT_CREATE_FILE |
                INFS_RIGHT_CREATE_DIRECTORY | INFS_RIGHT_TRAVERSE_DIRECTORY |
                INFS_RIGHT_DELETE_CHILD | INFS_RIGHT_DELETE),
           "Windows directory ACCESS_MASK projection");

    const infs_rights_mask win_generic_read =
        infs_win32_access_mask_to_rights(INFS_WIN32_GENERIC_READ, 0);
    expect(win_generic_read ==
               (INFS_RIGHT_READ_DATA | INFS_RIGHT_READ_NAMED_METADATA |
                INFS_RIGHT_READ_ATTRIBUTES | INFS_RIGHT_READ_PERMISSIONS),
           "Windows GENERIC_READ expansion");

    const infs_rights_mask win_generic_all_dir =
        infs_win32_access_mask_to_rights(INFS_WIN32_GENERIC_ALL, 1);
    expect(win_generic_all_dir ==
               (INFS_RIGHT_LIST_DIRECTORY | INFS_RIGHT_CREATE_FILE |
                INFS_RIGHT_CREATE_DIRECTORY | INFS_RIGHT_TRAVERSE_DIRECTORY |
                INFS_RIGHT_DELETE_CHILD | INFS_RIGHT_READ_NAMED_METADATA |
                INFS_RIGHT_WRITE_NAMED_METADATA |
                INFS_RIGHT_READ_ATTRIBUTES | INFS_RIGHT_WRITE_ATTRIBUTES |
                INFS_RIGHT_DELETE | INFS_RIGHT_READ_PERMISSIONS |
                INFS_RIGHT_CHANGE_PERMISSIONS | INFS_RIGHT_TAKE_OWNERSHIP),
           "Windows GENERIC_ALL directory expansion");

    expect(infs_win32_access_mask_to_rights(
               INFS_WIN32_SYNCHRONIZE | INFS_WIN32_ACCESS_SYSTEM_SECURITY |
               INFS_WIN32_MAXIMUM_ALLOWED, 0) == 0,
           "Windows host/meta rights must not widen portable ACL rights");

    puts("security-policy: PASS");
    return 0;
}
