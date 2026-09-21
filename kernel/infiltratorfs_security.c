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


static int infilfs_security_derived_object_id(
    const u8 domain[16], const u8 digest[32], u16 slot, u8 object_id[16])
{
    u8 material[50];
    u8 full[32];

    memcpy(material, domain, 16);
    memcpy(material + 16, digest, 32);
    material[48] = (u8)slot;
    material[49] = (u8)(slot >> 8);
    if (infilfs_crypto_sha256(material, sizeof(material), full))
        return -EIO;
    memcpy(object_id, full, 16);
    return 0;
}

static int infilfs_security_binding_digest(
    u16 type, const u8 *value, u16 size, u8 digest[32])
{
    static const u8 domain[16] = {
        'I','N','F','S','-','B','I','N','D','-','V','2',0,0,0,0
    };
    u8 material[16 + 4 + INFILFS_SECURITY_BINDING_MAX];

    if (!value || !size || size > INFILFS_SECURITY_BINDING_MAX)
        return -EINVAL;
    memcpy(material, domain, 16);
    material[16] = (u8)type;
    material[17] = (u8)(type >> 8);
    material[18] = (u8)size;
    material[19] = (u8)(size >> 8);
    memcpy(material + 20, value, size);
    return infilfs_crypto_sha256(material, 20u + size, digest);
}

static int infilfs_security_binding_object_id(
    const u8 digest[32], u16 slot, u8 object_id[16])
{
    static const u8 domain[16] = {
        'I','N','F','S','-','B','I','N','D','-','I','D','2',0,0,0
    };
    return infilfs_security_derived_object_id(domain, digest, slot, object_id);
}

static int infilfs_security_descriptor_object_id(
    const u8 digest[32], u16 slot, u8 object_id[16])
{
    static const u8 domain[16] = {
        'I','N','F','S','-','D','E','S','C','-','I','D','2',0,0,0
    };
    return infilfs_security_derived_object_id(domain, digest, slot, object_id);
}

bool infilfs_security_binding_index_valid(
    const struct infilfs_object_header_disk *header,
    const struct infilfs_security_binding_index_payload_disk *payload)
{
    u16 type, size, slot;
    u8 digest[32], expected_id[16];

    if (!header || !payload)
        return false;
    type = le16_to_cpu(payload->binding_type);
    size = le16_to_cpu(payload->binding_size);
    slot = le16_to_cpu(payload->slot);

    if (le16_to_cpu(payload->version) != INFILFS_SECURITY_VERSION ||
        slot >= INFILFS_SECURITY_HASH_SLOTS ||
        le32_to_cpu(payload->reserved) != 0 ||
        !memchr_inv(payload->principal_id, 0, 16) ||
        infilfs_security_reserved_principal_id(payload->principal_id) ||
        memcmp(header->parent_id, payload->principal_id, 16) != 0 ||
        !size || size > INFILFS_SECURITY_BINDING_MAX ||
        type == INFILFS_SECURITY_BINDING_OPAQUE)
        return false;

    if (type == INFILFS_SECURITY_BINDING_POSIX_UID ||
        type == INFILFS_SECURITY_BINDING_POSIX_GID) {
        if (size != INFILFS_SECURITY_POSIX_BINDING_SIZE ||
            !memchr_inv(payload->value, 0,
                        INFILFS_SECURITY_POSIX_AUTHORITY_SIZE))
            return false;
    } else if (type == INFILFS_SECURITY_BINDING_WINDOWS_SID) {
        if (!infilfs_security_sid_valid(payload->value, size))
            return false;
    } else {
        return false;
    }

    if (memchr_inv(payload->value + size, 0,
                   INFILFS_SECURITY_BINDING_MAX - size))
        return false;
    if (infilfs_security_binding_digest(type, payload->value, size, digest))
        return false;
    if (memcmp(digest, payload->binding_digest, sizeof(digest)) != 0)
        return false;
    if (infilfs_security_binding_object_id(digest, slot, expected_id))
        return false;
    return memcmp(expected_id, header->object_id, 16) == 0;
}

bool infilfs_security_descriptor_valid(
    struct super_block *sb, const struct infilfs_object_header_disk *header,
    u32 payload_size)
{
    const struct infilfs_security_payload_disk *security;
    const struct infilfs_security_ace_disk *aces;
    const __le64 *pages;
    u32 ace_count, page_count, expected_pages, copied = 0;
    u16 version, slot;
    size_t canonical_size;
    u8 *canonical = NULL;
    u8 *page_block = NULL;
    u8 digest[32], expected_id[16];
    bool valid = false;

    if (!sb || !header || payload_size < sizeof(*security))
        return false;
    if (memchr_inv(header->parent_id, 0, sizeof(header->parent_id)))
        return false;

    version = le16_to_cpu(header->object_version);
    security = (const struct infilfs_security_payload_disk *)(header + 1);
    ace_count = le32_to_cpu(security->ace_count);
    page_count = le32_to_cpu(security->page_count);
    slot = le16_to_cpu(security->identity_slot);

    if (le16_to_cpu(security->version) != INFILFS_SECURITY_VERSION ||
        (le16_to_cpu(security->flags) & ~INFILFS_SECURITY_KNOWN_FLAGS) ||
        !(le16_to_cpu(security->flags) & INFILFS_SECURITY_DACL_PRESENT) ||
        slot >= INFILFS_SECURITY_HASH_SLOTS ||
        le16_to_cpu(security->reserved) != 0 ||
        !memchr_inv(security->owner_principal_id, 0, 16) ||
        !memchr_inv(security->primary_group_principal_id, 0, 16) ||
        infilfs_security_reserved_principal_id(
            security->owner_principal_id) ||
        infilfs_security_reserved_principal_id(
            security->primary_group_principal_id) ||
        ace_count > INFILFS_SECURITY_MAX_ACES)
        return false;

    canonical_size = 40u + (size_t)ace_count *
        sizeof(struct infilfs_security_ace_disk);
    canonical = kvmalloc(canonical_size ? canonical_size : 1u, GFP_NOFS);
    if (!canonical)
        return false;

    memcpy(canonical, &security->version, 2);
    memcpy(canonical + 2, &security->flags, 2);
    memcpy(canonical + 4, &security->ace_count, 4);
    memcpy(canonical + 8, security->owner_principal_id, 16);
    memcpy(canonical + 24, security->primary_group_principal_id, 16);

    if (version == INFILFS_OBJECT_VERSION_CLASSIC) {
        if (page_count != 0 ||
            ace_count > INFILFS_SECURITY_INLINE_ACES ||
            payload_size != sizeof(*security) +
                (size_t)ace_count * sizeof(*aces))
            goto out;

        aces = (const struct infilfs_security_ace_disk *)(security + 1);
        for (u32 i = 0; i < ace_count; ++i)
            if (!infilfs_security_ace_valid(&aces[i]))
                goto out;
        memcpy(canonical + 40, aces,
               (size_t)ace_count * sizeof(*aces));
        copied = ace_count;
    } else if (version == INFILFS_OBJECT_VERSION_PAGED) {
        expected_pages = ace_count ?
            DIV_ROUND_UP(ace_count, INFILFS_SECURITY_ACES_PER_PAGE) : 0u;
        if (page_count != expected_pages ||
            page_count > INFILFS_SECURITY_PAGE_POINTERS ||
            payload_size != sizeof(*security) +
                (size_t)page_count * sizeof(*pages))
            goto out;

        pages = (const __le64 *)(security + 1);
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!page_block && page_count)
            goto out;

        for (u32 p = 0; p < page_count; ++p) {
            u64 block = le64_to_cpu(pages[p]);
            const struct infilfs_metadata_page_disk *page;
            u32 count, bytes;
            const struct infilfs_security_ace_disk *page_aces;
            int ret;

            if (!block)
                goto out;
            ret = infilfs_read_allocated_block(sb, block, page_block);
            if (ret)
                goto out;
            if (!infilfs_metadata_page_valid(
                    sb, page_block,
                    (const u8 *)INFILFS_SECURITY_ACE_PAGE_MAGIC,
                    header->object_id))
                goto out;

            page = (const struct infilfs_metadata_page_disk *)page_block;
            count = le32_to_cpu(page->entry_count);
            bytes = le32_to_cpu(page->bytes_used);
            if (count > INFILFS_SECURITY_ACES_PER_PAGE ||
                bytes != count * sizeof(*page_aces) ||
                count > ace_count - copied)
                goto out;
            page_aces = (const struct infilfs_security_ace_disk *)(page + 1);
            for (u32 i = 0; i < count; ++i)
                if (!infilfs_security_ace_valid(&page_aces[i]))
                    goto out;
            memcpy(canonical + 40 +
                       (size_t)copied * sizeof(*page_aces),
                   page_aces, bytes);
            copied += count;
        }
    } else {
        goto out;
    }

    if (copied != ace_count)
        goto out;
    if (infilfs_crypto_sha256(canonical, canonical_size, digest))
        goto out;
    if (memcmp(digest, security->semantic_digest, sizeof(digest)) != 0)
        goto out;
    if (infilfs_security_descriptor_object_id(digest, slot, expected_id))
        goto out;
    valid = memcmp(expected_id, header->object_id, 16) == 0;

out:
    kfree(page_block);
    kvfree(canonical);
    return valid;
}
