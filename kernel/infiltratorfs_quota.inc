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
    int ret;

    mutex_lock(&sbi->quota_lock);
    ret = infilfs_quota_decode_locked(sb);
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

static int infilfs_quota_set_rule(struct super_block *sb,
                                  struct infilfs_quota_request *request)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct infilfs_quota_rule *grown;
    int idx;
    int ret;

    if (!infilfs_quota_type_valid(request->type) || request->flags ||
        request->reserved0 || memchr_inv(request->reserved, 0, sizeof(request->reserved)))
        return -EINVAL;
    if (sb_rdonly(sb))
        return -EROFS;
    ret = infilfs_native_pending_flush_sb(sb);
    if (ret)
        return ret;
    mutex_lock(&sbi->quota_lock);
    idx = infilfs_quota_find_rule_locked(sbi, request->type, request->id);
    if (!request->hard_bytes && !request->hard_objects) {
        if (idx >= 0) {
            memmove(&sbi->quota_rules[idx], &sbi->quota_rules[idx + 1],
                    (sbi->quota_rule_count - (size_t)idx - 1u) * sizeof(*sbi->quota_rules));
            sbi->quota_rule_count--;
        }
    } else if (idx >= 0) {
        sbi->quota_rules[idx].hard_bytes = request->hard_bytes;
        sbi->quota_rules[idx].hard_objects = request->hard_objects;
    } else {
        if (sbi->quota_rule_count >= INFILFS_QUOTA_MAX_RULES) { ret = -E2BIG; goto out; }
        grown = kvmalloc_array(sbi->quota_rule_count + 1u, sizeof(*grown), GFP_NOFS);
        if (!grown) { ret = -ENOMEM; goto out; }
        if (sbi->quota_rule_count)
            memcpy(grown, sbi->quota_rules, sbi->quota_rule_count * sizeof(*grown));
        memset(&grown[sbi->quota_rule_count], 0, sizeof(*grown));
        grown[sbi->quota_rule_count].type = request->type;
        grown[sbi->quota_rule_count].id = request->id;
        grown[sbi->quota_rule_count].hard_bytes = request->hard_bytes;
        grown[sbi->quota_rule_count].hard_objects = request->hard_objects;
        kvfree(sbi->quota_rules);
        sbi->quota_rules = grown;
        sbi->quota_rule_count++;
    }
    ret = infilfs_quota_encode_locked(sb);
    if (!ret)
        ret = infilfs_quota_rebuild_usage_locked(sb);
    if (!ret) {
        idx = infilfs_quota_find_rule_locked(sbi, request->type, request->id);
        if (idx >= 0) {
            request->used_bytes = sbi->quota_rules[idx].used_bytes;
            request->used_objects = sbi->quota_rules[idx].used_objects;
        } else {
            request->used_bytes = 0;
            request->used_objects = 0;
        }
    }
out:
    mutex_unlock(&sbi->quota_lock);
    return ret;
}

static int infilfs_quota_get_rule(struct super_block *sb,
                                  struct infilfs_quota_request *request)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    int idx;

    if (!infilfs_quota_type_valid(request->type) || request->flags ||
        request->reserved0 || memchr_inv(request->reserved, 0, sizeof(request->reserved)))
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
    int idx = -1;
    size_t i;
    int ret;

    if (!S_ISDIR(inode->i_mode) || !ii || request->flags || request->reserved0 ||
        memchr_inv(request->reserved, 0, sizeof(request->reserved)))
        return -EINVAL;
    if (sb_rdonly(inode->i_sb))
        return -EROFS;
    ret = infilfs_native_pending_flush_sb(inode->i_sb);
    if (ret)
        return ret;
    mutex_lock(&sbi->quota_lock);
    for (i = 0; i < sbi->project_root_count; ++i)
        if (!memcmp(sbi->project_roots[i].object_id, ii->object_id, 16)) { idx = (int)i; break; }
    if (!request->project_id) {
        if (idx >= 0) {
            memmove(&sbi->project_roots[idx], &sbi->project_roots[idx + 1],
                    (sbi->project_root_count - (size_t)idx - 1u) * sizeof(*sbi->project_roots));
            sbi->project_root_count--;
        }
    } else if (idx >= 0) {
        sbi->project_roots[idx].project_id = request->project_id;
    } else {
        if (sbi->project_root_count >= INFILFS_QUOTA_MAX_ROOTS) { ret = -E2BIG; goto out; }
        grown = kvmalloc_array(sbi->project_root_count + 1u, sizeof(*grown), GFP_NOFS);
        if (!grown) { ret = -ENOMEM; goto out; }
        if (sbi->project_root_count)
            memcpy(grown, sbi->project_roots, sbi->project_root_count * sizeof(*grown));
        memcpy(grown[sbi->project_root_count].object_id, ii->object_id, 16);
        grown[sbi->project_root_count].project_id = request->project_id;
        kvfree(sbi->project_roots);
        sbi->project_roots = grown;
        sbi->project_root_count++;
    }
    ret = infilfs_quota_encode_locked(inode->i_sb);
    if (!ret)
        ret = infilfs_quota_rebuild_usage_locked(inode->i_sb);
    if (!ret) {
        u8 *object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!object) ret = -ENOMEM;
        else {
            struct infilfs_object_header_disk *header;
            ret = infilfs_read_object(inode->i_sb, ii->object_block, ii->object_type, ii->object_id, object);
            if (!ret) {
                header = (struct infilfs_object_header_disk *)object;
                ret = infilfs_quota_effective_project_locked(inode->i_sb, ii->object_id, header->parent_id,
                                                               &request->effective_project_id);
            }
            kfree(object);
        }
    }
out:
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
