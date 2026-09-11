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


/* O(1) UUID-sidecar lookup after one bounded per-mount cache build. */
static struct infilfs_linux_meta_cache_entry *
infilfs_linux_meta_cache_lookup(struct infilfs_sb_info *sbi,
                                const u8 target_object_id[16])
{
    struct infilfs_linux_meta_cache_entry *entry;
    u32 bucket;

    if (!sbi || !sbi->linux_meta_cache)
        return NULL;
    bucket = infilfs_native_id_hash(target_object_id) &
        (INFILFS_LINUX_META_CACHE_BUCKETS - 1u);
    hlist_for_each_entry(entry, &sbi->linux_meta_cache[bucket], node) {
        if (memcmp(entry->target_object_id, target_object_id, 16) == 0)
            return entry;
    }
    return NULL;
}

static int infilfs_linux_meta_cache_prepare(struct infilfs_sb_info *sbi)
{
    u32 i;

    if (sbi->linux_meta_cache)
        return 0;
    sbi->linux_meta_cache = kvcalloc(
        INFILFS_LINUX_META_CACHE_BUCKETS,
        sizeof(*sbi->linux_meta_cache), GFP_NOFS);
    if (!sbi->linux_meta_cache)
        return -ENOMEM;
    for (i = 0; i < INFILFS_LINUX_META_CACHE_BUCKETS; ++i)
        INIT_HLIST_HEAD(&sbi->linux_meta_cache[i]);
    return 0;
}

static void infilfs_linux_meta_cache_reset(struct infilfs_sb_info *sbi)
{
    u32 i;

    if (!sbi || !sbi->linux_meta_cache)
        return;
    for (i = 0; i < INFILFS_LINUX_META_CACHE_BUCKETS; ++i) {
        struct infilfs_linux_meta_cache_entry *entry;
        struct hlist_node *tmp;

        hlist_for_each_entry_safe(entry, tmp,
                                  &sbi->linux_meta_cache[i], node) {
            hlist_del(&entry->node);
            kfree(entry);
        }
        INIT_HLIST_HEAD(&sbi->linux_meta_cache[i]);
    }
    sbi->linux_meta_cache_valid = false;
}

static void infilfs_linux_meta_cache_destroy(struct super_block *sb)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);

    if (!sbi)
        return;
    infilfs_linux_meta_cache_reset(sbi);
    kvfree(sbi->linux_meta_cache);
    sbi->linux_meta_cache = NULL;
    sbi->linux_meta_dir_checked = false;
    sbi->linux_meta_dir_cached = false;
}

static int infilfs_linux_meta_cache_insert(
    struct infilfs_sb_info *sbi, const u8 target_object_id[16],
    const u8 sidecar_object_id[16], u64 sidecar_object_block)
{
    struct infilfs_linux_meta_cache_entry *entry;
    u32 bucket;
    int ret;

    ret = infilfs_linux_meta_cache_prepare(sbi);
    if (ret)
        return ret;
    entry = infilfs_linux_meta_cache_lookup(sbi, target_object_id);
    if (entry) {
        memcpy(entry->sidecar_object_id, sidecar_object_id, 16);
        entry->sidecar_object_block = sidecar_object_block;
        return 0;
    }
    entry = kzalloc(sizeof(*entry), GFP_NOFS);
    if (!entry)
        return -ENOMEM;
    memcpy(entry->target_object_id, target_object_id, 16);
    memcpy(entry->sidecar_object_id, sidecar_object_id, 16);
    entry->sidecar_object_block = sidecar_object_block;
    bucket = infilfs_native_id_hash(target_object_id) &
        (INFILFS_LINUX_META_CACHE_BUCKETS - 1u);
    hlist_add_head(&entry->node, &sbi->linux_meta_cache[bucket]);
    return 0;
}

static void infilfs_linux_meta_cache_remove(
    struct infilfs_sb_info *sbi, const u8 target_object_id[16])
{
    struct infilfs_linux_meta_cache_entry *entry =
        infilfs_linux_meta_cache_lookup(sbi, target_object_id);

    if (!entry)
        return;
    hlist_del(&entry->node);
    kfree(entry);
}

static int infilfs_linux_meta_hex_nibble(u8 c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -EINVAL;
}

static bool infilfs_linux_meta_parse_uuid_name(
    const u8 *name, size_t length, u8 object_id[16])
{
    static const u8 hyphen_at[4] = {8, 13, 18, 23};
    size_t i;
    unsigned int nibble = 0;
    unsigned int h = 0;

    if (!name || length != 36)
        return false;
    memset(object_id, 0, 16);
    for (i = 0; i < length; ++i) {
        int value;

        if (h < ARRAY_SIZE(hyphen_at) && i == hyphen_at[h]) {
            if (name[i] != '-')
                return false;
            ++h;
            continue;
        }
        value = infilfs_linux_meta_hex_nibble(name[i]);
        if (value < 0 || nibble >= 32)
            return false;
        if ((nibble & 1u) == 0)
            object_id[nibble >> 1] = (u8)value << 4;
        else
            object_id[nibble >> 1] |= (u8)value;
        ++nibble;
    }
    return nibble == 32 && h == ARRAY_SIZE(hyphen_at);
}

struct infilfs_linux_meta_cache_build {
    struct super_block *sb;
    struct infilfs_sb_info *sbi;
};

static int infilfs_linux_meta_cache_build_visitor(
    const struct infilfs_dirent_disk *entry, const u8 *name, void *arg)
{
    struct infilfs_linux_meta_cache_build *build = arg;
    u8 target_object_id[16];
    u64 block;
    u16 type;
    int ret;

    if (le16_to_cpu(entry->object_type) != INFILFS_OBJECT_FILE ||
        !infilfs_linux_meta_parse_uuid_name(
            name, le16_to_cpu(entry->name_length), target_object_id))
        return 0;
    ret = infilfs_index_lookup(build->sb, entry->object_id, &block, &type);
    if (ret)
        return ret;
    if (type != INFILFS_OBJECT_FILE)
        return -EFSCORRUPTED;
    return infilfs_linux_meta_cache_insert(
        build->sbi, target_object_id, entry->object_id, block);
}

static int infilfs_linux_meta_cache_build(struct super_block *sb,
                                          struct inode *dir)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct infilfs_linux_meta_cache_build build = {.sb = sb, .sbi = sbi};
    int ret;

    if (sbi->linux_meta_cache_valid)
        return 0;
    ret = infilfs_linux_meta_cache_prepare(sbi);
    if (ret)
        return ret;
    infilfs_linux_meta_cache_reset(sbi);
    mutex_lock(&sbi->write_lock);
    ret = infilfs_for_each_dirent(
        dir, infilfs_linux_meta_cache_build_visitor, &build);
    mutex_unlock(&sbi->write_lock);
    if (ret < 0) {
        infilfs_linux_meta_cache_reset(sbi);
        return ret;
    }
    sbi->linux_meta_cache_valid = true;
    return 0;
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

