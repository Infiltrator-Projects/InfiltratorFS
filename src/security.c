// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/security.h"

#include <stdlib.h>
#include <string.h>

const uint8_t infs_principal_owner_id[16] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1
};
const uint8_t infs_principal_group_id[16] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2
};
const uint8_t infs_principal_everyone_id[16] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3
};
const uint8_t infs_principal_creator_owner_id[16] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4
};
const uint8_t infs_principal_creator_group_id[16] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5
};

static int id_nonzero(const uint8_t id[16])
{
    uint8_t bits = 0;
    if (!id)
        return 0;
    for (unsigned i = 0; i < 16u; ++i)
        bits |= id[i];
    return bits != 0;
}

static int id_equal(const uint8_t a[16], const uint8_t b[16])
{
    return a && b && memcmp(a, b, 16u) == 0;
}

int infs_security_principal_id_is_reserved(const uint8_t principal_id[16])
{
    if (!principal_id)
        return 0;
    for (unsigned i = 0; i < 15u; ++i)
        if (principal_id[i] != 0)
            return 0;
    return principal_id[15] != 0 &&
        principal_id[15] <= INFS_PRINCIPAL_RESERVED_MAX_CODE;
}

int infs_security_principal_id_is_well_known(const uint8_t principal_id[16])
{
    return id_equal(principal_id, infs_principal_owner_id) ||
        id_equal(principal_id, infs_principal_group_id) ||
        id_equal(principal_id, infs_principal_everyone_id) ||
        id_equal(principal_id, infs_principal_creator_owner_id) ||
        id_equal(principal_id, infs_principal_creator_group_id);
}

int infs_security_principal_id_is_creator(const uint8_t principal_id[16])
{
    return id_equal(principal_id, infs_principal_creator_owner_id) ||
        id_equal(principal_id, infs_principal_creator_group_id);
}

int infs_security_windows_sid_valid(const uint8_t *sid, size_t size)
{
    if (!sid || size < INFS_SECURITY_WINDOWS_SID_MIN ||
        size > INFS_SECURITY_WINDOWS_SID_MAX ||
        sid[0] != INFS_SECURITY_WINDOWS_SID_REVISION ||
        sid[1] > INFS_SECURITY_WINDOWS_SID_MAX_SUB_AUTHORITIES)
        return 0;
    return size == INFS_SECURITY_WINDOWS_SID_MIN +
        (size_t)sid[1] * 4u;
}

int infs_security_binding_is_valid(const struct infs_security_binding *binding)
{
    if (!binding || binding->flags != 0 || binding->size == 0 ||
        binding->size > INFS_SECURITY_BINDING_MAX)
        return 0;

    if (binding->type == INFS_BINDING_POSIX_UID ||
        binding->type == INFS_BINDING_POSIX_GID) {
        if (binding->size != INFS_SECURITY_POSIX_BINDING_SIZE)
            return 0;
        return id_nonzero(binding->value);
    }
    if (binding->type == INFS_BINDING_WINDOWS_SID)
        return infs_security_windows_sid_valid(
            binding->value, binding->size);
    if (binding->type == INFS_BINDING_OPAQUE)
        return 1;
    return 0;
}

infs_status infs_security_binding_init_posix(
    struct infs_security_binding *binding, uint16_t type,
    const uint8_t authority_id[16], uint32_t numeric_id)
{
    if (!binding || !authority_id || !id_nonzero(authority_id) ||
        (type != INFS_BINDING_POSIX_UID && type != INFS_BINDING_POSIX_GID))
        return INFS_STATUS_INVALID_ARGUMENT;
    memset(binding, 0, sizeof(*binding));
    binding->type = type;
    binding->size = INFS_SECURITY_POSIX_BINDING_SIZE;
    memcpy(binding->value, authority_id, INFS_SECURITY_POSIX_AUTHORITY_SIZE);
    binding->value[16] = (uint8_t)numeric_id;
    binding->value[17] = (uint8_t)(numeric_id >> 8u);
    binding->value[18] = (uint8_t)(numeric_id >> 16u);
    binding->value[19] = (uint8_t)(numeric_id >> 24u);
    return INFS_STATUS_OK;
}

infs_status infs_security_binding_get_posix(
    const struct infs_security_binding *binding, uint8_t authority_id[16],
    uint32_t *numeric_id)
{
    if (!binding || !authority_id || !numeric_id ||
        (binding->type != INFS_BINDING_POSIX_UID &&
         binding->type != INFS_BINDING_POSIX_GID) ||
        !infs_security_binding_is_valid(binding))
        return INFS_STATUS_INVALID_ARGUMENT;
    memcpy(authority_id, binding->value, INFS_SECURITY_POSIX_AUTHORITY_SIZE);
    *numeric_id = (uint32_t)binding->value[16] |
        ((uint32_t)binding->value[17] << 8u) |
        ((uint32_t)binding->value[18] << 16u) |
        ((uint32_t)binding->value[19] << 24u);
    return INFS_STATUS_OK;
}

infs_status infs_security_binding_init_windows_sid(
    struct infs_security_binding *binding, const uint8_t *sid, size_t size)
{
    if (!binding || !infs_security_windows_sid_valid(sid, size))
        return INFS_STATUS_INVALID_ARGUMENT;
    memset(binding, 0, sizeof(*binding));
    binding->type = INFS_BINDING_WINDOWS_SID;
    binding->size = (uint16_t)size;
    memcpy(binding->value, sid, size);
    return INFS_STATUS_OK;
}

int infs_security_ace_is_valid(const struct infs_security_ace *ace)
{
    if (!ace || !id_nonzero(ace->principal_id) || ace->rights == 0 ||
        (ace->rights & ~INFS_RIGHT_ALL) != 0 ||
        (ace->disposition != INFS_ACE_ALLOW &&
         ace->disposition != INFS_ACE_DENY) ||
        (ace->flags & ~INFS_ACE_KNOWN_FLAGS) != 0)
        return 0;

    if (infs_security_principal_id_is_reserved(ace->principal_id) &&
        !infs_security_principal_id_is_well_known(ace->principal_id))
        return 0;

    if (infs_security_principal_id_is_creator(ace->principal_id)) {
        uint16_t inherit = ace->flags &
            (INFS_ACE_INHERIT_FILE | INFS_ACE_INHERIT_DIRECTORY);
        if (!inherit || (ace->flags & INFS_ACE_INHERIT_ONLY) == 0)
            return 0;
    }
    return 1;
}

int infs_security_descriptor_is_valid(
    const struct infs_security_descriptor *descriptor)
{
    if (!descriptor ||
        !id_nonzero(descriptor->owner_principal_id) ||
        !id_nonzero(descriptor->primary_group_principal_id) ||
        infs_security_principal_id_is_reserved(
            descriptor->owner_principal_id) ||
        infs_security_principal_id_is_reserved(
            descriptor->primary_group_principal_id) ||
        (descriptor->flags & ~INFS_SECURITY_KNOWN_FLAGS) != 0 ||
        (descriptor->flags & INFS_SECURITY_DACL_PRESENT) == 0 ||
        (descriptor->ace_count && !descriptor->aces))
        return 0;
    for (size_t i = 0; i < descriptor->ace_count; ++i)
        if (!infs_security_ace_is_valid(&descriptor->aces[i]))
            return 0;
    return 1;
}

static int subject_has_principal(const uint8_t *ids, size_t count,
                                 const uint8_t id[16])
{
    if (!ids)
        return 0;
    for (size_t i = 0; i < count; ++i)
        if (memcmp(ids + i * 16u, id, 16u) == 0)
            return 1;
    return 0;
}

static int ace_applies(const struct infs_security_descriptor *descriptor,
                       const uint8_t *subject_ids, size_t subject_count,
                       const uint8_t ace_principal_id[16])
{
    if (id_equal(ace_principal_id, infs_principal_everyone_id))
        return 1;
    if (id_equal(ace_principal_id, infs_principal_owner_id))
        return subject_has_principal(
            subject_ids, subject_count, descriptor->owner_principal_id);
    if (id_equal(ace_principal_id, infs_principal_group_id))
        return subject_has_principal(
            subject_ids, subject_count,
            descriptor->primary_group_principal_id);
    if (infs_security_principal_id_is_creator(ace_principal_id) ||
        (infs_security_principal_id_is_reserved(ace_principal_id) &&
         !infs_security_principal_id_is_well_known(ace_principal_id)))
        return 0;
    return subject_has_principal(subject_ids, subject_count, ace_principal_id);
}

int infs_security_access_allowed(
    const struct infs_security_descriptor *descriptor,
    const uint8_t *principal_ids, size_t principal_count,
    infs_rights_mask requested)
{
    if (!infs_security_descriptor_is_valid(descriptor) ||
        (requested & ~INFS_RIGHT_ALL) != 0 ||
        (principal_count && !principal_ids) ||
        principal_count > SIZE_MAX / 16u)
        return 0;
    if (requested == 0)
        return 1;

    infs_rights_mask remaining = requested;
    for (size_t i = 0; i < descriptor->ace_count; ++i) {
        const struct infs_security_ace *ace = &descriptor->aces[i];
        if ((ace->flags & INFS_ACE_INHERIT_ONLY) != 0 ||
            !ace_applies(descriptor, principal_ids, principal_count,
                         ace->principal_id))
            continue;
        infs_rights_mask hit = remaining & ace->rights;
        if (!hit)
            continue;
        if (ace->disposition == INFS_ACE_DENY)
            return 0;
        remaining &= ~hit;
        if (!remaining)
            return 1;
    }
    return 0;
}

infs_status infs_security_inherit_descriptor(
    const struct infs_security_descriptor *parent, int child_is_directory,
    const uint8_t owner_principal_id[16],
    const uint8_t primary_group_principal_id[16],
    struct infs_security_descriptor *child)
{
    if (!infs_security_descriptor_is_valid(parent) ||
        !owner_principal_id || !primary_group_principal_id || !child ||
        !id_nonzero(owner_principal_id) ||
        !id_nonzero(primary_group_principal_id) ||
        infs_security_principal_id_is_reserved(owner_principal_id) ||
        infs_security_principal_id_is_reserved(primary_group_principal_id))
        return INFS_STATUS_INVALID_ARGUMENT;

    memset(child, 0, sizeof(*child));
    memcpy(child->owner_principal_id, owner_principal_id, 16u);
    memcpy(child->primary_group_principal_id, primary_group_principal_id, 16u);
    child->flags = INFS_SECURITY_DACL_PRESENT |
        (parent->flags & INFS_SECURITY_AUTO_INHERIT);

    size_t count = 0;
    for (size_t i = 0; i < parent->ace_count; ++i) {
        uint16_t flags = parent->aces[i].flags;
        if (child_is_directory) {
            int file_only_no_propagate =
                (flags & INFS_ACE_INHERIT_FILE) != 0 &&
                (flags & INFS_ACE_INHERIT_DIRECTORY) == 0 &&
                (flags & INFS_ACE_NO_PROPAGATE) != 0;
            if (!file_only_no_propagate &&
                (flags & (INFS_ACE_INHERIT_DIRECTORY |
                          INFS_ACE_INHERIT_FILE)) != 0)
                ++count;
        } else if (flags & INFS_ACE_INHERIT_FILE) {
            ++count;
        }
    }
    if (!count)
        return INFS_STATUS_OK;

    child->aces = calloc(count, sizeof(*child->aces));
    if (!child->aces)
        return INFS_STATUS_NO_MEMORY;

    size_t out = 0;
    for (size_t i = 0; i < parent->ace_count; ++i) {
        const struct infs_security_ace *source = &parent->aces[i];
        uint16_t flags = source->flags;
        int inherit = child_is_directory ?
            ((flags & (INFS_ACE_INHERIT_DIRECTORY |
                       INFS_ACE_INHERIT_FILE)) != 0) :
            ((flags & INFS_ACE_INHERIT_FILE) != 0);
        if (child_is_directory &&
            (flags & INFS_ACE_INHERIT_FILE) != 0 &&
            (flags & INFS_ACE_INHERIT_DIRECTORY) == 0 &&
            (flags & INFS_ACE_NO_PROPAGATE) != 0)
            inherit = 0;
        if (!inherit)
            continue;

        struct infs_security_ace *target = &child->aces[out++];
        *target = *source;
        target->flags |= INFS_ACE_INHERITED;

        if (id_equal(source->principal_id, infs_principal_creator_owner_id))
            memcpy(target->principal_id, owner_principal_id, 16u);
        else if (id_equal(source->principal_id,
                          infs_principal_creator_group_id))
            memcpy(target->principal_id, primary_group_principal_id, 16u);

        if (!child_is_directory) {
            target->flags &= ~(INFS_ACE_INHERIT_FILE |
                               INFS_ACE_INHERIT_DIRECTORY |
                               INFS_ACE_INHERIT_ONLY |
                               INFS_ACE_NO_PROPAGATE);
            continue;
        }

        if ((flags & INFS_ACE_INHERIT_DIRECTORY) != 0)
            target->flags &= ~INFS_ACE_INHERIT_ONLY;
        else
            target->flags |= INFS_ACE_INHERIT_ONLY;

        if ((flags & INFS_ACE_NO_PROPAGATE) != 0)
            target->flags &= ~(INFS_ACE_INHERIT_FILE |
                               INFS_ACE_INHERIT_DIRECTORY |
                               INFS_ACE_NO_PROPAGATE);
    }
    child->ace_count = out;
    return INFS_STATUS_OK;
}

void infs_free_security_descriptor(struct infs_security_descriptor *descriptor)
{
    if (!descriptor)
        return;
    free(descriptor->aces);
    memset(descriptor, 0, sizeof(*descriptor));
}

void infs_free_security_principal(struct infs_security_principal *principal)
{
    if (!principal)
        return;
    free(principal->bindings);
    memset(principal, 0, sizeof(*principal));
}
