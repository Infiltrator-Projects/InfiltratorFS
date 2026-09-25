// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>

#include "infiltratorfs-windows-metadata.h"
#include "infilfs/win32_security.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int infilfs_windows_ticks_to_timestamp(INT64 ticks,
                                       struct infs_timestamp *out)
{
    const INT64 epoch = INT64_C(116444736000000000);
    INT64 delta;
    INT64 seconds;
    INT64 remainder;

    if (!out)
        return 0;
    delta = ticks - epoch;
    seconds = delta / INT64_C(10000000);
    remainder = delta % INT64_C(10000000);
    if (remainder < 0) {
        remainder += INT64_C(10000000);
        --seconds;
    }
    out->seconds = seconds;
    out->nanoseconds = (uint32_t)(remainder * 100);
    return 1;
}

INT64 infilfs_timestamp_to_windows_ticks(const struct infs_timestamp *value)
{
    const INT64 epoch_seconds = INT64_C(11644473600);
    if (!infs_timestamp_valid(value) || value->seconds < -epoch_seconds)
        return 0;
    if (value->seconds >
        INT64_MAX / INT64_C(10000000) - epoch_seconds)
        return INT64_MAX;
    return (value->seconds + epoch_seconds) * INT64_C(10000000) +
           (INT64)(value->nanoseconds / 100u);
}

uint64_t infilfs_windows_attributes_to_portable(DWORD attributes)
{
    uint64_t flags = 0;
    if (attributes & FILE_ATTRIBUTE_READONLY)
        flags |= INFS_ATTR_READ_ONLY;
    if (attributes & FILE_ATTRIBUTE_HIDDEN)
        flags |= INFS_ATTR_HIDDEN;
    if (attributes & FILE_ATTRIBUTE_SYSTEM)
        flags |= INFS_ATTR_SYSTEM;
    if (attributes & FILE_ATTRIBUTE_ARCHIVE)
        flags |= INFS_ATTR_ARCHIVE;
    if (attributes & FILE_ATTRIBUTE_TEMPORARY)
        flags |= INFS_ATTR_TEMPORARY;
    if (attributes & FILE_ATTRIBUTE_NOT_CONTENT_INDEXED)
        flags |= INFS_ATTR_NOT_CONTENT_INDEXED;
    return flags;
}

DWORD infilfs_portable_to_windows_attributes(uint64_t flags, int directory)
{
    DWORD attributes = directory ? FILE_ATTRIBUTE_DIRECTORY : 0;
    if (flags & INFS_ATTR_READ_ONLY)
        attributes |= FILE_ATTRIBUTE_READONLY;
    if (flags & INFS_ATTR_HIDDEN)
        attributes |= FILE_ATTRIBUTE_HIDDEN;
    if (flags & INFS_ATTR_SYSTEM)
        attributes |= FILE_ATTRIBUTE_SYSTEM;
    if (flags & INFS_ATTR_ARCHIVE)
        attributes |= FILE_ATTRIBUTE_ARCHIVE;
    if (flags & INFS_ATTR_TEMPORARY)
        attributes |= FILE_ATTRIBUTE_TEMPORARY;
    if (flags & INFS_ATTR_NOT_CONTENT_INDEXED)
        attributes |= FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    if (!attributes && !directory)
        attributes = FILE_ATTRIBUTE_NORMAL;
    return attributes;
}

void infilfs_windows_basic_to_time_update(
    const FILE_BASIC_INFO *basic, struct infs_time_update *update)
{
    if (!basic || !update)
        return;
    memset(update, 0, sizeof(*update));
    update->birth_action = INFS_TIME_SET;
    update->access_action = INFS_TIME_SET;
    update->modification_action = INFS_TIME_SET;
    update->change_action = INFS_TIME_SET;
    (void)infilfs_windows_ticks_to_timestamp(
        basic->CreationTime.QuadPart, &update->birth_time);
    (void)infilfs_windows_ticks_to_timestamp(
        basic->LastAccessTime.QuadPart, &update->access_time);
    (void)infilfs_windows_ticks_to_timestamp(
        basic->LastWriteTime.QuadPart, &update->modification_time);
    (void)infilfs_windows_ticks_to_timestamp(
        basic->ChangeTime.QuadPart, &update->change_time);
}


static int windows_id_equal(const uint8_t left[16], const uint8_t right[16])
{
    return memcmp(left, right, 16u) == 0;
}

static uint16_t windows_ace_flags_to_portable(BYTE flags)
{
    uint16_t out = 0;
    if (flags & OBJECT_INHERIT_ACE)
        out |= INFS_ACE_INHERIT_FILE;
    if (flags & CONTAINER_INHERIT_ACE)
        out |= INFS_ACE_INHERIT_DIRECTORY;
    if (flags & INHERIT_ONLY_ACE)
        out |= INFS_ACE_INHERIT_ONLY;
    if (flags & NO_PROPAGATE_INHERIT_ACE)
        out |= INFS_ACE_NO_PROPAGATE;
    if (flags & INHERITED_ACE)
        out |= INFS_ACE_INHERITED;
    return out;
}

static BYTE portable_ace_flags_to_windows(uint16_t flags)
{
    BYTE out = 0;
    if (flags & INFS_ACE_INHERIT_FILE)
        out |= OBJECT_INHERIT_ACE;
    if (flags & INFS_ACE_INHERIT_DIRECTORY)
        out |= CONTAINER_INHERIT_ACE;
    if (flags & INFS_ACE_INHERIT_ONLY)
        out |= INHERIT_ONLY_ACE;
    if (flags & INFS_ACE_NO_PROPAGATE)
        out |= NO_PROPAGATE_INHERIT_ACE;
    if (flags & INFS_ACE_INHERITED)
        out |= INHERITED_ACE;
    return out;
}

static infs_status windows_sid_principal(
    struct infs_volume *volume, PSID sid, uint16_t kind,
    uint8_t principal_id[16])
{
    if (!volume || !sid || !IsValidSid(sid) || !principal_id)
        return INFS_STATUS_INVALID_ARGUMENT;

    DWORD sid_size = GetLengthSid(sid);
    if (sid_size < INFS_SECURITY_WINDOWS_SID_MIN ||
        sid_size > INFS_SECURITY_WINDOWS_SID_MAX)
        return INFS_STATUS_NOT_SUPPORTED;

    struct infs_security_binding binding;
    infs_status status = infs_security_binding_init_windows_sid(
        &binding, sid, sid_size);
    if (status != INFS_STATUS_OK)
        return status;

    status = infs_find_security_principal_by_binding(
        volume, &binding, principal_id);
    if (status == INFS_STATUS_OK)
        return status;
    if (status != INFS_STATUS_NOT_FOUND)
        return status;

    struct infs_security_principal principal;
    memset(&principal, 0, sizeof(principal));
    principal.kind = kind;
    principal.bindings = &binding;
    principal.binding_count = 1u;
    status = infs_put_security_principal(volume, &principal);
    if (status == INFS_STATUS_OK)
        memcpy(principal_id, principal.principal_id, 16u);
    return status;
}

static infs_status windows_ace_to_portable(
    struct infs_volume *volume, const ACE_HEADER *header, int directory,
    struct infs_security_ace *out)
{
    if (!volume || !header || !out)
        return INFS_STATUS_INVALID_ARGUMENT;

    ACCESS_MASK mask;
    PSID sid;
    uint16_t disposition;
    switch (header->AceType) {
    case ACCESS_ALLOWED_ACE_TYPE: {
        const ACCESS_ALLOWED_ACE *ace = (const ACCESS_ALLOWED_ACE *)header;
        mask = ace->Mask;
        sid = (PSID)&ace->SidStart;
        disposition = INFS_ACE_ALLOW;
        break;
    }
    case ACCESS_DENIED_ACE_TYPE: {
        const ACCESS_DENIED_ACE *ace = (const ACCESS_DENIED_ACE *)header;
        mask = ace->Mask;
        sid = (PSID)&ace->SidStart;
        disposition = INFS_ACE_DENY;
        break;
    }
    default:
        return INFS_STATUS_NOT_SUPPORTED;
    }

    if (!IsValidSid(sid))
        return INFS_STATUS_CORRUPT;

    memset(out, 0, sizeof(*out));
    infs_status status = windows_sid_principal(
        volume, sid, INFS_PRINCIPAL_USER, out->principal_id);
    if (status != INFS_STATUS_OK)
        return status;
    out->rights = infs_win32_access_mask_to_rights(mask, directory);
    out->disposition = disposition;
    out->flags = windows_ace_flags_to_portable(header->AceFlags);
    return INFS_STATUS_OK;
}

infs_status infilfs_windows_import_security(
    struct infs_volume *volume, const char *path,
    const wchar_t *native_path, int directory)
{
    if (!volume || !path || !native_path)
        return INFS_STATUS_INVALID_ARGUMENT;

    PSID owner = NULL;
    PSID group = NULL;
    PACL dacl = NULL;
    PSECURITY_DESCRIPTOR native = NULL;
    DWORD error = GetNamedSecurityInfoW(
        (LPWSTR)native_path, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
            DACL_SECURITY_INFORMATION,
        &owner, &group, &dacl, NULL, &native);
    if (error != ERROR_SUCCESS)
        return INFS_STATUS_IO_ERROR;

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(
            native, &control, &revision)) {
        LocalFree(native);
        return INFS_STATUS_IO_ERROR;
    }

    struct infs_security_descriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    infs_status status = windows_sid_principal(
        volume, owner, INFS_PRINCIPAL_USER,
        descriptor.owner_principal_id);
    if (status == INFS_STATUS_OK)
        status = windows_sid_principal(
            volume, group, INFS_PRINCIPAL_GROUP,
            descriptor.primary_group_principal_id);
    if (status != INFS_STATUS_OK) {
        LocalFree(native);
        return status;
    }

    descriptor.flags = INFS_SECURITY_DACL_PRESENT;
    if (control & SE_DACL_PROTECTED)
        descriptor.flags |= INFS_SECURITY_PROTECTED;
    if (control & SE_DACL_AUTO_INHERITED)
        descriptor.flags |= INFS_SECURITY_AUTO_INHERIT;

    /*
     * Windows treats an absent or NULL DACL as unrestricted. The portable
     * evaluator is deny-by-default, so represent that semantic explicitly as
     * one Everyone allow-all ACE rather than silently turning it into deny-all.
     */
    if (!(control & SE_DACL_PRESENT) || !dacl) {
        descriptor.aces = calloc(1u, sizeof(*descriptor.aces));
        if (!descriptor.aces) {
            LocalFree(native);
            return INFS_STATUS_NO_MEMORY;
        }
        memcpy(descriptor.aces[0].principal_id,
               infs_principal_everyone_id, 16u);
        descriptor.aces[0].rights = INFS_RIGHT_ALL;
        descriptor.aces[0].disposition = INFS_ACE_ALLOW;
        descriptor.ace_count = 1u;
    } else if (dacl->AceCount) {
        descriptor.aces = calloc(
            dacl->AceCount, sizeof(*descriptor.aces));
        if (!descriptor.aces) {
            LocalFree(native);
            return INFS_STATUS_NO_MEMORY;
        }
        for (DWORD i = 0; i < dacl->AceCount; ++i) {
            void *ace = NULL;
            if (!GetAce(dacl, i, &ace)) {
                status = INFS_STATUS_CORRUPT;
                break;
            }
            status = windows_ace_to_portable(
                volume, (const ACE_HEADER *)ace, directory,
                &descriptor.aces[descriptor.ace_count]);
            if (status != INFS_STATUS_OK)
                break;
            descriptor.ace_count++;
        }
    }

    if (status == INFS_STATUS_OK)
        status = infs_set_security_descriptor(
            volume, path, &descriptor);
    infs_free_security_descriptor(&descriptor);
    LocalFree(native);
    return status;
}

static infs_status windows_well_known_sid(
    WELL_KNOWN_SID_TYPE type, PSID *sid_out)
{
    if (!sid_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    *sid_out = NULL;
    DWORD size = SECURITY_MAX_SID_SIZE;
    PSID sid = malloc(size);
    if (!sid)
        return INFS_STATUS_NO_MEMORY;
    if (!CreateWellKnownSid(type, NULL, sid, &size)) {
        free(sid);
        return INFS_STATUS_IO_ERROR;
    }
    *sid_out = sid;
    return INFS_STATUS_OK;
}

static infs_status windows_principal_sid(
    struct infs_volume *volume, const uint8_t principal_id[16],
    PSID owner_sid, PSID group_sid, PSID *sid_out)
{
    if (!volume || !principal_id || !sid_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    *sid_out = NULL;

    if (windows_id_equal(principal_id, infs_principal_owner_id)) {
        DWORD size = GetLengthSid(owner_sid);
        PSID sid = malloc(size);
        if (!sid)
            return INFS_STATUS_NO_MEMORY;
        if (!CopySid(size, sid, owner_sid)) {
            free(sid);
            return INFS_STATUS_IO_ERROR;
        }
        *sid_out = sid;
        return INFS_STATUS_OK;
    }
    if (windows_id_equal(principal_id, infs_principal_group_id)) {
        DWORD size = GetLengthSid(group_sid);
        PSID sid = malloc(size);
        if (!sid)
            return INFS_STATUS_NO_MEMORY;
        if (!CopySid(size, sid, group_sid)) {
            free(sid);
            return INFS_STATUS_IO_ERROR;
        }
        *sid_out = sid;
        return INFS_STATUS_OK;
    }
    if (windows_id_equal(principal_id, infs_principal_everyone_id))
        return windows_well_known_sid(WinWorldSid, sid_out);
    if (windows_id_equal(principal_id, infs_principal_creator_owner_id))
        return windows_well_known_sid(WinCreatorOwnerSid, sid_out);
    if (windows_id_equal(principal_id, infs_principal_creator_group_id))
        return windows_well_known_sid(WinCreatorGroupSid, sid_out);

    struct infs_security_principal principal;
    memset(&principal, 0, sizeof(principal));
    infs_status status = infs_get_security_principal(
        volume, principal_id, &principal);
    if (status != INFS_STATUS_OK)
        return status;

    status = INFS_STATUS_NOT_FOUND;
    for (size_t i = 0; i < principal.binding_count; ++i) {
        const struct infs_security_binding *binding =
            &principal.bindings[i];
        if (binding->type != INFS_BINDING_WINDOWS_SID)
            continue;
        if (!infs_security_windows_sid_valid(
                binding->value, binding->size)) {
            status = INFS_STATUS_CORRUPT;
            break;
        }
        PSID sid = malloc(binding->size);
        if (!sid) {
            status = INFS_STATUS_NO_MEMORY;
            break;
        }
        memcpy(sid, binding->value, binding->size);
        *sid_out = sid;
        status = INFS_STATUS_OK;
        break;
    }
    infs_free_security_principal(&principal);
    return status;
}

static infs_status windows_build_acl(
    struct infs_volume *volume,
    const struct infs_security_descriptor *descriptor,
    int directory, PSID owner_sid, PSID group_sid, PACL *acl_out)
{
    if (!volume || !descriptor || !acl_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    *acl_out = NULL;

    PSID *sids = calloc(
        descriptor->ace_count ? descriptor->ace_count : 1u,
        sizeof(*sids));
    if (!sids)
        return INFS_STATUS_NO_MEMORY;

    size_t acl_size = sizeof(ACL);
    infs_status status = INFS_STATUS_OK;
    for (size_t i = 0; i < descriptor->ace_count; ++i) {
        status = windows_principal_sid(
            volume, descriptor->aces[i].principal_id,
            owner_sid, group_sid, &sids[i]);
        if (status != INFS_STATUS_OK)
            break;
        DWORD sid_size = GetLengthSid(sids[i]);
        size_t base = descriptor->aces[i].disposition == INFS_ACE_DENY ?
            sizeof(ACCESS_DENIED_ACE) : sizeof(ACCESS_ALLOWED_ACE);
        if (acl_size > SIZE_MAX - (base - sizeof(DWORD) + sid_size)) {
            status = INFS_STATUS_OVERFLOW;
            break;
        }
        acl_size += base - sizeof(DWORD) + sid_size;
    }
    if (status != INFS_STATUS_OK)
        goto out;
    if (acl_size > UINT32_MAX) {
        status = INFS_STATUS_OVERFLOW;
        goto out;
    }

    PACL acl = malloc(acl_size);
    if (!acl) {
        status = INFS_STATUS_NO_MEMORY;
        goto out;
    }
    if (!InitializeAcl(acl, (DWORD)acl_size, ACL_REVISION)) {
        free(acl);
        status = INFS_STATUS_IO_ERROR;
        goto out;
    }

    for (size_t i = 0; i < descriptor->ace_count; ++i) {
        const struct infs_security_ace *ace = &descriptor->aces[i];
        DWORD mask = infs_win32_rights_to_access_mask(
            ace->rights, directory);
        BYTE flags = portable_ace_flags_to_windows(ace->flags);
        BOOL okay = ace->disposition == INFS_ACE_DENY ?
            AddAccessDeniedAceEx(
                acl, ACL_REVISION, flags, mask, sids[i]) :
            AddAccessAllowedAceEx(
                acl, ACL_REVISION, flags, mask, sids[i]);
        if (!okay) {
            free(acl);
            status = INFS_STATUS_IO_ERROR;
            goto out;
        }
    }

    *acl_out = acl;
out:
    for (size_t i = 0; i < descriptor->ace_count; ++i)
        free(sids[i]);
    free(sids);
    return status;
}

infs_status infilfs_windows_export_security(
    struct infs_volume *volume, const char *path,
    const wchar_t *native_path, int directory)
{
    if (!volume || !path || !native_path)
        return INFS_STATUS_INVALID_ARGUMENT;

    struct infs_security_descriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    infs_status status = infs_get_security_descriptor(
        volume, path, &descriptor);
    if (status == INFS_STATUS_NOT_FOUND)
        return INFS_STATUS_OK;
    if (status != INFS_STATUS_OK)
        return status;

    PSID owner = NULL;
    PSID group = NULL;
    PACL dacl = NULL;
    status = windows_principal_sid(
        volume, descriptor.owner_principal_id,
        NULL, NULL, &owner);
    if (status == INFS_STATUS_OK)
        status = windows_principal_sid(
            volume, descriptor.primary_group_principal_id,
            NULL, NULL, &group);
    if (status == INFS_STATUS_OK &&
        (descriptor.flags & INFS_SECURITY_DACL_PRESENT))
        status = windows_build_acl(
            volume, &descriptor, directory, owner, group, &dacl);

    if (status == INFS_STATUS_OK) {
        SECURITY_INFORMATION info =
            OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION;
        if (descriptor.flags & INFS_SECURITY_DACL_PRESENT) {
            info |= DACL_SECURITY_INFORMATION;
            info |= (descriptor.flags & INFS_SECURITY_PROTECTED) ?
                PROTECTED_DACL_SECURITY_INFORMATION :
                UNPROTECTED_DACL_SECURITY_INFORMATION;
        }
        DWORD error = SetNamedSecurityInfoW(
            (LPWSTR)native_path, SE_FILE_OBJECT, info,
            owner, group, dacl, NULL);
        if (error != ERROR_SUCCESS)
            status = INFS_STATUS_IO_ERROR;
    }

    free(dacl);
    free(group);
    free(owner);
    infs_free_security_descriptor(&descriptor);
    return status;
}
#endif
