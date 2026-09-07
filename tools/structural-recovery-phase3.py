#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Convert native Linux paged file extents to the same unbounded root+chain layout."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def write(path, text):
    (ROOT / path).write_text(text, encoding="utf-8")


def replace_function(text, signature, replacement, label):
    start = text.find(signature)
    if start < 0:
        raise SystemExit(f"{label}: signature not found")
    brace = text.find("{", start + len(signature))
    if brace < 0:
        raise SystemExit(f"{label}: opening brace not found")
    depth = 0
    i = brace
    in_str = in_char = in_line = in_block = False
    esc = False
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if in_line:
            if c == "\n":
                in_line = False
        elif in_block:
            if c == "*" and n == "/":
                in_block = False
                i += 1
        elif in_str:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
        elif in_char:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == "'":
                in_char = False
        else:
            if c == "/" and n == "/":
                in_line = True
                i += 1
            elif c == "/" and n == "*":
                in_block = True
                i += 1
            elif c == '"':
                in_str = True
            elif c == "'":
                in_char = True
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return text[:start] + replacement.rstrip() + "\n" + text[i + 1:]
        i += 1
    raise SystemExit(f"{label}: unterminated function")


# Shared native helpers: chain links are part of bytes_used and therefore CRC64-covered.
path = "kernel/infiltratorfs_internal.h"
text = read(path)
anchor = '#define INFILFS_LINUX_META_DIRECTORY ".infilfs-posix-meta"\n'
helpers = r'''

static inline bool infilfs_extent_page_shape_valid(
    const u8 block[INFILFS_DISK_BLOCK_SIZE], u32 *count_out)
{
    const struct infilfs_metadata_page_disk *page =
        (const struct infilfs_metadata_page_disk *)block;
    u32 count = le32_to_cpu(page->entry_count);
    size_t extent_bytes;

    if (!count || count > INFILFS_EXTENTS_PER_PAGE)
        return false;
    extent_bytes = (size_t)count * sizeof(struct infilfs_extent_disk);
    if (extent_bytes > INFILFS_METADATA_PAGE_DATA_SIZE - sizeof(__le64) ||
        le32_to_cpu(page->bytes_used) != extent_bytes + sizeof(__le64))
        return false;
    if (count_out)
        *count_out = count;
    return true;
}

static inline u64 infilfs_extent_page_next_block(
    const u8 block[INFILFS_DISK_BLOCK_SIZE])
{
    const struct infilfs_metadata_page_disk *page =
        (const struct infilfs_metadata_page_disk *)block;
    u32 count = le32_to_cpu(page->entry_count);
    size_t offset;
    __le64 encoded = 0;

    if (!count || count > INFILFS_EXTENTS_PER_PAGE)
        return 0;
    offset = sizeof(*page) +
        (size_t)count * sizeof(struct infilfs_extent_disk);
    if (offset > INFILFS_DISK_BLOCK_SIZE - sizeof(encoded))
        return 0;
    memcpy(&encoded, block + offset, sizeof(encoded));
    return le64_to_cpu(encoded);
}

static inline int infilfs_extent_page_set_next_block(
    u8 block[INFILFS_DISK_BLOCK_SIZE], u64 next)
{
    struct infilfs_metadata_page_disk *page =
        (struct infilfs_metadata_page_disk *)block;
    u32 count = le32_to_cpu(page->entry_count);
    size_t extent_bytes;
    size_t offset;
    __le64 encoded = cpu_to_le64(next);

    if (!count || count > INFILFS_EXTENTS_PER_PAGE)
        return -EINVAL;
    extent_bytes = (size_t)count * sizeof(struct infilfs_extent_disk);
    if (extent_bytes > INFILFS_METADATA_PAGE_DATA_SIZE - sizeof(encoded))
        return -EOVERFLOW;
    offset = sizeof(*page) + extent_bytes;
    memcpy(block + offset, &encoded, sizeof(encoded));
    page->bytes_used = cpu_to_le32(extent_bytes + sizeof(encoded));
    return 0;
}
'''
if "infilfs_extent_page_shape_valid" not in text:
    if anchor not in text:
        raise SystemExit("internal helper anchor not found")
    text = text.replace(anchor, anchor + helpers, 1)
write(path, text)


# Mounted verified-read cursor follows the chain and carries the next page across sequential reads.
path = "kernel/infiltratorfs_read_cache.c"
text = read(path)
old_struct = '''struct infilfs_native_read_extent_cursor {\n    u32 page_index;\n    u64 first_logical;\n    u64 last_logical;\n    bool valid;\n};'''
new_struct = '''struct infilfs_native_read_extent_cursor {\n    u32 page_index;\n    u64 page_block;\n    u64 next_page_block;\n    u64 first_logical;\n    u64 last_logical;\n    bool valid;\n};'''
if old_struct in text:
    text = text.replace(old_struct, new_struct, 1)
elif new_struct not in text:
    raise SystemExit("read extent cursor shape not found")
text = replace_function(text, "static int infilfs_native_map_file_block_cached(", r'''static int infilfs_native_map_file_block_cached(
    struct inode *inode, const u8 *object, u64 logical,
    u8 extent_page[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_native_read_extent_cursor *cursor,
    u64 *physical_out, u32 *flags_out,
    u64 *extent_logical_out, u32 *extent_blocks_out)
{
    const struct infilfs_object_header_disk *header =
        (const struct infilfs_object_header_disk *)object;
    const struct infilfs_file_payload_disk *file =
        (const struct infilfs_file_payload_disk *)(header + 1);
    const struct infilfs_extent_head_disk *head;
    const __le64 *root_ptr;
    const struct infilfs_metadata_page_disk *page;
    const struct infilfs_extent_disk *ext;
    u16 version = le16_to_cpu(header->object_version);
    u32 page_count;
    u32 count;
    u32 lo;
    u32 hi;
    u32 p;
    u64 block;
    u64 root;
    int ret;

    if (version != INFILFS_OBJECT_VERSION_PAGED)
        return infilfs_map_file_block_detail(
            inode, object, logical, physical_out, flags_out,
            extent_logical_out, extent_blocks_out);

    head = (const struct infilfs_extent_head_disk *)(file + 1);
    root_ptr = (const __le64 *)(head + 1);
    page_count = le32_to_cpu(head->page_count);
    root = le64_to_cpu(*root_ptr);
    if (!page_count || page_count > le32_to_cpu(file->extent_count) ||
        le32_to_cpu(head->reserved) != 0 || !root ||
        sizeof(*file) + sizeof(*head) + sizeof(*root_ptr) !=
            le32_to_cpu(header->payload_size))
        return -EFSCORRUPTED;

    if (cursor->valid && logical >= cursor->first_logical &&
        logical < cursor->last_logical)
        goto have_page;

    if (cursor->valid && logical >= cursor->last_logical &&
        cursor->page_index + 1u < page_count && cursor->next_page_block) {
        p = cursor->page_index + 1u;
        block = cursor->next_page_block;
    } else {
        p = 0;
        block = root;
    }
    cursor->valid = false;

    for (; p < page_count; ++p) {
        u64 first;
        u64 last;
        u64 next;

        ret = infilfs_read_allocated_block(inode->i_sb, block, extent_page);
        if (ret)
            return ret;
        if (!infilfs_metadata_page_valid(
                inode->i_sb, extent_page, infilfs_extent_page_magic,
                header->object_id) ||
            !infilfs_extent_page_shape_valid(extent_page, &count))
            return -EFSCORRUPTED;
        page = (const struct infilfs_metadata_page_disk *)extent_page;
        ext = (const struct infilfs_extent_disk *)(page + 1);
        first = le64_to_cpu(ext[0].logical_block);
        last = le64_to_cpu(ext[count - 1u].logical_block) +
            le32_to_cpu(ext[count - 1u].block_count);
        next = infilfs_extent_page_next_block(extent_page);
        if (last <= first ||
            (p + 1u < page_count && !next) ||
            (p + 1u == page_count && next))
            return -EFSCORRUPTED;
        if (logical < first)
            return -EFSCORRUPTED;
        if (logical < last) {
            cursor->page_index = p;
            cursor->page_block = block;
            cursor->next_page_block = next;
            cursor->first_logical = first;
            cursor->last_logical = last;
            cursor->valid = true;
            break;
        }
        block = next;
    }
    if (!cursor->valid)
        return -EFSCORRUPTED;

have_page:
    page = (const struct infilfs_metadata_page_disk *)extent_page;
    if (!infilfs_extent_page_shape_valid(extent_page, &count))
        return -EFSCORRUPTED;
    ext = (const struct infilfs_extent_disk *)(page + 1);
    lo = 0;
    hi = count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        u64 start = le64_to_cpu(ext[mid].logical_block);
        u32 blocks = le32_to_cpu(ext[mid].block_count);
        u32 flags = le32_to_cpu(ext[mid].flags);
        u64 end;

        if (!blocks || start > U64_MAX - blocks)
            return -EFSCORRUPTED;
        end = start + blocks;
        if (logical < start) {
            hi = mid;
            continue;
        }
        if (logical >= end) {
            lo = mid + 1u;
            continue;
        }
        if (!infilfs_extent_flags_valid(
                blocks, le64_to_cpu(ext[mid].physical_block), flags))
            return -EFSCORRUPTED;
        *flags_out = flags;
        if (infilfs_extent_kind(flags) == INFILFS_EXTENT_HOLE)
            *physical_out = 0;
        else if (infilfs_extent_is_compressed(flags))
            *physical_out = le64_to_cpu(ext[mid].physical_block);
        else
            *physical_out =
                le64_to_cpu(ext[mid].physical_block) + (logical - start);
        if (extent_logical_out)
            *extent_logical_out = start;
        if (extent_blocks_out)
            *extent_blocks_out = blocks;
        return 0;
    }
    return -EFSCORRUPTED;
}''', "native cached extent-chain mapper")
write(path, text)


# Core checkpoint validation, generic mapping and allocation accounting all walk the same chain.
path = "kernel/infiltratorfs_core.c"
text = read(path)
text = replace_function(text, "static int infilfs_validate_checkpoint_file(", r'''static int infilfs_validate_checkpoint_file(
    struct super_block *sb,
    const u8 object[INFILFS_DISK_BLOCK_SIZE])
{
    const struct infilfs_object_header_disk *header =
        (const struct infilfs_object_header_disk *)object;
    const struct infilfs_file_payload_disk *file =
        (const struct infilfs_file_payload_disk *)(header + 1);
    u32 extent_count = le32_to_cpu(file->extent_count);
    u16 version = le16_to_cpu(header->object_version);
    u64 last_logical = 0;
    bool has_normal = false;

    if (le32_to_cpu(header->payload_size) < sizeof(*file))
        return -EFSCORRUPTED;
    if (!extent_count) {
        u64 size = le64_to_cpu(file->attributes.logical_size);
        size_t expected = sizeof(*file);

        if (version != INFILFS_OBJECT_VERSION_CLASSIC ||
            size > INFILFS_INLINE_DATA_MAX ||
            le32_to_cpu(file->data_checksum_type) !=
                INFILFS_CHECKSUM_SHA256 ||
            memchr_inv(file->checksum_head_id, 0,
                       sizeof(file->checksum_head_id)))
            return -EFSCORRUPTED;
        if (size)
            expected += sizeof(struct infilfs_data_checksum_disk) +
                (size_t)size;
        return expected == le32_to_cpu(header->payload_size) ?
            0 : -EFSCORRUPTED;
    }
    if (le32_to_cpu(file->data_checksum_type) != INFILFS_CHECKSUM_SHA256)
        return -EFSCORRUPTED;
    if (version == INFILFS_OBJECT_VERSION_CLASSIC) {
        const struct infilfs_extent_disk *extents =
            (const struct infilfs_extent_disk *)(file + 1);

        if (extent_count > (INFILFS_DISK_BLOCK_SIZE - sizeof(*header) -
                            sizeof(*file)) / sizeof(*extents) ||
            sizeof(*file) + (size_t)extent_count * sizeof(*extents) !=
            le32_to_cpu(header->payload_size))
            return -EFSCORRUPTED;
        {
            int ret = infilfs_validate_checkpoint_extents(
                sb, extents, extent_count, &last_logical, &has_normal);
            bool has_checksum_head = memchr_inv(
                file->checksum_head_id, 0,
                sizeof(file->checksum_head_id)) != NULL;

            return ret ? ret :
                (has_normal == has_checksum_head ? 0 : -EFSCORRUPTED);
        }
    }
    if (version == INFILFS_OBJECT_VERSION_PAGED) {
        const struct infilfs_extent_head_disk *head =
            (const struct infilfs_extent_head_disk *)(file + 1);
        const __le64 *root_ptr = (const __le64 *)(head + 1);
        u32 page_count = le32_to_cpu(head->page_count);
        u32 copied = 0;
        u64 block = le64_to_cpu(*root_ptr);
        u8 *page_block;
        u32 p;
        int ret = 0;

        if (!page_count || page_count > extent_count ||
            page_count > infilfs_volume_blocks(INFILFS_SB(sb)) ||
            le32_to_cpu(head->reserved) != 0 || !block ||
            sizeof(*file) + sizeof(*head) + sizeof(*root_ptr) !=
                le32_to_cpu(header->payload_size))
            return -EFSCORRUPTED;
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!page_block)
            return -ENOMEM;
        for (p = 0; p < page_count; ++p) {
            const struct infilfs_metadata_page_disk *page;
            const struct infilfs_extent_disk *extents;
            u32 count;
            u64 next;

            ret = infilfs_read_allocated_block(sb, block, page_block);
            if (ret)
                break;
            if (!infilfs_metadata_page_valid(
                    sb, page_block, infilfs_extent_page_magic,
                    header->object_id) ||
                !infilfs_extent_page_shape_valid(page_block, &count) ||
                copied > extent_count || count > extent_count - copied) {
                ret = -EFSCORRUPTED;
                break;
            }
            page = (const struct infilfs_metadata_page_disk *)page_block;
            extents = (const struct infilfs_extent_disk *)(page + 1);
            ret = infilfs_validate_checkpoint_extents(
                sb, extents, count, &last_logical, &has_normal);
            if (ret)
                break;
            copied += count;
            next = infilfs_extent_page_next_block(page_block);
            if ((p + 1u < page_count && !next) ||
                (p + 1u == page_count && next)) {
                ret = -EFSCORRUPTED;
                break;
            }
            block = next;
        }
        kfree(page_block);
        if (!ret && copied != extent_count)
            ret = -EFSCORRUPTED;
        if (!ret && last_logical != DIV_ROUND_UP_ULL(
                le64_to_cpu(file->attributes.logical_size),
                INFILFS_DISK_BLOCK_SIZE))
            ret = -EFSCORRUPTED;
        if (!ret && has_normal !=
            (memchr_inv(file->checksum_head_id, 0,
                        sizeof(file->checksum_head_id)) != NULL))
            ret = -EFSCORRUPTED;
        return ret;
    }
    return -EFSCORRUPTED;
}''', "checkpoint extent-chain validation")

text = replace_function(text, "int infilfs_map_file_block_detail(", r'''int infilfs_map_file_block_detail(
    struct inode *inode, const u8 *object, u64 logical,
    u64 *physical_out, u32 *flags_out,
    u64 *extent_logical_out, u32 *extent_blocks_out)
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
            if (!infilfs_extent_flags_valid(
                    blocks, le64_to_cpu(ext[i].physical_block), flags))
                return -EFSCORRUPTED;
            *flags_out = flags;
            if (infilfs_extent_kind(flags) == INFILFS_EXTENT_HOLE)
                *physical_out = 0;
            else if (infilfs_extent_is_compressed(flags))
                *physical_out = le64_to_cpu(ext[i].physical_block);
            else
                *physical_out = le64_to_cpu(ext[i].physical_block) +
                    (logical - start);
            if (extent_logical_out)
                *extent_logical_out = start;
            if (extent_blocks_out)
                *extent_blocks_out = blocks;
            return 0;
        }
        return -EFSCORRUPTED;
    }

    if (version == INFILFS_OBJECT_VERSION_PAGED) {
        const struct infilfs_extent_head_disk *head =
            (const struct infilfs_extent_head_disk *)(file + 1);
        const __le64 *root_ptr = (const __le64 *)(head + 1);
        u32 page_count = le32_to_cpu(head->page_count);
        u64 block = le64_to_cpu(*root_ptr);
        u8 *page_block;
        u32 p;
        int ret = -EFSCORRUPTED;

        if (!page_count || page_count > extent_count || !block ||
            le32_to_cpu(head->reserved) != 0 ||
            sizeof(*file) + sizeof(*head) + sizeof(*root_ptr) !=
                le32_to_cpu(header->payload_size))
            return -EFSCORRUPTED;
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
        if (!page_block)
            return -ENOMEM;

        for (p = 0; p < page_count; ++p) {
            const struct infilfs_metadata_page_disk *page;
            const struct infilfs_extent_disk *ext;
            u32 count;
            u64 first;
            u64 last;
            u64 next;
            u32 lo;
            u32 hi;

            ret = infilfs_read_allocated_block(inode->i_sb, block, page_block);
            if (ret)
                break;
            if (!infilfs_metadata_page_valid(
                    inode->i_sb, page_block, infilfs_extent_page_magic,
                    header->object_id) ||
                !infilfs_extent_page_shape_valid(page_block, &count)) {
                ret = -EFSCORRUPTED;
                break;
            }
            page = (const struct infilfs_metadata_page_disk *)page_block;
            ext = (const struct infilfs_extent_disk *)(page + 1);
            first = le64_to_cpu(ext[0].logical_block);
            last = le64_to_cpu(ext[count - 1u].logical_block) +
                le32_to_cpu(ext[count - 1u].block_count);
            next = infilfs_extent_page_next_block(page_block);
            if (last <= first ||
                (p + 1u < page_count && !next) ||
                (p + 1u == page_count && next)) {
                ret = -EFSCORRUPTED;
                break;
            }
            if (logical < first) {
                ret = -EFSCORRUPTED;
                break;
            }
            if (logical >= last) {
                block = next;
                continue;
            }

            lo = 0;
            hi = count;
            while (lo < hi) {
                u32 mid = lo + (hi - lo) / 2u;
                u64 start = le64_to_cpu(ext[mid].logical_block);
                u32 blocks = le32_to_cpu(ext[mid].block_count);
                u32 flags = le32_to_cpu(ext[mid].flags);
                u64 end_logical;

                if (!blocks || start > U64_MAX - blocks) {
                    ret = -EFSCORRUPTED;
                    goto paged_out;
                }
                end_logical = start + blocks;
                if (logical < start) {
                    hi = mid;
                    continue;
                }
                if (logical >= end_logical) {
                    lo = mid + 1u;
                    continue;
                }
                if (!infilfs_extent_flags_valid(
                        blocks, le64_to_cpu(ext[mid].physical_block), flags)) {
                    ret = -EFSCORRUPTED;
                    goto paged_out;
                }
                *flags_out = flags;
                if (infilfs_extent_kind(flags) == INFILFS_EXTENT_HOLE)
                    *physical_out = 0;
                else if (infilfs_extent_is_compressed(flags))
                    *physical_out = le64_to_cpu(ext[mid].physical_block);
                else
                    *physical_out = le64_to_cpu(ext[mid].physical_block) +
                        (logical - start);
                if (extent_logical_out)
                    *extent_logical_out = start;
                if (extent_blocks_out)
                    *extent_blocks_out = blocks;
                ret = 0;
                goto paged_out;
            }
            ret = -EFSCORRUPTED;
            break;
        }
paged_out:
        kfree(page_block);
        return ret;
    }

    return -EFSCORRUPTED;
}''', "generic native extent-chain mapper")

text = replace_function(text, "static int infilfs_file_allocated_blocks(", r'''static int infilfs_file_allocated_blocks(
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
        for (i = 0; i < extent_count; ++i) {
            u32 flags = le32_to_cpu(extents[i].flags);
            u32 logical_blocks = le32_to_cpu(extents[i].block_count);

            allocated += infilfs_extent_physical_blocks(logical_blocks, flags);
        }
    } else if (le16_to_cpu(header->object_version) ==
               INFILFS_OBJECT_VERSION_PAGED) {
        const struct infilfs_extent_head_disk *head =
            (const struct infilfs_extent_head_disk *)(file + 1);
        const __le64 *root_ptr = (const __le64 *)(head + 1);
        u32 page_count = le32_to_cpu(head->page_count);
        u64 block = le64_to_cpu(*root_ptr);
        u8 *page_block;
        u32 p;

        if (!page_count || page_count > extent_count || !block ||
            le32_to_cpu(head->reserved) != 0 ||
            sizeof(*file) + sizeof(*head) + sizeof(*root_ptr) !=
                le32_to_cpu(header->payload_size))
            return -EFSCORRUPTED;
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
        if (!page_block)
            return -ENOMEM;
        for (p = 0; p < page_count; ++p) {
            const struct infilfs_metadata_page_disk *page;
            const struct infilfs_extent_disk *extents;
            u32 count;
            u32 i;
            u64 next;

            ret = infilfs_read_allocated_block(inode->i_sb, block, page_block);
            if (ret)
                break;
            if (!infilfs_metadata_page_valid(
                    inode->i_sb, page_block, infilfs_extent_page_magic,
                    header->object_id) ||
                !infilfs_extent_page_shape_valid(page_block, &count) ||
                copied > extent_count || count > extent_count - copied) {
                ret = -EFSCORRUPTED;
                break;
            }
            page = (const struct infilfs_metadata_page_disk *)page_block;
            extents = (const struct infilfs_extent_disk *)(page + 1);
            for (i = 0; i < count; ++i) {
                u32 flags = le32_to_cpu(extents[i].flags);
                u32 logical_blocks = le32_to_cpu(extents[i].block_count);

                allocated += infilfs_extent_physical_blocks(
                    logical_blocks, flags);
            }
            copied += count;
            next = infilfs_extent_page_next_block(page_block);
            if ((p + 1u < page_count && !next) ||
                (p + 1u == page_count && next)) {
                ret = -EFSCORRUPTED;
                break;
            }
            block = next;
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
}''', "native extent-chain allocation accounting")
write(path, text)


# Native write collection and rebuild now use dynamic chain storage rather than the fixed head vector.
path = "kernel/infiltratorfs_rw_data.inc"
text = read(path)
text = replace_function(text, "static int infilfs_native_collect_extents(", r'''static int infilfs_native_collect_extents(
    struct infilfs_native_pending *pending, struct inode *inode,
    u8 object[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_extent_disk **extents_out, u32 *count_out,
    u64 *old_blocks_out, bool *was_inline_out)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_object_header_disk *header;
    struct infilfs_file_payload_disk *file;
    struct infilfs_extent_disk *extents = NULL;
    u32 count;
    u64 logical = 0;
    int ret;
    u32 i;

    ret = infilfs_read_object(inode->i_sb, ii->object_block,
                              INFILFS_OBJECT_FILE, ii->object_id, object);
    if (ret)
        return ret;
    header = (struct infilfs_object_header_disk *)object;
    file = (struct infilfs_file_payload_disk *)(header + 1);
    count = le32_to_cpu(file->extent_count);
    *was_inline_out = false;

    if (!count) {
        u64 size = le64_to_cpu(file->attributes.logical_size);
        if (size > INFILFS_INLINE_DATA_MAX)
            return -EFSCORRUPTED;
        *was_inline_out = size != 0;
        *extents_out = NULL;
        *count_out = 0;
        *old_blocks_out = 0;
        return 0;
    }

    extents = kvmalloc_array(count, sizeof(*extents), GFP_NOFS);
    if (!extents)
        return -ENOMEM;

    if (le16_to_cpu(header->object_version) == INFILFS_OBJECT_VERSION_CLASSIC) {
        size_t need = sizeof(*file) + (size_t)count * sizeof(*extents);
        if (need != le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto fail;
        }
        memcpy(extents, file + 1, (size_t)count * sizeof(*extents));
    } else if (le16_to_cpu(header->object_version) ==
               INFILFS_OBJECT_VERSION_PAGED) {
        struct infilfs_extent_head_disk *head =
            (struct infilfs_extent_head_disk *)(file + 1);
        __le64 *root_ptr = (__le64 *)(head + 1);
        u32 page_count = le32_to_cpu(head->page_count);
        u32 copied = 0;
        u64 block = le64_to_cpu(*root_ptr);
        u8 *page_block;
        u32 p;

        if (!page_count || page_count > count || !block ||
            le32_to_cpu(head->reserved) != 0 ||
            sizeof(*file) + sizeof(*head) + sizeof(*root_ptr) !=
                le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto fail;
        }
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!page_block) {
            ret = -ENOMEM;
            goto fail;
        }
        for (p = 0; p < page_count; ++p) {
            struct infilfs_metadata_page_disk *page;
            u32 n;
            u64 next;

            ret = infilfs_read_allocated_block(inode->i_sb, block, page_block);
            if (ret)
                break;
            if (!infilfs_metadata_page_valid(
                    inode->i_sb, page_block, infilfs_extent_page_magic,
                    ii->object_id) ||
                !infilfs_extent_page_shape_valid(page_block, &n) ||
                copied > count || n > count - copied) {
                ret = -EFSCORRUPTED;
                break;
            }
            page = (struct infilfs_metadata_page_disk *)page_block;
            memcpy(extents + copied, page + 1, (size_t)n * sizeof(*extents));
            copied += n;
            next = infilfs_extent_page_next_block(page_block);
            if ((p + 1u < page_count && !next) ||
                (p + 1u == page_count && next)) {
                ret = -EFSCORRUPTED;
                break;
            }
            block = next;
        }
        kfree(page_block);
        if (ret)
            goto fail;
        if (copied != count) {
            ret = -EFSCORRUPTED;
            goto fail;
        }
    } else {
        ret = -EFSCORRUPTED;
        goto fail;
    }

    for (i = 0; i < count; ++i) {
        u64 start = le64_to_cpu(extents[i].logical_block);
        u64 physical = le64_to_cpu(extents[i].physical_block);
        u32 blocks = le32_to_cpu(extents[i].block_count);
        u32 flags = le32_to_cpu(extents[i].flags);

        if (!blocks || start != logical ||
            !infilfs_extent_flags_valid(blocks, physical, flags)) {
            ret = -EFSCORRUPTED;
            goto fail;
        }
        if (infilfs_extent_is_compressed(flags) &&
            (le64_to_cpu(INFILFS_SB(inode->i_sb)->disk.incompat_flags) &
             INFILFS_INCOMPAT_COMPRESSED_EXTENTS) == 0) {
            ret = -EFSCORRUPTED;
            goto fail;
        }
        logical += blocks;
    }
    *extents_out = extents;
    *count_out = count;
    *old_blocks_out = logical;
    return 0;
fail:
    kvfree(extents);
    return ret;
}''', "native extent-chain collection")

text = replace_function(text, "static int infilfs_native_build_file_object(", r'''static int infilfs_native_build_file_object(
    struct infilfs_native_pending *pending, struct inode *inode,
    const u8 old_object[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_file_payload_disk *working_file,
    const struct infilfs_extent_disk *extents, u32 extent_count,
    u64 new_size, u8 new_object[INFILFS_DISK_BLOCK_SIZE],
    u64 *new_object_block, bool *repoint_needed)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    const struct infilfs_object_header_disk *old_header =
        (const struct infilfs_object_header_disk *)old_object;
    const struct infilfs_file_payload_disk *old_file =
        (const struct infilfs_file_payload_disk *)(old_header + 1);
    u64 *old_pages = NULL;
    u32 old_page_count = 0;
    struct infilfs_object_header_disk *header;
    struct infilfs_file_payload_disk *file;
    const u32 classic_max = (INFILFS_DISK_BLOCK_SIZE -
        sizeof(struct infilfs_object_header_disk) -
        sizeof(struct infilfs_file_payload_disk)) /
        sizeof(struct infilfs_extent_disk);
    bool paged = extent_count && (extent_count > classic_max ||
        le16_to_cpu(old_header->object_version) == INFILFS_OBJECT_VERSION_PAGED);
    u64 stored_block;
    int ret = 0;

    if (le16_to_cpu(old_header->object_version) ==
        INFILFS_OBJECT_VERSION_PAGED) {
        const struct infilfs_extent_head_disk *old_head =
            (const struct infilfs_extent_head_disk *)(old_file + 1);
        const __le64 *root_ptr = (const __le64 *)(old_head + 1);
        u64 block = le64_to_cpu(*root_ptr);
        u8 *page_block;
        u32 p;

        old_page_count = le32_to_cpu(old_head->page_count);
        if (!old_page_count || old_page_count > le32_to_cpu(old_file->extent_count) ||
            le32_to_cpu(old_head->reserved) != 0 || !block ||
            sizeof(*old_file) + sizeof(*old_head) + sizeof(*root_ptr) !=
                le32_to_cpu(old_header->payload_size))
            return -EFSCORRUPTED;
        old_pages = kvmalloc_array(old_page_count, sizeof(*old_pages), GFP_NOFS);
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!old_pages || !page_block) {
            kvfree(old_pages);
            kfree(page_block);
            return -ENOMEM;
        }
        for (p = 0; p < old_page_count; ++p) {
            u32 n;
            u64 next;

            old_pages[p] = block;
            ret = infilfs_read_allocated_block(pending->sb, block, page_block);
            if (ret)
                break;
            if (!infilfs_metadata_page_valid(
                    pending->sb, page_block, infilfs_extent_page_magic,
                    ii->object_id) ||
                !infilfs_extent_page_shape_valid(page_block, &n)) {
                ret = -EFSCORRUPTED;
                break;
            }
            next = infilfs_extent_page_next_block(page_block);
            if ((p + 1u < old_page_count && !next) ||
                (p + 1u == old_page_count && next)) {
                ret = -EFSCORRUPTED;
                break;
            }
            block = next;
        }
        kfree(page_block);
        if (ret)
            goto out;
    }

    memset(new_object, 0, INFILFS_DISK_BLOCK_SIZE);
    memcpy(new_object, old_object,
           sizeof(struct infilfs_object_header_disk) +
           sizeof(struct infilfs_file_payload_disk));
    header = (struct infilfs_object_header_disk *)new_object;
    file = (struct infilfs_file_payload_disk *)(header + 1);
    memcpy(file->checksum_head_id, working_file->checksum_head_id, 16);
    file->attributes.logical_size = cpu_to_le64(new_size);
    infilfs_timestamp_encode_ns(&file->attributes.modification_time,
                                ktime_get_real_ns());
    file->attributes.change_time = file->attributes.modification_time;
    file->extent_count = cpu_to_le32(extent_count);
    file->data_checksum_type = cpu_to_le32(INFILFS_CHECKSUM_SHA256);
    header->generation = cpu_to_le64(pending->tx.generation);

    if (!paged) {
        u32 p;

        header->object_version = cpu_to_le16(INFILFS_OBJECT_VERSION_CLASSIC);
        header->payload_size = cpu_to_le32(sizeof(*file) +
            (size_t)extent_count * sizeof(*extents));
        if (extent_count)
            memcpy(file + 1, extents, (size_t)extent_count * sizeof(*extents));
        for (p = 0; p < old_page_count; ++p) {
            ret = infilfs_rw_tx_defer_free(&pending->tx, old_pages[p], 1);
            if (ret)
                goto out;
        }
    } else {
        struct infilfs_extent_head_disk *head =
            (struct infilfs_extent_head_disk *)(file + 1);
        __le64 *root_ptr = (__le64 *)(head + 1);
        u32 pages = DIV_ROUND_UP(extent_count, INFILFS_EXTENTS_PER_PAGE);
        u64 next_block = 0;
        u8 *page_block;
        u32 p;

        if (!(le64_to_cpu(pending->tx.next_sb.incompat_flags) &
              INFILFS_INCOMPAT_PAGED_EXTENTS) || !pages || pages > extent_count ||
            pages > infilfs_volume_blocks(pending->tx.sbi)) {
            ret = -EFBIG;
            goto out;
        }
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!page_block) {
            ret = -ENOMEM;
            goto out;
        }

        header->object_version = cpu_to_le16(INFILFS_OBJECT_VERSION_PAGED);
        head->page_count = cpu_to_le32(pages);
        head->reserved = 0;

        for (p = pages; p-- > 0;) {
            u32 at = p * INFILFS_EXTENTS_PER_PAGE;
            u32 n = min_t(u32, INFILFS_EXTENTS_PER_PAGE, extent_count - at);
            u64 old_page_no = p < old_page_count ? old_pages[p] : 0;
            u64 block;

            ret = infilfs_native_build_extent_page(
                pending, ii->object_id, extents + at, n, next_block, page_block);
            if (ret)
                break;
            ret = infilfs_native_store_extent_page(
                pending, old_page_no, page_block, &block);
            if (ret)
                break;
            next_block = block;
        }
        if (!ret) {
            for (p = pages; p < old_page_count; ++p) {
                ret = infilfs_rw_tx_defer_free(&pending->tx, old_pages[p], 1);
                if (ret)
                    break;
            }
        }
        if (!ret) {
            *root_ptr = cpu_to_le64(next_block);
            header->payload_size = cpu_to_le32(
                sizeof(*file) + sizeof(*head) + sizeof(*root_ptr));
        }
        kfree(page_block);
        if (ret)
            goto out;
    }

    ret = infilfs_rw_finalize_object(new_object);
    if (ret)
        goto out;
    ret = infilfs_native_store_private_or_cow(
        pending, ii->object_block, new_object, &stored_block);
    if (ret)
        goto out;
    *new_object_block = stored_block;
    *repoint_needed = stored_block != ii->object_block;
out:
    kvfree(old_pages);
    return ret;
}''', "native chain file-object builder")

text = replace_function(text, "static int infilfs_native_build_extent_page(", r'''static int infilfs_native_build_extent_page(
    struct infilfs_native_pending *pending, const u8 owner_id[16],
    const struct infilfs_extent_disk *extents, u32 count, u64 next_block,
    u8 page_block[INFILFS_DISK_BLOCK_SIZE])
{
    struct infilfs_metadata_page_disk *page;
    int ret;

    if (!count || count > INFILFS_EXTENTS_PER_PAGE)
        return -EINVAL;
    infilfs_rw_init_page(page_block, infilfs_extent_page_magic,
                         owner_id, pending->tx.generation);
    page = (struct infilfs_metadata_page_disk *)page_block;
    page->entry_count = cpu_to_le32(count);
    memcpy(page + 1, extents, (size_t)count * sizeof(*extents));
    ret = infilfs_extent_page_set_next_block(page_block, next_block);
    if (ret)
        return ret;
    return infilfs_rw_finalize_page(page_block);
}''', "native chain extent-page builder")

text = replace_function(text, "static int infilfs_native_try_single_paged_overwrite(", r'''static int infilfs_native_try_single_paged_overwrite(
    struct infilfs_native_pending *pending, struct inode *inode,
    const u8 write_data[INFILFS_DISK_BLOCK_SIZE], u64 pos, size_t bytes,
    const u8 old_object[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_parallel_reservation *reservation,
    u64 *new_file_block_out, u64 *new_size_out,
    u64 *allocated_blocks_out, u8 final_tail_id[16],
    u64 *final_tail_block, u64 *final_tail_start)
{
    /*
     * The old page-local fast path depended on random indexing into the bounded
     * head pointer array.  The authoritative generic path is chain-aware and
     * remains CoW-correct, so use it until a prefix-rewrite chain fast path is
     * introduced.  This is deliberately fail-safe rather than interpreting a
     * root pointer as the former vector.
     */
    (void)pending;
    (void)inode;
    (void)write_data;
    (void)pos;
    (void)bytes;
    (void)old_object;
    (void)reservation;
    (void)new_file_block_out;
    (void)new_size_out;
    (void)allocated_blocks_out;
    (void)final_tail_id;
    (void)final_tail_block;
    (void)final_tail_start;
    return -EOPNOTSUPP;
}''', "disable stale bounded paged-overwrite fast path")
write(path, text)


# Namespace/reflink walkers and unlink reclamation follow and free the physical chain.
path = "kernel/infiltratorfs_rw_namespace.inc"
text = read(path)
text = replace_function(text, "static int infilfs_ns_read_file_extents(", r'''static int infilfs_ns_read_file_extents(
    struct super_block *sb, u64 object_block, const u8 object_id[16],
    struct infilfs_extent_disk **extents_out, u32 *count_out)
{
    u8 *object = NULL, *page_block = NULL;
    struct infilfs_object_header_disk *header;
    struct infilfs_file_payload_disk *file;
    struct infilfs_extent_disk *extents = NULL;
    u32 count, copied = 0;
    int ret;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;
    ret = infilfs_read_object(sb, object_block, INFILFS_OBJECT_FILE,
                              object_id, object);
    if (ret)
        goto out;
    header = (struct infilfs_object_header_disk *)object;
    file = (struct infilfs_file_payload_disk *)(header + 1);
    count = le32_to_cpu(file->extent_count);
    if (!count) {
        *extents_out = NULL;
        *count_out = 0;
        ret = 0;
        goto out;
    }
    extents = kvmalloc_array(count, sizeof(*extents), GFP_NOFS);
    if (!extents) {
        ret = -ENOMEM;
        goto out;
    }
    if (le16_to_cpu(header->object_version) == INFILFS_OBJECT_VERSION_CLASSIC) {
        size_t need = sizeof(*file) + (size_t)count * sizeof(*extents);
        if (need != le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        memcpy(extents, file + 1, (size_t)count * sizeof(*extents));
        copied = count;
    } else if (le16_to_cpu(header->object_version) == INFILFS_OBJECT_VERSION_PAGED) {
        struct infilfs_extent_head_disk *head =
            (struct infilfs_extent_head_disk *)(file + 1);
        __le64 *root_ptr = (__le64 *)(head + 1);
        u32 page_count = le32_to_cpu(head->page_count);
        u64 block = le64_to_cpu(*root_ptr);
        u32 p;

        if (!page_count || page_count > count || !block ||
            le32_to_cpu(head->reserved) != 0 ||
            sizeof(*file) + sizeof(*head) + sizeof(*root_ptr) !=
                le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!page_block) {
            ret = -ENOMEM;
            goto out;
        }
        for (p = 0; p < page_count; ++p) {
            struct infilfs_metadata_page_disk *page;
            u32 n;
            u64 next;

            ret = infilfs_read_allocated_block(sb, block, page_block);
            if (ret)
                goto out;
            if (!infilfs_metadata_page_valid(
                    sb, page_block, infilfs_extent_page_magic, object_id) ||
                !infilfs_extent_page_shape_valid(page_block, &n) ||
                copied > count || n > count - copied) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            page = (struct infilfs_metadata_page_disk *)page_block;
            memcpy(extents + copied, page + 1, (size_t)n * sizeof(*extents));
            copied += n;
            next = infilfs_extent_page_next_block(page_block);
            if ((p + 1u < page_count && !next) ||
                (p + 1u == page_count && next)) {
                ret = -EFSCORRUPTED;
                goto out;
            }
            block = next;
        }
    } else {
        ret = -EFSCORRUPTED;
        goto out;
    }
    if (copied != count) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    *extents_out = extents;
    *count_out = count;
    extents = NULL;
    ret = 0;
out:
    kvfree(extents);
    kfree(page_block);
    kfree(object);
    return ret;
}''', "namespace extent-chain reader")

text = replace_function(text, "static int infilfs_ns_delete_file_resources(", r'''static int infilfs_ns_delete_file_resources(
    struct infilfs_native_pending *pending, struct inode *inode,
    struct infilfs_ns_index_change **changes, u32 *change_count,
    u32 *change_capacity)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    u8 *object;
    struct infilfs_object_header_disk *header;
    struct infilfs_file_payload_disk *file;
    struct infilfs_extent_disk *extents = NULL;
    u32 extent_count = 0, i;
    u64 old_blocks = 0;
    bool was_inline = false;
    int ret;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;
    ret = infilfs_native_collect_extents(pending, inode, object, &extents,
                                         &extent_count, &old_blocks,
                                         &was_inline);
    if (ret)
        goto out;
    (void)old_blocks;
    (void)was_inline;
    header = (struct infilfs_object_header_disk *)object;
    file = (struct infilfs_file_payload_disk *)(header + 1);
    ret = infilfs_ns_collect_checksum_removals(
        pending, file, ii->object_id, changes, change_count, change_capacity);
    if (ret)
        goto out;
    for (i = 0; i < extent_count; ++i) {
        u32 flags = le32_to_cpu(extents[i].flags);
        u32 logical_blocks = le32_to_cpu(extents[i].block_count);

        if (infilfs_extent_kind(flags) != INFILFS_EXTENT_NORMAL)
            continue;
        ret = infilfs_ns_free_unshared_run(
            pending, ii->object_id,
            le64_to_cpu(extents[i].physical_block),
            infilfs_extent_physical_blocks(logical_blocks, flags));
        if (ret)
            goto out;
    }

    if (le16_to_cpu(header->object_version) == INFILFS_OBJECT_VERSION_PAGED) {
        struct infilfs_extent_head_disk *head =
            (struct infilfs_extent_head_disk *)(file + 1);
        __le64 *root_ptr = (__le64 *)(head + 1);
        u32 page_count = le32_to_cpu(head->page_count);
        u64 block = le64_to_cpu(*root_ptr);
        u8 *page_block = NULL;
        u32 p;

        if (!page_count || page_count > extent_count || !block ||
            le32_to_cpu(head->reserved) != 0 ||
            sizeof(*file) + sizeof(*head) + sizeof(*root_ptr) !=
                le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!page_block) {
            ret = -ENOMEM;
            goto out;
        }
        for (p = 0; p < page_count; ++p) {
            u32 n;
            u64 next;

            ret = infilfs_read_allocated_block(pending->sb, block, page_block);
            if (ret)
                break;
            if (!infilfs_metadata_page_valid(
                    pending->sb, page_block, infilfs_extent_page_magic,
                    ii->object_id) ||
                !infilfs_extent_page_shape_valid(page_block, &n)) {
                ret = -EFSCORRUPTED;
                break;
            }
            next = infilfs_extent_page_next_block(page_block);
            if ((p + 1u < page_count && !next) ||
                (p + 1u == page_count && next)) {
                ret = -EFSCORRUPTED;
                break;
            }
            ret = infilfs_rw_tx_defer_free(&pending->tx, block, 1);
            if (ret)
                break;
            block = next;
        }
        kfree(page_block);
        if (ret)
            goto out;
    }

    ret = infilfs_rw_tx_defer_free(&pending->tx, ii->object_block, 1);
    if (ret)
        goto out;
    ret = infilfs_ns_changes_append(changes, change_count, change_capacity,
                                    ii->object_id, INFILFS_NS_REMOVE, 0,
                                    INFILFS_OBJECT_FILE);
out:
    kvfree(extents);
    kfree(object);
    return ret;
}''', "namespace chain metadata reclamation")
write(path, text)


# The prior policy guarded the bounded vector fast path. During chain migration,
# guard that no native consumer still treats the root as an array.
policy_path = ROOT / "tests/native-random-write-optimization-policy.sh"
policy = policy_path.read_text(encoding="utf-8")
if "Native extent-chain migration policy guard passed." not in policy:
    policy = '''#!/usr/bin/env bash
set -euo pipefail
root="${1:-.}"
data="$root/kernel/infiltratorfs_rw_data.inc"
readcache="$root/kernel/infiltratorfs_read_cache.c"
core="$root/kernel/infiltratorfs_core.c"
ns="$root/kernel/infiltratorfs_rw_namespace.inc"
internal="$root/kernel/infiltratorfs_internal.h"

grep -Fq 'infilfs_extent_page_shape_valid' "$internal"
grep -Fq 'infilfs_extent_page_next_block' "$readcache"
grep -Fq 'sizeof(*file) + sizeof(*head) + sizeof(*root_ptr)' "$core"
grep -Fq 'for (p = pages; p-- > 0;)' "$data"
grep -Fq 'return -EOPNOTSUPP;' "$data"
grep -Fq 'infilfs_extent_page_next_block(page_block)' "$ns"
! grep -Fq 'INFILFS_EXTENT_PAGE_POINTERS' "$readcache"
! grep -Fq 'INFILFS_EXTENT_PAGE_POINTERS' "$core"
! grep -Fq 'INFILFS_EXTENT_PAGE_POINTERS' "$data"
! grep -Fq 'INFILFS_EXTENT_PAGE_POINTERS' "$ns"

printf 'Native extent-chain migration policy guard passed.\\n'
'''
    policy_path.write_text(policy, encoding="utf-8")

structural = ROOT / "tests/structural-overhaul-policy.sh"
p = structural.read_text(encoding="utf-8")
if "native kernel must not retain the bounded extent head vector" not in p:
    p += '''
# The native kernel must not retain the bounded extent head vector semantics.
! grep -Fq 'INFILFS_EXTENT_PAGE_POINTERS' "$root/kernel/infiltratorfs_read_cache.c"
! grep -Fq 'INFILFS_EXTENT_PAGE_POINTERS' "$root/kernel/infiltratorfs_core.c"
! grep -Fq 'INFILFS_EXTENT_PAGE_POINTERS' "$root/kernel/infiltratorfs_rw_data.inc"
! grep -Fq 'INFILFS_EXTENT_PAGE_POINTERS' "$root/kernel/infiltratorfs_rw_namespace.inc"
grep -Fq 'infilfs_extent_page_shape_valid' "$root/kernel/infiltratorfs_internal.h"
'''
    structural.write_text(p, encoding="utf-8")

print("Structural recovery phase 3 native extent chain applied.")
