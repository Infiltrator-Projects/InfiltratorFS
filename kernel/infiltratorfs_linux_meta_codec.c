// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"

void infilfs_linux_meta_uuid(const u8 id[16], char out[37])
{
    scnprintf(out, 37,
              "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
              "%02x%02x%02x%02x%02x%02x",
              id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
              id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
}

void infilfs_linux_meta_init(struct infilfs_linux_meta_header *header)
{
    memset(header, 0, sizeof(*header));
    memcpy(header->magic, INFILFS_LINUX_META_MAGIC, sizeof(header->magic));
    header->version = cpu_to_le32(INFILFS_LINUX_META_VERSION);
}


int infilfs_linux_meta_validate_blob(const u8 *blob, size_t size)
{
    const struct infilfs_linux_meta_header *header;
    size_t offset;

    if (size < sizeof(*header) || size > INFILFS_LINUX_META_MAX)
        return -EFSCORRUPTED;
    header = (const struct infilfs_linux_meta_header *)blob;
    if (memcmp(header->magic, INFILFS_LINUX_META_MAGIC,
               sizeof(header->magic)) != 0 ||
        le32_to_cpu(header->version) != INFILFS_LINUX_META_VERSION ||
        le32_to_cpu(header->reserved) != 0 ||
        le32_to_cpu(header->xattr_bytes) != size - sizeof(*header))
        return -EFSCORRUPTED;

    offset = sizeof(*header);
    while (offset < size) {
        const struct infilfs_linux_xattr_record *record;
        size_t name_length;
        size_t value_length;
        size_t record_size;

        if (size - offset < sizeof(*record))
            return -EFSCORRUPTED;
        record = (const struct infilfs_linux_xattr_record *)(blob + offset);
        name_length = le16_to_cpu(record->name_length);
        value_length = le32_to_cpu(record->value_length);
        if (le16_to_cpu(record->reserved) != 0 || !name_length ||
            name_length > size - offset - sizeof(*record))
            return -EFSCORRUPTED;
        if (value_length >
            size - offset - sizeof(*record) - name_length)
            return -EFSCORRUPTED;
        if (name_length > XATTR_NAME_MAX ||
            value_length > XATTR_SIZE_MAX ||
            memchr(blob + offset + sizeof(*record), '\0', name_length))
            return -EFSCORRUPTED;
        record_size = sizeof(*record) + name_length + value_length;
        if (record_size > size - offset)
            return -EFSCORRUPTED;
        offset += record_size;
    }
    return offset == size ? 0 : -EFSCORRUPTED;
}

int infilfs_linux_meta_find_xattr(
    const u8 *blob, size_t size, const char *name, size_t *offset_out,
    size_t *record_size_out, size_t *value_offset_out,
    size_t *value_length_out)
{
    size_t wanted = strlen(name);
    size_t offset = sizeof(struct infilfs_linux_meta_header);

    while (offset < size) {
        const struct infilfs_linux_xattr_record *record =
            (const struct infilfs_linux_xattr_record *)(blob + offset);
        size_t name_length = le16_to_cpu(record->name_length);
        size_t value_length = le32_to_cpu(record->value_length);
        size_t record_size = sizeof(*record) + name_length + value_length;

        if (name_length == wanted &&
            memcmp(blob + offset + sizeof(*record), name, wanted) == 0) {
            if (offset_out)
                *offset_out = offset;
            if (record_size_out)
                *record_size_out = record_size;
            if (value_offset_out)
                *value_offset_out = offset + sizeof(*record) + name_length;
            if (value_length_out)
                *value_length_out = value_length;
            return 1;
        }
        offset += record_size;
    }
    return 0;
}

int infilfs_linux_xattr_name(const struct xattr_handler *handler,
                                    const char *name, char **full_out)
{
    size_t prefix;
    size_t suffix;
    char *full;

    *full_out = NULL;
    if (!handler || !handler->prefix || !name)
        return -EINVAL;
    prefix = strlen(handler->prefix);
    suffix = strnlen(name, XATTR_NAME_MAX + 1u);
    if (!suffix)
        return -EINVAL;
    if (prefix > XATTR_NAME_MAX || suffix > XATTR_NAME_MAX ||
        suffix > XATTR_NAME_MAX - prefix)
        return -ERANGE;
    full = kmalloc(prefix + suffix + 1u, GFP_NOFS);
    if (!full)
        return -ENOMEM;
    memcpy(full, handler->prefix, prefix);
    memcpy(full + prefix, name, suffix + 1u);
    *full_out = full;
    return 0;
}

