#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Repair extent-chain link placement so metadata finalization preserves it."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


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


path = ROOT / "src/volume/paged-extents.inc"
text = path.read_text(encoding="utf-8")

text = replace_function(text, "static uint64_t extent_page_next_block(", r'''static uint64_t extent_page_next_block(const uint8_t block[INFS_BLOCK_SIZE])
{
    const struct infs_metadata_page_disk *page =
        (const struct infs_metadata_page_disk *)block;
    uint32_t count = infs_le32_to_cpu(page->entry_count);
    size_t offset = sizeof(*page) +
        (size_t)count * sizeof(struct infs_extent_disk);
    uint64_t encoded = 0;

    if (!count || count > INFS_EXTENTS_PER_PAGE ||
        offset > INFS_BLOCK_SIZE - sizeof(encoded))
        return 0;
    memcpy(&encoded, block + offset, sizeof(encoded));
    return infs_le64_to_cpu(encoded);
}''', "extent-chain next-link reader")

text = replace_function(text, "static void extent_page_set_next_block(", r'''static void extent_page_set_next_block(uint8_t block[INFS_BLOCK_SIZE],
                                       uint64_t next)
{
    struct infs_metadata_page_disk *page =
        (struct infs_metadata_page_disk *)block;
    uint32_t count = infs_le32_to_cpu(page->entry_count);
    size_t extent_bytes = (size_t)count * sizeof(struct infs_extent_disk);
    size_t offset = sizeof(*page) + extent_bytes;
    uint64_t encoded = infs_cpu_to_le64(next);

    if (!count || count > INFS_EXTENTS_PER_PAGE ||
        offset > INFS_BLOCK_SIZE - sizeof(encoded))
        return;
    memcpy(block + offset, &encoded, sizeof(encoded));
    page->bytes_used = infs_cpu_to_le32(
        (uint32_t)(extent_bytes + sizeof(encoded)));
}''', "extent-chain next-link writer")

text = replace_function(text, "static int extent_page_validate(", r'''static int extent_page_validate(struct infs_volume *vol,
                                const uint8_t block[INFS_BLOCK_SIZE],
                                const uint8_t owner_id[16],
                                struct infs_extent_disk **entries_out,
                                uint32_t *count_out)
{
    if (!infs_validate_metadata_page(block, INFS_EXTENT_PAGE_MAGIC, owner_id) ||
        !metadata_page_generation_valid(vol, block))
        return INFS_STATUS_CORRUPT;
    const struct infs_metadata_page_disk *page =
        (const struct infs_metadata_page_disk *)block;
    uint32_t count = infs_le32_to_cpu(page->entry_count);
    uint32_t bytes = infs_le32_to_cpu(page->bytes_used);
    uint64_t extent_bytes;

    if (!count || count > INFS_EXTENTS_PER_PAGE)
        return INFS_STATUS_CORRUPT;
    extent_bytes = (uint64_t)count * sizeof(struct infs_extent_disk);
    if (extent_bytes + sizeof(uint64_t) > INFS_METADATA_PAGE_DATA_SIZE ||
        bytes != extent_bytes + sizeof(uint64_t))
        return INFS_STATUS_CORRUPT;

    struct infs_extent_disk *ext = (struct infs_extent_disk *)(
        (uint8_t *)block + sizeof(*page));
    uint64_t next = infs_le64_to_cpu(ext[0].logical_block);
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t logical = infs_le64_to_cpu(ext[i].logical_block);
        uint64_t physical = infs_le64_to_cpu(ext[i].physical_block);
        uint32_t blocks = infs_le32_to_cpu(ext[i].block_count);
        uint32_t flags = infs_le32_to_cpu(ext[i].flags);
        uint64_t physical_blocks;
        if (!blocks || logical != next || logical > UINT64_MAX - blocks ||
            !extent_flags_valid(blocks, physical, flags))
            return INFS_STATUS_CORRUPT;
        if (extent_kind(flags) == INFS_EXTENT_HOLE) {
            next += blocks;
            continue;
        }
        if (extent_is_compressed(flags) &&
            (infs_le64_to_cpu(vol->sb.incompat_flags) &
             INFS_INCOMPAT_COMPRESSED_EXTENTS) == 0)
            return INFS_STATUS_CORRUPT;
        physical_blocks = extent_physical_blocks(blocks, flags);
        if (!physical_blocks || physical >= total ||
            physical_blocks > total - physical)
            return INFS_STATUS_CORRUPT;
        next += blocks;
    }
    if (entries_out)
        *entries_out = ext;
    if (count_out)
        *count_out = count;
    return INFS_STATUS_OK;
}''', "extent-chain page validation")

path.write_text(text, encoding="utf-8")

policy = ROOT / "tests/structural-overhaul-policy.sh"
p = policy.read_text(encoding="utf-8")
needle = "bytes != extent_bytes + sizeof(uint64_t)"
if needle not in p:
    p += """
# The chain link is part of bytes_used, so metadata finalization checksums it
# instead of zeroing it as unused tail padding.
grep -Fq 'bytes != extent_bytes + sizeof(uint64_t)' "$root/src/volume/paged-extents.inc"
grep -Fq 'extent_bytes + sizeof(encoded)' "$root/src/volume/paged-extents.inc"
"""
    policy.write_text(p, encoding="utf-8")

print("Structural recovery phase 2 chain-link finalization fix applied.")
