// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"

/*
 * Native mirror of the portable security-record canonicality rules.
 * Persistent security meaning belongs to the format, not to VFS credentials,
 * so keep these byte-level validators isolated from Linux access policy.
 */

bool infilfs_security_reserved_principal_id(const u8 id[16])
{
    return id && !memchr_inv(id, 0, 15) && id[15] != 0 &&
        id[15] <= INFILFS_SECURITY_PRINCIPAL_RESERVED_MAX_CODE;
}

static bool infilfs_security_known_well_known_id(const u8 id[16])
{
    if (!infilfs_security_reserved_principal_id(id))
        return false;
    return id[15] >= INFILFS_SECURITY_PRINCIPAL_OWNER_CODE &&
        id[15] <= INFILFS_SECURITY_PRINCIPAL_CREATOR_GROUP_CODE;
}

static bool infilfs_security_creator_id(const u8 id[16])
{
    return infilfs_security_reserved_principal_id(id) &&
        (id[15] == INFILFS_SECURITY_PRINCIPAL_CREATOR_OWNER_CODE ||
         id[15] == INFILFS_SECURITY_PRINCIPAL_CREATOR_GROUP_CODE);
}

bool infilfs_security_sid_valid(const u8 *sid, u16 size)
{
    if (!sid || size < INFILFS_SECURITY_WINDOWS_SID_MIN ||
        size > INFILFS_SECURITY_WINDOWS_SID_MAX ||
        sid[0] != INFILFS_SECURITY_WINDOWS_SID_REVISION ||
        sid[1] > INFILFS_SECURITY_WINDOWS_SID_MAX_SUB_AUTHORITIES)
        return false;
    return size == INFILFS_SECURITY_WINDOWS_SID_MIN +
        (u16)sid[1] * 4u;
}

bool infilfs_security_ace_valid(
    const struct infilfs_security_ace_disk *ace)
{
    u64 rights;
    u16 disposition, flags;

    if (!ace || !memchr_inv(ace->principal_id, 0, 16) ||
        le32_to_cpu(ace->reserved) != 0)
        return false;
    rights = le64_to_cpu(ace->rights);
    disposition = le16_to_cpu(ace->disposition);
    flags = le16_to_cpu(ace->flags);
    if (!rights || (rights & ~INFILFS_SECURITY_RIGHT_ALL) ||
        (disposition != INFILFS_SECURITY_ACE_ALLOW &&
         disposition != INFILFS_SECURITY_ACE_DENY) ||
        (flags & ~INFILFS_SECURITY_ACE_KNOWN_FLAGS))
        return false;
    if (infilfs_security_reserved_principal_id(ace->principal_id) &&
        !infilfs_security_known_well_known_id(ace->principal_id))
        return false;
    if (infilfs_security_creator_id(ace->principal_id)) {
        u16 inherit = flags &
            (INFILFS_SECURITY_ACE_INHERIT_FILE |
             INFILFS_SECURITY_ACE_INHERIT_DIRECTORY);
        if (!inherit || !(flags & INFILFS_SECURITY_ACE_INHERIT_ONLY))
            return false;
    }
    return true;
}
