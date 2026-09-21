// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/security.h"

#include <stdlib.h>
#include <string.h>

static int id_nonzero(const uint8_t id[16])
{
    uint8_t bits = 0;
    for (unsigned i = 0; i < 16u; ++i)
        bits |= id[i];
    return bits != 0;
}

static int security_ace_valid(const struct infs_security_ace *ace)
{
    return ace && id_nonzero(ace->principal_id) && ace->rights != 0 &&
        (ace->rights & ~INFS_RIGHT_ALL) == 0 &&
        (ace->disposition == INFS_ACE_ALLOW ||
         ace->disposition == INFS_ACE_DENY) &&
        (ace->flags & ~INFS_ACE_KNOWN_FLAGS) == 0;
}

static int security_descriptor_valid(
    const struct infs_security_descriptor *descriptor)
{
    if (!descriptor ||
        !id_nonzero(descriptor->owner_principal_id) ||
        !id_nonzero(descriptor->primary_group_principal_id) ||
        (descriptor->flags & ~INFS_SECURITY_KNOWN_FLAGS) != 0 ||
        (descriptor->flags & INFS_SECURITY_DACL_PRESENT) == 0 ||
        (descriptor->ace_count && !descriptor->aces))
        return 0;
    for (size_t i = 0; i < descriptor->ace_count; ++i)
        if (!security_ace_valid(&descriptor->aces[i]))
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

int infs_security_access_allowed(
    const struct infs_security_descriptor *descriptor,
    const uint8_t *principal_ids, size_t principal_count,
    infs_rights_mask requested)
{
    if (!security_descriptor_valid(descriptor) ||
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
            !subject_has_principal(principal_ids, principal_count,
                                   ace->principal_id))
            continue;
        infs_rights_mask hit = remaining & ace->rights;
        if (!hit)
            continue;
        if (ace->disposition == INFS_ACE_DENY)
            return 0;
        if (ace->disposition != INFS_ACE_ALLOW)
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
    if (!security_descriptor_valid(parent) ||
        !owner_principal_id || !primary_group_principal_id || !child ||
        !id_nonzero(owner_principal_id) ||
        !id_nonzero(primary_group_principal_id))
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
