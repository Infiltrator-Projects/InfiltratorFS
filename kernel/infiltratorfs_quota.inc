// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Native Linux user/group/project quotas.
 *
 * Quota policy is Linux-adapter metadata, not portable object identity. The
 * policy database is stored as one trusted root sidecar xattr so Format 0.17
 * objects remain unchanged. Project quotas are directory-tree quotas: a
 * project root assigns its project ID to that directory and descendants until
 * a nested project root overrides it. Usage is rebuilt from authoritative
 * objects at mount and whenever project topology changes; counters never need
 * crash recovery of their own.
 */

#define INFILFS_QUOTA_DB_MAGIC "INFSQD01"
#define INFILFS_QUOTA_DB_VERSION 1u
#define INFILFS_QUOTA_DB_XATTR "infiltratorfs.quota-db"
#define INFILFS_QUOTA_MAX_RULES 8192u
#define INFILFS_QUOTA_MAX_ROOTS 8192u

struct infilfs_quota_db_header {
    u8 magic[8];
    __le32 version;
    __le32 rule_count;
    __le32 root_count;
    __le32 reserved;
} __packed;

struct infilfs_quota_rule_disk {
    __le32 type;
    __le32 id;
    __le64 hard_bytes;
    __le64 hard_objects;
} __packed;

struct infilfs_project_root_disk {
    u8 object_id[16];
    __le32 project_id;
    __le32 reserved;
} __packed;

struct infilfs_quota_rule {
    u32 type;
    u32 id;
    u64 hard_bytes;
    u64 hard_objects;
    u64 used_bytes;
    u64 used_objects;
};

struct infilfs_project_root {
    u8 object_id[16];
    u32 project_id;
};

static int infilfs_quota_reparent_member_locked(
    struct super_block *sb, const u8 object_id[16],
    const u8 source_id[16], bool *member_out);
static int infilfs_quota_inode_project_locked(
    struct inode *inode, u32 *project_out);

static_assert(sizeof(struct infilfs_quota_db_header) == 24);
static_assert(sizeof(struct infilfs_quota_rule_disk) == 24);
static_assert(sizeof(struct infilfs_project_root_disk) == 24);

static bool infilfs_quota_type_valid(u32 type)
{
    return type == INFILFS_QUOTA_USER ||
           type == INFILFS_QUOTA_GROUP ||
           type == INFILFS_QUOTA_PROJECT;
}

static int infilfs_quota_xattr_read(struct super_block *sb,
                                    u8 **value_out, size_t *size_out)
{
    struct inode *inode;
    ssize_t size;
    u8 *value;

    *value_out = NULL;
    *size_out = 0;
    if (!sb || !sb->s_root)
        return -EINVAL;
    inode = d_inode(sb->s_root);
    size = infilfs_linux_xattr_get(
        &infilfs_linux_trusted_xattr_handler, sb->s_root, inode,
        INFILFS_QUOTA_DB_XATTR, NULL, 0);
    if (size == -ENODATA)
        return 0;
    if (size < 0)
        return (int)size;
    if (!size)
        return 0;
    if ((size_t)size > INFILFS_LINUX_META_MAX)
        return -EFSCORRUPTED;
    value = kvmalloc((size_t)size, GFP_NOFS);
    if (!value)
        return -ENOMEM;
    if (infilfs_linux_xattr_get(
            &infilfs_linux_trusted_xattr_handler, sb->s_root, inode,
            INFILFS_QUOTA_DB_XATTR, value, (size_t)size) != size) {
        kvfree(value);
        return -EIO;
    }
    *value_out = value;
    *size_out = (size_t)size;
    return 0;
}

static int infilfs_quota_xattr_write(struct super_block *sb,
                                     const void *value, size_t size)
{
    struct inode *inode;
    int ret;

    if (!sb || !sb->s_root)
        return -EINVAL;
    inode = d_inode(sb->s_root);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    ret = infilfs_linux_xattr_set(
        &infilfs_linux_trusted_xattr_handler, &nop_mnt_idmap,
        sb->s_root, inode, INFILFS_QUOTA_DB_XATTR,
        size ? value : NULL, size, 0);
#else
    ret = infilfs_linux_xattr_set(
        &infilfs_linux_trusted_xattr_handler, &init_user_ns,
        sb->s_root, inode, INFILFS_QUOTA_DB_XATTR,
        size ? value : NULL, size, 0);
#endif
    if (!size && ret == -ENODATA)
        ret = 0;
    return ret;
}

static int infilfs_quota_encode_locked(struct super_block *sb)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct infilfs_quota_db_header *header;
    struct infilfs_quota_rule_disk *rules;
    struct infilfs_project_root_disk *roots;
    u8 *blob;
    size_t rules_bytes;
    size_t roots_bytes;
    size_t size;
    size_t i;
    int ret;

    if (!sbi->quota_rule_count && !sbi->project_root_count)
        return infilfs_quota_xattr_write(sb, NULL, 0);
    if (sbi->quota_rule_count > INFILFS_QUOTA_MAX_RULES ||
        sbi->project_root_count > INFILFS_QUOTA_MAX_ROOTS)
        return -E2BIG;
    if (check_mul_overflow(sbi->quota_rule_count, sizeof(*rules), &rules_bytes) ||
        check_mul_overflow(sbi->project_root_count, sizeof(*roots), &roots_bytes) ||
        check_add_overflow(sizeof(*header), rules_bytes, &size) ||
        check_add_overflow(size, roots_bytes, &size) ||
        size > INFILFS_LINUX_META_MAX)
        return -E2BIG;

    blob = kvzalloc(size, GFP_NOFS);
    if (!blob)
        return -ENOMEM;
    header = (struct infilfs_quota_db_header *)blob;
    memcpy(header->magic, INFILFS_QUOTA_DB_MAGIC, sizeof(header->magic));
    header->version = cpu_to_le32(INFILFS_QUOTA_DB_VERSION);
    header->rule_count = cpu_to_le32((u32)sbi->quota_rule_count);
    header->root_count = cpu_to_le32((u32)sbi->project_root_count);
    rules = (struct infilfs_quota_rule_disk *)(header + 1);
    roots = (struct infilfs_project_root_disk *)(rules + sbi->quota_rule_count);
    for (i = 0; i < sbi->quota_rule_count; ++i) {
        rules[i].type = cpu_to_le32(sbi->quota_rules[i].type);
        rules[i].id = cpu_to_le32(sbi->quota_rules[i].id);
        rules[i].hard_bytes = cpu_to_le64(sbi->quota_rules[i].hard_bytes);
        rules[i].hard_objects = cpu_to_le64(sbi->quota_rules[i].hard_objects);
    }
    for (i = 0; i < sbi->project_root_count; ++i) {
        memcpy(roots[i].object_id, sbi->project_roots[i].object_id, 16);
        roots[i].project_id = cpu_to_le32(sbi->project_roots[i].project_id);
    }
    ret = infilfs_quota_xattr_write(sb, blob, size);
    kvfree(blob);
    return ret;
}

static int infilfs_quota_decode_locked(struct super_block *sb)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    const struct infilfs_quota_db_header *header;
    const struct infilfs_quota_rule_disk *rules;
    const struct infilfs_project_root_disk *roots;
    struct infilfs_quota_rule *new_rules = NULL;
    struct infilfs_project_root *new_roots = NULL;
    u8 *blob = NULL;
    size_t size = 0;
    size_t need;
    u32 rule_count;
    u32 root_count;
    u32 i;
    int ret;

    ret = infilfs_quota_xattr_read(sb, &blob, &size);
    if (ret)
        return ret;
    if (!size) {
        kvfree(sbi->quota_rules);
        kvfree(sbi->project_roots);
        sbi->quota_rules = NULL;
        sbi->project_roots = NULL;
        sbi->quota_rule_count = 0;
        sbi->project_root_count = 0;
        return 0;
    }
    if (size < sizeof(*header)) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    header = (const struct infilfs_quota_db_header *)blob;
    if (memcmp(header->magic, INFILFS_QUOTA_DB_MAGIC, 8) ||
        le32_to_cpu(header->version) != INFILFS_QUOTA_DB_VERSION ||
        le32_to_cpu(header->reserved) != 0) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    rule_count = le32_to_cpu(header->rule_count);
    root_count = le32_to_cpu(header->root_count);
    if (rule_count > INFILFS_QUOTA_MAX_RULES || root_count > INFILFS_QUOTA_MAX_ROOTS) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    need = sizeof(*header) + (size_t)rule_count * sizeof(*rules) +
        (size_t)root_count * sizeof(*roots);
    if (need != size) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    if (rule_count) {
        new_rules = kvmalloc_array(rule_count, sizeof(*new_rules), GFP_NOFS);
        if (!new_rules) { ret = -ENOMEM; goto out; }
    }
    if (root_count) {
        new_roots = kvmalloc_array(root_count, sizeof(*new_roots), GFP_NOFS);
        if (!new_roots) { ret = -ENOMEM; goto out; }
    }
    rules = (const struct infilfs_quota_rule_disk *)(header + 1);
    roots = (const struct infilfs_project_root_disk *)(rules + rule_count);
    for (i = 0; i < rule_count; ++i) {
        u32 type = le32_to_cpu(rules[i].type);
        if (!infilfs_quota_type_valid(type) ||
            (!le64_to_cpu(rules[i].hard_bytes) &&
             !le64_to_cpu(rules[i].hard_objects))) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        memset(&new_rules[i], 0, sizeof(new_rules[i]));
        new_rules[i].type = type;
        new_rules[i].id = le32_to_cpu(rules[i].id);
        new_rules[i].hard_bytes = le64_to_cpu(rules[i].hard_bytes);
        new_rules[i].hard_objects = le64_to_cpu(rules[i].hard_objects);
    }
    for (i = 0; i < root_count; ++i) {
        if (!le32_to_cpu(roots[i].project_id) ||
            le32_to_cpu(roots[i].reserved) != 0 ||
            !memchr_inv(roots[i].object_id, 0, 16)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        memcpy(new_roots[i].object_id, roots[i].object_id, 16);
        new_roots[i].project_id = le32_to_cpu(roots[i].project_id);
    }
    kvfree(sbi->quota_rules);
    kvfree(sbi->project_roots);
    sbi->quota_rules = new_rules;
    sbi->quota_rule_count = rule_count;
    sbi->project_roots = new_roots;
    sbi->project_root_count = root_count;
    new_rules = NULL;
    new_roots = NULL;
    ret = 0;
out:
    kvfree(new_rules);
    kvfree(new_roots);
    kvfree(blob);
    return ret;
}

static int infilfs_quota_find_rule_locked(struct infilfs_sb_info *sbi,
                                          u32 type, u32 id)
{
    size_t i;
    for (i = 0; i < sbi->quota_rule_count; ++i)
        if (sbi->quota_rules[i].type == type && sbi->quota_rules[i].id == id)
            return (int)i;
    return -1;
}

static u32 infilfs_quota_root_match_locked(const struct infilfs_sb_info *sbi,
                                           const u8 object_id[16])
{
    size_t i;
    for (i = 0; i < sbi->project_root_count; ++i)
        if (!memcmp(sbi->project_roots[i].object_id, object_id, 16))
            return sbi->project_roots[i].project_id;
    return 0;
}

static int infilfs_quota_project_root_index_locked(
    const struct infilfs_sb_info *sbi, const u8 object_id[16])
{
    size_t i;

    for (i = 0; i < sbi->project_root_count; ++i)
        if (!memcmp(sbi->project_roots[i].object_id, object_id, 16))
            return (int)i;
    return -1;
}

static bool infilfs_quota_remove_project_root_locked(
    struct super_block *sb, const u8 object_id[16])
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    int idx;

    if (!sbi)
        return false;
    idx = infilfs_quota_project_root_index_locked(sbi, object_id);
    if (idx < 0)
        return false;
    memmove(&sbi->project_roots[idx], &sbi->project_roots[idx + 1],
            (sbi->project_root_count - (size_t)idx - 1u) *
                sizeof(*sbi->project_roots));
    sbi->project_root_count--;
    return true;
}

static int infilfs_quota_persist_locked(struct super_block *sb)
{
    return infilfs_quota_encode_locked(sb);
}

static int infilfs_quota_prune_project_roots_locked(
    struct super_block *sb, bool *changed_out)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    size_t i = 0;
    bool changed = false;

    while (i < sbi->project_root_count) {
        u64 block = 0;
        u16 type = 0;
        int ret = infilfs_index_lookup(
            sb, sbi->project_roots[i].object_id, &block, &type);

        if (!ret && type == INFILFS_OBJECT_DIRECTORY) {
            ++i;
            continue;
        }
        if (ret && ret != -ENOENT)
            return ret;

        memmove(&sbi->project_roots[i], &sbi->project_roots[i + 1],
                (sbi->project_root_count - i - 1u) *
                    sizeof(*sbi->project_roots));
        sbi->project_root_count--;
        changed = true;
    }

    if (changed_out)
        *changed_out = changed;
    return 0;
}

static int infilfs_quota_effective_project_locked(
    struct super_block *sb, const u8 object_id[16],
    const u8 parent_id[16], u32 *project_out)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    u8 current[16];
    unsigned int depth;

    *project_out = infilfs_quota_root_match_locked(sbi, object_id);
    if (*project_out)
        return 0;
    memcpy(current, parent_id, 16);
    for (depth = 0; memchr_inv(current, 0, 16) && depth < 1024u; ++depth) {
        struct infilfs_object_header_disk *header;
        u8 *object;
        u64 block;
        u16 type;
        int ret;

        *project_out = infilfs_quota_root_match_locked(sbi, current);
        if (*project_out)
            return 0;
        ret = infilfs_index_lookup(sb, current, &block, &type);
        if (ret)
            return ret;
        if (type != INFILFS_OBJECT_DIRECTORY)
            return -EFSCORRUPTED;
        object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!object)
            return -ENOMEM;
        ret = infilfs_read_object(sb, block, type, current, object);
        if (ret) { kfree(object); return ret; }
        header = (struct infilfs_object_header_disk *)object;
        memcpy(current, header->parent_id, 16);
        kfree(object);
    }
    if (depth >= 1024u)
        return -EFSCORRUPTED;
    *project_out = 0;
    return 0;
}

static int infilfs_quota_rebuild_usage_locked(struct super_block *sb)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct infilfs_index_entry_disk *entries = NULL;
    u32 count = 0;
    u32 i;
    int ret;

    for (i = 0; i < sbi->quota_rule_count; ++i) {
        sbi->quota_rules[i].used_bytes = 0;
        sbi->quota_rules[i].used_objects = 0;
    }
    if (!sbi->quota_rule_count)
        return 0;
    ret = infilfs_ns_index_snapshot(sb, &entries, &count);
    if (ret)
        return ret;
    for (i = 0; i < count; ++i) {
        struct infilfs_object_header_disk *header;
        struct infilfs_attributes_disk *attributes;
        struct infilfs_posix_compat_disk *posix;
        u8 *object;
        u16 type = le16_to_cpu(entries[i].object_type);
        u64 bytes;
        u32 uid, gid, project = 0;
        size_t r;

        if (type != INFILFS_OBJECT_FILE && type != INFILFS_OBJECT_DIRECTORY &&
            type != INFILFS_OBJECT_SYMLINK)
            continue;
        object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!object) { ret = -ENOMEM; break; }
        ret = infilfs_read_object(sb, le64_to_cpu(entries[i].object_block),
                                  type, entries[i].object_id, object);
        if (ret) { kfree(object); break; }
        header = (struct infilfs_object_header_disk *)object;
        ret = infilfs_posix_object_fields(object, &attributes, &posix);
        if (ret) { kfree(object); break; }
        if (le64_to_cpu(attributes->portable_flags) & INFILFS_ATTR_SYSTEM) {
            kfree(object);
            continue;
        }
        bytes = le64_to_cpu(attributes->logical_size);
        uid = le32_to_cpu(posix->uid);
        gid = le32_to_cpu(posix->gid);
        ret = infilfs_quota_effective_project_locked(
            sb, header->object_id, header->parent_id, &project);
        if (ret) { kfree(object); break; }
        for (r = 0; r < sbi->quota_rule_count; ++r) {
            bool match =
                (sbi->quota_rules[r].type == INFILFS_QUOTA_USER && sbi->quota_rules[r].id == uid) ||
                (sbi->quota_rules[r].type == INFILFS_QUOTA_GROUP && sbi->quota_rules[r].id == gid) ||
                (sbi->quota_rules[r].type == INFILFS_QUOTA_PROJECT && sbi->quota_rules[r].id == project);
            if (!match)
                continue;
            if (sbi->quota_rules[r].used_bytes > U64_MAX - bytes ||
                sbi->quota_rules[r].used_objects == U64_MAX) {
                ret = -EOVERFLOW;
                break;
            }
            sbi->quota_rules[r].used_bytes += bytes;
            sbi->quota_rules[r].used_objects++;
        }
        kfree(object);
        if (ret)
            break;
    }
    kvfree(entries);
    return ret;
}

static int infilfs_quota_mount_init(struct super_block *sb)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    bool pruned = false;
    int ret;

    mutex_lock(&sbi->quota_lock);
    ret = infilfs_quota_decode_locked(sb);
    if (!ret)
        ret = infilfs_quota_prune_project_roots_locked(sb, &pruned);
    if (!ret && pruned && !sb_rdonly(sb))
        ret = infilfs_quota_encode_locked(sb);
    if (!ret)
        ret = infilfs_quota_rebuild_usage_locked(sb);
    mutex_unlock(&sbi->quota_lock);
    return ret;
}

static void infilfs_quota_mount_destroy(struct super_block *sb)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    if (!sbi)
        return;
    kvfree(sbi->quota_rules);
    kvfree(sbi->project_roots);
    sbi->quota_rules = NULL;
    sbi->project_roots = NULL;
    sbi->quota_rule_count = 0;
    sbi->project_root_count = 0;
}


static int infilfs_quota_rule_usage_locked(
    struct super_block *sb, u32 wanted_type, u32 wanted_id,
    u64 *bytes_out, u64 *objects_out)
{
    struct infilfs_index_entry_disk *entries = NULL;
    u32 count = 0;
    u32 i;
    u64 bytes = 0;
    u64 objects = 0;
    int ret;

    *bytes_out = 0;
    *objects_out = 0;
    ret = infilfs_ns_index_snapshot(sb, &entries, &count);
    if (ret)
        return ret;

    for (i = 0; i < count; ++i) {
        struct infilfs_object_header_disk *header;
        struct infilfs_attributes_disk *attributes;
        struct infilfs_posix_compat_disk *posix;
        u8 *object;
        u16 type = le16_to_cpu(entries[i].object_type);
        u32 uid;
        u32 gid;
        u32 project = 0;
        u64 logical;
        bool match;

        if (type != INFILFS_OBJECT_FILE &&
            type != INFILFS_OBJECT_DIRECTORY &&
            type != INFILFS_OBJECT_SYMLINK)
            continue;

        object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!object) {
            ret = -ENOMEM;
            break;
        }
        ret = infilfs_read_object(
            sb, le64_to_cpu(entries[i].object_block),
            type, entries[i].object_id, object);
        if (ret) {
            kfree(object);
            break;
        }
        header = (struct infilfs_object_header_disk *)object;
        ret = infilfs_posix_object_fields(object, &attributes, &posix);
        if (ret) {
            kfree(object);
            break;
        }
        if (le64_to_cpu(attributes->portable_flags) & INFILFS_ATTR_SYSTEM) {
            kfree(object);
            continue;
        }
        uid = le32_to_cpu(posix->uid);
        gid = le32_to_cpu(posix->gid);
        if (wanted_type == INFILFS_QUOTA_PROJECT) {
            ret = infilfs_quota_effective_project_locked(
                sb, header->object_id, header->parent_id, &project);
            if (ret) {
                kfree(object);
                break;
            }
        }

        match =
            (wanted_type == INFILFS_QUOTA_USER && wanted_id == uid) ||
            (wanted_type == INFILFS_QUOTA_GROUP && wanted_id == gid) ||
            (wanted_type == INFILFS_QUOTA_PROJECT && wanted_id == project);
        if (!match) {
            kfree(object);
            continue;
        }

        logical = le64_to_cpu(attributes->logical_size);
        if (bytes > U64_MAX - logical || objects == U64_MAX) {
            kfree(object);
            ret = -EOVERFLOW;
            break;
        }
        bytes += logical;
        objects++;
        kfree(object);
    }

    kvfree(entries);
    if (!ret) {
        *bytes_out = bytes;
        *objects_out = objects;
    }
    return ret;
}

static int infilfs_quota_parent_project_locked(
    struct inode *inode, u32 *project_out)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_sb_info *sbi = INFILFS_SB(inode->i_sb);
    struct infilfs_object_header_disk *header;
    u8 current[16];
    u8 *object;
    unsigned int depth;
    int ret;

    *project_out = 0;
    if (!ii)
        return -EINVAL;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;
    ret = infilfs_read_object(
        inode->i_sb, ii->object_block, ii->object_type, ii->object_id, object);
    if (ret) {
        kfree(object);
        return ret;
    }
    header = (struct infilfs_object_header_disk *)object;
    memcpy(current, header->parent_id, 16);
    kfree(object);

    for (depth = 0; memchr_inv(current, 0, 16) && depth < 1024u; ++depth) {
        u64 block;
        u16 type;

        *project_out = infilfs_quota_root_match_locked(sbi, current);
        if (*project_out)
            return 0;
        ret = infilfs_index_lookup(inode->i_sb, current, &block, &type);
        if (ret)
            return ret;
        if (type != INFILFS_OBJECT_DIRECTORY)
            return -EFSCORRUPTED;

        object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!object)
            return -ENOMEM;
        ret = infilfs_read_object(
            inode->i_sb, block, type, current, object);
        if (ret) {
            kfree(object);
            return ret;
        }
        header = (struct infilfs_object_header_disk *)object;
        memcpy(current, header->parent_id, 16);
        kfree(object);
    }
    if (depth >= 1024u)
        return -EFSCORRUPTED;
    *project_out = 0;
    return 0;
}

static int infilfs_quota_domain_usage_locked(
    struct inode *root, u64 *bytes_out, u64 *objects_out)
{
    struct infilfs_inode_info *root_ii = INFILFS_I(root);
    struct infilfs_index_entry_disk *entries = NULL;
    u32 count = 0;
    u32 i;
    u64 bytes = 0;
    u64 objects = 0;
    int ret;

    *bytes_out = 0;
    *objects_out = 0;
    if (!root_ii)
        return -EINVAL;

    ret = infilfs_ns_index_snapshot(root->i_sb, &entries, &count);
    if (ret)
        return ret;

    for (i = 0; i < count; ++i) {
        struct infilfs_attributes_disk *attributes;
        struct infilfs_posix_compat_disk *posix;
        u8 *object;
        u16 type = le16_to_cpu(entries[i].object_type);
        bool member = false;
        u64 logical;

        if (type != INFILFS_OBJECT_FILE &&
            type != INFILFS_OBJECT_DIRECTORY &&
            type != INFILFS_OBJECT_SYMLINK)
            continue;

        ret = infilfs_quota_reparent_member_locked(
            root->i_sb, entries[i].object_id,
            root_ii->object_id, &member);
        if (ret || !member) {
            if (ret)
                break;
            continue;
        }

        object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!object) {
            ret = -ENOMEM;
            break;
        }
        ret = infilfs_read_object(
            root->i_sb, le64_to_cpu(entries[i].object_block),
            type, entries[i].object_id, object);
        if (ret) {
            kfree(object);
            break;
        }
        ret = infilfs_posix_object_fields(object, &attributes, &posix);
        if (ret) {
            kfree(object);
            break;
        }
        (void)posix;
        if (le64_to_cpu(attributes->portable_flags) & INFILFS_ATTR_SYSTEM) {
            kfree(object);
            continue;
        }
        if (type == INFILFS_OBJECT_FILE &&
            le64_to_cpu(attributes->link_count) > 1u) {
            /*
             * A pre-existing multiply-linked inode may have names outside this
             * subtree. Re-projecting it without proving every alias is inside
             * the same domain would make accounting path-dependent.
             */
            kfree(object);
            ret = -EXDEV;
            break;
        }

        logical = le64_to_cpu(attributes->logical_size);
        if (bytes > U64_MAX - logical || objects == U64_MAX) {
            kfree(object);
            ret = -EOVERFLOW;
            break;
        }
        bytes += logical;
        objects++;
        kfree(object);
    }

    kvfree(entries);
    if (!ret) {
        *bytes_out = bytes;
        *objects_out = objects;
    }
    return ret;
}

static int infilfs_quota_set_rule(struct super_block *sb,
                                  struct infilfs_quota_request *request)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct infilfs_quota_rule *grown;
    struct infilfs_quota_rule *old_rules = NULL;
    size_t old_count;
    u64 initial_bytes = 0;
    u64 initial_objects = 0;
    int idx;
    int ret = 0;

    if (!infilfs_quota_type_valid(request->type) ||
        (request->type == INFILFS_QUOTA_PROJECT && request->id == 0) ||
        request->flags || request->reserved0 ||
        memchr_inv(request->reserved, 0, sizeof(request->reserved)))
        return -EINVAL;
    if (sb_rdonly(sb))
        return -EROFS;

    ret = infilfs_native_pending_flush_sb(sb);
    if (ret)
        return ret;

    mutex_lock(&sbi->quota_lock);
    old_count = sbi->quota_rule_count;
    if (old_count) {
        old_rules = kvmalloc_array(old_count, sizeof(*old_rules), GFP_NOFS);
        if (!old_rules) {
            ret = -ENOMEM;
            goto out;
        }
        memcpy(old_rules, sbi->quota_rules, old_count * sizeof(*old_rules));
    }

    idx = infilfs_quota_find_rule_locked(sbi, request->type, request->id);

    /*
     * A newly introduced rule has no historical volatile counter.  Snapshot
     * the authoritative object graph while holding write_lock so the initial
     * usage cannot race a namespace or data mutation. Existing rules retain
     * their live reservation-aware counters and therefore must not be rebuilt.
     */
    if (idx < 0 && (request->hard_bytes || request->hard_objects)) {
        mutex_lock(&sbi->write_lock);
        ret = infilfs_quota_rule_usage_locked(
            sb, request->type, request->id,
            &initial_bytes, &initial_objects);
        mutex_unlock(&sbi->write_lock);
        if (ret)
            goto rollback;
    }

    if (!request->hard_bytes && !request->hard_objects) {
        if (idx >= 0) {
            memmove(&sbi->quota_rules[idx], &sbi->quota_rules[idx + 1],
                    (sbi->quota_rule_count - (size_t)idx - 1u) *
                        sizeof(*sbi->quota_rules));
            sbi->quota_rule_count--;
        }
    } else if (idx >= 0) {
        sbi->quota_rules[idx].hard_bytes = request->hard_bytes;
        sbi->quota_rules[idx].hard_objects = request->hard_objects;
    } else {
        if (sbi->quota_rule_count >= INFILFS_QUOTA_MAX_RULES) {
            ret = -E2BIG;
            goto rollback;
        }
        grown = kvmalloc_array(
            sbi->quota_rule_count + 1u, sizeof(*grown), GFP_NOFS);
        if (!grown) {
            ret = -ENOMEM;
            goto rollback;
        }
        if (sbi->quota_rule_count)
            memcpy(grown, sbi->quota_rules,
                   sbi->quota_rule_count * sizeof(*grown));
        memset(&grown[sbi->quota_rule_count], 0, sizeof(*grown));
        grown[sbi->quota_rule_count].type = request->type;
        grown[sbi->quota_rule_count].id = request->id;
        grown[sbi->quota_rule_count].hard_bytes = request->hard_bytes;
        grown[sbi->quota_rule_count].hard_objects = request->hard_objects;
        grown[sbi->quota_rule_count].used_bytes = initial_bytes;
        grown[sbi->quota_rule_count].used_objects = initial_objects;
        kvfree(sbi->quota_rules);
        sbi->quota_rules = grown;
        sbi->quota_rule_count++;
    }

    ret = infilfs_quota_encode_locked(sb);
    if (ret)
        goto rollback;

    idx = infilfs_quota_find_rule_locked(sbi, request->type, request->id);
    if (idx >= 0) {
        request->used_bytes = sbi->quota_rules[idx].used_bytes;
        request->used_objects = sbi->quota_rules[idx].used_objects;
    } else {
        request->used_bytes = 0;
        request->used_objects = 0;
    }
    kvfree(old_rules);
    old_rules = NULL;
    goto out;

rollback:
    kvfree(sbi->quota_rules);
    sbi->quota_rules = old_rules;
    sbi->quota_rule_count = old_count;
    old_rules = NULL;
out:
    kvfree(old_rules);
    mutex_unlock(&sbi->quota_lock);
    return ret;
}

static int infilfs_quota_get_rule(struct super_block *sb,
                                  struct infilfs_quota_request *request)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    int idx;

    if (!infilfs_quota_type_valid(request->type) ||
        (request->type == INFILFS_QUOTA_PROJECT && request->id == 0) ||
        request->flags || request->reserved0 ||
        memchr_inv(request->reserved, 0, sizeof(request->reserved)))
        return -EINVAL;
    mutex_lock(&sbi->quota_lock);
    idx = infilfs_quota_find_rule_locked(sbi, request->type, request->id);
    if (idx < 0) { mutex_unlock(&sbi->quota_lock); return -ENOENT; }
    request->hard_bytes = sbi->quota_rules[idx].hard_bytes;
    request->hard_objects = sbi->quota_rules[idx].hard_objects;
    request->used_bytes = sbi->quota_rules[idx].used_bytes;
    request->used_objects = sbi->quota_rules[idx].used_objects;
    mutex_unlock(&sbi->quota_lock);
    return 0;
}

static int infilfs_quota_set_project(struct inode *inode,
                                     struct infilfs_project_request *request)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(inode->i_sb);
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_project_root *grown;
    struct infilfs_project_root *old_roots = NULL;
    size_t old_root_count;
    u64 transfer_bytes = 0;
    u64 transfer_objects = 0;
    u32 old_effective = 0;
    u32 new_effective = 0;
    int idx = -1;
    int target_rule;
    int old_rule;
    size_t i;
    int ret = 0;

    if (!S_ISDIR(inode->i_mode) || !ii || request->flags ||
        request->reserved0 ||
        memchr_inv(request->reserved, 0, sizeof(request->reserved)))
        return -EINVAL;
    if (sb_rdonly(inode->i_sb))
        return -EROFS;

    ret = infilfs_native_pending_flush_sb(inode->i_sb);
    if (ret)
        return ret;

    mutex_lock(&sbi->quota_lock);

    old_root_count = sbi->project_root_count;
    if (old_root_count) {
        old_roots = kvmalloc_array(
            old_root_count, sizeof(*old_roots), GFP_NOFS);
        if (!old_roots) {
            ret = -ENOMEM;
            goto out;
        }
        memcpy(old_roots, sbi->project_roots,
               old_root_count * sizeof(*old_roots));
    }

    for (i = 0; i < sbi->project_root_count; ++i) {
        if (!memcmp(sbi->project_roots[i].object_id, ii->object_id, 16)) {
            idx = (int)i;
            break;
        }
    }

    ret = infilfs_quota_inode_project_locked(inode, &old_effective);
    if (ret)
        goto rollback;
    if (request->project_id) {
        new_effective = request->project_id;
    } else {
        ret = infilfs_quota_parent_project_locked(inode, &new_effective);
        if (ret)
            goto rollback;
    }

    if (old_effective != new_effective) {
        /*
         * quota_lock -> write_lock is the native quota ordering.  A writer
         * that reserved quota before us releases write_lock before finalising
         * its reservation, so this scan observes its committed bytes while
         * its conservative reservation remains visible in the old counter.
         */
        mutex_lock(&sbi->write_lock);
        ret = infilfs_quota_domain_usage_locked(
            inode, &transfer_bytes, &transfer_objects);
        mutex_unlock(&sbi->write_lock);
        if (ret)
            goto rollback;

        target_rule = new_effective ?
            infilfs_quota_find_rule_locked(
                sbi, INFILFS_QUOTA_PROJECT, new_effective) : -1;
        if (target_rule >= 0) {
            struct infilfs_quota_rule *rule = &sbi->quota_rules[target_rule];

            if (rule->hard_bytes &&
                (rule->used_bytes > rule->hard_bytes ||
                 transfer_bytes > rule->hard_bytes - rule->used_bytes)) {
                ret = -EDQUOT;
                goto rollback;
            }
            if (rule->hard_objects &&
                (rule->used_objects > rule->hard_objects ||
                 transfer_objects >
                    rule->hard_objects - rule->used_objects)) {
                ret = -EDQUOT;
                goto rollback;
            }
            if (rule->used_bytes > U64_MAX - transfer_bytes ||
                rule->used_objects > U64_MAX - transfer_objects) {
                ret = -EOVERFLOW;
                goto rollback;
            }
        }
    }

    if (!request->project_id) {
        if (idx >= 0) {
            memmove(&sbi->project_roots[idx],
                    &sbi->project_roots[idx + 1],
                    (sbi->project_root_count - (size_t)idx - 1u) *
                        sizeof(*sbi->project_roots));
            sbi->project_root_count--;
        }
    } else if (idx >= 0) {
        sbi->project_roots[idx].project_id = request->project_id;
    } else {
        if (sbi->project_root_count >= INFILFS_QUOTA_MAX_ROOTS) {
            ret = -E2BIG;
            goto rollback;
        }
        grown = kvmalloc_array(
            sbi->project_root_count + 1u, sizeof(*grown), GFP_NOFS);
        if (!grown) {
            ret = -ENOMEM;
            goto rollback;
        }
        if (sbi->project_root_count)
            memcpy(grown, sbi->project_roots,
                   sbi->project_root_count * sizeof(*grown));
        memcpy(grown[sbi->project_root_count].object_id, ii->object_id, 16);
        grown[sbi->project_root_count].project_id = request->project_id;
        kvfree(sbi->project_roots);
        sbi->project_roots = grown;
        sbi->project_root_count++;
    }

    ret = infilfs_quota_encode_locked(inode->i_sb);
    if (ret)
        goto rollback;

    if (old_effective != new_effective) {
        old_rule = old_effective ?
            infilfs_quota_find_rule_locked(
                sbi, INFILFS_QUOTA_PROJECT, old_effective) : -1;
        target_rule = new_effective ?
            infilfs_quota_find_rule_locked(
                sbi, INFILFS_QUOTA_PROJECT, new_effective) : -1;

        if (old_rule >= 0) {
            struct infilfs_quota_rule *rule = &sbi->quota_rules[old_rule];
            rule->used_bytes = rule->used_bytes > transfer_bytes ?
                rule->used_bytes - transfer_bytes : 0;
            rule->used_objects = rule->used_objects > transfer_objects ?
                rule->used_objects - transfer_objects : 0;
        }
        if (target_rule >= 0) {
            sbi->quota_rules[target_rule].used_bytes += transfer_bytes;
            sbi->quota_rules[target_rule].used_objects += transfer_objects;
        }
    }

    request->effective_project_id = new_effective;
    kvfree(old_roots);
    old_roots = NULL;
    goto out;

rollback:
    kvfree(sbi->project_roots);
    sbi->project_roots = old_roots;
    sbi->project_root_count = old_root_count;
    old_roots = NULL;
out:
    kvfree(old_roots);
    mutex_unlock(&sbi->quota_lock);
    return ret;
}

static int infilfs_quota_get_project(struct inode *inode,
                                     struct infilfs_project_request *request)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(inode->i_sb);
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    u8 *object;
    size_t i;
    int ret;

    if (!ii || request->flags || request->reserved0 ||
        memchr_inv(request->reserved, 0, sizeof(request->reserved)))
        return -EINVAL;
    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;
    mutex_lock(&sbi->quota_lock);
    request->project_id = 0;
    for (i = 0; i < sbi->project_root_count; ++i)
        if (!memcmp(sbi->project_roots[i].object_id, ii->object_id, 16)) {
            request->project_id = sbi->project_roots[i].project_id;
            break;
        }
    ret = infilfs_read_object(inode->i_sb, ii->object_block, ii->object_type, ii->object_id, object);
    if (!ret) {
        struct infilfs_object_header_disk *header = (struct infilfs_object_header_disk *)object;
        ret = infilfs_quota_effective_project_locked(inode->i_sb, ii->object_id, header->parent_id,
                                                       &request->effective_project_id);
    }
    mutex_unlock(&sbi->quota_lock);
    kfree(object);
    return ret;
}

static int infilfs_quota_inode_project_locked(struct inode *inode, u32 *project_out)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_object_header_disk *header;
    u8 *object;
    int ret;

    *project_out = 0;
    if (!ii)
        return -EINVAL;
    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;
    ret = infilfs_read_object(inode->i_sb, ii->object_block, ii->object_type,
                              ii->object_id, object);
    if (!ret) {
        header = (struct infilfs_object_header_disk *)object;
        ret = infilfs_quota_effective_project_locked(
            inode->i_sb, ii->object_id, header->parent_id, project_out);
    }
    kfree(object);
    return ret;
}

static int infilfs_quota_candidate_subjects_locked(
    struct inode *inode, struct infilfs_quota_subject candidate[3],
    u32 *candidate_count)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    u32 project = 0;
    uid_t uid;
    gid_t gid;
    int ret;

    *candidate_count = 0;
    if (!ii || (ii->portable_flags & INFILFS_ATTR_SYSTEM))
        return 0;
    uid = from_kuid(&init_user_ns, inode->i_uid);
    gid = from_kgid(&init_user_ns, inode->i_gid);
    candidate[(*candidate_count)++] = (struct infilfs_quota_subject){
        .type = INFILFS_QUOTA_USER, .id = (u32)uid };
    candidate[(*candidate_count)++] = (struct infilfs_quota_subject){
        .type = INFILFS_QUOTA_GROUP, .id = (u32)gid };
    ret = infilfs_quota_inode_project_locked(inode, &project);
    if (ret)
        return ret;
    if (project)
        candidate[(*candidate_count)++] = (struct infilfs_quota_subject){
            .type = INFILFS_QUOTA_PROJECT, .id = project };
    return 0;
}

static int infilfs_quota_reserve_candidates_locked(
    struct infilfs_sb_info *sbi, struct super_block *sb,
    const struct infilfs_quota_subject candidate[3], u32 candidate_count,
    u64 bytes, u64 objects, struct infilfs_quota_reservation *reservation)
{
    u32 i;

    memset(reservation, 0, sizeof(*reservation));
    reservation->sb = sb;
    reservation->bytes = bytes;
    reservation->objects = objects;

    for (i = 0; i < candidate_count; ++i) {
        int idx = infilfs_quota_find_rule_locked(
            sbi, candidate[i].type, candidate[i].id);
        struct infilfs_quota_rule *rule;

        if (idx < 0)
            continue;
        rule = &sbi->quota_rules[idx];
        if (rule->hard_bytes &&
            (rule->used_bytes > rule->hard_bytes ||
             bytes > rule->hard_bytes - rule->used_bytes))
            return -EDQUOT;
        if (rule->hard_objects &&
            (rule->used_objects > rule->hard_objects ||
             objects > rule->hard_objects - rule->used_objects))
            return -EDQUOT;
    }

    for (i = 0; i < candidate_count; ++i) {
        int idx = infilfs_quota_find_rule_locked(
            sbi, candidate[i].type, candidate[i].id);
        struct infilfs_quota_rule *rule;

        if (idx < 0)
            continue;
        rule = &sbi->quota_rules[idx];
        if (rule->used_bytes > U64_MAX - bytes ||
            rule->used_objects > U64_MAX - objects)
            return -EOVERFLOW;
        rule->used_bytes += bytes;
        rule->used_objects += objects;
        reservation->subject[reservation->subject_count++] = candidate[i];
    }
    reservation->active = reservation->subject_count != 0;
    return 0;
}

static int infilfs_quota_reserve_inode(
    struct inode *inode, u64 bytes, u64 objects,
    struct infilfs_quota_reservation *reservation)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(inode->i_sb);
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_quota_subject candidate[3];
    u32 candidate_count = 0;
    int ret;

    memset(reservation, 0, sizeof(*reservation));
    if (!sbi || !sbi->quota_rule_count || (!bytes && !objects) ||
        !ii || (ii->portable_flags & INFILFS_ATTR_SYSTEM))
        return 0;
    mutex_lock(&sbi->quota_lock);
    ret = infilfs_quota_candidate_subjects_locked(
        inode, candidate, &candidate_count);
    if (!ret)
        ret = infilfs_quota_reserve_candidates_locked(
            sbi, inode->i_sb, candidate, candidate_count,
            bytes, objects, reservation);
    mutex_unlock(&sbi->quota_lock);
    return ret;
}

static int infilfs_quota_reserve_create(
    struct inode *parent, u64 bytes, u64 objects,
    struct infilfs_quota_reservation *reservation)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(parent->i_sb);
    struct infilfs_quota_subject candidate[3];
    kuid_t kuid;
    kgid_t kgid;
    u32 uid = 0, gid = 0, project = 0;
    u32 count = 0;
    int ret;

    memset(reservation, 0, sizeof(*reservation));
    if (!sbi || !sbi->quota_rule_count || (!bytes && !objects))
        return 0;
    infilfs_posix_current_ids(&kuid, &kgid);
    ret = infilfs_posix_uid_to_disk(kuid, &uid);
    if (ret)
        return ret;
    if (parent->i_mode & S_ISGID)
        kgid = parent->i_gid;
    ret = infilfs_posix_gid_to_disk(kgid, &gid);
    if (ret)
        return ret;

    mutex_lock(&sbi->quota_lock);
    candidate[count++] = (struct infilfs_quota_subject){
        .type = INFILFS_QUOTA_USER, .id = uid };
    candidate[count++] = (struct infilfs_quota_subject){
        .type = INFILFS_QUOTA_GROUP, .id = gid };
    ret = infilfs_quota_inode_project_locked(parent, &project);
    if (!ret && project)
        candidate[count++] = (struct infilfs_quota_subject){
            .type = INFILFS_QUOTA_PROJECT, .id = project };
    if (!ret)
        ret = infilfs_quota_reserve_candidates_locked(
            sbi, parent->i_sb, candidate, count, bytes, objects, reservation);
    mutex_unlock(&sbi->quota_lock);
    return ret;
}

static void infilfs_quota_adjust_locked(
    struct infilfs_quota_reservation *reservation, u64 bytes, u64 objects)
{
    struct infilfs_sb_info *sbi;
    u32 i;

    if (!reservation || !reservation->active || !reservation->sb)
        return;
    sbi = INFILFS_SB(reservation->sb);
    for (i = 0; i < reservation->subject_count; ++i) {
        int idx = infilfs_quota_find_rule_locked(
            sbi, reservation->subject[i].type, reservation->subject[i].id);
        if (idx < 0)
            continue;
        sbi->quota_rules[idx].used_bytes =
            sbi->quota_rules[idx].used_bytes > bytes ?
            sbi->quota_rules[idx].used_bytes - bytes : 0;
        sbi->quota_rules[idx].used_objects =
            sbi->quota_rules[idx].used_objects > objects ?
            sbi->quota_rules[idx].used_objects - objects : 0;
    }
}

static void infilfs_quota_reservation_finish(
    struct infilfs_quota_reservation *reservation,
    u64 actual_bytes, u64 actual_objects)
{
    struct infilfs_sb_info *sbi;
    u64 release_bytes;
    u64 release_objects;

    if (!reservation || !reservation->active || !reservation->sb)
        return;
    actual_bytes = min_t(u64, actual_bytes, reservation->bytes);
    actual_objects = min_t(u64, actual_objects, reservation->objects);
    release_bytes = reservation->bytes - actual_bytes;
    release_objects = reservation->objects - actual_objects;
    sbi = INFILFS_SB(reservation->sb);
    mutex_lock(&sbi->quota_lock);
    infilfs_quota_adjust_locked(reservation, release_bytes, release_objects);
    mutex_unlock(&sbi->quota_lock);
    reservation->active = false;
}

static void infilfs_quota_reservation_abort(
    struct infilfs_quota_reservation *reservation)
{
    struct infilfs_sb_info *sbi;

    if (!reservation || !reservation->active || !reservation->sb)
        return;
    sbi = INFILFS_SB(reservation->sb);
    mutex_lock(&sbi->quota_lock);
    infilfs_quota_adjust_locked(
        reservation, reservation->bytes, reservation->objects);
    mutex_unlock(&sbi->quota_lock);
    reservation->active = false;
}

static int infilfs_quota_capture_inode_locked(
    struct inode *inode, struct infilfs_quota_reservation *reservation)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(inode->i_sb);
    struct infilfs_quota_subject candidate[3];
    u32 count = 0;
    u32 i;
    int ret;

    memset(reservation, 0, sizeof(*reservation));
    reservation->sb = inode->i_sb;
    if (!sbi || !sbi->quota_rule_count)
        return 0;
    ret = infilfs_quota_candidate_subjects_locked(inode, candidate, &count);
    if (!ret) {
        for (i = 0; i < count; ++i) {
            if (infilfs_quota_find_rule_locked(
                    sbi, candidate[i].type, candidate[i].id) < 0)
                continue;
            reservation->subject[reservation->subject_count++] = candidate[i];
        }
        reservation->active = reservation->subject_count != 0;
    }
    return ret;
}

static int infilfs_quota_capture_inode(
    struct inode *inode, struct infilfs_quota_reservation *reservation)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(inode->i_sb);
    int ret;

    if (!sbi) {
        memset(reservation, 0, sizeof(*reservation));
        return 0;
    }
    mutex_lock(&sbi->quota_lock);
    ret = infilfs_quota_capture_inode_locked(inode, reservation);
    mutex_unlock(&sbi->quota_lock);
    return ret;
}

static void infilfs_quota_apply_release(
    struct infilfs_quota_reservation *reservation, u64 bytes, u64 objects)
{
    struct infilfs_sb_info *sbi;

    if (!reservation || !reservation->active || !reservation->sb)
        return;
    sbi = INFILFS_SB(reservation->sb);
    mutex_lock(&sbi->quota_lock);
    infilfs_quota_adjust_locked(reservation, bytes, objects);
    mutex_unlock(&sbi->quota_lock);
    reservation->active = false;
}

static int infilfs_quota_prepare_identity_change(
    struct inode *inode, const struct iattr *attr,
    struct infilfs_quota_reservation *new_quota,
    struct infilfs_quota_reservation *old_quota)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(inode->i_sb);
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_quota_subject targets[3];
    u32 target_count = 0;
    u64 bytes = (u64)i_size_read(inode);
    u32 old_uid;
    u32 old_gid;
    u32 new_uid;
    u32 new_gid;
    int ret = 0;

    memset(new_quota, 0, sizeof(*new_quota));
    memset(old_quota, 0, sizeof(*old_quota));
    old_quota->sb = inode->i_sb;

    if (!sbi || !sbi->quota_rule_count || !ii ||
        (ii->portable_flags & INFILFS_ATTR_SYSTEM) ||
        !(attr->ia_valid & (ATTR_UID | ATTR_GID)))
        return 0;

    old_uid = (u32)from_kuid(&init_user_ns, inode->i_uid);
    old_gid = (u32)from_kgid(&init_user_ns, inode->i_gid);
    new_uid = old_uid;
    new_gid = old_gid;
    if (attr->ia_valid & ATTR_UID) {
        ret = infilfs_posix_uid_to_disk(attr->ia_uid, &new_uid);
        if (ret)
            return ret;
    }
    if (attr->ia_valid & ATTR_GID) {
        ret = infilfs_posix_gid_to_disk(attr->ia_gid, &new_gid);
        if (ret)
            return ret;
    }

    mutex_lock(&sbi->quota_lock);
    if (new_uid != old_uid) {
        int old_idx = infilfs_quota_find_rule_locked(
            sbi, INFILFS_QUOTA_USER, old_uid);
        int new_idx = infilfs_quota_find_rule_locked(
            sbi, INFILFS_QUOTA_USER, new_uid);

        if (old_idx >= 0)
            old_quota->subject[old_quota->subject_count++] =
                (struct infilfs_quota_subject){
                    .type = INFILFS_QUOTA_USER, .id = old_uid };
        if (new_idx >= 0)
            targets[target_count++] = (struct infilfs_quota_subject){
                .type = INFILFS_QUOTA_USER, .id = new_uid };
    }
    if (new_gid != old_gid) {
        int old_idx = infilfs_quota_find_rule_locked(
            sbi, INFILFS_QUOTA_GROUP, old_gid);
        int new_idx = infilfs_quota_find_rule_locked(
            sbi, INFILFS_QUOTA_GROUP, new_gid);

        if (old_idx >= 0)
            old_quota->subject[old_quota->subject_count++] =
                (struct infilfs_quota_subject){
                    .type = INFILFS_QUOTA_GROUP, .id = old_gid };
        if (new_idx >= 0)
            targets[target_count++] = (struct infilfs_quota_subject){
                .type = INFILFS_QUOTA_GROUP, .id = new_gid };
    }
    old_quota->active = old_quota->subject_count != 0;
    old_quota->bytes = bytes;
    old_quota->objects = 1;

    if (target_count)
        ret = infilfs_quota_reserve_candidates_locked(
            sbi, inode->i_sb, targets, target_count, bytes, 1, new_quota);
    mutex_unlock(&sbi->quota_lock);
    return ret;
}


static int infilfs_quota_reparent_member_locked(
    struct super_block *sb, const u8 object_id[16],
    const u8 source_id[16], bool *member_out)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    u8 current[16];
    unsigned int depth;

    *member_out = false;
    memcpy(current, object_id, 16);
    for (depth = 0; memchr_inv(current, 0, 16) && depth < 1024u; ++depth) {
        struct infilfs_object_header_disk *header;
        u8 *object;
        u64 block;
        u16 type;
        int ret;

        if (!memcmp(current, source_id, 16)) {
            *member_out = true;
            return 0;
        }

        /*
         * A nested project root owns itself and everything below it, so that
         * branch does not move between project-accounting domains when an
         * ancestor directory is reparented.
         */
        if (infilfs_quota_root_match_locked(sbi, current))
            return 0;

        ret = infilfs_index_lookup(sb, current, &block, &type);
        if (ret)
            return ret;
        if (type != INFILFS_OBJECT_FILE &&
            type != INFILFS_OBJECT_DIRECTORY &&
            type != INFILFS_OBJECT_SYMLINK)
            return -EFSCORRUPTED;

        object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!object)
            return -ENOMEM;
        ret = infilfs_read_object(sb, block, type, current, object);
        if (ret) {
            kfree(object);
            return ret;
        }
        header = (struct infilfs_object_header_disk *)object;
        memcpy(current, header->parent_id, 16);
        kfree(object);
    }
    if (depth >= 1024u)
        return -EFSCORRUPTED;
    return 0;
}

static int infilfs_quota_prepare_reparent_locked(
    struct inode *source, struct inode *new_dir,
    struct infilfs_quota_reservation *new_project,
    struct infilfs_quota_reservation *old_project)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(source->i_sb);
    struct infilfs_inode_info *source_ii = INFILFS_I(source);
    struct infilfs_index_entry_disk *entries = NULL;
    struct infilfs_quota_subject candidate;
    u64 bytes = 0;
    u64 objects = 0;
    u32 entry_count = 0;
    u32 old_project_id = 0;
    u32 new_project_id = 0;
    u32 i;
    int ret = 0;

    memset(new_project, 0, sizeof(*new_project));
    memset(old_project, 0, sizeof(*old_project));
    old_project->sb = source->i_sb;

    if (!sbi || !source_ii || source->i_sb != new_dir->i_sb ||
        !sbi->quota_rule_count)
        return 0;

    /*
     * Moving a project root does not transfer its accounting domain: the
     * root's project identity travels with the subtree.
     */
    if (infilfs_quota_root_match_locked(sbi, source_ii->object_id))
        goto out;

    ret = infilfs_quota_inode_project_locked(source, &old_project_id);
    if (ret)
        goto out;
    ret = infilfs_quota_inode_project_locked(new_dir, &new_project_id);
    if (ret)
        goto out;
    if (old_project_id == new_project_id)
        goto out;

    ret = infilfs_ns_index_snapshot(source->i_sb, &entries, &entry_count);
    if (ret)
        goto out;

    for (i = 0; i < entry_count; ++i) {
        struct infilfs_object_header_disk *header;
        struct infilfs_attributes_disk *attributes;
        struct infilfs_posix_compat_disk *posix;
        u8 *object;
        u16 type = le16_to_cpu(entries[i].object_type);
        bool member = false;
        u64 logical;

        if (type != INFILFS_OBJECT_FILE &&
            type != INFILFS_OBJECT_DIRECTORY &&
            type != INFILFS_OBJECT_SYMLINK)
            continue;

        ret = infilfs_quota_reparent_member_locked(
            source->i_sb, entries[i].object_id,
            source_ii->object_id, &member);
        if (ret || !member) {
            if (ret)
                break;
            continue;
        }

        object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!object) {
            ret = -ENOMEM;
            break;
        }
        ret = infilfs_read_object(
            source->i_sb, le64_to_cpu(entries[i].object_block),
            type, entries[i].object_id, object);
        if (ret) {
            kfree(object);
            break;
        }
        header = (struct infilfs_object_header_disk *)object;
        ret = infilfs_posix_object_fields(object, &attributes, &posix);
        if (ret) {
            kfree(object);
            break;
        }
        (void)header;
        (void)posix;
        if (le64_to_cpu(attributes->portable_flags) & INFILFS_ATTR_SYSTEM) {
            kfree(object);
            continue;
        }
        if (type == INFILFS_OBJECT_FILE &&
            le64_to_cpu(attributes->link_count) > 1u) {
            kfree(object);
            ret = -EXDEV;
            break;
        }

        logical = le64_to_cpu(attributes->logical_size);
        if (bytes > U64_MAX - logical || objects == U64_MAX) {
            kfree(object);
            ret = -EOVERFLOW;
            break;
        }
        bytes += logical;
        objects++;
        kfree(object);
    }
    if (ret)
        goto out;

    if (new_project_id) {
        candidate.type = INFILFS_QUOTA_PROJECT;
        candidate.id = new_project_id;
        ret = infilfs_quota_reserve_candidates_locked(
            sbi, source->i_sb, &candidate, 1u,
            bytes, objects, new_project);
        if (ret)
            goto out;
    }

    if (old_project_id &&
        infilfs_quota_find_rule_locked(
            sbi, INFILFS_QUOTA_PROJECT, old_project_id) >= 0) {
        old_project->subject[0].type = INFILFS_QUOTA_PROJECT;
        old_project->subject[0].id = old_project_id;
        old_project->subject_count = 1;
        old_project->bytes = bytes;
        old_project->objects = objects;
        old_project->active = true;
    }

out:
    kvfree(entries);
    return ret;
}

static int infilfs_quota_hardlink_check(
    struct inode *source, struct inode *destination_dir)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(source->i_sb);
    u32 source_project = 0;
    u32 destination_project = 0;
    int ret;

    if (!sbi || source->i_sb != destination_dir->i_sb ||
        !sbi->project_root_count)
        return 0;

    mutex_lock(&sbi->quota_lock);
    ret = infilfs_quota_inode_project_locked(source, &source_project);
    if (!ret)
        ret = infilfs_quota_inode_project_locked(
            destination_dir, &destination_project);
    mutex_unlock(&sbi->quota_lock);
    if (ret)
        return ret;

    /*
     * Project quotas are inode/object domains. A multiply-linked inode cannot
     * simultaneously inherit two different directory-tree projects without
     * ambiguous accounting, so match native project-quota semantics and reject
     * cross-project links/moves.
     */
    return source_project == destination_project ? 0 : -EXDEV;
}
