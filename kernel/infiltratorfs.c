// SPDX-License-Identifier: GPL-3.0-or-later
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/dirent.h>
#include <linux/falloc.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/cred.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/timekeeping.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>
#include <linux/vmalloc.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uio.h>
#include <linux/version.h>

#include "infiltratorfs_format.h"

#define INFILTRATORFS_NAME "infiltratorfs"
#define INFILTRATORFS_MAGIC 0x494e4653u

static const u8 infilfs_disk_magic[8] = {
    'I', 'N', 'F', 'S', '2', '0', '2', '6'
};
static const u8 infilfs_object_magic[8] = {
    'I', 'N', 'F', 'O', 'B', 'J', '0', '1'
};
static const u8 infilfs_directory_page_magic[8] = {
    'I', 'N', 'F', 'S', 'D', 'P', '0', '1'
};
static const u8 infilfs_index_page_magic[8] = {
    'I', 'N', 'F', 'S', 'I', 'P', '0', '1'
};
static const u8 infilfs_extent_page_magic[8] = {
    'I', 'N', 'F', 'S', 'E', 'P', '0', '1'
};

struct infilfs_sb_info {
    struct infilfs_superblock_disk disk;
    u64 device_blocks;
    struct mutex write_lock;
    u8 *bitmap;
    size_t bitmap_bytes;
    u8 *snapshot_bitmap;
    bool rw_enabled;
    bool write_poisoned;
};

struct infilfs_inode_info {
    u64 object_block;
    u16 object_type;
    u8 object_id[16];
    char *symlink_target;
};

struct infilfs_dir_lookup {
    const char *name;
    size_t name_len;
    u8 object_id[16];
    u16 object_type;
    bool found;
};

struct infilfs_dir_emit_state {
    struct dir_context *ctx;
    u64 index;
};

static const struct inode_operations infilfs_dir_inode_operations;
static const struct inode_operations infilfs_symlink_inode_operations;
static const struct inode_operations infilfs_file_inode_operations;
static const struct file_operations infilfs_dir_operations;
static const struct file_operations infilfs_file_operations;


static u64 infilfs_rw_crc64_zeroed(const u8 *data, size_t length,
                                    size_t zero_offset, size_t zero_length);

static bool infilfs_crc64_block_valid(const u8 block[INFILFS_DISK_BLOCK_SIZE],
                                      size_t checksum_offset,
                                      size_t checksum_size)
{
    __le64 stored;
    size_t i;
    if (checksum_size < sizeof(stored) || checksum_offset > INFILFS_DISK_BLOCK_SIZE - checksum_size)
        return false;
    memcpy(&stored, block + checksum_offset, sizeof(stored));
    for (i = sizeof(stored); i < checksum_size; ++i)
        if (block[checksum_offset + i] != 0)
            return false;
    return le64_to_cpu(stored) == infilfs_rw_crc64_zeroed(
        block, INFILFS_DISK_BLOCK_SIZE, checksum_offset, checksum_size);
}

static struct infilfs_sb_info *INFILFS_SB(struct super_block *sb)
{
    return sb->s_fs_info;
}

static struct infilfs_inode_info *INFILFS_I(struct inode *inode)
{
    return inode->i_private;
}

static bool infilfs_inode_is_new(struct inode *inode)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
    return (inode_state_read_once(inode) & I_NEW) != 0;
#else
    return (READ_ONCE(inode->i_state) & I_NEW) != 0;
#endif
}

static int infilfs_read_block(struct super_block *sb, u64 block, void *out)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct buffer_head *bh;

    if (!sbi || block >= sbi->device_blocks)
        return -EIO;

    bh = sb_bread(sb, (sector_t)block);
    if (!bh)
        return -EIO;
    memcpy(out, bh->b_data, INFILFS_DISK_BLOCK_SIZE);
    brelse(bh);
    return 0;
}

static bool infilfs_checkpoint_basic_valid(
    const struct infilfs_superblock_disk *disk, u64 device_blocks)
{
    u64 total;
    u64 incompat;
    u64 expected[INFILFS_CHECKPOINT_COUNT];
    unsigned int i;

    if (memcmp(disk->magic, infilfs_disk_magic, sizeof(infilfs_disk_magic)) != 0)
        return false;
    if (le16_to_cpu(disk->format_major) != INFILFS_FORMAT_MAJOR ||
        le16_to_cpu(disk->format_minor) != INFILFS_FORMAT_MINOR ||
        le16_to_cpu(disk->header_size) != sizeof(*disk) ||
        le16_to_cpu(disk->block_shift) != INFILFS_DISK_BLOCK_SHIFT ||
        le32_to_cpu(disk->checksum_type) != INFILFS_CHECKSUM_CRC64_ECMA ||
        le64_to_cpu(disk->generation) == 0)
        return false;

    total = le64_to_cpu(disk->total_blocks);
    if (total != device_blocks || total < 3 ||
        le64_to_cpu(disk->free_blocks) > total)
        return false;

    expected[0] = 0;
    expected[1] = total / 2;
    expected[2] = total - 1;
    for (i = 0; i < INFILFS_CHECKPOINT_COUNT; ++i)
        if (le64_to_cpu(disk->checkpoint_block[i]) != expected[i])
            return false;

    if (le64_to_cpu(disk->bitmap_block_count) == 0 ||
        le64_to_cpu(disk->bitmap_start_block) >= total ||
        le64_to_cpu(disk->object_index_block) >= total ||
        le64_to_cpu(disk->root_object_block) >= total)
        return false;

    incompat = le64_to_cpu(disk->incompat_flags);
    if (incompat & ~INFILFS_KNOWN_INCOMPAT_FLAGS)
        return false;
    if ((incompat & (INFILFS_INCOMPAT_UTF8_NAMES |
                     INFILFS_INCOMPAT_SPARSE_EXTENTS)) !=
        (INFILFS_INCOMPAT_UTF8_NAMES | INFILFS_INCOMPAT_SPARSE_EXTENTS))
        return false;
    if (le64_to_cpu(disk->compat_flags) != 0 ||
        le64_to_cpu(disk->ro_compat_flags) != 0)
        return false;

    return true;
}

static int infilfs_select_checkpoint(struct super_block *sb,
                                     struct infilfs_superblock_disk *selected)
{
    u64 blocks = INFILFS_SB(sb)->device_blocks;
    u64 candidates[INFILFS_CHECKPOINT_COUNT];
    struct infilfs_superblock_disk current;
    u64 best_generation = 0;
    bool found = false;
    unsigned int i;
    u8 *raw;

    candidates[0] = 0;
    candidates[1] = blocks / 2;
    candidates[2] = blocks - 1;

    raw = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!raw)
        return -ENOMEM;

    for (i = 0; i < INFILFS_CHECKPOINT_COUNT; ++i) {
        if (infilfs_read_block(sb, candidates[i], raw) != 0)
            continue;
        if (!infilfs_crc64_block_valid(raw,
                offsetof(struct infilfs_superblock_disk, checksum),
                sizeof(((struct infilfs_superblock_disk *)raw)->checksum)))
            continue;
        memcpy(&current, raw, sizeof(current));
        if (!infilfs_checkpoint_basic_valid(&current, blocks))
            continue;
        if (!found || le64_to_cpu(current.generation) > best_generation) {
            *selected = current;
            best_generation = le64_to_cpu(current.generation);
            found = true;
        }
    }

    kfree(raw);
    return found ? 0 : -EINVAL;
}

static bool infilfs_object_basic_valid(struct super_block *sb,
                                       const u8 *block,
                                       u16 expected_type,
                                       const u8 *expected_id)
{
    const struct infilfs_object_header_disk *header =
        (const struct infilfs_object_header_disk *)block;
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    u16 version;
    u32 payload;

    if (memcmp(header->magic, infilfs_object_magic,
               sizeof(infilfs_object_magic)) != 0)
        return false;
    if (!infilfs_crc64_block_valid(block,
            offsetof(struct infilfs_object_header_disk, checksum),
            sizeof(header->checksum)))
        return false;
    if (le32_to_cpu(header->header_size) != sizeof(*header) ||
        le32_to_cpu(header->checksum_type) != INFILFS_CHECKSUM_CRC64_ECMA)
        return false;
    if (le64_to_cpu(header->generation) == 0 ||
        le64_to_cpu(header->generation) > le64_to_cpu(sbi->disk.generation))
        return false;

    if (expected_type && le16_to_cpu(header->object_type) != expected_type)
        return false;
    if (expected_id && memcmp(header->object_id, expected_id, 16) != 0)
        return false;

    version = le16_to_cpu(header->object_version);
    if (version != INFILFS_OBJECT_VERSION_CLASSIC &&
        version != INFILFS_OBJECT_VERSION_PAGED)
        return false;

    payload = le32_to_cpu(header->payload_size);
    if (payload > INFILFS_DISK_BLOCK_SIZE - sizeof(*header))
        return false;
    return true;
}

static int infilfs_read_object(struct super_block *sb, u64 object_block,
                               u16 expected_type, const u8 *expected_id,
                               u8 *out)
{
    int ret = infilfs_read_block(sb, object_block, out);

    if (ret)
        return ret;
    if (!infilfs_object_basic_valid(sb, out, expected_type, expected_id))
        return -EFSCORRUPTED;
    return 0;
}

static bool infilfs_metadata_page_valid(struct super_block *sb,
                                        const u8 *block,
                                        const u8 magic[8],
                                        const u8 owner_id[16])
{
    const struct infilfs_metadata_page_disk *page =
        (const struct infilfs_metadata_page_disk *)block;
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    u32 bytes;

    if (memcmp(page->magic, magic, 8) != 0 ||
        memcmp(page->owner_object_id, owner_id, 16) != 0)
        return false;
    if (!infilfs_crc64_block_valid(block,
            offsetof(struct infilfs_metadata_page_disk, checksum),
            sizeof(page->checksum)))
        return false;
    if (le64_to_cpu(page->generation) == 0 ||
        le64_to_cpu(page->generation) > le64_to_cpu(sbi->disk.generation))
        return false;
    if (le32_to_cpu(page->checksum_type) != INFILFS_CHECKSUM_CRC64_ECMA ||
        le32_to_cpu(page->reserved) != 0)
        return false;

    bytes = le32_to_cpu(page->bytes_used);
    if (bytes > INFILFS_DISK_BLOCK_SIZE - sizeof(*page))
        return false;
    return true;
}

static u64 infilfs_object_ino(const u8 id[16])
{
    u64 hash = 1469598103934665603ULL;
    unsigned int i;

    for (i = 0; i < 16; ++i) {
        hash ^= id[i];
        hash *= 1099511628211ULL;
    }
    hash &= 0x7fffffffffffffffULL;
    if (hash < 3)
        hash += 3;
    return hash;
}

static unsigned int infilfs_dtype(u16 type)
{
    switch (type) {
    case INFILFS_OBJECT_DIRECTORY:
        return DT_DIR;
    case INFILFS_OBJECT_FILE:
        return DT_REG;
    case INFILFS_OBJECT_SYMLINK:
        return DT_LNK;
    default:
        return DT_UNKNOWN;
    }
}

static int infilfs_index_lookup(struct super_block *sb, const u8 object_id[16],
                                u64 *object_block_out, u16 *type_out)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    const struct infilfs_object_header_disk *header;
    const struct infilfs_index_payload_disk *payload;
    const struct infilfs_index_entry_disk *entries;
    u32 total_entries;
    u16 version;
    u8 *head;
    u8 *page_block;
    int ret = -ENOENT;
    u32 i;

    if (memcmp(object_id, sbi->disk.root_object_id, 16) == 0) {
        *object_block_out = le64_to_cpu(sbi->disk.root_object_block);
        *type_out = INFILFS_OBJECT_DIRECTORY;
        return 0;
    }

    head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!head || !page_block) {
        ret = -ENOMEM;
        goto out;
    }

    ret = infilfs_read_object(sb, le64_to_cpu(sbi->disk.object_index_block),
                              INFILFS_OBJECT_INDEX, NULL, head);
    if (ret)
        goto out;

    header = (const struct infilfs_object_header_disk *)head;
    payload = (const struct infilfs_index_payload_disk *)(header + 1);
    total_entries = le32_to_cpu(payload->entry_count);
    version = le16_to_cpu(header->object_version);

    if (version == INFILFS_OBJECT_VERSION_CLASSIC) {
        size_t needed = sizeof(*payload) +
            (size_t)total_entries * sizeof(struct infilfs_index_entry_disk);

        if (le32_to_cpu(payload->reserved) != 0 ||
            needed > le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        entries = (const struct infilfs_index_entry_disk *)(payload + 1);
        for (i = 0; i < total_entries; ++i) {
            if (memcmp(entries[i].object_id, object_id, 16) != 0)
                continue;
            *object_block_out = le64_to_cpu(entries[i].object_block);
            *type_out = le16_to_cpu(entries[i].object_type);
            ret = 0;
            goto out;
        }
        ret = -ENOENT;
        goto out;
    }

    if (version == INFILFS_OBJECT_VERSION_PAGED) {
        const __le64 *pages = (const __le64 *)(payload + 1);
        u32 page_count = le32_to_cpu(payload->reserved);
        u32 seen = 0;
        u32 p;

        if (page_count == 0 || page_count > INFILFS_INDEX_PAGE_POINTERS) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        for (p = 0; p < page_count; ++p) {
            u64 block_no = le64_to_cpu(pages[p]);
            const struct infilfs_metadata_page_disk *page;
            u32 count;

            ret = infilfs_read_block(sb, block_no, page_block);
            if (ret)
                goto out;
            if (!infilfs_metadata_page_valid(sb, page_block,
                                             infilfs_index_page_magic,
                                             header->object_id)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            page = (const struct infilfs_metadata_page_disk *)page_block;
            count = le32_to_cpu(page->entry_count);
            if (count > INFILFS_INDEX_ENTRIES_PER_PAGE ||
                le32_to_cpu(page->bytes_used) !=
                    count * sizeof(struct infilfs_index_entry_disk)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            entries = (const struct infilfs_index_entry_disk *)(page + 1);
            for (i = 0; i < count; ++i) {
                if (memcmp(entries[i].object_id, object_id, 16) != 0)
                    continue;
                *object_block_out = le64_to_cpu(entries[i].object_block);
                *type_out = le16_to_cpu(entries[i].object_type);
                ret = 0;
                goto out;
            }
            seen += count;
        }
        ret = seen == total_entries ? -ENOENT : -EFSCORRUPTED;
        goto out;
    }

    ret = -EFSCORRUPTED;
out:
    kfree(page_block);
    kfree(head);
    return ret;
}

static int infilfs_walk_dir_buffer(const u8 *buffer, u32 bytes,
                                   int (*visitor)(const struct infilfs_dirent_disk *,
                                                  const u8 *, void *),
                                   void *arg)
{
    u32 offset = 0;

    while (offset < bytes) {
        const struct infilfs_dirent_disk *entry;
        u16 record_size;
        u16 name_len;
        const u8 *name;
        int ret;

        if (bytes - offset < sizeof(*entry))
            return -EFSCORRUPTED;
        entry = (const struct infilfs_dirent_disk *)(buffer + offset);
        record_size = le16_to_cpu(entry->record_size);
        name_len = le16_to_cpu(entry->name_length);
        if (record_size < sizeof(*entry) || (record_size & 7u) != 0 ||
            record_size > bytes - offset || name_len == 0 ||
            name_len > INFILFS_NAME_MAX || sizeof(*entry) + name_len > record_size)
            return -EFSCORRUPTED;

        name = buffer + offset + sizeof(*entry);
        if (memchr(name, '\0', name_len) || memchr(name, '/', name_len))
            return -EFSCORRUPTED;

        ret = visitor(entry, name, arg);
        if (ret)
            return ret;
        offset += record_size;
    }
    return offset == bytes ? 0 : -EFSCORRUPTED;
}

static int infilfs_for_each_dirent(struct inode *inode,
                                   int (*visitor)(const struct infilfs_dirent_disk *,
                                                  const u8 *, void *),
                                   void *arg)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    const struct infilfs_object_header_disk *header;
    const struct infilfs_directory_payload_disk *payload;
    u8 *object;
    u8 *page_block;
    u16 version;
    int ret;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!object || !page_block) {
        ret = -ENOMEM;
        goto out;
    }

    ret = infilfs_read_object(inode->i_sb, ii->object_block,
                              INFILFS_OBJECT_DIRECTORY, ii->object_id, object);
    if (ret)
        goto out;
    header = (const struct infilfs_object_header_disk *)object;
    payload = (const struct infilfs_directory_payload_disk *)(header + 1);
    version = le16_to_cpu(header->object_version);

    if (version == INFILFS_OBJECT_VERSION_CLASSIC) {
        u32 bytes = le32_to_cpu(payload->bytes_used);
        size_t needed = sizeof(*payload) + bytes;

        if (needed > le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        ret = infilfs_walk_dir_buffer((const u8 *)(payload + 1), bytes,
                                      visitor, arg);
        goto out;
    }

    if (version == INFILFS_OBJECT_VERSION_PAGED) {
        const __le64 *pages = (const __le64 *)(payload + 1);
        u32 page_count = le32_to_cpu(payload->bytes_used);
        u32 p;

        if (page_count == 0) {
            ret = le32_to_cpu(payload->entry_count) == 0 ? 0 : -EFSCORRUPTED;
            goto out;
        }
        if (page_count > INFILFS_DIRECTORY_PAGE_POINTERS) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        for (p = 0; p < page_count; ++p) {
            const struct infilfs_metadata_page_disk *page;
            u32 bytes;

            ret = infilfs_read_block(inode->i_sb, le64_to_cpu(pages[p]),
                                     page_block);
            if (ret)
                goto out;
            if (!infilfs_metadata_page_valid(inode->i_sb, page_block,
                                             infilfs_directory_page_magic,
                                             header->object_id)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            page = (const struct infilfs_metadata_page_disk *)page_block;
            bytes = le32_to_cpu(page->bytes_used);
            ret = infilfs_walk_dir_buffer((const u8 *)(page + 1), bytes,
                                          visitor, arg);
            if (ret)
                goto out;
        }
        ret = 0;
        goto out;
    }

    ret = -EFSCORRUPTED;
out:
    kfree(page_block);
    kfree(object);
    return ret;
}

static int infilfs_lookup_visitor(const struct infilfs_dirent_disk *entry,
                                  const u8 *name, void *arg)
{
    struct infilfs_dir_lookup *search = arg;
    u16 name_len = le16_to_cpu(entry->name_length);

    if (name_len != search->name_len || memcmp(name, search->name, name_len) != 0)
        return 0;
    memcpy(search->object_id, entry->object_id, 16);
    search->object_type = le16_to_cpu(entry->object_type);
    search->found = true;
    return 1;
}

static int infilfs_emit_visitor(const struct infilfs_dirent_disk *entry,
                                const u8 *name, void *arg)
{
    struct infilfs_dir_emit_state *state = arg;
    u16 name_len = le16_to_cpu(entry->name_length);
    u16 type = le16_to_cpu(entry->object_type);

    if (state->index < state->ctx->pos - 2) {
        state->index++;
        return 0;
    }

    if (!dir_emit(state->ctx, name, name_len,
                  infilfs_object_ino(entry->object_id), infilfs_dtype(type)))
        return 1;
    state->index++;
    state->ctx->pos++;
    return 0;
}

static int infilfs_map_file_block(struct inode *inode, const u8 *object,
                                  u64 logical, u64 *physical_out,
                                  u32 *flags_out)
{
    const struct infilfs_object_header_disk *header =
        (const struct infilfs_object_header_disk *)object;
    const struct infilfs_file_payload_disk *file =
        (const struct infilfs_file_payload_disk *)(header + 1);
    u16 version = le16_to_cpu(header->object_version);
    u32 extent_count = le32_to_cpu(file->extent_count);
    u32 i;

    if (version == INFILFS_OBJECT_VERSION_CLASSIC) {
        const struct infilfs_extent_disk *ext =
            (const struct infilfs_extent_disk *)(file + 1);
        size_t needed = sizeof(*file) +
            (size_t)extent_count * sizeof(struct infilfs_extent_disk);

        if (needed > le32_to_cpu(header->payload_size))
            return -EFSCORRUPTED;
        for (i = 0; i < extent_count; ++i) {
            u64 start = le64_to_cpu(ext[i].logical_block);
            u32 blocks = le32_to_cpu(ext[i].block_count);
            u32 flags = le32_to_cpu(ext[i].flags);

            if (!blocks || logical < start || logical >= start + blocks)
                continue;
            *flags_out = flags;
            *physical_out = flags == INFILFS_EXTENT_HOLE ? 0 :
                le64_to_cpu(ext[i].physical_block) + (logical - start);
            return 0;
        }
        return -EFSCORRUPTED;
    }

    if (version == INFILFS_OBJECT_VERSION_PAGED) {
        const struct infilfs_extent_head_disk *head =
            (const struct infilfs_extent_head_disk *)(file + 1);
        const __le64 *pages = (const __le64 *)(head + 1);
        u32 page_count = le32_to_cpu(head->page_count);
        u8 *page_block;
        u32 p;
        int ret = -EFSCORRUPTED;

        if (page_count == 0 || page_count > INFILFS_EXTENT_PAGE_POINTERS)
            return -EFSCORRUPTED;
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
        if (!page_block)
            return -ENOMEM;

        for (p = 0; p < page_count; ++p) {
            const struct infilfs_metadata_page_disk *page;
            const struct infilfs_extent_disk *ext;
            u32 count;

            ret = infilfs_read_block(inode->i_sb, le64_to_cpu(pages[p]),
                                     page_block);
            if (ret)
                break;
            if (!infilfs_metadata_page_valid(inode->i_sb, page_block,
                                             infilfs_extent_page_magic,
                                             header->object_id)) {
                ret = -EFSCORRUPTED;
                break;
            }
            page = (const struct infilfs_metadata_page_disk *)page_block;
            count = le32_to_cpu(page->entry_count);
            if (count == 0 || count > INFILFS_EXTENTS_PER_PAGE ||
                le32_to_cpu(page->bytes_used) !=
                    count * sizeof(struct infilfs_extent_disk)) {
                ret = -EFSCORRUPTED;
                break;
            }
            ext = (const struct infilfs_extent_disk *)(page + 1);
            for (i = 0; i < count; ++i) {
                u64 start = le64_to_cpu(ext[i].logical_block);
                u32 blocks = le32_to_cpu(ext[i].block_count);
                u32 flags = le32_to_cpu(ext[i].flags);

                if (!blocks || logical < start || logical >= start + blocks)
                    continue;
                *flags_out = flags;
                *physical_out = flags == INFILFS_EXTENT_HOLE ? 0 :
                    le64_to_cpu(ext[i].physical_block) + (logical - start);
                ret = 0;
                goto paged_out;
            }
        }
paged_out:
        kfree(page_block);
        return ret;
    }

    return -EFSCORRUPTED;
}

static __maybe_unused ssize_t infilfs_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct file *filep = iocb->ki_filp;
    struct inode *inode = file_inode(filep);
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    const struct infilfs_object_header_disk *header;
    const struct infilfs_file_payload_disk *file;
    loff_t pos = iocb->ki_pos;
    u64 file_size;
    size_t requested;
    size_t done = 0;
    u8 *object;
    u8 *data;
    int ret;

    if (pos < 0)
        return -EINVAL;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    data = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!object || !data) {
        ret = -ENOMEM;
        goto out;
    }

    ret = infilfs_read_object(inode->i_sb, ii->object_block,
                              INFILFS_OBJECT_FILE, ii->object_id, object);
    if (ret)
        goto out;
    header = (const struct infilfs_object_header_disk *)object;
    file = (const struct infilfs_file_payload_disk *)(header + 1);
    file_size = le64_to_cpu(file->attributes.logical_size);
    if ((u64)pos >= file_size) {
        ret = 0;
        goto out;
    }

    requested = min_t(u64, iov_iter_count(to), file_size - (u64)pos);

    if (le32_to_cpu(file->extent_count) == 0 &&
        file_size <= INFILFS_INLINE_DATA_MAX) {
        const u8 *inline_bytes = (const u8 *)(file + 1) +
            sizeof(struct infilfs_data_checksum_disk);
        size_t copied = copy_to_iter(inline_bytes + pos, requested, to);

        iocb->ki_pos += copied;
        ret = copied;
        goto out;
    }

    while (done < requested) {
        u64 absolute = (u64)pos + done;
        u64 logical = absolute >> INFILFS_DISK_BLOCK_SHIFT;
        size_t within = absolute & (INFILFS_DISK_BLOCK_SIZE - 1u);
        size_t chunk = min_t(size_t, INFILFS_DISK_BLOCK_SIZE - within,
                             requested - done);
        u64 physical = 0;
        u32 flags = 0;
        size_t copied;

        ret = infilfs_map_file_block(inode, object, logical,
                                     &physical, &flags);
        if (ret)
            goto out_partial;
        if (flags == INFILFS_EXTENT_HOLE) {
            memset(data, 0, INFILFS_DISK_BLOCK_SIZE);
        } else if (flags == INFILFS_EXTENT_NORMAL) {
            ret = infilfs_read_block(inode->i_sb, physical, data);
            if (ret)
                goto out_partial;
        } else {
            ret = -EFSCORRUPTED;
            goto out_partial;
        }

        copied = copy_to_iter(data + within, chunk, to);
        done += copied;
        if (copied != chunk)
            break;
    }

    iocb->ki_pos += done;
    ret = done;
    goto out;

out_partial:
    if (done) {
        iocb->ki_pos += done;
        ret = done;
    }
out:
    kfree(data);
    kfree(object);
    return ret;
}

static const char *infilfs_get_link(struct dentry *dentry, struct inode *inode,
                                    struct delayed_call *done)
{
    struct infilfs_inode_info *ii;

    (void)dentry;
    (void)done;
    if (!inode)
        return ERR_PTR(-ECHILD);
    ii = INFILFS_I(inode);
    if (!ii || !ii->symlink_target)
        return ERR_PTR(-EIO);
    return ii->symlink_target;
}

static int infilfs_file_allocated_blocks(
    struct inode *inode, const u8 object[INFILFS_DISK_BLOCK_SIZE],
    u64 *allocated_out)
{
    const struct infilfs_object_header_disk *header =
        (const struct infilfs_object_header_disk *)object;
    const struct infilfs_file_payload_disk *file =
        (const struct infilfs_file_payload_disk *)(header + 1);
    u32 extent_count = le32_to_cpu(file->extent_count);
    u64 allocated = 0;
    u32 copied = 0;
    int ret = 0;

    if (!extent_count) {
        *allocated_out = 0;
        return 0;
    }
    if (le16_to_cpu(header->object_version) == INFILFS_OBJECT_VERSION_CLASSIC) {
        const struct infilfs_extent_disk *extents =
            (const struct infilfs_extent_disk *)(file + 1);
        u32 i;

        if (sizeof(*file) + (size_t)extent_count * sizeof(*extents) !=
            le32_to_cpu(header->payload_size))
            return -EFSCORRUPTED;
        for (i = 0; i < extent_count; ++i)
            if (le32_to_cpu(extents[i].flags) == INFILFS_EXTENT_NORMAL)
                allocated += le32_to_cpu(extents[i].block_count);
    } else if (le16_to_cpu(header->object_version) ==
               INFILFS_OBJECT_VERSION_PAGED) {
        const struct infilfs_extent_head_disk *head =
            (const struct infilfs_extent_head_disk *)(file + 1);
        const __le64 *pages = (const __le64 *)(head + 1);
        u32 page_count = le32_to_cpu(head->page_count);
        u8 *page_block;
        u32 p;

        if (!page_count || page_count > INFILFS_EXTENT_PAGE_POINTERS ||
            le32_to_cpu(head->reserved) != 0)
            return -EFSCORRUPTED;
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
        if (!page_block)
            return -ENOMEM;
        for (p = 0; p < page_count; ++p) {
            const struct infilfs_metadata_page_disk *page;
            const struct infilfs_extent_disk *extents;
            u32 count;
            u32 i;

            ret = infilfs_read_block(inode->i_sb,
                                     le64_to_cpu(pages[p]), page_block);
            if (ret)
                break;
            if (!infilfs_metadata_page_valid(inode->i_sb, page_block,
                                             infilfs_extent_page_magic,
                                             header->object_id)) {
                ret = -EFSCORRUPTED;
                break;
            }
            page = (const struct infilfs_metadata_page_disk *)page_block;
            count = le32_to_cpu(page->entry_count);
            if (!count || count > INFILFS_EXTENTS_PER_PAGE ||
                copied > extent_count || count > extent_count - copied ||
                le32_to_cpu(page->bytes_used) !=
                    count * sizeof(*extents)) {
                ret = -EFSCORRUPTED;
                break;
            }
            extents = (const struct infilfs_extent_disk *)(page + 1);
            for (i = 0; i < count; ++i)
                if (le32_to_cpu(extents[i].flags) == INFILFS_EXTENT_NORMAL)
                    allocated += le32_to_cpu(extents[i].block_count);
            copied += count;
        }
        kfree(page_block);
        if (ret)
            return ret;
        if (copied != extent_count)
            return -EFSCORRUPTED;
    } else {
        return -EFSCORRUPTED;
    }
    *allocated_out = allocated;
    return 0;
}

static int infilfs_populate_inode(struct inode *inode, u64 object_block,
                                  u16 expected_type, const u8 expected_id[16])
{
    struct infilfs_inode_info *ii;
    const struct infilfs_object_header_disk *header;
    const struct infilfs_attributes_disk *attributes;
    const struct infilfs_posix_compat_disk *posix;
    u8 *object;
    umode_t permissions;
    u64 links;
    int ret;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!object)
        return -ENOMEM;
    ret = infilfs_read_object(inode->i_sb, object_block, expected_type,
                              expected_id, object);
    if (ret)
        goto out;

    header = (const struct infilfs_object_header_disk *)object;
    ii = kzalloc(sizeof(*ii), GFP_KERNEL);
    if (!ii) {
        ret = -ENOMEM;
        goto out;
    }
    ii->object_block = object_block;
    ii->object_type = le16_to_cpu(header->object_type);
    memcpy(ii->object_id, header->object_id, 16);
    inode->i_private = ii;

    if (ii->object_type == INFILFS_OBJECT_DIRECTORY) {
        const struct infilfs_directory_payload_disk *payload =
            (const struct infilfs_directory_payload_disk *)(header + 1);
        attributes = &payload->attributes;
        posix = &payload->posix;
        permissions = le32_to_cpu(posix->permissions) & 07777;
        inode->i_mode = S_IFDIR | permissions;
        inode->i_op = &infilfs_dir_inode_operations;
        inode->i_fop = &infilfs_dir_operations;
        links = le64_to_cpu(attributes->link_count);
        set_nlink(inode, links >= 2 ? links : 2);
        i_size_write(inode, le64_to_cpu(attributes->logical_size));
    } else if (ii->object_type == INFILFS_OBJECT_FILE) {
        const struct infilfs_file_payload_disk *payload =
            (const struct infilfs_file_payload_disk *)(header + 1);
        attributes = &payload->attributes;
        posix = &payload->posix;
        permissions = le32_to_cpu(posix->permissions) & 07777;
        inode->i_mode = S_IFREG | permissions;
        inode->i_op = &infilfs_file_inode_operations;
        inode->i_fop = &infilfs_file_operations;
        links = le64_to_cpu(attributes->link_count);
        set_nlink(inode, links ? links : 1);
        i_size_write(inode, le64_to_cpu(attributes->logical_size));
        {
            u64 allocated;

            ret = infilfs_file_allocated_blocks(inode, object, &allocated);
            if (ret)
                goto fail_private;
            inode->i_blocks = allocated *
                (INFILFS_DISK_BLOCK_SIZE >> 9);
        }
    } else if (ii->object_type == INFILFS_OBJECT_SYMLINK) {
        const struct infilfs_symlink_payload_disk *payload =
            (const struct infilfs_symlink_payload_disk *)(header + 1);
        u32 target_len = le32_to_cpu(payload->target_length);
        const u8 *target = (const u8 *)(payload + 1);

        attributes = &payload->attributes;
        posix = &payload->posix;
        if (target_len == 0 || target_len > INFILFS_DISK_BLOCK_SIZE -
            sizeof(*header) - sizeof(*payload) ||
            sizeof(*payload) + target_len > le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto fail_private;
        }
        ii->symlink_target = kmemdup_nul(target, target_len, GFP_KERNEL);
        if (!ii->symlink_target) {
            ret = -ENOMEM;
            goto fail_private;
        }
        inode->i_mode = S_IFLNK | 0777;
        inode->i_op = &infilfs_symlink_inode_operations;
        set_nlink(inode, 1);
        i_size_write(inode, target_len);
    } else {
        ret = -EOPNOTSUPP;
        goto fail_private;
    }

    {
        kuid_t uid = make_kuid(&init_user_ns, le32_to_cpu(posix->uid));
        kgid_t gid = make_kgid(&init_user_ns, le32_to_cpu(posix->gid));
        inode->i_uid = uid_valid(uid) ? uid : GLOBAL_ROOT_UID;
        inode->i_gid = gid_valid(gid) ? gid : GLOBAL_ROOT_GID;
    }
    ret = 0;
    goto out;

fail_private:
    kfree(ii->symlink_target);
    kfree(ii);
    inode->i_private = NULL;
out:
    kfree(object);
    return ret;
}

static struct inode *infilfs_get_inode(struct super_block *sb, u64 object_block,
                                       u16 expected_type,
                                       const u8 expected_id[16])
{
    struct inode *inode;
    u64 ino = infilfs_object_ino(expected_id);
    int ret;

    inode = iget_locked(sb, ino);
    if (!inode)
        return ERR_PTR(-ENOMEM);
    if (!infilfs_inode_is_new(inode))
        return inode;

    ret = infilfs_populate_inode(inode, object_block, expected_type, expected_id);
    if (ret) {
        iget_failed(inode);
        return ERR_PTR(ret);
    }
    unlock_new_inode(inode);
    return inode;
}

static struct dentry *infilfs_lookup(struct inode *dir, struct dentry *dentry,
                                     unsigned int flags)
{
    struct infilfs_dir_lookup search = {
        .name = dentry->d_name.name,
        .name_len = dentry->d_name.len,
    };
    struct inode *inode;
    u64 object_block;
    u16 indexed_type;
    int ret;

    (void)flags;
    if (search.name_len == 0 || search.name_len > INFILFS_NAME_MAX)
        return ERR_PTR(-ENAMETOOLONG);

    ret = infilfs_for_each_dirent(dir, infilfs_lookup_visitor, &search);
    if (ret < 0)
        return ERR_PTR(ret);
    if (!search.found) {
        d_add(dentry, NULL);
        return NULL;
    }

    ret = infilfs_index_lookup(dir->i_sb, search.object_id,
                               &object_block, &indexed_type);
    if (ret)
        return ERR_PTR(ret);
    if (indexed_type != search.object_type)
        return ERR_PTR(-EFSCORRUPTED);

    inode = infilfs_get_inode(dir->i_sb, object_block, indexed_type,
                              search.object_id);
    if (IS_ERR(inode))
        return ERR_CAST(inode);
    return d_splice_alias(inode, dentry);
}

static int infilfs_iterate_shared(struct file *file, struct dir_context *ctx)
{
    struct infilfs_dir_emit_state state = {
        .ctx = ctx,
        .index = 0,
    };
    int ret;

    if (!dir_emit_dots(file, ctx))
        return 0;
    ret = infilfs_for_each_dirent(file_inode(file), infilfs_emit_visitor, &state);
    return ret < 0 ? ret : 0;
}

#include "infiltratorfs_rw.inc"

static void infilfs_evict_inode(struct inode *inode)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    int ret;

    truncate_inode_pages_final(&inode->i_data);
    ret = infilfs_ns_evict_unlinked_file(inode);
    if (ret)
        pr_err("InfiltratorFS: could not reclaim unlinked inode %lu: %d\n",
               inode->i_ino, ret);
    clear_inode(inode);
    if (ii) {
        kfree(ii->symlink_target);
        kfree(ii);
        inode->i_private = NULL;
    }
}

static void infilfs_put_super(struct super_block *sb)
{
    infilfs_rw_mount_destroy(sb);
    kfree(sb->s_fs_info);
    sb->s_fs_info = NULL;
}

static const struct super_operations infilfs_super_operations = {
    .statfs = simple_statfs,
    .sync_fs = infilfs_sync_fs,
    .evict_inode = infilfs_evict_inode,
    .put_super = infilfs_put_super,
};

static const struct inode_operations infilfs_dir_inode_operations = {
    .lookup = infilfs_lookup,
    .create = infilfs_rw_create,
    .mkdir = infilfs_rw_mkdir,
};

static const struct inode_operations infilfs_file_inode_operations = {
    .setattr = infilfs_rw_setattr,
};

static const struct inode_operations infilfs_symlink_inode_operations = {
    .get_link = infilfs_get_link,
};

static const struct file_operations infilfs_dir_operations = {
    .owner = THIS_MODULE,
    .llseek = generic_file_llseek,
    .iterate_shared = infilfs_iterate_shared,
};

static const struct file_operations infilfs_file_operations = {
    .owner = THIS_MODULE,
    .llseek = generic_file_llseek,
    .read_iter = infilfs_file_read_iter,
    .write_iter = infilfs_file_write_iter,
    .fallocate = infilfs_file_fallocate,
    .fsync = infilfs_file_fsync,
};

static int infilfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
    struct infilfs_sb_info *sbi;
    struct inode *root_inode;
    u64 bytes;
    int ret;

    (void)fc;

    if (!sb_set_blocksize(sb, INFILFS_DISK_BLOCK_SIZE))
        return -EINVAL;

    bytes = bdev_nr_bytes(sb->s_bdev);
    if (bytes < INFILFS_DISK_BLOCK_SIZE * 3ULL ||
        (bytes & (INFILFS_DISK_BLOCK_SIZE - 1u)) != 0)
        return -EINVAL;

    sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
    if (!sbi)
        return -ENOMEM;
    sbi->device_blocks = bytes >> INFILFS_DISK_BLOCK_SHIFT;
    mutex_init(&sbi->write_lock);
    sb->s_fs_info = sbi;

    ret = infilfs_select_checkpoint(sb, &sbi->disk);
    if (ret)
        goto fail;
    ret = infilfs_rw_mount_init(sb);
    if (ret)
        goto fail;

    sb->s_magic = INFILTRATORFS_MAGIC;
    sb->s_maxbytes = MAX_LFS_FILESIZE;
    sb->s_time_gran = 1;
    sb->s_op = &infilfs_super_operations;

    root_inode = infilfs_get_inode(sb, le64_to_cpu(sbi->disk.root_object_block),
                                   INFILFS_OBJECT_DIRECTORY,
                                   sbi->disk.root_object_id);
    if (IS_ERR(root_inode)) {
        ret = PTR_ERR(root_inode);
        goto fail;
    }

    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root) {
        ret = -ENOMEM;
        goto fail;
    }
    ret = infilfs_native_recover_unlinked_files(sb);
    if (ret)
        goto fail;

    pr_info("InfiltratorFS: native %s mount Format %u.%u generation %llu\n",
            sb_rdonly(sb) ? "read-only" : "read-write",
            INFILFS_FORMAT_MAJOR, INFILFS_FORMAT_MINOR,
            (unsigned long long)le64_to_cpu(sbi->disk.generation));
    return 0;

fail:
    if (sb->s_root) {
        dput(sb->s_root);
        sb->s_root = NULL;
    }
    infilfs_rw_mount_destroy(sb);
    kfree(sbi);
    sb->s_fs_info = NULL;
    return ret;
}

static int infilfs_get_tree(struct fs_context *fc)
{
    return get_tree_bdev(fc, infilfs_fill_super);
}

static const struct fs_context_operations infilfs_context_operations = {
    .get_tree = infilfs_get_tree,
};

static int infilfs_init_fs_context(struct fs_context *fc)
{
    fc->ops = &infilfs_context_operations;
    return 0;
}

static struct file_system_type infilfs_type = {
    .owner = THIS_MODULE,
    .name = INFILTRATORFS_NAME,
    .init_fs_context = infilfs_init_fs_context,
    .kill_sb = kill_block_super,
    .fs_flags = FS_REQUIRES_DEV,
};

static int __init infilfs_init(void)
{
    int status = register_filesystem(&infilfs_type);

    if (status == 0)
        pr_info("InfiltratorFS: native Linux VFS registered with initial RW support\n");
    return status;
}

static void __exit infilfs_exit(void)
{
    unregister_filesystem(&infilfs_type);
    pr_info("InfiltratorFS: native Linux VFS unloaded\n");
}

module_init(infilfs_init);
module_exit(infilfs_exit);

MODULE_DESCRIPTION("InfiltratorFS native Linux VFS driver with initial RW support");
MODULE_AUTHOR("The First Infiltrator");
MODULE_LICENSE("GPL");
MODULE_ALIAS_FS(INFILTRATORFS_NAME);
