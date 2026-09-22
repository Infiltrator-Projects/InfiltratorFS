// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"

/*
 * Keep typed-extension payload validation outside the VFS orchestration unit.
 * The extension envelope is host-neutral; the native reader validates only its
 * format, integrity and fail-closed flag/version contract.
 */
bool infilfs_extension_object_valid(
    const struct infilfs_object_header_disk *header,
    u16 version, u32 payload_size)
{
    const struct infilfs_extension_payload_disk *extension;
    u32 data_size;
    u8 digest[32];

    if (!header || version != INFILFS_OBJECT_VERSION_CLASSIC ||
        payload_size < sizeof(*extension) ||
        memchr_inv(header->parent_id, 0, sizeof(header->parent_id)))
        return false;

    extension =
        (const struct infilfs_extension_payload_disk *)(header + 1);
    data_size = le32_to_cpu(extension->data_size);
    if (le16_to_cpu(extension->version) != INFILFS_EXTENSION_VERSION ||
        !le32_to_cpu(extension->type_version) ||
        le32_to_cpu(extension->reserved) != 0 ||
        (le16_to_cpu(extension->flags) & ~INFILFS_KNOWN_EXTENSION_FLAGS) ||
        !memchr_inv(extension->type_id, 0, sizeof(extension->type_id)) ||
        data_size > INFILFS_DISK_BLOCK_SIZE - sizeof(*header) -
            sizeof(*extension) ||
        payload_size != sizeof(*extension) + data_size)
        return false;

    if (infilfs_crypto_sha256(
            (const u8 *)(extension + 1), data_size, digest))
        return false;
    return !memcmp(digest, extension->data_digest, sizeof(digest));
}
